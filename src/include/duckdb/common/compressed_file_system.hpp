//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/compressed_file_system.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/query_context.hpp"

namespace duckdb {
class CompressedFile;

struct StreamData {
	// various buffers & pointers
	bool write = false;
	bool refresh = false;
	AllocatedData in_buff;
	AllocatedData out_buff;
	data_ptr_t out_buff_start = nullptr;
	data_ptr_t out_buff_end = nullptr;
	data_ptr_t in_buff_start = nullptr;
	data_ptr_t in_buff_end = nullptr;

	idx_t in_buf_size = 0;
	idx_t out_buf_size = 0;
};

struct StreamWrapper {
	DUCKDB_API virtual ~StreamWrapper();

	DUCKDB_API virtual void Initialize(QueryContext context, CompressedFile &file, bool write) = 0;
	DUCKDB_API virtual bool Read(StreamData &stream_data) = 0;
	DUCKDB_API virtual void FinalizeRead(StreamData &stream_data);
	DUCKDB_API virtual void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer,
	                              int64_t nr_bytes) = 0;
	DUCKDB_API virtual void Close() = 0;
	DUCKDB_API virtual void AbortWrite();
};

class CompressedFileSystem : public FileSystem {
public:
	DUCKDB_API int64_t Read(FileHandle &handle, void *buffer, int64_t nr_bytes) override;
	DUCKDB_API int64_t Write(FileHandle &handle, void *buffer, int64_t nr_bytes) override;

	DUCKDB_API void Reset(FileHandle &handle) override;

	DUCKDB_API int64_t GetFileSize(FileHandle &handle) override;

	DUCKDB_API bool OnDiskFile(FileHandle &handle) override;
	DUCKDB_API bool CanSeek() override;
	DUCKDB_API void AbortFileWrite(FileHandle &handle) override;

	//! The compression scheme provided by this filesystem, e.g. "gzip"
	DUCKDB_API virtual FileCompressionType GetCompressionType() = 0;
	//! Whether this filesystem can decompress the given file - used to auto-detect compression from the file name.
	//! Filesystems that do not override this do not participate in compression auto-detection.
	DUCKDB_API bool CanHandleFile(const string &fpath) override;

	DUCKDB_API virtual unique_ptr<StreamWrapper> CreateStream() = 0;
	DUCKDB_API virtual idx_t InBufferSize() = 0;
	DUCKDB_API virtual idx_t OutBufferSize() = 0;
};

class CompressedFile : public FileHandle {
public:
	DUCKDB_API CompressedFile(CompressedFileSystem &fs, unique_ptr<FileHandle> child_handle_p, const string &path);
	DUCKDB_API ~CompressedFile() override;

	DUCKDB_API idx_t GetProgress() override;

	CompressedFileSystem &compressed_fs;
	unique_ptr<FileHandle> child_handle;
	//! Whether the file is opened for reading or for writing
	bool write = false;
	StreamData stream_data;
	//! The query context, used to attribute the (compressed) on-disk I/O of the child handle to the query
	QueryContext context;

public:
	DUCKDB_API void Initialize(QueryContext context, bool write);
	DUCKDB_API int64_t ReadData(void *buffer, int64_t nr_bytes);
	DUCKDB_API int64_t WriteData(data_ptr_t buffer, int64_t nr_bytes);
	DUCKDB_API void Close() override;
	DUCKDB_API void AbortCompressedWrite();
	//! Counts bytes the stream read from the child handle itself, outside ReadData (e.g. a header), as consumed
	DUCKDB_API void AddConsumedBytes(idx_t bytes);

private:
	void Clear(); // for Initialize re-use to support FS.Reset()
	void ResetStreamData();

	//! The number of compressed bytes read from the child handle
	idx_t compressed_bytes_read = 0;
	//! The number of compressed bytes consumed by the decompressor (for progress)
	atomic<idx_t> compressed_bytes_consumed {0};
	bool initialized = false;
	unique_ptr<StreamWrapper> stream_wrapper;
};

} // namespace duckdb
