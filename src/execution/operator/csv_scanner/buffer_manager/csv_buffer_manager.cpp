#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_sequential_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_random_access_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/function/table/read_csv.hpp"
namespace duckdb {

CSVBufferManager::CSVBufferManager(ClientContext &context_p, const CSVReaderOptions &options,
                                   const OpenFileInfo &file_p, bool per_file_single_threaded_p,
                                   unique_ptr<CSVFileHandle> file_handle_p)
    : file_handle(std::move(file_handle_p)), context(context_p), per_file_single_threaded(per_file_single_threaded_p),
      file(file_p), buffer_size(options.buffer_size_option.GetValue()) {
	D_ASSERT(file_handle);
}

shared_ptr<CSVBufferManager> CSVBufferManager::Open(ClientContext &context, const CSVReaderOptions &options,
                                                    const OpenFileInfo &file, bool per_file_single_threaded,
                                                    unique_ptr<CSVFileHandle> file_handle) {
	D_ASSERT(!file.path.empty());
	if (!file_handle) {
		file_handle = ReadCSV::OpenCSV(file, options, context);
	}
	if (file_handle->HasKnownBufferRanges()) {
		return make_shared_ptr<CSVRandomAccessBufferManager>(context, options, file, per_file_single_threaded,
		                                                     std::move(file_handle));
	}
	return make_shared_ptr<CSVSequentialBufferManager>(context, options, file, per_file_single_threaded,
	                                                   std::move(file_handle));
}

bool CSVBufferManager::HasKnownBufferRanges() const {
	return file_handle->HasKnownBufferRanges();
}

idx_t CSVBufferManager::KnownBufferCount() const {
	D_ASSERT(HasKnownBufferRanges());
	const idx_t file_size = file_handle->FileSize();
	if (file_size == 0) {
		// an empty file still materializes a single empty buffer
		return 1;
	}
	return (file_size + buffer_size - 1) / buffer_size;
}

idx_t CSVBufferManager::KnownBufferSize(const idx_t buffer_idx) const {
	D_ASSERT(HasKnownBufferRanges() && buffer_idx < KnownBufferCount());
	const idx_t file_size = file_handle->FileSize();
	const idx_t buffer_start = buffer_idx * buffer_size;
	return MinValue<idx_t>(buffer_size, file_size - buffer_start);
}

idx_t CSVBufferManager::GetBufferSize() const {
	return buffer_size;
}

idx_t CSVBufferManager::BufferCount() const {
	lock_guard<mutex> parallel_lock(main_mutex);
	return cached_buffers.size();
}

string CSVBufferManager::GetFilePath() const {
	return file.path;
}

bool CSVBufferManager::IsBlockUnloaded(idx_t block_idx) {
	if (block_idx < cached_buffers.size()) {
		return cached_buffers[block_idx]->IsUnloaded();
	}
	return false;
}

} // namespace duckdb
