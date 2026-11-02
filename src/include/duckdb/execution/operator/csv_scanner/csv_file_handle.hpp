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

private:
	QueryContext context;
	unique_ptr<FileHandle> file_handle;
	CSVEncoder encoder;
	const OpenFileInfo file;
	bool can_seek = false;
	bool on_disk_file = false;
	bool is_pipe = false;
	atomic<idx_t> uncompressed_bytes_read {0};

	idx_t file_size = 0;

	idx_t requested_bytes = 0;
	//! If we finished reading the file
	bool finished = false;
};

} // namespace duckdb
