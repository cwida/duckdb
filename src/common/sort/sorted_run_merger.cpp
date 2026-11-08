#include "duckdb/common/sorting/sorted_run_merger.hpp"

#include "duckdb/common/array_ptr.hpp"
#include "duckdb/main/client_context.hpp"

#include "duckdb/common/mutex.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/common/thread_annotation.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/sorting/buffered_merge.hpp"
#include "duckdb/common/types/row/block_iterator.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/parallel/parallel_destroy_task.hpp"

#include <algorithm>
#include <new>
#include <type_traits>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Sorted Run Merger Utility
//===--------------------------------------------------------------------===//
static idx_t SortedRunsTotalCount(const vector<unique_ptr<SortedRun>> &sorted_runs) {
	idx_t total_count = 0;
	for (const auto &sorted_run : sorted_runs) {
		D_ASSERT(sorted_run->finalized);
		total_count += sorted_run->Count();
	}
	return total_count;
}

static idx_t RowPartitionCount(const SortedRunMerger &merger) {
	if (merger.total_count == 0) {
		return 0;
	}
	return merger.total_count / merger.partition_size + (merger.total_count % merger.partition_size != 0);
}

struct SortedRunPartitionBoundary {
	idx_t begin;
	idx_t end;
};

struct SortedRunBoundaryReader {
	idx_t partition_idx;
	idx_t lower_partition_idx;
};

//! Longest common key prefix that merging skips
static constexpr idx_t MAXIMUM_MERGE_PREFIX = 512;

template <class SORT_KEY>
static idx_t MergePartitionPrefix(const unsafe_array_ptr<const SORT_KEY> &keys,
                                  const unsafe_vector<SortedRunPartitionBoundary> &boundaries) {
	const auto &reference = keys[0];
	if (reference.IsInlined()) {
		return 0;
	}
	auto prefix = MinValue<idx_t>(reference.GetHeapSize(), MAXIMUM_MERGE_PREFIX);
	idx_t offset = 0;
	for (const auto &boundary : boundaries) {
		const auto count = boundary.end - boundary.begin;
		if (count == 0) {
			continue;
		}
		for (const auto endpoint_idx : {offset, offset + count - 1}) {
			prefix = reference.GetHeapPrefixLength(keys[endpoint_idx], prefix);
			if (prefix == 0) {
				return 0;
			}
		}
		offset += count;
	}
	return prefix;
}

template <class SORT_KEY>
struct MergePrefixLess {
	explicit MergePrefixLess(idx_t prefix_p) : prefix(prefix_p) {
	}

	bool operator()(const SORT_KEY &lhs, const SORT_KEY &rhs) const {
		const auto lhs_size = lhs.GetHeapSize();
		const auto rhs_size = rhs.GetHeapSize();
		D_ASSERT(lhs_size > SORT_KEY::INLINE_LENGTH && rhs_size > SORT_KEY::INLINE_LENGTH);
		D_ASSERT(prefix <= MinValue(lhs_size, rhs_size));
		const auto comp = memcmp(lhs.GetData() + prefix, rhs.GetData() + prefix, MinValue(lhs_size, rhs_size) - prefix);
		return comp < 0 || (comp == 0 && lhs_size < rhs_size);
	}

	const idx_t prefix;
};

//! Merges the consecutive sorted fragments of a partition (ending at the given offsets) pairwise, bottom-up. Unlike
//! sorting the partition, this stays a merge when there are many short fragments (many threads or many external runs)
template <class SORT_KEY, class COMPARE>
static void MergeSortedFragments(SORT_KEY *const keys, unsafe_vector<idx_t> &fragment_ends, const COMPARE &compare,
                                 SortMergeBuffer &merge_buffer, const idx_t target_fragment_count = 1) {
	const auto buffer = merge_buffer.Keys<SORT_KEY>();
	const auto buffer_count = merge_buffer.Count();
	while (fragment_ends.size() > target_fragment_count) {
		idx_t merged_count = 0;
		idx_t begin = 0;
		for (idx_t i = 0; i < fragment_ends.size(); i += 2) {
			if (i + 1 == fragment_ends.size()) {
				fragment_ends[merged_count++] = fragment_ends[i];
				break;
			}
			const auto middle = fragment_ends[i];
			const auto end = fragment_ends[i + 1];
			BufferedMerge(keys + begin, keys + middle, keys + end, compare, buffer, buffer_count);
			fragment_ends[merged_count++] = end;
			begin = end;
		}
		fragment_ends.resize(merged_count);
	}
}

//! Partitions with fragments of more runs than this are merged in cache-sized chunks: merging them pairwise takes
//! several passes over the whole partition, which costs memory bandwidth when every thread is merging at once
static constexpr idx_t MAX_PAIRWISE_MERGE_FRAGMENTS = 4;
//! Finding the slices of a chunk takes about #fragments^2 * log(chunk size) comparisons, so beyond this many fragments
//! the partition is merged pairwise again
static constexpr idx_t MAX_CHUNKED_MERGE_FRAGMENTS = 64;
//! Size of the keys of one chunk in bytes
static constexpr idx_t MERGE_CHUNK_SIZE = 512ULL * 1024ULL;

//! A sorted fragment of a run that is merged in chunks
template <class ITERATOR>
struct MergeFragment {
	MergeFragment(const ITERATOR &keys_p, const idx_t begin, const idx_t end_p)
	    : keys(keys_p), cursor(begin), end(end_p) {
	}
	//! Indexes the keys of the run
	ITERATOR keys;
	//! First key of the fragment that has not been merged yet
	idx_t cursor;
	//! End of the fragment
	idx_t end;
};

template <class SORT_KEY, class ITERATOR>
static idx_t MergeFragmentsPrefix(const unsafe_vector<MergeFragment<ITERATOR>> &fragments) {
	const SORT_KEY reference = fragments[0].keys[fragments[0].cursor];
	if (reference.IsInlined()) {
		return 0;
	}
	auto prefix = MinValue<idx_t>(reference.GetHeapSize(), MAXIMUM_MERGE_PREFIX);
	for (const auto &fragment : fragments) {
		for (const auto idx : {fragment.cursor, fragment.end - 1}) {
			prefix = reference.GetHeapPrefixLength(fragment.keys[idx], prefix);
			if (prefix == 0) {
				return 0;
			}
		}
	}
	return prefix;
}

//! Computes where the fragments are after the next "count" merged keys, with the K-way Merge Path search that also
//! computes the partition boundaries
template <class SORT_KEY, class ITERATOR, class COMPARE>
static void ComputeChunkEnds(const unsafe_vector<MergeFragment<ITERATOR>> &fragments, idx_t count,
                             const COMPARE &compare, unsafe_vector<idx_t> &chunk_ends,
                             unsafe_vector<idx_t> &active_fragments) {
	active_fragments.clear();
	for (idx_t fragment_idx = 0; fragment_idx < fragments.size(); fragment_idx++) {
		chunk_ends[fragment_idx] = fragments[fragment_idx].cursor;
		if (chunk_ends[fragment_idx] != fragments[fragment_idx].end) {
			active_fragments.push_back(fragment_idx);
		}
	}
	while (count != 0) {
		D_ASSERT(!active_fragments.empty());
		const idx_t k = active_fragments.size();
		const auto step = AlignValue(count, k) / k;
		idx_t min_active_idx = 0;
		auto min_fragment_idx = active_fragments[0];
		auto min_delta = MinValue(step, fragments[min_fragment_idx].end - chunk_ends[min_fragment_idx]);
		SORT_KEY min_value = fragments[min_fragment_idx].keys[chunk_ends[min_fragment_idx] + min_delta - 1];
		for (idx_t active_idx = 1; active_idx < k; active_idx++) {
			const auto fragment_idx = active_fragments[active_idx];
			const auto &fragment = fragments[fragment_idx];
			const auto delta = MinValue(step, fragment.end - chunk_ends[fragment_idx]);
			const SORT_KEY &value = fragment.keys[chunk_ends[fragment_idx] + delta - 1];
			if (compare(value, min_value)) {
				min_active_idx = active_idx;
				min_fragment_idx = fragment_idx;
				min_delta = delta;
				min_value = value;
			}
		}
		chunk_ends[min_fragment_idx] += min_delta;
		if (chunk_ends[min_fragment_idx] == fragments[min_fragment_idx].end) {
			active_fragments.erase(active_fragments.begin() + static_cast<int64_t>(min_active_idx));
		}
		D_ASSERT(min_delta <= count);
		count -= min_delta;
	}
}

