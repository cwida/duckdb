//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_file_handle.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/execution/operator/csv_scanner/encode/csv_encoder.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {
class Allocator;
class FileSystem;
struct CSVReaderOptions;

class CSVFileHandle {
public:
	CSVFileHandle(ClientContext &context, unique_ptr<FileHandle> file_handle_p, const OpenFileInfo &file,
	              const CSVReaderOptions &options);

	mutex main_mutex;

	bool CanSeek() const;
	void Seek(idx_t position) const;
	bool OnDiskFile() const;
	bool IsPipe() const;

	void Reset();

	idx_t FileSize() const;

	//! Whether buffer byte ranges follow deterministically from the file size
	//! (i.e., seekable+uncompressed+utf-8+not a pipe)
	bool HasKnownBufferRanges() const;

	bool FinishedReading() const;

	idx_t Read(void *buffer, idx_t nr_bytes);

	//! Random-access read that bypasses the handle's seek state, only valid for files with known buffer ranges
	void ReadAt(void *buffer, idx_t nr_bytes, idx_t position);

	string ReadLine();

	string GetFilePath();

	static unique_ptr<FileHandle> OpenFileHandle(FileSystem &fs, Allocator &allocator, const OpenFileInfo &file,
	                                             FileCompressionType compression);
	static unique_ptr<CSVFileHandle> OpenFile(ClientContext &context, const OpenFileInfo &file,
	                                          const CSVReaderOptions &options);
	FileCompressionType compression_type;

	double GetProgress() const;
	//! The number of (decompressed) bytes that have been read from the file
	idx_t UncompressedBytesRead() const {
		return uncompressed_bytes_read.load(std::memory_order_relaxed);
	}
	//! The bytes of the stored file read since the last call - for a compressed file, its compressed bytes. Bytes read
	//! again are not returned again
	idx_t TakeStoredBytesRead() {
		return stored_bytes_untaken.exchange(0);
	}
	void AddStoredBytesRead(idx_t bytes) {
		stored_bytes_untaken += bytes;
	}

private:
	QueryContext context;
	unique_ptr<FileHandle> file_handle;
	CSVEncoder encoder;
	const OpenFileInfo file;
	bool can_seek = false;
	bool on_disk_file = false;
	bool is_pipe = false;
	atomic<idx_t> uncompressed_bytes_read {0};
	atomic<idx_t> stored_bytes_untaken {0};
	//! How far into the stored file sequential reads have counted, so a read again after a seek is not counted twice
	idx_t stored_bytes_counted = 0;

	idx_t file_size = 0;

	idx_t requested_bytes = 0;
	//! If we finished reading the file
	bool finished = false;
};

} // namespace duckdb
