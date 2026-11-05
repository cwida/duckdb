#include "duckdb/execution/operator/csv_scanner/csv_sequential_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"

namespace duckdb {

CSVSequentialBufferManager::CSVSequentialBufferManager(ClientContext &context_p, const CSVReaderOptions &options,
                                                       const OpenFileInfo &file_p, bool per_file_single_threaded_p,
                                                       unique_ptr<CSVFileHandle> file_handle_p)
    : CSVBufferManager(context_p, options, file_p, per_file_single_threaded_p, std::move(file_handle_p)) {
	is_pipe = file_handle->IsPipe();
	Initialize();
}

void CSVSequentialBufferManager::Initialize() {
	if (cached_buffers.empty()) {
		cached_buffers.emplace_back(make_shared_ptr<CSVBuffer>(context, buffer_size, *file_handle, global_csv_pos));
		last_buffer = cached_buffers.front();
	}
}

bool CSVSequentialBufferManager::ReadNextAndCacheIt() {
	D_ASSERT(last_buffer);
	for (idx_t i = 0; i < 2; i++) {
		if (!last_buffer->IsCSVFileLastBuffer()) {
			auto maybe_last_buffer = last_buffer->Next(*file_handle, buffer_size);
			if (!maybe_last_buffer) {
				last_buffer->last_buffer = true;
				return false;
			}
			last_buffer = std::move(maybe_last_buffer);
			cached_buffers.emplace_back(last_buffer);
			return true;
		}
	}
	return false;
}

shared_ptr<CSVBufferHandle> CSVSequentialBufferManager::GetBuffer(const idx_t pos) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (pos == 0 && done && cached_buffers.empty()) {
		if (is_pipe) {
			throw InvalidInputException("Recursive CTEs are not allowed when using piped csv files");
		}
		// This is a recursive CTE, we have to reset out whole buffer
		done = false;
		file_handle->Reset();
		Initialize();
	}
	while (pos >= cached_buffers.size()) {
		if (done) {
			return nullptr;
		}
		if (!ReadNextAndCacheIt()) {
			done = true;
		}
	}
	UnpinPrevious(pos);
	return cached_buffers[pos]->Pin(*file_handle);
}

void CSVSequentialBufferManager::UnpinPrevious(const idx_t pos) {
	if (pos != 0 && (sniffing || file_handle->CanSeek() || per_file_single_threaded)) {
		// We don't need to unpin the buffers here if we are not sniffing since we
		// control it per-thread on the scan
		if (cached_buffers[pos - 1]) {
			cached_buffers[pos - 1]->Unpin();
		}
	}
}

CSVBufferResidency CSVSequentialBufferManager::GetBufferResidency(const idx_t pos,
                                                                  shared_ptr<CSVBufferHandle> &handle) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (pos < cached_buffers.size()) {
		D_ASSERT(cached_buffers[pos]);
		if (!cached_buffers[pos]->IsInMemory()) {
			return CSVBufferResidency::NEEDS_LOAD;
		}
		UnpinPrevious(pos);
		handle = cached_buffers[pos]->Pin(*file_handle);
		return CSVBufferResidency::IN_MEMORY;
	}
	if (!cached_buffers.empty() && cached_buffers.back() && cached_buffers.back()->last_buffer) {
		done = true;
	}
	if (done) {
		return CSVBufferResidency::END_OF_FILE;
	}
	return CSVBufferResidency::NEEDS_LOAD;
}

void CSVSequentialBufferManager::ResetBuffer(const idx_t buffer_idx) {
	lock_guard<mutex> parallel_lock(main_mutex);
	if (buffer_idx >= cached_buffers.size()) {
		// Nothing to reset
		return;
	}
	D_ASSERT(cached_buffers[buffer_idx]);
	if (buffer_idx == 0 && cached_buffers.size() > 1) {
		cached_buffers[buffer_idx].reset();
		idx_t cur_buffer = buffer_idx + 1;
		while (reset_when_possible.find(cur_buffer) != reset_when_possible.end()) {
			cached_buffers[cur_buffer].reset();
			reset_when_possible.erase(cur_buffer);
			cur_buffer++;
		}
		return;
	}
	// We only reset if previous one was also already reset
	if (buffer_idx == 0 || cached_buffers[buffer_idx - 1]) {
		reset_when_possible.insert(buffer_idx);
		return;
	}
	if (cached_buffers[buffer_idx]->last_buffer) {
		// We clear the whole shebang
		cached_buffers.clear();
		reset_when_possible.clear();
		return;
	}
	cached_buffers[buffer_idx].reset();
	idx_t cur_buffer = buffer_idx + 1;
	while (reset_when_possible.find(cur_buffer) != reset_when_possible.end()) {
		cached_buffers[cur_buffer].reset();
		reset_when_possible.erase(cur_buffer);
		cur_buffer++;
	}
}

bool CSVSequentialBufferManager::Done() const {
	lock_guard<mutex> parallel_lock(main_mutex);
	return done;
}

void CSVSequentialBufferManager::ResetBufferManager() {
	if (!file_handle->IsPipe()) {
		// If this is not a pipe we reset the buffer manager and restart it when doing the actual scan
		cached_buffers.clear();
		reset_when_possible.clear();
		file_handle->Reset();
		last_buffer = nullptr;
		done = false;
		global_csv_pos = 0;
		Initialize();
	}
}

} // namespace duckdb