//! Merges the fragments into "out" one chunk at a time: the keys of a chunk are gathered into the chunk buffer, merged
//! there while they are in cache, and the last merge writes them to the output. Every key is read from its run and
//! written to the output once
template <class SORT_KEY, class ITERATOR, class COMPARE>
static void MergeFragmentsInChunks(SORT_KEY *out, unsafe_vector<MergeFragment<ITERATOR>> &fragments, idx_t total,
                                   const COMPARE &compare, SORT_KEY *const chunk, const idx_t chunk_capacity,
                                   SortMergeBuffer &merge_buffer) {
	unsafe_vector<idx_t> chunk_ends(fragments.size());
	unsafe_vector<idx_t> active_fragments;
	unsafe_vector<idx_t> fragment_ends;
	while (total != 0) {
		const auto count = MinValue(chunk_capacity, total);
		if (count == total) {
			for (idx_t fragment_idx = 0; fragment_idx < fragments.size(); fragment_idx++) {
				chunk_ends[fragment_idx] = fragments[fragment_idx].end;
			}
		} else {
			ComputeChunkEnds<SORT_KEY>(fragments, count, compare, chunk_ends, active_fragments);
		}

		idx_t chunk_count = 0;
		fragment_ends.clear();
		for (idx_t fragment_idx = 0; fragment_idx < fragments.size(); fragment_idx++) {
			auto &fragment = fragments[fragment_idx];
			const auto chunk_end = chunk_ends[fragment_idx];
			if (fragment.cursor == chunk_end) {
				continue;
			}
			const auto end_it = fragment.keys + static_cast<int64_t>(chunk_end);
			for (auto it = fragment.keys + static_cast<int64_t>(fragment.cursor); it != end_it; ++it) {
				chunk[chunk_count++] = *it;
			}
			fragment_ends.push_back(chunk_count);
			fragment.cursor = chunk_end;
		}
		D_ASSERT(chunk_count == count);

		MergeSortedFragments(chunk, fragment_ends, compare, merge_buffer, 2);
		if (fragment_ends.size() == 1) {
			std::copy(chunk, chunk + count, out);
		} else {
			const auto middle = chunk + fragment_ends[0];
			std::merge(chunk, middle, middle, chunk + count, out, compare);
		}
		out += count;
		total -= count;
	}
}

struct SortedRunMergePartition {
public:
	SortedRunMergePartition(const SortedRunMerger &merger, const idx_t partition_idx)
	    : begin_computed(partition_idx == 0), gathered(false) {
		// Initialize each partition with sensible defaults
		run_boundaries.resize(merger.sorted_runs.size());
		const auto maximum_end = merger.PartitionRowCount(partition_idx + 1);
		for (idx_t run_idx = 0; run_idx < merger.sorted_runs.size(); run_idx++) {
			const auto end = MinValue(merger.sorted_runs[run_idx]->Count(), maximum_end);
			run_boundaries[run_idx] = {0, end};
		}
	}

public:
	unsafe_vector<SortedRunPartitionBoundary> &GetRunBoundaries() DUCKDB_REQUIRES(lock) {
		return run_boundaries;
	}

	bool GetBeginComputed() const {
		return begin_computed;
	}

	void SetBeginComputed() {
		begin_computed = true;
	}

public:
	annotated_mutex lock;

private:
	unsafe_vector<SortedRunPartitionBoundary> run_boundaries DUCKDB_GUARDED_BY(lock);
	atomic<bool> begin_computed;

public:
	atomic<bool> gathered;
};

enum class SortedRunMergerTask : uint8_t {
	//! Compute boundaries of the assigned partition
	COMPUTE_BOUNDARIES,
	//! Acquire boundaries of the previous partition
	ACQUIRE_BOUNDARIES,
	//! Merge the assigned partition
	MERGE_PARTITION,
	//! Scan the merged partition
	SCAN_PARTITION,
	//! No task
	FINISHED,
};

//===--------------------------------------------------------------------===//
// Local State Header
//===--------------------------------------------------------------------===//
class SortedRunMergerGlobalState;

class SortedRunMergerLocalState : public LocalSourceState {
public:
	explicit SortedRunMergerLocalState(SortedRunMergerGlobalState &gstate_p);

public:
	//! Whether this thread has finished the work it has been assigned
	bool TaskFinished() const;
	//! Do the work this thread has been assigned
	SourceResultType ExecuteTask(SortedRunMergerGlobalState &gstate, optional_ptr<DataChunk> chunk);
	//! Clear outstanding allocations
	void Clear();

private:
	//! Computes upper partition boundaries using K-way Merge Path
	void ComputePartitionBoundaries(SortedRunMergerGlobalState &gstate, const optional_idx &p_idx);
	template <class STATE>
	void ComputePartitionBoundariesSwitch(SortedRunMergerGlobalState &gstate, const optional_idx &p_idx,
	                                      unsafe_vector<STATE> &states);
	template <class STATE, SortKeyType SORT_KEY_TYPE>
	void TemplatedComputePartitionBoundaries(SortedRunMergerGlobalState &gstate, const optional_idx &p_idx,
	                                         unsafe_vector<STATE> &states);

	//! Acquires lower partition boundaries from the global state
	void AcquirePartitionBoundaries(SortedRunMergerGlobalState &gstate);

	//! Merge the partition to obtain the next chunk
	void MergePartition(SortedRunMergerGlobalState &gstate);
	//! Allocates the buffer for merging a partition in chunks, unless it cannot be allocated
	bool TryAllocateMergeChunk(SortedRunMergerGlobalState &gstate, idx_t size);
	template <class STATE>
	void MergePartitionSwitch(SortedRunMergerGlobalState &gstate, unsafe_vector<STATE> &states);
	template <class STATE, SortKeyType SORT_KEY_TYPE>
	void TemplatedMergePartition(SortedRunMergerGlobalState &gstate, unsafe_vector<STATE> &states);

	//! Scan from the merged partition
	void ScanPartition(SortedRunMergerGlobalState &gstate, DataChunk &chunk);
	template <SortKeyType SORT_KEY_TYPE>
	void TemplatedScanPartition(SortedRunMergerGlobalState &gstate, DataChunk &chunk);

	//! Materialize the merge
	void MaterializePartition(SortedRunMergerGlobalState &gstate);
	template <SortKeyType SORT_KEY_TYPE>
	unique_ptr<SortedRun> TemplatedMaterializePartition(SortedRunMergerGlobalState &gstate);

public:
	//! Types for templating
	const BlockIteratorStateType iterator_state_type;
	const SortKeyType sort_key_type;

	SortedRunMergerTask task;
	optional_idx partition_idx;

private:
	//! Computed run boundaries
	unsafe_vector<SortedRunPartitionBoundary> run_boundaries;

	//! States for the iterator types
	unsafe_vector<BlockIteratorState<BlockIteratorStateType::IN_MEMORY>> in_memory_states;
	unsafe_vector<BlockIteratorState<BlockIteratorStateType::EXTERNAL>> external_states;

	//! Allocation for merging/scanning the partition
	AllocatedData merged_partition;
	//! Buffers for merging partitions in chunks (allocated once, reused for every partition)
	AllocatedData merge_chunk;
	unique_ptr<SortMergeBuffer> merge_chunk_buffer;

	//! Variables for scanning
	idx_t merged_partition_count;
	idx_t merged_partition_index;
	//! Rows of the current partition that were added to the global progress
	idx_t progress_scanned;

