//===----------------------------------------------------------------------===//
//                         DuckDB
//
// thrift_tools.hpp
//
//
//===----------------------------------------------------------------------===/

#pragma once

#include "thrift/protocol/TCompactProtocol.h"
#include "thrift/transport/TBufferTransports.h"

#include "duckdb.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/storage/external_file_cache/file_buffer_handle_group.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/allocator.hpp"

namespace duckdb {

// A ReadHead for prefetching data in a specific range
struct ReadHead {
	ReadHead(idx_t location, uint64_t size) : location(location), size(size) {};
	// Hint info
	idx_t location;
	uint64_t size;

	// Current info
	FileBufferHandleGroup handle_group;
	AllocatedData local_buffer;
	const_data_ptr_t buffer_ptr = nullptr;
	bool data_isset = false;

	idx_t GetEnd() const {
		return size + location;
	}

	// Materialize [buffer_ptr], should call before access.
	// TODO(hjiang): Currently it's only used for `Prefetch` operation, should be able to save one copy.
	void Materialize(Allocator &allocator) {
		if (handle_group.GetHandles().size() == 1) {
			buffer_ptr = handle_group.Ptr();
		} else {
			local_buffer = allocator.Allocate(size);
			handle_group.CopyTo(local_buffer.get(), size);
			buffer_ptr = local_buffer.get();
		}
	}

	void Fetch(CachingFileHandle &file_handle) {
		if (GetEnd() > file_handle.GetFileSize()) {
			throw std::runtime_error("Prefetch registered requested for bytes outside file");
		}
		handle_group = file_handle.Read(size, location);
		Materialize(file_handle.GetBufferAllocator());
		data_isset = true;
	}
};

struct ReadHeadComparator {
	static constexpr uint64_t DEFAULT_ACCEPTED_COLUMN_GAP = 1 << 14; // 16 KiB

	ReadHeadComparator() = default;
	explicit ReadHeadComparator(uint64_t accepted_column_gap_p) : accepted_column_gap(accepted_column_gap_p) {
	}

	uint64_t accepted_column_gap = DEFAULT_ACCEPTED_COLUMN_GAP;

	bool operator()(const ReadHead *a, const ReadHead *b) const {
		auto a_start = a->location;
		auto a_end = a->location + a->size;
		auto b_start = b->location;

		if (a_end <= NumericLimits<idx_t>::Maximum() - accepted_column_gap) {
			a_end += accepted_column_gap;
		}

		return a_start < b_start && a_end < b_start;
	}
};

// Two-step read ahead buffer
// 1: register all ranges that will be read, merging ranges that are consecutive
// 2: prefetch all registered ranges
struct ReadAheadBuffer {
	explicit ReadAheadBuffer(CachingFileHandle &file_handle_p,
	                         uint64_t accepted_column_gap = ReadHeadComparator::DEFAULT_ACCEPTED_COLUMN_GAP)
	    : merge_set(ReadHeadComparator(accepted_column_gap)), file_handle(file_handle_p) {
	}

	// The list of read heads.
	vector<shared_ptr<ReadHead>> read_heads;
	// Set for merging consecutive ranges
	std::set<ReadHead *, ReadHeadComparator> merge_set;

	CachingFileHandle &file_handle;

	idx_t total_size = 0;

	void SetAcceptedColumnGap(uint64_t accepted_column_gap) {
		D_ASSERT(merge_set.empty());
		merge_set = std::set<ReadHead *, ReadHeadComparator>(ReadHeadComparator(accepted_column_gap));
	}

	// Add a read head to the prefetching list
	void AddReadHead(idx_t pos, uint64_t len, bool merge_buffers = true) {
		// Attempt to merge with existing
		if (merge_buffers) {
			ReadHead new_read_head {pos, len};
			auto lookup_set = merge_set.find(&new_read_head);
			if (lookup_set != merge_set.end()) {
				auto existing_head = *lookup_set;
				auto new_start = MinValue<idx_t>(existing_head->location, new_read_head.location);
				auto new_length = MaxValue<idx_t>(existing_head->GetEnd(), new_read_head.GetEnd()) - new_start;
				existing_head->location = new_start;
				existing_head->size = new_length;
				return;
			}
		}

		read_heads.insert(read_heads.begin(), make_shared_ptr<ReadHead>(pos, len));
		total_size += len;
		auto &read_head = *read_heads.front();

		if (merge_buffers) {
			merge_set.insert(&read_head);
		}

		if (read_head.GetEnd() > file_handle.GetFileSize()) {
			throw std::runtime_error("Prefetch registered for bytes outside file: " + file_handle.GetPath() +
			                         ", attempted range: [" + std::to_string(pos) + ", " +
			                         std::to_string(read_head.GetEnd()) +
			                         "), file size: " + std::to_string(file_handle.GetFileSize()));
		}
	}

	// Add a read head whose bytes are already in memory; the caller keeps them alive while the head exists
	void AddFilledReadHead(idx_t pos, uint64_t len, const_data_ptr_t data) {
		read_heads.insert(read_heads.begin(), make_shared_ptr<ReadHead>(pos, len));
		auto &read_head = *read_heads.front();
		read_head.buffer_ptr = data;
		read_head.data_isset = true;
	}

