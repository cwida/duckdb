#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"

#include "miniz.hpp"

#include "duckdb/common/limits.hpp"

namespace duckdb {

/*

  0      2 bytes  magic header  0x1f, 0x8b (\037 \213)
  2      1 byte   compression method
                     0: store (copied)
                     1: compress
                     2: pack
                     3: lzh
                     4..7: reserved
                     8: deflate
  3      1 byte   flags
                     bit 0 set: file probably ascii text
                     bit 1 set: continuation of multi-part gzip file, part number present
                     bit 2 set: extra field present
                     bit 3 set: original file name present
                     bit 4 set: file comment present
                     bit 5 set: file is encrypted, encryption header present
                     bit 6,7:   reserved
  4      4 bytes  file modification time in Unix format
  8      1 byte   extra flags (depend on compression method)
  9      1 byte   OS type
[
         2 bytes  optional part number (second part=1)
]?
[
         2 bytes  optional extra field length (e)
        (e)bytes  optional extra field
]?
[
           bytes  optional original file name, zero terminated
]?
[
           bytes  optional file comment, zero terminated
]?
[
        12 bytes  optional encryption header
]?
           bytes  compressed data
         4 bytes  crc32
         4 bytes  uncompressed input size modulo 2^32

 */

static idx_t GZipConsumeString(FileHandle &input) {
	idx_t size = 1; // terminator
	char buffer[1];
	while (input.Read(buffer, 1) == 1) {
		if (buffer[0] == '\0') {
			break;
		}
		size++;
	}
	return size;
}

static constexpr const uint8_t GZIP_COMPRESSION_DEFLATE = 0x08;

static constexpr const uint8_t GZIP_FLAG_ASCII = 0x1;
static constexpr const uint8_t GZIP_FLAG_MULTIPART = 0x2;
static constexpr const uint8_t GZIP_FLAG_EXTRA = 0x4;
static constexpr const uint8_t GZIP_FLAG_NAME = 0x8;
static constexpr const uint8_t GZIP_FLAG_COMMENT = 0x10;
static constexpr const uint8_t GZIP_FLAG_ENCRYPT = 0x20;

static constexpr const uint8_t GZIP_HEADER_MINSIZE = 10;

static constexpr const unsigned char GZIP_FLAG_UNSUPPORTED =
    GZIP_FLAG_ASCII | GZIP_FLAG_MULTIPART | GZIP_FLAG_EXTRA | GZIP_FLAG_COMMENT | GZIP_FLAG_ENCRYPT;

struct MiniZStreamWrapper {
	~MiniZStreamWrapper() {
		Close();
	}

	duckdb_miniz::mz_stream *mz_stream_ptr = nullptr;

public:
	void Initialize() {
		Close();
		mz_stream_ptr = new duckdb_miniz::mz_stream();
		memset(mz_stream_ptr, 0, sizeof(duckdb_miniz::mz_stream));
	}

	void Close() {
		if (!mz_stream_ptr) {
			return;
		}
		duckdb_miniz::mz_inflateEnd(mz_stream_ptr);
		delete mz_stream_ptr;
		mz_stream_ptr = nullptr;
	}
};

class GZipFile : public FileHandle {
	static constexpr const idx_t BUFFER_SIZE = 1024;

public:
	GZipFile(unique_ptr<FileHandle> child_handle_p, const string &path)
	    : FileHandle(gzip_fs, path), child_handle(move(child_handle_p)) {
		Initialize();
	}
	~GZipFile() override {
		Close();
	}

	void Initialize();
	int64_t ReadData(void *buffer, int64_t nr_bytes);