	//! For scanning
	Vector sort_key_pointers;
	SortedRunScanState sorted_run_scan_state;
};

//===--------------------------------------------------------------------===//
// Global State
//===--------------------------------------------------------------------===//
class SortedRunMergerGlobalState : public GlobalSourceState {
public:
	explicit SortedRunMergerGlobalState(ClientContext &context_p, const SortedRunMerger &merger_p)
	    : context(context_p), merger(merger_p), num_runs(merger.sorted_runs.size()),
	      num_partitions(merger.PartitionCount()), iterator_state_type(GetBlockIteratorStateType(merger.external)),
	      sort_key_type(merger.sort.key_layout->GetSortKeyType()), next_partition_idx(0), total_scanned(0),
	      progress_scanned(0), destroy_key_partition_idx(0), gathered_partition_idx(0) {
		if (merger.external) {
			boundary_readers.reserve(TaskScheduler::GetScheduler(context).NumberOfThreads());
		}
		// Initialize partitions
		partitions.resize(num_partitions);
		for (idx_t partition_idx = 0; partition_idx < num_partitions; partition_idx++) {
			partitions[partition_idx] = make_uniq<SortedRunMergePartition>(merger, partition_idx);
		}
	}

public:
	class BoundaryGuard {
	public:
		BoundaryGuard(SortedRunMergerGlobalState &gstate_p, const idx_t partition_idx_p)
		    : gstate(gstate_p), partition_idx(partition_idx_p) {
			if (!gstate.merger.external) {
				return;
			}
			lock_guard<mutex> guard(gstate.destroy_lock);
			gstate.boundary_readers.push_back({partition_idx, gstate.gathered_partition_idx});
		}

		BoundaryGuard(const BoundaryGuard &) = delete;
		BoundaryGuard &operator=(const BoundaryGuard &) = delete;

		~BoundaryGuard() {
			if (!gstate.merger.external) {
				return;
			}
			lock_guard<mutex> guard(gstate.destroy_lock);
			auto &readers = gstate.boundary_readers;
			for (idx_t i = 0; i < readers.size(); i++) {
				if (readers[i].partition_idx == partition_idx) {
					readers[i] = readers.back();
					readers.pop_back();
					return;
				}
			}
			D_ASSERT(false);
		}

	private:
		SortedRunMergerGlobalState &gstate;
		const idx_t partition_idx;
	};

	void SetBoundaryLowerBound(const idx_t partition_idx, const idx_t lower_partition_idx) {
		if (!merger.external) {
			return;
		}
		lock_guard<mutex> guard(destroy_lock);
		for (auto &reader : boundary_readers) {
			if (reader.partition_idx == partition_idx) {
				D_ASSERT(reader.lower_partition_idx <= lower_partition_idx);
				reader.lower_partition_idx = lower_partition_idx;
				return;
			}
		}
		D_ASSERT(false);
	}

public:
	bool AssignTask(SortedRunMergerLocalState &lstate) {
		D_ASSERT(!lstate.partition_idx.IsValid());
		D_ASSERT(lstate.task == SortedRunMergerTask::FINISHED);
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (next_partition_idx == num_partitions) {
			return false; // Nothing left to do
		}
		lstate.partition_idx = next_partition_idx++;
		lstate.task = SortedRunMergerTask::COMPUTE_BOUNDARIES;
		return true;
	}

	idx_t MaxThreads() override {
		return MaxValue<idx_t>(RowPartitionCount(merger), 1);
	}

	void DestroyPinnedData(const idx_t partition_idx) {
		D_ASSERT(merger.external);
		partitions[partition_idx]->gathered = true;

		lock_guard<mutex> guard(destroy_lock);
		auto end_partition_idx = gathered_partition_idx;
		while (end_partition_idx < num_partitions && partitions[end_partition_idx]->gathered) {
			end_partition_idx++;
		}
		if (end_partition_idx != gathered_partition_idx) {
			// Boundary searches only need keys. Gathered payload stays pinned until its output is consumed.
			if (merger.sorted_runs[0]->payload_data) {
				for (idx_t run_idx = 0; run_idx < num_runs; run_idx++) {
					idx_t begin_idx = 0;
					if (gathered_partition_idx != 0) {
						auto &begin_partition = *partitions[gathered_partition_idx - 1];
						const annotated_lock_guard<annotated_mutex> partition_guard(begin_partition.lock);
						begin_idx = begin_partition.GetRunBoundaries()[run_idx].end;
					}
					auto &end_partition = *partitions[end_partition_idx - 1];
					const annotated_lock_guard<annotated_mutex> partition_guard(end_partition.lock);
					const auto end_idx = end_partition.GetRunBoundaries()[run_idx].end;
					merger.sorted_runs[run_idx]->DestroyPayloadData(begin_idx, end_idx);
				}
			}
			gathered_partition_idx = end_partition_idx;
		}
		DestroyPinnedKeyData();
	}

private:
	void DestroyPinnedKeyData() {
		auto end_partition_idx = gathered_partition_idx;
		for (const auto &reader : boundary_readers) {
			end_partition_idx = MinValue(end_partition_idx, reader.lower_partition_idx);
		}
		D_ASSERT(destroy_key_partition_idx <= end_partition_idx);
		if (destroy_key_partition_idx == end_partition_idx) {
			return;
		}
		// Retire only the prefix no active boundary search can revisit; gathered references stay pinned.
		for (idx_t run_idx = 0; run_idx < num_runs; run_idx++) {
			idx_t begin_idx = 0;
			if (destroy_key_partition_idx != 0) {
				auto &begin_partition = *partitions[destroy_key_partition_idx - 1];
				const annotated_lock_guard<annotated_mutex> partition_guard(begin_partition.lock);
				begin_idx = begin_partition.GetRunBoundaries()[run_idx].end;
			}
			auto &end_partition = *partitions[end_partition_idx - 1];
			const annotated_lock_guard<annotated_mutex> partition_guard(end_partition.lock);
			const auto end_idx = end_partition.GetRunBoundaries()[run_idx].end;
			merger.sorted_runs[run_idx]->DestroyKeyData(begin_idx, end_idx);
		}
		destroy_key_partition_idx = end_partition_idx;
	}

private:
	static BlockIteratorStateType GetBlockIteratorStateType(const bool &external) {
		return external ? BlockIteratorStateType::EXTERNAL : BlockIteratorStateType::IN_MEMORY;
	}

public:
	ClientContext &context;

	const SortedRunMerger &merger;
	const idx_t num_runs;
	const idx_t num_partitions;

	const BlockIteratorStateType iterator_state_type;
	const SortKeyType sort_key_type;

	idx_t next_partition_idx;
	vector<unique_ptr<SortedRunMergePartition>> partitions;
	atomic<idx_t> total_scanned;
	//! Rows scanned so far, updated per chunk (for progress)
	atomic<idx_t> progress_scanned;

	mutex destroy_lock;
	idx_t destroy_key_partition_idx;
	idx_t gathered_partition_idx;
	unsafe_vector<SortedRunBoundaryReader> boundary_readers;

	mutex materialized_partition_lock;
	vector<unique_ptr<SortedRun>> materialized_partitions;
};

//===--------------------------------------------------------------------===//
// Local State Implementation
//===--------------------------------------------------------------------===//
SortedRunMergerLocalState::SortedRunMergerLocalState(SortedRunMergerGlobalState &gstate)
    : iterator_state_type(gstate.iterator_state_type), sort_key_type(gstate.sort_key_type),
      task(SortedRunMergerTask::FINISHED), run_boundaries(gstate.num_runs),
      merged_partition_count(DConstants::INVALID_INDEX), merged_partition_index(DConstants::INVALID_INDEX),
      progress_scanned(0), sort_key_pointers(LogicalType::POINTER),
      sorted_run_scan_state(gstate.context, gstate.merger.sort) {
	for (const auto &run : gstate.merger.sorted_runs) {
		auto &key_data = *run->key_data;
		switch (iterator_state_type) {
		case BlockIteratorStateType::IN_MEMORY:
			in_memory_states.emplace_back(key_data);
			break;
		case BlockIteratorStateType::EXTERNAL:
			external_states.emplace_back(key_data, run->payload_data.get());
			break;
		default:
			throw NotImplementedException("SortedRunMergerLocalState::SortedRunMergerLocalState for %s",
			                              EnumUtil::ToString(iterator_state_type));
		}
	}
}

