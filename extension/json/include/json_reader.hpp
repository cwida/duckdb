//===----------------------------------------------------------------------===//
//                         DuckDB
//
// json_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/enums/file_compression_type.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "json_reader_options.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/query_context.hpp"
#include "json_common.hpp"
#include "json_enums.hpp"
#include "yyjson_memory.hpp"

namespace duckdb {
struct JSONScanGlobalState;
class JSONReader;

struct JSONBufferHandle {
public:
	JSONBufferHandle(JSONReader &reader, idx_t buffer_index, idx_t readers, AllocatedData buffer, idx_t buffer_size,
	                 idx_t buffer_start);

public:
	//! The reader this buffer comes from
	JSONReader &reader;
	//! Buffer index (within same file)
	const idx_t buffer_index;

	//! Number of readers for this buffer
	atomic<idx_t> readers;
	//! The buffer
	AllocatedData buffer;
	//! The size of the data in the buffer (can be less than buffer.GetSize())
	const idx_t buffer_size;
	//! The start position in the buffer
	idx_t buffer_start;
};

struct JSONFileHandle {
public:
	JSONFileHandle(QueryContext context, unique_ptr<FileHandle> file_handle, Allocator &allocator);

	bool IsOpen() const;
	void Close();

	void Reset();
	bool RequestedReadsComplete();
	bool LastReadRequested() const;

	idx_t FileSize() const;
	idx_t Remaining() const;
	//! The fraction of the file that has been read, in [0, 1]
	double GetProgress() const;

	bool CanSeek() const;
	bool IsPipe() const;

	FileHandle &GetHandle();

	//! The next two functions return whether the read was successful
	bool GetPositionAndSize(idx_t &position, idx_t &size, idx_t requested_size);
	bool Read(char *pointer, idx_t &read_size, idx_t requested_size);
	//! Read at position optionally allows passing a custom handle to read from, otherwise the default one is used
	void ReadAtPosition(char *pointer, idx_t size, idx_t position, optional_ptr<FileHandle> override_handle = nullptr);
	//! The bytes of the stored file read since the last call - for a compressed file, its compressed bytes. Bytes read
	//! again after a reset are only returned once
	idx_t TakeStoredBytesRead();

private:
	idx_t ReadInternal(char *pointer, const idx_t requested_size);
	idx_t ReadFromCache(char *&pointer, idx_t &size, atomic<idx_t> &position);

private:
	QueryContext context;

	//! The JSON file handle
	unique_ptr<FileHandle> file_handle;
	Allocator &allocator;

	//! File properties
	const bool can_seek;
	const idx_t file_size;
	//! Whether the file is compressed - the read position then refers to the decompressed data
	const bool compressed;

	//! Read properties
	atomic<idx_t> read_position;
	atomic<idx_t> requested_reads;
	atomic<idx_t> actual_reads;
	atomic<bool> last_read_requested;

	//! The bytes of the stored file read and not yet returned by TakeStoredBytesRead
	atomic<idx_t> stored_bytes_untaken {0};
	//! For a compressed file, the compressed bytes added to stored_bytes_untaken since the file was (re)opened
	idx_t compressed_bytes_counted = 0;

	//! Cached buffers for resetting when reading stream
	vector<AllocatedData> cached_buffers;
	idx_t cached_size;
};

struct JSONString {
public:
	JSONString() {
	}
	JSONString(const char *pointer_p, idx_t size_p) : pointer(pointer_p), size(size_p) {
	}

	const char *pointer;
	idx_t size;

public:
	string ToString() {
		return string(pointer, size);
	}

	const char &operator[](size_t i) const {
		return pointer[i];
	}
};

enum class JSONFileReadType { SCAN_ENTIRE_FILE, SCAN_PARTIAL };

struct JSONReaderScanState {
	explicit JSONReaderScanState(ClientContext &context, Allocator &global_allocator,
	                             idx_t reconstruct_buffer_capacity);

	FileSystem &fs;
	Allocator &global_allocator;
	//! Thread-local allocator
	JSONAllocator allocator;
	idx_t buffer_capacity;
	bool initialized = false;
	// if we have a buffer already - this is our buffer index
	optional_idx buffer_index;
	//! Whether or not we are scanning the entire file
	//! If we are scanning the entire file we don't share reads between threads and just read the file until we are done
	JSONFileReadType file_read_type = JSONFileReadType::SCAN_PARTIAL;
	// Data for reading (if we have postponed reading)
	//! Buffer (if we have one)
	AllocatedData read_buffer;
	bool needs_to_read = false;
	idx_t request_size;
	idx_t read_position;
	idx_t read_size;
	//! Current scan data
	idx_t scan_count = 0;
	JSONString units[STANDARD_VECTOR_SIZE];
	yyjson_val *values[STANDARD_VECTOR_SIZE];
	optional_ptr<JSONBufferHandle> current_buffer_handle;
	//! Current buffer read info
	optional_ptr<JSONReader> current_reader;
	char *buffer_ptr = nullptr;
	idx_t buffer_size = 0;
	idx_t buffer_offset = 0;
	idx_t prev_buffer_remainder = 0;
	idx_t prev_buffer_offset = 0;
	idx_t lines_or_objects_in_buffer = 0;
	//! Whether this is the first time scanning this buffer
	bool is_first_scan = false;
	//! Whether this is the last batch of the file
	bool is_last = false;
	//! Whether the remainder of the file is deliberately not read (everything after a FeatureCollection's
	//! "features" array), i.e. no further buffers should be read even if the file has more data
	bool skip_remainder_of_file = false;
	//! Buffer to reconstruct split values
	optional_idx batch_index;