	GZipFileSystem gzip_fs;
	unique_ptr<FileHandle> child_handle;

protected:
	void Close() override {
		miniz_stream.reset();
		in_buff.reset();
		out_buff.reset();
	}

private:
	idx_t data_start = 0;
	unique_ptr<MiniZStreamWrapper> miniz_stream;
	// various buffers & pointers
	unique_ptr<data_t[]> in_buff;
	unique_ptr<data_t[]> out_buff;
	data_ptr_t out_buff_start = nullptr;
	data_ptr_t out_buff_end = nullptr;
	data_ptr_t in_buff_start = nullptr;
	data_ptr_t in_buff_end = nullptr;
};

void GZipFile::Initialize() {
	Close();

	D_ASSERT(BUFFER_SIZE >= 3); // found to work fine with 3
	uint8_t gzip_hdr[10];
	data_start = GZIP_HEADER_MINSIZE;

	in_buff = unique_ptr<data_t[]>(new data_t[BUFFER_SIZE]);
	in_buff_start = in_buff.get();
	in_buff_end = in_buff.get();
	out_buff = unique_ptr<data_t[]>(new data_t[BUFFER_SIZE]);
	out_buff_start = out_buff.get();
	out_buff_end = out_buff.get();

	miniz_stream = make_unique<MiniZStreamWrapper>();
	miniz_stream->Initialize();

	auto &mz_stream_ptr = miniz_stream->mz_stream_ptr;

	// TODO use custom alloc/free methods in miniz to throw exceptions on OOM
	auto read_count = child_handle->Read(gzip_hdr, GZIP_HEADER_MINSIZE);

	// check for incorrectly formatted files
	// LCOV_EXCL_START
	if (read_count != GZIP_HEADER_MINSIZE) {
		throw Exception("Input is not a GZIP stream");
	}
	if (gzip_hdr[0] != 0x1F || gzip_hdr[1] != 0x8B) { // magic header
		throw Exception("Input is not a GZIP stream");
	}
	if (gzip_hdr[2] != GZIP_COMPRESSION_DEFLATE) { // compression method
		throw Exception("Unsupported GZIP compression method");
	}
	if (gzip_hdr[3] & GZIP_FLAG_UNSUPPORTED) {
		throw Exception("Unsupported GZIP archive");
	}
	// LCOV_EXCL_STOP

	if (gzip_hdr[3] & GZIP_FLAG_NAME) {
		child_handle->Seek(data_start);
		data_start += GZipConsumeString(*child_handle);
	}
	child_handle->Seek(data_start);
	// stream is now set to beginning of payload data
	auto ret = duckdb_miniz::mz_inflateInit2((duckdb_miniz::mz_streamp)mz_stream_ptr, -MZ_DEFAULT_WINDOW_BITS);
	if (ret != duckdb_miniz::MZ_OK) {
		throw InternalException("Failed to initialize miniz");
	}
}

unique_ptr<FileHandle> GZipFileSystem::OpenCompressedFile(unique_ptr<FileHandle> handle) {
	auto path = handle->path;
	return make_unique<GZipFile>(move(handle), path);
}

int64_t GZipFile::ReadData(void *buffer, int64_t remaining) {
	auto &mz_stream_ptr = miniz_stream->mz_stream_ptr;
	idx_t total_read = 0;
	while (true) {
		// first check if there are input bytes available in the output buffers
		if (out_buff_start != out_buff_end) {
			// there is! copy it into the output buffer
			idx_t available = MinValue<idx_t>(remaining, out_buff_end - out_buff_start);
			memcpy(data_ptr_t(buffer) + total_read, out_buff_start, available);

			// increment the total read variables as required
			out_buff_start += available;
			total_read += available;
			remaining -= available;
			if (remaining == 0) {
				// done! read enough
				return total_read;
			}
		}
		if (!mz_stream_ptr) {
			return total_read;
		}

		// ran out of buffer: read more data from the child stream
		out_buff_start = out_buff.get();
		out_buff_end = out_buff.get();
		D_ASSERT(in_buff_start <= in_buff_end);
		D_ASSERT(in_buff_end <= in_buff_start + GZipFile::BUFFER_SIZE);

		// read more input if none available
		if (in_buff_start == in_buff_end) {
			// empty input buffer: refill from the start
			in_buff_start = in_buff.get();
			auto sz = child_handle->Read(in_buff.get(), BUFFER_SIZE);
			if (sz <= 0) {
				break;
			}
			in_buff_end = in_buff_start + sz;
		}

		// actually decompress
		mz_stream_ptr->next_in = (data_ptr_t)in_buff_start;
		D_ASSERT(in_buff_end - in_buff_start < NumericLimits<int32_t>::Maximum());
		mz_stream_ptr->avail_in = (uint32_t)(in_buff_end - in_buff_start);
		mz_stream_ptr->next_out = (data_ptr_t)out_buff_end;
		D_ASSERT((out_buff.get() + BUFFER_SIZE) - out_buff_end < NumericLimits<int32_t>::Maximum());
		mz_stream_ptr->avail_out = (uint32_t)((out_buff.get() + BUFFER_SIZE) - out_buff_end);
		auto ret = duckdb_miniz::mz_inflate(mz_stream_ptr, duckdb_miniz::MZ_NO_FLUSH);
		if (ret != duckdb_miniz::MZ_OK && ret != duckdb_miniz::MZ_STREAM_END) {
			throw Exception(duckdb_miniz::mz_error(ret));
		}
		// update pointers following inflate()
		in_buff_start = (data_ptr_t)mz_stream_ptr->next_in;
		in_buff_end = in_buff_start + mz_stream_ptr->avail_in;
		out_buff_end = (data_ptr_t)mz_stream_ptr->next_out;
		D_ASSERT(out_buff_end + mz_stream_ptr->avail_out == out_buff.get() + BUFFER_SIZE);
		// if stream ended, deallocate inflator
		if (ret == duckdb_miniz::MZ_STREAM_END) {
			miniz_stream->Close();
		}
	}
	return total_read;
}

int64_t GZipFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &gzip_file = (GZipFile &)handle;
	return gzip_file.ReadData(buffer, nr_bytes);
}

// unsupported operations
// LCOV_EXCL_START
void GZipFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw NotImplementedException("Unsupported: Random read in gzip file not supported");
}

void GZipFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes, idx_t location) {
	throw NotImplementedException("Unsupported: Write to gzip file");
}

int64_t GZipFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	throw NotImplementedException("Unsupported: Write to gzip file");
}

void GZipFileSystem::Truncate(FileHandle &handle, int64_t new_size) {
	throw NotImplementedException("Unsupported: Truncate gzip file");
}

void GZipFileSystem::FileSync(FileHandle &handle) {
	throw NotImplementedException("Unsupported: Sync  gzip file");
}

void GZipFileSystem::Seek(FileHandle &handle, idx_t location) {
	throw NotImplementedException("Unsupported: Seek within gzip file");
}
// LCOV_EXCL_STOP

void GZipFileSystem::Reset(FileHandle &handle) {
	auto &gzip_file = (GZipFile &)handle;
	gzip_file.child_handle->Reset();
	gzip_file.Initialize();
}

int64_t GZipFileSystem::GetFileSize(FileHandle &handle) {
	auto &gzip_file = (GZipFile &)handle;
	return gzip_file.child_handle->GetFileSize();
}

bool GZipFileSystem::OnDiskFile(FileHandle &handle) {
	auto &gzip_file = (GZipFile &)handle;
	return gzip_file.child_handle->OnDiskFile();
}

} // namespace duckdb