void SortedRunMergerLocalState::Clear() {
	in_memory_states.clear();
	external_states.clear();
	merged_partition.Reset();
	merge_chunk.Reset();
	merge_chunk_buffer.reset();
	sorted_run_scan_state.Clear();
}

bool SortedRunMergerLocalState::TaskFinished() const {
	switch (task) {
	case SortedRunMergerTask::COMPUTE_BOUNDARIES:
	case SortedRunMergerTask::ACQUIRE_BOUNDARIES:
	case SortedRunMergerTask::MERGE_PARTITION:
	case SortedRunMergerTask::SCAN_PARTITION:
		D_ASSERT(partition_idx.IsValid());
		return false;
	case SortedRunMergerTask::FINISHED:
		D_ASSERT(!partition_idx.IsValid());
		return true;
	default:
		throw NotImplementedException("SortedRunMergerLocalState::TaskFinished for task");
	}
}

SourceResultType SortedRunMergerLocalState::ExecuteTask(SortedRunMergerGlobalState &gstate,
                                                        optional_ptr<DataChunk> chunk) {
	D_ASSERT(task != SortedRunMergerTask::FINISHED);
	switch (task) {
	case SortedRunMergerTask::COMPUTE_BOUNDARIES:
	case SortedRunMergerTask::ACQUIRE_BOUNDARIES: {
		const SortedRunMergerGlobalState::BoundaryGuard boundary_guard(gstate, partition_idx.GetIndex());
		if (task == SortedRunMergerTask::COMPUTE_BOUNDARIES) {
			ComputePartitionBoundaries(gstate, partition_idx);
			task = SortedRunMergerTask::ACQUIRE_BOUNDARIES;
		} else {
			AcquirePartitionBoundaries(gstate);
			task = SortedRunMergerTask::MERGE_PARTITION;
		}
		break;
	}
	case SortedRunMergerTask::MERGE_PARTITION:
		MergePartition(gstate);
		task = SortedRunMergerTask::SCAN_PARTITION;
		break;
	case SortedRunMergerTask::SCAN_PARTITION:
		if (chunk) {
			ScanPartition(gstate, *chunk);
			gstate.progress_scanned.fetch_add(chunk->size(), std::memory_order_relaxed);
			progress_scanned += chunk->size();
		} else {
			MaterializePartition(gstate);
		}
		if (!chunk || chunk->size() == 0) {
			gstate.progress_scanned.fetch_add(merged_partition_count - progress_scanned, std::memory_order_relaxed);
			progress_scanned = 0;
			//	fetch_add returns the _previous_ value!
			const auto scan_count_before_adding = gstate.total_scanned.fetch_add(merged_partition_count);
			const auto scan_count_after_adding = scan_count_before_adding + merged_partition_count;
			partition_idx = optional_idx::Invalid();
			task = SortedRunMergerTask::FINISHED;
			if (scan_count_after_adding == gstate.merger.total_count) {
				return SourceResultType::FINISHED;
			}
		}
		break;
	default:
		throw NotImplementedException("SortedRunMergerLocalState::ExecuteTask for task");
	}
	return SourceResultType::HAVE_MORE_OUTPUT;
}

void SortedRunMergerLocalState::ComputePartitionBoundaries(SortedRunMergerGlobalState &gstate,
                                                           const optional_idx &p_idx) {
	D_ASSERT(p_idx.IsValid());
	D_ASSERT(task == SortedRunMergerTask::COMPUTE_BOUNDARIES);

	// Copy over the run boundaries from the assigned partition (under lock)
	auto &current_partition = *gstate.partitions[p_idx.GetIndex()];
	bool begin_computed;
	{
		const annotated_lock_guard<annotated_mutex> current_partition_guard(current_partition.lock);
		begin_computed = current_partition.GetBeginComputed();
		run_boundaries = current_partition.GetRunBoundaries();
	}

	auto lower_partition_idx = begin_computed ? p_idx.GetIndex() : 0;
	if (!begin_computed) {
		// We can use information from previous partitions to speed up computing this partition
		for (idx_t prev = p_idx.GetIndex(); prev > 1; prev--) {
			auto &prev_partition = *gstate.partitions[prev - 1];
			if (!prev_partition.GetBeginComputed()) {
				continue;
			}
			const annotated_lock_guard<annotated_mutex> prev_partition_guard(prev_partition.lock);
			const auto &prev_partition_run_boundaries = prev_partition.GetRunBoundaries();
			for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
				run_boundaries[run_idx].begin = prev_partition_run_boundaries[run_idx].begin;
			}
			lower_partition_idx = prev - 1;
			break;
		}
	}

	// Publish the copied lower bound before accessing keys, including fallback searches.
	gstate.SetBoundaryLowerBound(partition_idx.GetIndex(), lower_partition_idx);

	// Compute the end partition boundaries (lock-free)
	switch (iterator_state_type) {
	case BlockIteratorStateType::IN_MEMORY:
		ComputePartitionBoundariesSwitch<BlockIteratorState<BlockIteratorStateType::IN_MEMORY>>(gstate, p_idx,
		                                                                                        in_memory_states);
		break;
	case BlockIteratorStateType::EXTERNAL:
		ComputePartitionBoundariesSwitch<BlockIteratorState<BlockIteratorStateType::EXTERNAL>>(gstate, p_idx,
		                                                                                       external_states);
		break;
	default:
		throw NotImplementedException(
		    "SortedRunMergerLocalState::ComputePartitionBoundaries for SortedRunIteratorType");
	}

	// The computed boundaries of the current partition may be the start boundaries of the next partition
	// Another thread depends on this, set them first
	if (p_idx.GetIndex() != gstate.num_partitions - 1) {
		auto &next_partition = *gstate.partitions[p_idx.GetIndex() + 1];
		if (!next_partition.GetBeginComputed()) {
			annotated_lock_guard<annotated_mutex> next_partition_guard(next_partition.lock);
			if (!next_partition.GetBeginComputed()) {
				auto &next_partition_run_boundaries = next_partition.GetRunBoundaries();
				for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
					const auto &computed_boundary = run_boundaries[run_idx];
					D_ASSERT(computed_boundary.begin == computed_boundary.end);
					next_partition_run_boundaries[run_idx].begin = computed_boundary.begin;
				}
				next_partition.SetBeginComputed();
			}
		}
	}

	// Set the computed end partition boundaries of the current partition
	{
		annotated_lock_guard<annotated_mutex> current_partition_guard(current_partition.lock);
		auto &current_partition_run_boundaries = current_partition.GetRunBoundaries();
		for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
			const auto &computed_boundary = run_boundaries[run_idx];
			D_ASSERT(computed_boundary.begin == computed_boundary.end);
			current_partition_run_boundaries[run_idx].end = computed_boundary.end;
		}
	}
}

template <class STATE>
void SortedRunMergerLocalState::ComputePartitionBoundariesSwitch(SortedRunMergerGlobalState &gstate,
                                                                 const optional_idx &p_idx,
                                                                 unsafe_vector<STATE> &states) {
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedComputePartitionBoundaries<STATE, SortKeyType::SORT_KEY_TYPE>(gstate, p_idx, states);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SortedRunMergerLocalState::ComputePartitionBoundariesSwitch for %s",
		                              EnumUtil::ToString(sort_key_type));
	}
}

