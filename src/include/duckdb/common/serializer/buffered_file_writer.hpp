//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/buffered_file_writer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb {

#define FILE_BUFFER_SIZE 4096

class BufferedFileWriter : public WriteStream {
public:
	DUCKDB_API static const FileOpenFlags DEFAULT_OPEN_FLAGS;

	//! Serializes to a buffer allocated by the serializer, will expand when
	//! writing past the initial threshold. The optional QueryContext is used to attribute
	//! the written bytes to the query's I/O metrics.
	DUCKDB_API BufferedFileWriter(FileSystem &fs, const string &path,
	                              const FileOpenFlags &open_flags = DEFAULT_OPEN_FLAGS,
	                              QueryContext context = QueryContext());

	FileSystem &fs;
	string path;
	unsafe_unique_array<data_t> data;
	idx_t offset;
	idx_t total_written;
	unique_ptr<FileHandle> handle;
	QueryContext context;

public:
	DUCKDB_API void WriteData(const_data_ptr_t buffer, idx_t write_size) override;
	//! Flush all changes to the file and then close the file
	DUCKDB_API void Close();
	//! Flush all changes and fsync the file to disk
	DUCKDB_API void Sync();
	//! Fsync the file handle without flushing the buffer
	DUCKDB_API void SyncHandle();
	//! Flush the buffer to the file (without sync)
	DUCKDB_API void Flush();
	//! Returns the current size of the file
	DUCKDB_API idx_t GetFileSize();
	//! Truncate the size to a previous size (given that size <= GetFileSize())
	DUCKDB_API void Truncate(idx_t size);

	DUCKDB_API idx_t GetTotalWritten() const;
};

} // namespace duckdb
