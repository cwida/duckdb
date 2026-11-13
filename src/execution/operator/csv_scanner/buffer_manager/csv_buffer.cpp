#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

CSVBuffer::CSVBuffer(ClientContext &context, idx_t buffer_size_p, CSVFileHandle &file_handle,
                     const idx_t &global_csv_current_position)
    : context(context), requested_size(buffer_size_p) {
	global_csv_start = global_csv_current_position;
	Load(file_handle);
}

CSVBuffer::CSVBuffer(CSVFileHandle &file_handle, ClientContext &context, idx_t buffer_size,
                     idx_t global_csv_current_position, idx_t buffer_idx_p)
    : context(context), requested_size(buffer_size), global_csv_start(global_csv_current_position),
      buffer_idx(buffer_idx_p) {
	Load(file_handle);
}

CSVBuffer::CSVBuffer(ClientContext &context, idx_t requested_size_p, idx_t actual_size, idx_t global_csv_start_p,
                     idx_t buffer_idx_p, bool last_buffer_p)
    : last_buffer(last_buffer_p), context(context), actual_buffer_size(actual_size), requested_size(requested_size_p),
      global_csv_start(global_csv_start_p), random_access(true), buffer_idx(buffer_idx_p) {
}

void CSVBuffer::Load(CSVFileHandle &file_handle) {
	AllocateBuffer(requested_size);
	auto buffer = char_ptr_cast(handle.GetDataMutable());
	actual_buffer_size = file_handle.Read(buffer, requested_size);
	while (actual_buffer_size < requested_size && !file_handle.FinishedReading()) {
		// We keep reading until this block is full
		actual_buffer_size += file_handle.Read(&buffer[actual_buffer_size], requested_size - actual_buffer_size);
	}
	last_buffer = file_handle.FinishedReading();
}

shared_ptr<CSVBuffer> CSVBuffer::Next(CSVFileHandle &file_handle, idx_t buffer_size) const {
	auto next_csv_buffer = make_shared_ptr<CSVBuffer>(file_handle, context, buffer_size,
	                                                  global_csv_start + actual_buffer_size, buffer_idx + 1);
	if (next_csv_buffer->GetBufferSize() == 0) {
		// We are done reading
		return nullptr;
	}
	return next_csv_buffer;
}

void CSVBuffer::AllocateBuffer(idx_t buffer_size) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	// Only random-access buffers can be re-read from the file; sequential buffers are spilled instead
	const bool can_destroy = random_access;
	handle = buffer_manager.Allocate(MemoryTag::CSV_READER, MaxValue<idx_t>(buffer_manager.GetBlockSize(), buffer_size),
	                                 can_destroy);
	block = handle.GetBlockHandle();
}

idx_t CSVBuffer::GetBufferSize() const {
	return actual_buffer_size;
}

void CSVBuffer::LoadRandomAccess(CSVFileHandle &file_handle) {
	D_ASSERT(random_access);
	AllocateBuffer(actual_buffer_size);
	if (actual_buffer_size > 0) {
		file_handle.ReadAt(handle.GetDataMutable(), actual_buffer_size, global_csv_start);
	}
}

shared_ptr<CSVBufferHandle> CSVBuffer::Pin(CSVFileHandle &file_handle) {
	lock_guard<mutex> guard(load_lock);
	while (true) {
		if (!IsInMemory()) {
			// Only random-access buffers can be destroyed, re-read the byte range from the file
			D_ASSERT(random_access);
			block = nullptr;
			LoadRandomAccess(file_handle);
		}
		auto pinned = CreatePin();
		if (pinned->handle.IsValid()) {
			return pinned;
		}
		// the block is destroyable and was evicted between the in-memory check and the pin - reload it
		block = nullptr;
	}
}

bool CSVBuffer::TryPin(shared_ptr<CSVBufferHandle> &pinned) {
	unique_lock<mutex> guard(load_lock);
	if (!guard.owns_lock() || !IsInMemory()) {
		return false;
	}
	pinned = CreatePin();
	return true;
}

shared_ptr<CSVBufferHandle> CSVBuffer::CreatePin() {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	auto pinned = make_shared_ptr<CSVBufferHandle>(buffer_manager.Pin(block), actual_buffer_size, requested_size,
	                                               last_buffer, buffer_idx);
	if (random_access) {
		// Random-access buffers can always be re-read
		Unpin();
	}
	return pinned;
}

void CSVBuffer::Unpin() {
	if (handle.IsValid()) {
		handle.Destroy();
	}
}

bool CSVBuffer::IsCSVFileLastBuffer() const {
	return last_buffer;
}

} // namespace duckdb