static idx_t ComputeBoundaryDelta(const idx_t &total_remaining, const idx_t &k,
                                  const SortedRunPartitionBoundary &run_boundary) {
	D_ASSERT(run_boundary.begin < run_boundary.end);
	const auto run_remaining = run_boundary.end - run_boundary.begin;
	return MinValue(AlignValue(total_remaining, k) / k, run_remaining);
}

template <class STATE, SortKeyType SORT_KEY_TYPE>
void SortedRunMergerLocalState::TemplatedComputePartitionBoundaries(SortedRunMergerGlobalState &gstate,
                                                                    const optional_idx &p_idx,
                                                                    unsafe_vector<STATE> &states) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<STATE, SORT_KEY>;

	D_ASSERT(run_boundaries.size() == gstate.num_runs);

	// Check if last partition: boundary is always end of each sorted run
	if (p_idx == gstate.num_partitions - 1) {
		for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
			run_boundaries[run_idx].begin = gstate.merger.sorted_runs[run_idx]->Count();
		}
		return;
	}

	// Initialize "total_remaining", i.e., how much we still need to update the boundaries until we're done
	idx_t total_remaining = gstate.merger.PartitionRowCount(p_idx.GetIndex() + 1);

	// Initialize iterators, and track of which runs are actively being used in the computation, i.e., not yet fixed
	unsafe_vector<BLOCK_ITERATOR> run_iterators;
	run_iterators.reserve(gstate.num_runs);
	unsafe_vector<idx_t> active_run_idxs;
	active_run_idxs.reserve(gstate.num_runs);
	for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
		auto &state = states[run_idx];
		state.SetKeepPinned(false);
		state.SetPinPayload(false);

		run_iterators.emplace_back(state);
		const auto &run_boundary = run_boundaries[run_idx];
		if (run_boundary.begin != run_boundary.end) {
			active_run_idxs.emplace_back(run_idx);
		}

		// Reduce total remaining by what we already have (from previous partition)
		D_ASSERT(run_boundaries[run_idx].begin <= total_remaining);
		total_remaining -= run_boundaries[run_idx].begin;
	}

	D_ASSERT(total_remaining <
	         gstate.merger.total_count); // This is not the last partition, so should be less than total
	while (total_remaining != 0) {
		D_ASSERT(!active_run_idxs.empty());
		const idx_t k = active_run_idxs.size();

		// Compute step size for each run: ceil(total_remaining / k), or less if run has less remaining
		auto min_idx = active_run_idxs[0];
		auto min_delta = ComputeBoundaryDelta(total_remaining, k, run_boundaries[min_idx]);
		reference<SORT_KEY> min_value = run_iterators[min_idx][run_boundaries[min_idx].begin + min_delta - 1];
		for (idx_t i = 1; i < active_run_idxs.size(); i++) {
			const auto &active_run_idx = active_run_idxs[i];
			const auto &run_boundary = run_boundaries[active_run_idx];
			const auto run_delta = ComputeBoundaryDelta(total_remaining, k, run_boundary);
			auto &run_value = run_iterators[active_run_idx][run_boundary.begin + run_delta - 1];
			if (run_value < min_value.get()) {
				min_idx = active_run_idx;
				min_delta = run_delta;
				min_value = run_value;
			}
		}

		// Increment boundary begin value of the min run by the delta
		auto &min_run_boundary = run_boundaries[min_idx];
		min_run_boundary.begin += min_delta;

		// Erase from active if begin is equal to end
		if (min_run_boundary.begin == min_run_boundary.end) {
			auto it = std::find(active_run_idxs.begin(), active_run_idxs.end(), min_idx);
			D_ASSERT(it != active_run_idxs.end());
			active_run_idxs.erase(it);
		}

		// Update total remaining accordingly
		D_ASSERT(min_delta <= total_remaining);
		total_remaining -= min_delta;
	}

	// End of boundary is meaningless now, just set equal to begin so there's no confusion
	for (auto &run_boundary : run_boundaries) {
		run_boundary.end = run_boundary.begin;
	}
}

void SortedRunMergerLocalState::AcquirePartitionBoundaries(SortedRunMergerGlobalState &gstate) {
	D_ASSERT(partition_idx.IsValid());
	D_ASSERT(task == SortedRunMergerTask::ACQUIRE_BOUNDARIES);
	auto &current_partition = *gstate.partitions[partition_idx.GetIndex()];
	if (current_partition.GetBeginComputed()) {
		// Begin has been computed, boundaries are ready to use. Copy to local
		annotated_lock_guard<annotated_mutex> guard(current_partition.lock);
		run_boundaries = current_partition.GetRunBoundaries();
		return;
	}

	// Begin has not yet been computed by another thread, let this thread do it
	task = SortedRunMergerTask::COMPUTE_BOUNDARIES;
	ComputePartitionBoundaries(gstate, partition_idx.GetIndex() - 1);
	task = SortedRunMergerTask::ACQUIRE_BOUNDARIES;

	// Copy to local
	annotated_lock_guard<annotated_mutex> guard(current_partition.lock);
	D_ASSERT(current_partition.GetBeginComputed());
	run_boundaries = current_partition.GetRunBoundaries();
}

bool SortedRunMergerLocalState::TryAllocateMergeChunk(SortedRunMergerGlobalState &gstate, const idx_t size) {
	if (merge_chunk.IsSet()) {
		return true;
	}
	try {
		merge_chunk = BufferAllocator::Get(gstate.context).Allocate(size);
	} catch (OutOfMemoryException &) {
		return false; // merge the gathered partition instead
	}
	return true;
}

void SortedRunMergerLocalState::MergePartition(SortedRunMergerGlobalState &gstate) {
	switch (iterator_state_type) {
	case BlockIteratorStateType::IN_MEMORY:
		MergePartitionSwitch<BlockIteratorState<BlockIteratorStateType::IN_MEMORY>>(gstate, in_memory_states);
		break;
	case BlockIteratorStateType::EXTERNAL:
		MergePartitionSwitch<BlockIteratorState<BlockIteratorStateType::EXTERNAL>>(gstate, external_states);
		break;
	default:
		throw NotImplementedException("SortedRunMergerLocalState::MergePartition for %s",
		                              EnumUtil::ToString(iterator_state_type));
	}
}

template <class STATE>
void SortedRunMergerLocalState::MergePartitionSwitch(SortedRunMergerGlobalState &gstate, unsafe_vector<STATE> &states) {
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedMergePartition<STATE, SortKeyType::SORT_KEY_TYPE>(gstate, states);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SortedRunMergerLocalState::MergePartitionSwitch for %s",
		                              EnumUtil::ToString(sort_key_type));
	}
}