	// Returns the relevant read head
	ReadHead *GetReadHead(idx_t pos, idx_t len = 1) {
		optional_ptr<ReadHead> unfetched_head;
		for (auto &read_head : read_heads) {
			if (pos >= read_head->location && pos < read_head->GetEnd() && len <= read_head->GetEnd() - pos) {
				// Filter and payload ranges can overlap: reuse fetched bytes before fetching another range.
				if (read_head->data_isset) {
					return read_head.get();
				}
				if (!unfetched_head) {
					unfetched_head = read_head.get();
				}
			}
		}
		return unfetched_head.get();
	}

	// Prefetch all read heads
	void Prefetch() {
		for (auto &read_head : read_heads) {
			if (!read_head->data_isset) {
				read_head->Fetch(file_handle);
			}
		}
	}
};

class ThriftFileTransport : public duckdb_apache::thrift::transport::TVirtualTransport<ThriftFileTransport> {
public:
	static constexpr uint64_t PREFETCH_FALLBACK_BUFFERSIZE = 1000000;

	ThriftFileTransport(QueryContext context_p, CachingFileHandle &file_handle_p, bool prefetch_mode_p,
	                    uint64_t accepted_column_gap = ReadHeadComparator::DEFAULT_ACCEPTED_COLUMN_GAP)
	    : context(context_p), file_handle(file_handle_p), location(0), size(file_handle.GetFileSize()),
	      ra_buffer(ReadAheadBuffer(file_handle, accepted_column_gap)), prefetch_mode(prefetch_mode_p) {
	}

	void SetAcceptedColumnGap(uint64_t accepted_column_gap) {
		ra_buffer.SetAcceptedColumnGap(accepted_column_gap);
	}

	// The accepted column gap currently used to coalesce adjacent ranges.
	uint64_t GetAcceptedColumnGap() const {
		return ra_buffer.merge_set.key_comp().accepted_column_gap;
	}

	uint32_t read(uint8_t *buf, uint32_t len) {
		auto prefetch_buffer = ra_buffer.GetReadHead(location, len);
		if (prefetch_buffer != nullptr && location - prefetch_buffer->location + len <= prefetch_buffer->size) {
			D_ASSERT(location - prefetch_buffer->location + len <= prefetch_buffer->size);

			if (!prefetch_buffer->data_isset) {
				prefetch_buffer->Fetch(file_handle);
			}
			memcpy(buf, prefetch_buffer->buffer_ptr + location - prefetch_buffer->location, len);
		} else if (prefetch_mode && len < PREFETCH_FALLBACK_BUFFERSIZE && len > 0) {
			auto file_size = file_handle.GetFileSize();
			if (location >= file_size) {
				file_handle.GetFileHandle()->Read(context, buf, len, location);
				location += len;
				return len;
			}
			Prefetch(location, MinValue<uint64_t>(PREFETCH_FALLBACK_BUFFERSIZE, file_size - location));
			auto prefetch_buffer_fallback = ra_buffer.GetReadHead(location, len);
			if (!prefetch_buffer_fallback ||
			    location - prefetch_buffer_fallback->location + len > prefetch_buffer_fallback->size) {
				file_handle.GetFileHandle()->Read(context, buf, len, location);
				location += len;
				return len;
			}
			memcpy(buf, prefetch_buffer_fallback->buffer_ptr + location - prefetch_buffer_fallback->location, len);
		} else {
			// No prefetch, do a regular (non-caching) read
			file_handle.GetFileHandle()->Read(context, buf, len, location);
		}

		location += len;
		return len;
	}

	// Serve reads of [pos, pos + len) from bytes that are already in memory
	void AddFilledReadHead(idx_t pos, uint64_t len, const_data_ptr_t data) {
		ra_buffer.AddFilledReadHead(pos, len, data);
	}

	// Prefetch a single buffer
	void Prefetch(idx_t pos, uint64_t len) {
		RegisterPrefetch(pos, len, false);
		FinalizeRegistration();
		PrefetchRegistered();
	}

	// Register a buffer for prefixing
	void RegisterPrefetch(idx_t pos, uint64_t len, bool can_merge = true) {
		ra_buffer.AddReadHead(pos, len, can_merge);
	}

	// Prevents any further merges, should be called before PrefetchRegistered
	void FinalizeRegistration() {
		ra_buffer.merge_set.clear();
	}

	// Prefetch all previously registered ranges
	void PrefetchRegistered() {
		ra_buffer.Prefetch();
	}

	void ClearPrefetch() {
		ra_buffer.read_heads.clear();
		ra_buffer.merge_set.clear();
	}

	void Skip(idx_t skip_count) {
		location += skip_count;
	}

	bool HasPrefetch() const {
		return !ra_buffer.read_heads.empty() || !ra_buffer.merge_set.empty();
	}

	void SetLocation(idx_t location_p) {
		location = location_p;
	}

	idx_t GetLocation() const {
		return location;
	}

	optional_ptr<ReadHead> GetReadHead(idx_t pos) {
		return ra_buffer.GetReadHead(pos);
	}

	vector<shared_ptr<ReadHead>> &GetReadHeads() {
		return ra_buffer.read_heads;
	}

	CachingFileHandle &GetCachingFileHandle() const {
		return file_handle;
	}

	idx_t GetSize() const {
		return size;
	}

private:
	QueryContext context;

	CachingFileHandle &file_handle;
	idx_t location;
	idx_t size;

	// Multi-buffer prefetch
	ReadAheadBuffer ra_buffer;

	// Whether the prefetch mode is enabled. In this mode the DirectIO flag of the handle will be set and the parquet
	// reader will manage the read buffering.
	bool prefetch_mode;
};

} // namespace duckdb
