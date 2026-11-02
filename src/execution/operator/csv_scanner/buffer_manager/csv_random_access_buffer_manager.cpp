#include "duckdb/execution/operator/csv_scanner/csv_random_access_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"

namespace duckdb {

CSVRandomAccessBufferManager::CSVRandomAccessBufferManager(ClientContext &context_p, const CSVReaderOptions &options,
                                                           const OpenFileInfo &file_p, bool per_file_single_threaded_p,
                                                           unique_ptr<CSVFileHandle> file_handle_p)
    : CSVBufferManager(context_p, options, file_p, per_file_single_threaded_p, std::move(file_handle_p)) {
	Initialize();
}

void CSVRandomAccessBufferManager::Initialize() {
	const idx_t buffer_count = KnownBufferCount();
	cached_buffers.resize(buffer_count);
	created.assign(buffer_count, false);
	cached_buffers[0] = CreateBuffer(0);
	cached_buffers[0]->LoadRandomAccess(*file_handle);
}

shared_ptr<CSVBuffer> CSVRandomAccessBufferManager::CreateBuffer(idx_t pos) {
	if (!created[pos]) {
		created[pos] = true;
		file_handle->AddStoredBytesRead(KnownBufferSize(pos));
	}
	return make_shared_ptr<CSVBuffer>(context, buffer_size, KnownBufferSize(pos), pos * buffer_size, pos,
	                                  pos + 1 == cached_buffers.size());
}

shared_ptr<CSVBufferHandle> CSVRandomAccessBufferManager::GetBuffer(const idx_t pos) {
	shared_ptr<CSVBuffer> buffer;
	{
		lock_guard<mutex> parallel_lock(main_mutex);
		if (pos >= cached_buffers.size()) {
			return nullptr;
		}
		if (!cached_buffers[pos]) {
			cached_buffers[pos] = CreateBuffer(pos);
		}
		buffer = cached_buffers[pos];
	}
	// the load of a fresh or evicted buffer runs inside Pin, under the buffer's own lock,
	// so loads of different buffers proceed in parallel
	bool has_seeked = false;
	return buffer->Pin(*file_handle, has_seeked);
}

CSVBufferResidency CSVRandomAccessBufferManager::GetBufferResidency(const idx_t pos,
                                                                    shared_ptr<CSVBufferHandle> &handle) {
	shared_ptr<CSVBuffer> buffer;
	{
		lock_guard<mutex> parallel_lock(main_mutex);
		if (pos >= cached_buffers.size()) {
			return CSVBufferResidency::END_OF_FILE;
		}
		if (!cached_buffers[pos]) {
			return CSVBufferResidency::NEEDS_LOAD;
		}
		buffer = cached_buffers[pos];
	}
	// a buffer with a load in flight reports NEEDS_LOAD, the caller's load task then doubles as a wait handle
	return buffer->TryPin(handle) ? CSVBufferResidency::IN_MEMORY : CSVBufferResidency::NEEDS_LOAD;
}

void CSVRandomAccessBufferManager::ResetBuffer(const idx_t buffer_idx) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (buffer_idx >= cached_buffers.size()) {
		// Nothing to reset
		return;
	}
	cached_buffers[buffer_idx].reset();
}

bool CSVRandomAccessBufferManager::Done() const {
	// all buffer extents are known from construction
	return true;
}

void CSVRandomAccessBufferManager::ResetBufferManager() {
	cached_buffers.clear();
	Initialize();
}

} // namespace duckdb