template <class STATE, SortKeyType SORT_KEY_TYPE>
void SortedRunMergerLocalState::TemplatedMergePartition(SortedRunMergerGlobalState &gstate,
                                                        unsafe_vector<STATE> &states) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<STATE, SORT_KEY>;

	if (!merged_partition.IsSet()) {
		merged_partition =
		    BufferAllocator::Get(gstate.context).Allocate(gstate.merger.partition_size * sizeof(SORT_KEY));
	}
	auto merged_partition_keys = reinterpret_cast<SORT_KEY *>(merged_partition.get());
	merged_partition_count = 0;
	merged_partition_index = 0;

	// Collect the fragments of the runs that are actively being merged
	unsafe_vector<MergeFragment<BLOCK_ITERATOR>> fragments;
	idx_t total_count = 0;
	for (idx_t run_idx = 0; run_idx < gstate.num_runs; run_idx++) {
		auto &state = states[run_idx];
		state.SetKeepPinned(true);

		const auto &run_boundary = run_boundaries[run_idx];
		state.SetPinPayload(true, run_boundary.begin, run_boundary.end);
		if (run_boundary.begin == run_boundary.end) {
			continue;
		}
		fragments.emplace_back(BLOCK_ITERATOR(state), run_boundary.begin, run_boundary.end);
		total_count += run_boundary.end - run_boundary.begin;
	}

	// External runs are read through stateful iterators that fetch a whole chunk of keys (and payload) whenever they
	// move to another one, so only in-memory runs are merged in chunks
	if constexpr (std::is_same<STATE, InMemoryBlockIteratorState>::value) {
		const auto chunk_capacity = MERGE_CHUNK_SIZE / sizeof(SORT_KEY);
		if (fragments.size() > MAX_PAIRWISE_MERGE_FRAGMENTS && fragments.size() <= MAX_CHUNKED_MERGE_FRAGMENTS &&
		    total_count > chunk_capacity && !gstate.merger.is_index_sort &&
		    TryAllocateMergeChunk(gstate, chunk_capacity * sizeof(SORT_KEY))) {
			if (!merge_chunk_buffer) {
				merge_chunk_buffer =
				    make_uniq<SortMergeBuffer>(BufferAllocator::Get(gstate.context), chunk_capacity, sizeof(SORT_KEY));
			}
			const auto chunk = reinterpret_cast<SORT_KEY *>(merge_chunk.get());
			bool merged = false;
			if constexpr (!SORT_KEY::CONSTANT_SIZE) {
				const auto prefix = gstate.merger.sort.is_index_sort ? 0 : MergeFragmentsPrefix<SORT_KEY>(fragments);
				if (prefix != 0) {
					// Sorted endpoints establish this prefix for every key in each fragment.
					const MergePrefixLess<SORT_KEY> compare(prefix);
					MergeFragmentsInChunks(merged_partition_keys, fragments, total_count, compare, chunk,
					                       chunk_capacity, *merge_chunk_buffer);
					merged = true;
				}
			}
			if (!merged) {
				MergeFragmentsInChunks(merged_partition_keys, fragments, total_count, std::less<SORT_KEY>(), chunk,
				                       chunk_capacity, *merge_chunk_buffer);
			}
			merged_partition_count = total_count;
			return;
		}
	}

	// Gather the fragments, keeping track of where each one ends
	unsafe_vector<idx_t> fragment_ends;
	for (auto &fragment : fragments) {
		const auto end_it = fragment.keys + static_cast<int64_t>(fragment.end);
		for (auto it = fragment.keys + static_cast<int64_t>(fragment.cursor); it != end_it; ++it) {
			merged_partition_keys[merged_partition_count++] = *it;
		}
		fragment_ends.push_back(merged_partition_count);
	}

	if constexpr (std::is_same<STATE, ExternalBlockIteratorState>::value) {
		gstate.DestroyPinnedData(partition_idx.GetIndex());
	}

	if (fragment_ends.size() <= 1 || gstate.merger.is_index_sort) {
		return; // At most one active run, no need to merge (or index sort, which is approximate sorting)
	}

	// Each fragment is sorted, so merging them only needs a scratch buffer of up to half the keys - take it from the
	// buffer allocator so that it is counted against the memory limit
	SortMergeBuffer merge_buffer(BufferAllocator::Get(gstate.context), merged_partition_count, sizeof(SORT_KEY));
	if constexpr (!SORT_KEY::CONSTANT_SIZE) {
		if (!gstate.merger.sort.is_index_sort) {
			const auto prefix = MergePartitionPrefix(
			    unsafe_array_ptr<const SORT_KEY>(merged_partition_keys, merged_partition_count), run_boundaries);
			if (prefix != 0) {
				// Sorted endpoints establish this prefix for every key in each gathered fragment.
				const MergePrefixLess<SORT_KEY> compare(prefix);
				MergeSortedFragments(merged_partition_keys, fragment_ends, compare, merge_buffer);
				return;
			}
		}
	}
	MergeSortedFragments(merged_partition_keys, fragment_ends, std::less<SORT_KEY>(), merge_buffer);
}

void SortedRunMergerLocalState::ScanPartition(SortedRunMergerGlobalState &gstate, DataChunk &chunk) {
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedScanPartition<SortKeyType::SORT_KEY_TYPE>(gstate, chunk);
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SortedRunMergerLocalState::ScanPartition for %s",
		                              EnumUtil::ToString(sort_key_type));
	}
}

template <SortKeyType SORT_KEY_TYPE>
void SortedRunMergerLocalState::TemplatedScanPartition(SortedRunMergerGlobalState &gstate, DataChunk &chunk) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	const auto count = MinValue<idx_t>(merged_partition_count - merged_partition_index, STANDARD_VECTOR_SIZE);

	// Grab pointers to sort keys
	const auto merged_partition_keys = reinterpret_cast<SORT_KEY *>(merged_partition.get()) + merged_partition_index;
	{
		auto writer = FlatVector::Writer<SORT_KEY *>(sort_key_pointers, count);
		for (idx_t i = 0; i < count; i++) {
			writer.WriteValue(&merged_partition_keys[i]);
		}
	}
	merged_partition_index += count;

	// Scan
	sorted_run_scan_state.Scan(*gstate.merger.sorted_runs[0], sort_key_pointers, chunk);
}

void SortedRunMergerLocalState::MaterializePartition(SortedRunMergerGlobalState &gstate) {
	unique_ptr<SortedRun> sorted_run;
	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		sorted_run = TemplatedMaterializePartition<SortKeyType::SORT_KEY_TYPE>(gstate);                                \
		break;
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("SortedRunMergerLocalState::MaterializePartition for %s",
		                              EnumUtil::ToString(sort_key_type));
	}

	// Add to global state
	lock_guard<mutex> guard(gstate.materialized_partition_lock);
	if (gstate.materialized_partitions.size() < partition_idx.GetIndex() + 1) {
		gstate.materialized_partitions.resize(partition_idx.GetIndex() + 1);
	}
	gstate.materialized_partitions[partition_idx.GetIndex()] = std::move(sorted_run);
}

template <SortKeyType SORT_KEY_TYPE>
unique_ptr<SortedRun> SortedRunMergerLocalState::TemplatedMaterializePartition(SortedRunMergerGlobalState &gstate) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	const auto merged_partition_keys = reinterpret_cast<SORT_KEY *>(merged_partition.get()) + merged_partition_index;

	TupleDataChunkState key_data_input;
	const auto key_locations = FlatVector::GetDataMutable<data_ptr_t>(key_data_input.row_locations);
	const auto key_heap_locations = FlatVector::GetDataMutable<data_ptr_t>(key_data_input.heap_locations);
	const auto key_heap_sizes = FlatVector::GetDataMutable<idx_t>(key_data_input.heap_sizes);

	TupleDataChunkState payload_data_input;
	const auto payload_locations = FlatVector::GetDataMutable<data_ptr_t>(payload_data_input.row_locations);

	auto sorted_run = gstate.merger.sorted_runs[0]->CreateRunForMaterialization();

	while (merged_partition_index < merged_partition_count) {
		const auto count = MinValue<idx_t>(merged_partition_count - merged_partition_index, STANDARD_VECTOR_SIZE);
		for (idx_t i = 0; i < count; i++) {
			auto &key = merged_partition_keys[merged_partition_index + i];
			key_locations[i] = data_ptr_cast(&key);
			if (!SORT_KEY::CONSTANT_SIZE) {
				key_heap_locations[i] = key.GetData();
				key_heap_sizes[i] = key.GetHeapSize();
			}
			if (SORT_KEY::HAS_PAYLOAD) {
				payload_locations[i] = key.GetPayload();
			}
		}

		if (SORT_KEY::HAS_PAYLOAD) {
			if (!sorted_run->payload_data->GetLayout().AllConstant()) {
				sorted_run->payload_data->FindHeapPointers(payload_data_input, count);
			}
			sorted_run->payload_append_state.chunk_state.heap_sizes.Reference(payload_data_input.heap_sizes);
			sorted_run->payload_data->Build(sorted_run->payload_append_state.pin_state,
			                                sorted_run->payload_append_state.chunk_state, 0, count);
			sorted_run->payload_data->CopyRows(sorted_run->payload_append_state.chunk_state, payload_data_input,
			                                   *FlatVector::IncrementalSelectionVector(), count);

			const auto new_payload_locations =
			    FlatVector::GetData<const data_ptr_t>(sorted_run->payload_append_state.chunk_state.row_locations);
			for (idx_t i = 0; i < count; i++) {
				auto &key = merged_partition_keys[merged_partition_index + i];
				key.SetPayload(new_payload_locations[i]);
			}
		}

		sorted_run->key_append_state.chunk_state.heap_sizes.Reference(key_data_input.heap_sizes);
		sorted_run->key_data->Build(sorted_run->key_append_state.pin_state, sorted_run->key_append_state.chunk_state, 0,
		                            count);
		sorted_run->key_data->CopyRows(sorted_run->key_append_state.chunk_state, key_data_input,
		                               *FlatVector::IncrementalSelectionVector(), count);

		merged_partition_index += count;
	}

	sorted_run->key_data->FinalizePinState(sorted_run->key_append_state.pin_state);
	if (sorted_run->payload_data) {
		sorted_run->payload_data->FinalizePinState(sorted_run->payload_append_state.pin_state);
	}

	return sorted_run;
}