	//! For some filesystems (e.g. S3), using a filehandle per thread increases performance
	unique_ptr<FileHandle> thread_local_filehandle;

public:
	//! Reset for parsing the next batch of JSON from the current buffer
	void ResetForNextParse();
	//! Reset state for reading the next buffer
	void ResetForNextBuffer();
	//! Clear the buffer handle (if any)
	void ClearBufferHandle();
};

struct JSONError {
	idx_t buf_index;
	idx_t line_or_object_in_buf;
	string error_msg;
};

//! Reads a single JSON file
class JSONReader {
public:
	JSONReader(ClientContext &context, JSONReaderOptions options, OpenFileInfo file);

	void OpenJSONFile();
	void CloseHandle();
	void Reset();

	bool HasFileHandle() const;
	bool IsOpen() const;
	bool IsInitialized() const {
		return initialized;
	}

	JSONReaderOptions &GetOptions();

	JSONFormat GetFormat() const;
	void SetFormat(JSONFormat format);

	JSONRecordType GetRecordType() const;
	void SetRecordType(JSONRecordType type);

	const string &GetFileName() const;
	JSONFileHandle &GetFileHandle() const;

public:
	//! Get a new buffer index (must hold the lock)
	idx_t GetBufferIndex();
	//! Set line count for a buffer that is done (grabs the lock)
	void SetBufferLineOrObjectCount(JSONBufferHandle &handle, idx_t count);
	//! Records a parse error in the specified buffer
	void AddParseError(JSONReaderScanState &scan_state, idx_t line_or_object_in_buf, yyjson_read_err &err,
	                   const string &extra = "");
	//! Records a transform error in the specified buffer
	void AddTransformError(JSONReaderScanState &scan_state, idx_t object_index, const string &error_message);
	//! Whether this reader has thrown if an error has occurred
	bool HasThrown();

	void Initialize(Allocator &allocator, idx_t buffer_size);
	bool InitializeScan(JSONReaderScanState &state, JSONFileReadType file_read_type);
	bool ParseJSON(JSONReaderScanState &scan_state, char *const json_start, const idx_t json_size,
	               const idx_t remaining);
	bool ParseNextChunk(JSONReaderScanState &scan_state);
	idx_t Scan(JSONReaderScanState &scan_state);
	bool ReadNextBuffer(JSONReaderScanState &scan_state);
	bool PrepareBufferForRead(JSONReaderScanState &scan_state);

	//! Scan progress
	double GetProgress() const;

	//! Counts what the file was read for as scanned, once a query scans it
	void CountBytesScanned();
	//! Set when a query starts scanning the file, so reading samples of it to detect its schema is not counted
	bool scanning = false;
	//! The bytes of the stored file scanned but not yet reported to the profiler
	atomic<idx_t> bytes_scanned_unreported {0};

	void DecrementBufferUsage(JSONBufferHandle &handle, idx_t lines_or_object_in_buffer, AllocatedData &buffer);

private:
	void SkipOverArrayStart(JSONReaderScanState &scan_state);
	void AutoDetect(Allocator &allocator, idx_t buffer_size);
	bool CopyRemainderFromPreviousBuffer(JSONReaderScanState &scan_state);
	void FinalizeBufferInternal(JSONReaderScanState &scan_state, AllocatedData &buffer, idx_t buffer_index);
	void PrepareForReadInternal(JSONReaderScanState &scan_state);
	void PrepareForScan(JSONReaderScanState &scan_state);
	bool PrepareBufferSeek(JSONReaderScanState &scan_state);
	void ReadNextBufferSeek(JSONReaderScanState &scan_state);
	bool ReadNextBufferNoSeek(JSONReaderScanState &scan_state);
	void FinalizeBuffer(JSONReaderScanState &scan_state);

	//! Insert/get/remove buffer (grabs the lock)
	void InsertBuffer(idx_t buffer_idx, unique_ptr<JSONBufferHandle> buffer);
	optional_ptr<JSONBufferHandle> GetBuffer(idx_t buffer_idx);
	AllocatedData RemoveBuffer(JSONBufferHandle &handle);

	void ThrowObjectSizeError(const idx_t object_size);

private:
	//! Add an error to the buffer - requires the lock to be held
	void AddError(idx_t buf_index, idx_t line_or_object_in_buf, const string &error_msg);
	//! Throw errors if possible - requires the lock to be held
	void ThrowErrorsIfPossible();
	//! Try to get the line number - requires the lock to be held
	optional_idx TryGetLineNumber(idx_t buf_index, idx_t line_or_object_in_buf);

private:
	//! The file that is read
	OpenFileInfo file;
	ClientContext &context;
	JSONReaderOptions options;
	//! The flags documents are parsed with: numbers are kept as text when a column is or contains a DECIMAL
	yyjson_read_flag read_flag;

	//! File handle
	unique_ptr<JSONFileHandle> file_handle;

	//! Whether or not the reader has been initialized
	bool initialized;
	//! Next buffer index within the file
	idx_t next_buffer_index;
	//! Mapping from batch index to currently held buffers
	unordered_map<idx_t, unique_ptr<JSONBufferHandle>> buffer_map;

	//! Line count per buffer
	vector<int64_t> buffer_line_or_object_counts;
	//! Whether any of the reading threads has thrown an error
	bool thrown;

	//! If we have auto-detected, this is the buffer read by the auto-detection
	AllocatedData auto_detect_data;
	idx_t auto_detect_data_size = 0;

	//! Whether this file is a GeoJSON FeatureCollection, i.e. the rows live in its "features" array rather than at
	//! the top level. The array itself is then read using the regular JSONFormat::ARRAY handling
	bool skip_feature_collection_prefix = false;

	//! The first error we found in the file (if any)
	unique_ptr<JSONError> error;

public:
	mutable mutex lock;
};

} // namespace duckdb