//===--------------------------------------------------------------------===//
// Skewed Partition Splitting
//===--------------------------------------------------------------------===//
static constexpr idx_t MAX_BYTE_SAMPLES = 512;

template <class SORT_KEY>
struct MergeByteSample {
	SORT_KEY key;
	idx_t rows;
	double bytes_per_row;
	idx_t ordinal;

	bool operator<(const MergeByteSample &other) const {
		if (key < other.key) {
			return true;
		}
		if (other.key < key) {
			return false;
		}
		return ordinal < other.ordinal;
	}
};

template <class SORT_KEY>
static idx_t CollectMergeByteSamples(const SortedRunMerger &merger, const TupleDataLayout &payload_layout,
                                     unsafe_array_ptr<MergeByteSample<SORT_KEY>> samples) {
	static constexpr idx_t NEIGHBORS = 32;
	auto &context = merger.sort.client_context;
	const auto row_width = payload_layout.GetRowWidth();
	const auto heap_size_offset = payload_layout.GetHeapSizeOffset();

	idx_t sample_count = 0;
	const auto run_count = merger.sorted_runs.size();

	// Each sample represents an interval of rows; stagger its position between runs.
	for (idx_t run_idx = 0; run_idx < run_count; run_idx++) {
		auto &run = *merger.sorted_runs[run_idx];
		const auto count = run.Count();
		const auto intervals = MinValue<idx_t>(count, MAX_BYTE_SAMPLES / run_count);
		const auto interval_size = count / intervals;
		const auto remainder = count % intervals;

		ExternalBlockIteratorState key_state(*run.key_data, nullptr);
		idx_t begin = 0;
		for (idx_t interval_idx = 0; interval_idx < intervals; interval_idx++) {
			context.InterruptCheck();
			const auto width = interval_size + (interval_idx < remainder);
			const auto end = begin + width;
			const auto phase = (width / run_count) * run_idx + ((width % run_count) * run_idx) / run_count;
			const auto row_idx = begin + phase;
			auto key = key_state.GetValueAtIndex<SORT_KEY>(row_idx);

			double payload_bytes;
			if (merger.external) {
				// Estimate spilled payload bytes from chunk metadata without pinning payload blocks.
				const auto chunk_idx = row_idx / STANDARD_VECTOR_SIZE;
				const auto chunk_count =
				    MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - chunk_idx * STANDARD_VECTOR_SIZE);
				payload_bytes = static_cast<double>(run.payload_data->GetChunkDataSize(chunk_idx)) /
				                static_cast<double>(chunk_count);
			} else {
				const auto neighbors = MinValue<idx_t>(NEIGHBORS, width);
				auto neighbor_begin = row_idx - MinValue<idx_t>(row_idx - begin, neighbors / 2);
				neighbor_begin = MinValue(neighbor_begin, end - neighbors);

				payload_bytes = 0;
				for (idx_t i = 0; i < neighbors; i++) {
					const auto &neighbor = key_state.GetValueAtIndex<SORT_KEY>(neighbor_begin + i);
					payload_bytes += static_cast<double>(Load<idx_t>(neighbor.GetPayload() + heap_size_offset));
				}
				payload_bytes = static_cast<double>(row_width) + payload_bytes / static_cast<double>(neighbors);
			}

			key.SetPayload(nullptr);
			new (&samples[sample_count])
			    MergeByteSample<SORT_KEY> {key, width, sizeof(SORT_KEY) + payload_bytes, sample_count};
			sample_count++;
			begin = end;
		}
		D_ASSERT(begin == count);
	}

	return sample_count;
}

struct MergePartitionByteEstimate {
	idx_t largest_partition;
	double largest_bytes;
	double total_bytes;
};

template <class SORT_KEY>
static MergePartitionByteEstimate
EstimateLargestMergePartition(unsafe_array_ptr<const MergeByteSample<SORT_KEY>> samples, idx_t partition_size,
                              idx_t total_count) {
	// Use the weighted samples in key order to estimate bytes in each existing row partition.
	idx_t rank = 0;
	double partition_bytes = 0;
	MergePartitionByteEstimate estimate {0, 0, 0};

	for (idx_t i = 0; i < samples.size(); i++) {
		const auto &sample = samples[i];
		auto remaining = sample.rows;
		estimate.total_bytes += sample.bytes_per_row * sample.rows;
		while (remaining != 0) {
			const auto take = MinValue(remaining, partition_size - rank % partition_size);
			partition_bytes += sample.bytes_per_row * take;
			rank += take;
			remaining -= take;
			if (rank % partition_size == 0 || rank == total_count) {
				if (partition_bytes > estimate.largest_bytes) {
					estimate.largest_bytes = partition_bytes;
					estimate.largest_partition = (rank - 1) / partition_size;
				}
				partition_bytes = 0;
			}
		}
	}
	D_ASSERT(rank == total_count);

	return estimate;
}

template <class SORT_KEY>
static optional_idx FindMergePartitionByteMidpoint(unsafe_array_ptr<const MergeByteSample<SORT_KEY>> samples,
                                                   idx_t partition_begin, idx_t partition_end, double partition_bytes) {
	if (partition_end - partition_begin <= STANDARD_VECTOR_SIZE) {
		return optional_idx();
	}
	const auto first_interior = partition_begin + STANDARD_VECTOR_SIZE;
	const auto last_interior = ((partition_end - 1) / STANDARD_VECTOR_SIZE) * STANDARD_VECTOR_SIZE;

	// Add one interior vector-aligned boundary near the heavy partition's byte midpoint.
	auto median_bytes = partition_bytes / 2;
	idx_t rank = 0;
	for (idx_t i = 0; i < samples.size(); i++) {
		const auto &sample = samples[i];
		const auto begin = MaxValue(rank, partition_begin);
		const auto end = MinValue(rank + sample.rows, partition_end);
		rank += sample.rows;
		if (begin >= end) {
			continue;
		}

		const auto bytes = sample.bytes_per_row * (end - begin);
		if (median_bytes > bytes) {
			median_bytes -= bytes;
			continue;
		}

		const auto offset = median_bytes / sample.bytes_per_row;
		const auto rows = offset >= static_cast<double>(end - begin) ? end - begin : static_cast<idx_t>(offset);
		const auto aligned = ((begin + rows) / STANDARD_VECTOR_SIZE) * STANDARD_VECTOR_SIZE;
		return MinValue(MaxValue(aligned, first_interior), last_interior);
	}
	return optional_idx();
}

template <SortKeyType SORT_KEY_TYPE>
optional_idx SortedRunMerger::TemplatedFindSkewedPartitionSplit() const {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using SAMPLE = MergeByteSample<SORT_KEY>;
	static_assert(std::is_trivially_destructible<SAMPLE>::value, "Merge byte samples must not own resources");
	D_ASSERT(SORT_KEY::CONSTANT_SIZE && SORT_KEY::HAS_PAYLOAD);

	auto sample_buffer = BufferAllocator::Get(sort.client_context).Allocate(MAX_BYTE_SAMPLES * sizeof(SAMPLE));
	unsafe_array_ptr<SAMPLE> samples(reinterpret_cast<SAMPLE *>(sample_buffer.get()), MAX_BYTE_SAMPLES);
	const auto sample_count = CollectMergeByteSamples(*this, *sort.payload_layout, samples);
	std::sort(samples.data(), samples.data() + sample_count);
	const unsafe_array_ptr<const SAMPLE> sorted_samples(samples.data(), sample_count);

	const auto estimate = EstimateLargestMergePartition(sorted_samples, partition_size, total_count);
	const auto partition_count = RowPartitionCount(*this);

	// Only split when one partition is estimated to hold more than twice the average bytes.
	if (estimate.largest_bytes <= 2 * (estimate.total_bytes / static_cast<double>(partition_count))) {
		return optional_idx();
	}

	const auto partition_begin = estimate.largest_partition * partition_size;
	const auto partition_end =
	    estimate.largest_partition == partition_count - 1 ? total_count : partition_begin + partition_size;
	return FindMergePartitionByteMidpoint(sorted_samples, partition_begin, partition_end, estimate.largest_bytes);
}

optional_idx SortedRunMerger::FindSkewedPartitionSplit() const {
	const auto &merger = *this;
	const auto key_type = merger.sort.key_layout->GetSortKeyType();
	if (merger.total_count == 0 || merger.is_index_sort || merger.sort.is_index_sort || merger.sorted_runs.size() < 2 ||
	    merger.sorted_runs.size() > MAX_BYTE_SAMPLES || RowPartitionCount(merger) < 3 ||
	    merger.partition_size < idx_t(2) * STANDARD_VECTOR_SIZE || merger.partition_size % STANDARD_VECTOR_SIZE != 0 ||
	    !SortKeyUtils::IsConstantSize(key_type) || !SortKeyUtils::HasPayload(key_type) ||
	    merger.sort.payload_layout->AllConstant()) {
		return optional_idx();
	}
	for (const auto &run : merger.sorted_runs) {
		if (!run->finalized || run->Count() == 0 || !run->payload_data) {
			return optional_idx();
		}
	}
	try {
		switch (key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		return TemplatedFindSkewedPartitionSplit<SortKeyType::SORT_KEY_TYPE>();
			DUCKDB_FOR_EACH_PAYLOAD_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
		default:
			return optional_idx();
		}
	} catch (const OutOfMemoryException &) {
		return optional_idx();
	} catch (const std::bad_alloc &) {
		return optional_idx();
	}
}

//===--------------------------------------------------------------------===//
// Sorted Run Merger
//===--------------------------------------------------------------------===//
SortedRunMerger::SortedRunMerger(const Sort &sort_p, vector<unique_ptr<SortedRun>> &&sorted_runs_p,
                                 idx_t partition_size_p, bool external_p, bool is_index_sort_p)
    : scheduler(TaskScheduler::GetScheduler(*sort_p.client_context.db)), sort(sort_p),
      sorted_runs(std::move(sorted_runs_p)), total_count(SortedRunsTotalCount(sorted_runs)),
      partition_size(partition_size_p), external(external_p), is_index_sort(is_index_sort_p),
      extra_partition_end(FindSkewedPartitionSplit()) {
}

idx_t SortedRunMerger::PartitionCount() const {
	return RowPartitionCount(*this) + extra_partition_end.IsValid();
}

idx_t SortedRunMerger::PartitionRowCount(idx_t partition_count) const {
	D_ASSERT(partition_count <= PartitionCount());
	if (partition_count == 0) {
		return 0;
	}
	if (partition_count == PartitionCount()) {
		return total_count;
	}
	if (extra_partition_end.IsValid()) {
		const auto extra_end = extra_partition_end.GetIndex();
		const auto split_partition = extra_end / partition_size;
		if (partition_count == split_partition + 1) {
			return extra_end;
		}
		if (partition_count > split_partition + 1) {
			partition_count--;
		}
	}
	return partition_count * partition_size;
}

SortedRunMerger::~SortedRunMerger() {
	ParallelDestroyTask<decltype(sorted_runs)>::Schedule(scheduler, sorted_runs);
}

unique_ptr<LocalSourceState> SortedRunMerger::GetLocalSourceState(ExecutionContext &,
                                                                  GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<SortedRunMergerGlobalState>();
	annotated_lock_guard<annotated_mutex> guard(gstate.lock);
	return make_uniq<SortedRunMergerLocalState>(gstate);
}

unique_ptr<GlobalSourceState> SortedRunMerger::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<SortedRunMergerGlobalState>(context, *this);
}

SourceResultType SortedRunMerger::GetData(ExecutionContext &, DataChunk &chunk, OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<SortedRunMergerGlobalState>();
	auto &lstate = input.local_state.Cast<SortedRunMergerLocalState>();

	while (chunk.size() == 0) {
		if (!lstate.TaskFinished() || gstate.AssignTask(lstate)) {
			lstate.ExecuteTask(gstate, &chunk);
		} else {
			break;
		}
	}

	if (chunk.size() != 0) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	}

	// Done
	lstate.Clear();
	return SourceResultType::FINISHED;
}

OperatorPartitionData SortedRunMerger::GetPartitionData(ExecutionContext &, DataChunk &, GlobalSourceState &,
                                                        LocalSourceState &lstate_p,
                                                        const OperatorPartitionInfo &) const {
	auto &lstate = lstate_p.Cast<SortedRunMergerLocalState>();
	return OperatorPartitionData(lstate.partition_idx.GetIndex());
}

ProgressData SortedRunMerger::GetProgress(ClientContext &, GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<SortedRunMergerGlobalState>();
	ProgressData res;
	res.done = static_cast<double>(MinValue<idx_t>(gstate.progress_scanned, total_count));
	res.total = static_cast<double>(total_count);
	res.invalid = false;
	return res;
}

bool SortedRunMerger::IsFinished(GlobalSourceState &gstate_p) const {
	auto &gstate = gstate_p.Cast<SortedRunMergerGlobalState>();
	return gstate.total_scanned == total_count;
}

//===--------------------------------------------------------------------===//
// Non-Standard Interface
//===--------------------------------------------------------------------===//
SourceResultType SortedRunMerger::MaterializeSortedRun(ExecutionContext &, OperatorSourceInput &input) const {
	auto &gstate = input.global_state.Cast<SortedRunMergerGlobalState>();
	auto &lstate = input.local_state.Cast<SortedRunMergerLocalState>();

	SourceResultType res = SourceResultType::HAVE_MORE_OUTPUT;
	while (true) {
		if (!lstate.TaskFinished() || gstate.AssignTask(lstate)) {
			res = lstate.ExecuteTask(gstate, nullptr);
		} else {
			break;
		}
	}
	lstate.Clear(); // Done

	// The thread that completes the materialization returns FINISHED, all other threads return HAVE_MORE_OUTPUT
	return res;
}

unique_ptr<SortedRun> SortedRunMerger::GetSortedRun(GlobalSourceState &global_state) {
	auto &gstate = global_state.Cast<SortedRunMergerGlobalState>();
	D_ASSERT(total_count != 0);
	lock_guard<mutex> guard(gstate.materialized_partition_lock);
	if (gstate.materialized_partitions.empty()) {
		return nullptr;
	}
	auto &target = *gstate.materialized_partitions[0];
	for (idx_t i = 1; i < gstate.materialized_partitions.size(); i++) {
		auto &source = gstate.materialized_partitions[i];
		target.key_data->Combine(*source->key_data);
		if (target.payload_data) {
			target.payload_data->Combine(*source->payload_data);
		}
		source.reset();
	}
	auto res = std::move(gstate.materialized_partitions[0]);
	gstate.materialized_partitions.clear();
	return res;
}

} // namespace duckdb
