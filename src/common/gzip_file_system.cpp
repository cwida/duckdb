#include "duckdb/common/gzip_file_system.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/numeric_utils.hpp"

#include "miniz.hpp"
#include "miniz_wrapper.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/logging/logger.hpp"

namespace duckdb {

namespace {

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

idx_t GZipConsumeString(QueryContext context, FileHandle &input) {
	idx_t size = 1; // terminator
	char buffer[1];
	while (input.Read(context, buffer, 1) == 1) {
		if (buffer[0] == '\0') {
			break;
		}
		size++;
	}
	return size;
}

idx_t GZipReadExact(QueryContext context, FileHandle &input, data_ptr_t buffer, idx_t size) {
	idx_t total_read = 0;
	while (total_read < size) {
		auto read_count = input.Read(context, buffer + total_read, size - total_read);
		if (read_count <= 0) {
			break;
		}
		total_read += NumericCast<idx_t>(read_count);
	}
	return total_read;
}

enum class GZipReadState : uint8_t { DEFLATE, FOOTER, HEADER, EXTRA_LENGTH, EXTRA_DATA, FILE_NAME };

struct MiniZStreamWrapper : public StreamWrapper {
	~MiniZStreamWrapper() override;

	CompressedFile *file = nullptr;
	unique_ptr<duckdb_miniz::mz_stream> mz_stream_ptr;
	bool writing = false;
	duckdb_miniz::mz_ulong crc;
	idx_t total_size;
	GZipReadState read_state = GZipReadState::DEFLATE;
	idx_t state_bytes_remaining = 0;
	idx_t gzip_header_size = 0;
	idx_t gzip_header_bytes = 0;
	uint8_t gzip_header[GZIP_HEADER_MINSIZE];
	idx_t gzip_xlen_bytes = 0;
	uint8_t gzip_xlen[2];

public:
	void Initialize(QueryContext context, CompressedFile &file, bool write) override;

	bool Read(StreamData &stream_data) override;
	void FinalizeRead(StreamData &stream_data) override;
	void Write(CompressedFile &file, StreamData &stream_data, data_ptr_t buffer, int64_t nr_bytes) override;

	void Close() override;
	void AbortWrite() override;

	void FlushStream() const;
	bool ReadNextMemberHeader(StreamData &stream_data);
	void InitializeInflator();
};

MiniZStreamWrapper::~MiniZStreamWrapper() {
	// avoid closing if destroyed during stack unwinding
	if (Exception::UncaughtException()) {
		return;
	}
	try {
		MiniZStreamWrapper::Close();
	} catch (std::exception &ex) {
		if (file && file->child_handle) {
			// FIXME: Make any log context available here.
			ErrorData data(ex);
			try {
				const auto logger = file->child_handle->logger;
				if (logger) {
					DUCKDB_LOG_ERROR(logger, "MiniZStreamWrapper::~MiniZStreamWrapper()\t\t" + data.Message())
				}
			} catch (...) { // NOLINT
			}
		}
	} catch (...) { // NOLINT
	}
}

void MiniZStreamWrapper::Initialize(QueryContext context, CompressedFile &file, bool write) {
	D_ASSERT(mz_stream_ptr == nullptr);
	this->file = &file;
	mz_stream_ptr = make_uniq<duckdb_miniz::mz_stream>();
	memset(mz_stream_ptr.get(), 0, sizeof(duckdb_miniz::mz_stream));
	this->writing = write;

	// TODO use custom alloc/free methods in miniz to throw exceptions on OOM
	uint8_t gzip_hdr[GZIP_HEADER_MINSIZE];
	if (write) {
		crc = MZ_CRC32_INIT;
		total_size = 0;

		MiniZStream::InitializeGZIPHeader(gzip_hdr);
		file.child_handle->Write(context, gzip_hdr, GZIP_HEADER_MINSIZE);

		auto ret = mz_deflateInit2(mz_stream_ptr.get(), duckdb_miniz::MZ_DEFAULT_LEVEL, MZ_DEFLATED,
		                           -MZ_DEFAULT_WINDOW_BITS, 1, 0);
		if (ret != duckdb_miniz::MZ_OK) {
			throw InternalException("Failed to initialize miniz");
		}
	} else {
		idx_t data_start = GZIP_HEADER_MINSIZE;
		auto read_count = GZipReadExact(context, *file.child_handle, gzip_hdr, GZIP_HEADER_MINSIZE);
		GZipFileSystem::VerifyGZIPHeader(gzip_hdr, read_count, &file);
		// Skip over the extra field if necessary
		if (gzip_hdr[3] & GZIP_FLAG_EXTRA) {
			uint8_t gzip_xlen[2];
			file.child_handle->Seek(data_start);
			if (GZipReadExact(context, *file.child_handle, gzip_xlen, 2) != 2) {
				throw IOException("Input is not a GZIP stream: %s", file.path);
			}
			auto xlen = NumericCast<idx_t>((uint8_t)gzip_xlen[0] | (uint8_t)gzip_xlen[1] << 8);
			data_start += xlen + 2;
		}
		// Skip over the file name if necessary
		if (gzip_hdr[3] & GZIP_FLAG_NAME) {
			file.child_handle->Seek(data_start);
			data_start += GZipConsumeString(context, *file.child_handle);
		}
		file.child_handle->Seek(data_start);
		file.AddConsumedBytes(data_start);
		// stream is now set to beginning of payload data
		InitializeInflator();
	}
}

void MiniZStreamWrapper::InitializeInflator() {
	auto ret = duckdb_miniz::mz_inflateInit2(mz_stream_ptr.get(), -MZ_DEFAULT_WINDOW_BITS);
	if (ret != duckdb_miniz::MZ_OK) {
		throw InternalException("Failed to initialize miniz");
	}
}

bool MiniZStreamWrapper::ReadNextMemberHeader(StreamData &sd) {
	while (sd.in_buff_start < sd.in_buff_end) {
		auto available = NumericCast<idx_t>(sd.in_buff_end - sd.in_buff_start);
		switch (read_state) {
		case GZipReadState::FOOTER: {
			auto consume_count = MinValue<idx_t>(available, state_bytes_remaining);
			sd.in_buff_start += consume_count;
			state_bytes_remaining -= consume_count;
			if (state_bytes_remaining > 0) {
				return false;
			}
			read_state = GZipReadState::HEADER;
			gzip_header_size = GZIP_FOOTER_SIZE;
			gzip_header_bytes = 0;
			break;
		}
		case GZipReadState::HEADER: {
			auto consume_count = MinValue<idx_t>(available, GZIP_HEADER_MINSIZE - gzip_header_bytes);
			memcpy(gzip_header + gzip_header_bytes, sd.in_buff_start, consume_count);
			sd.in_buff_start += consume_count;
			gzip_header_bytes += consume_count;
			gzip_header_size += consume_count;
			if (gzip_header_bytes < GZIP_HEADER_MINSIZE) {
				return false;
			}
			GZipFileSystem::VerifyGZIPHeader(gzip_header, GZIP_HEADER_MINSIZE, nullptr);
			if (gzip_header[3] & GZIP_FLAG_EXTRA) {
				read_state = GZipReadState::EXTRA_LENGTH;
				gzip_xlen_bytes = 0;
			} else if (gzip_header[3] & GZIP_FLAG_NAME) {
				read_state = GZipReadState::FILE_NAME;
			} else {
				read_state = GZipReadState::DEFLATE;
				return true;
			}
			break;
		}
		case GZipReadState::EXTRA_LENGTH: {
			auto consume_count = MinValue<idx_t>(available, 2 - gzip_xlen_bytes);
			memcpy(gzip_xlen + gzip_xlen_bytes, sd.in_buff_start, consume_count);
			sd.in_buff_start += consume_count;
			gzip_xlen_bytes += consume_count;
			gzip_header_size += consume_count;
			if (gzip_xlen_bytes < 2) {
				return false;
			}
			state_bytes_remaining = NumericCast<idx_t>((uint8_t)gzip_xlen[0] | (uint8_t)gzip_xlen[1] << 8);
			if (gzip_header_size + state_bytes_remaining >= GZIP_HEADER_MAXSIZE) {
				throw DataCorruptionException("Extra field resulting in GZIP header larger than defined maximum (%d)",
				                              GZIP_HEADER_MAXSIZE);
			}
			read_state = GZipReadState::EXTRA_DATA;
			break;
		}
		case GZipReadState::EXTRA_DATA: {
			auto consume_count = MinValue<idx_t>(available, state_bytes_remaining);
			sd.in_buff_start += consume_count;
			state_bytes_remaining -= consume_count;
			gzip_header_size += consume_count;
			if (state_bytes_remaining > 0) {
				return false;
			}
			if (gzip_header[3] & GZIP_FLAG_NAME) {
				read_state = GZipReadState::FILE_NAME;
			} else {
				read_state = GZipReadState::DEFLATE;
				return true;
			}
			break;
		}
		case GZipReadState::FILE_NAME:
			while (sd.in_buff_start < sd.in_buff_end) {
				auto c = *sd.in_buff_start++;
				gzip_header_size++;
				if (gzip_header_size >= GZIP_HEADER_MAXSIZE) {
					throw DataCorruptionException("Filename resulting in GZIP header larger than defined maximum (%d)",
					                              GZIP_HEADER_MAXSIZE);
				}
				if (c == '\0') {
					read_state = GZipReadState::DEFLATE;
					return true;
				}
			}
			return false;
		case GZipReadState::DEFLATE:
			return true;
		}
	}
	return false;
}

bool MiniZStreamWrapper::Read(StreamData &sd) {
	// Handling for the concatenated files
	if (sd.refresh && read_state == GZipReadState::DEFLATE) {
		read_state = GZipReadState::FOOTER;
		state_bytes_remaining = GZIP_FOOTER_SIZE;
	}
	if (read_state != GZipReadState::DEFLATE) {
		if (!ReadNextMemberHeader(sd)) {
			return false;
		}
		sd.refresh = false;
		duckdb_miniz::mz_inflateEnd(mz_stream_ptr.get());
		InitializeInflator();
	}
	if (sd.in_buff_start == sd.in_buff_end) {
		return false;
	}

	// actually decompress
	mz_stream_ptr->next_in = sd.in_buff_start;
	D_ASSERT(sd.in_buff_end - sd.in_buff_start < NumericLimits<int32_t>::Maximum());
	mz_stream_ptr->avail_in = static_cast<uint32_t>(sd.in_buff_end - sd.in_buff_start);
	mz_stream_ptr->next_out = data_ptr_cast(sd.out_buff_end);
	mz_stream_ptr->avail_out = static_cast<uint32_t>((sd.out_buff.get() + sd.out_buf_size) - sd.out_buff_end);
	auto ret = duckdb_miniz::mz_inflate(mz_stream_ptr.get(), duckdb_miniz::MZ_NO_FLUSH);
	if (ret != duckdb_miniz::MZ_OK && ret != duckdb_miniz::MZ_STREAM_END) {
		throw IOException("Failed to decode gzip stream: %s", duckdb_miniz::mz_error(ret));
	}
	// update pointers following inflate()
	sd.in_buff_start = (data_ptr_t)mz_stream_ptr->next_in; // NOLINT
	sd.in_buff_end = sd.in_buff_start + mz_stream_ptr->avail_in;
	sd.out_buff_end = data_ptr_cast(mz_stream_ptr->next_out);
	D_ASSERT(sd.out_buff_end + mz_stream_ptr->avail_out == sd.out_buff.get() + sd.out_buf_size);

	// if stream ended, deallocate inflator
	if (ret == duckdb_miniz::MZ_STREAM_END) {
		// Concatenated GZIP potentially coming up - refresh input buffer
		sd.refresh = true;
	}
	return false;
}

void MiniZStreamWrapper::FinalizeRead(StreamData &) {
	if (read_state == GZipReadState::HEADER && gzip_header_bytes == 0) {
		return;
	}
	throw IOException("Unexpected end of GZIP stream: %s", file->path);
}

void MiniZStreamWrapper::Write(CompressedFile &file, StreamData &sd, data_ptr_t uncompressed_data,
                               int64_t uncompressed_size) {
	// update the src and the total size
	crc = duckdb_miniz::mz_crc32(crc, reinterpret_cast<const unsigned char *>(uncompressed_data),
	                             UnsafeNumericCast<size_t>(uncompressed_size));
	total_size += UnsafeNumericCast<idx_t>(uncompressed_size);

	auto remaining = uncompressed_size;
	while (remaining > 0) {
		auto output_remaining = UnsafeNumericCast<idx_t>((sd.out_buff.get() + sd.out_buf_size) - sd.out_buff_start);

		// miniz's avail_in is a platform-dependent unsigned int, cap ingestion bytes to avoid overflow.
		auto avail_in = MinValue<int64_t>(remaining, NumericLimits<unsigned int>::Maximum());

		mz_stream_ptr->next_in = reinterpret_cast<const unsigned char *>(uncompressed_data);
		mz_stream_ptr->avail_in = NumericCast<unsigned int>(avail_in);
		mz_stream_ptr->next_out = sd.out_buff_start;
		mz_stream_ptr->avail_out = NumericCast<unsigned int>(output_remaining);

		auto res = mz_deflate(mz_stream_ptr.get(), duckdb_miniz::MZ_NO_FLUSH);
		if (res != duckdb_miniz::MZ_OK) {
			D_ASSERT(res != duckdb_miniz::MZ_STREAM_END);
			throw InternalException("Failed to compress GZIP block");
		}
		sd.out_buff_start += output_remaining - mz_stream_ptr->avail_out;
		if (mz_stream_ptr->avail_out == 0) {
			// no more output buffer available: flush
			file.child_handle->Write(file.context, sd.out_buff.get(),
			                         UnsafeNumericCast<idx_t>(sd.out_buff_start - sd.out_buff.get()));
			sd.out_buff_start = sd.out_buff.get();
		}
		auto written = NumericCast<idx_t>(avail_in - mz_stream_ptr->avail_in);
		uncompressed_data += written;
		remaining -= NumericCast<int64_t>(written);
	}
}

void MiniZStreamWrapper::FlushStream() const {
	auto &sd = file->stream_data;
	mz_stream_ptr->next_in = nullptr;
	mz_stream_ptr->avail_in = 0;
	while (true) {
		auto output_remaining = (sd.out_buff.get() + sd.out_buf_size) - sd.out_buff_start;
		mz_stream_ptr->next_out = sd.out_buff_start;
		mz_stream_ptr->avail_out = NumericCast<unsigned int>(output_remaining);

		auto res = mz_deflate(mz_stream_ptr.get(), duckdb_miniz::MZ_FINISH);
		sd.out_buff_start += (output_remaining - mz_stream_ptr->avail_out);
		if (sd.out_buff_start > sd.out_buff.get()) {
			file->child_handle->Write(file->context, sd.out_buff.get(),
			                          UnsafeNumericCast<idx_t>(sd.out_buff_start - sd.out_buff.get()));
			sd.out_buff_start = sd.out_buff.get();
		}
		if (res == duckdb_miniz::MZ_STREAM_END) {
			break;
		}
		if (res != duckdb_miniz::MZ_OK) {
			throw InternalException("Failed to compress GZIP block");
		}
	}
}

void MiniZStreamWrapper::Close() {
	if (!mz_stream_ptr) {
		return;
	}
	if (writing) {
		// flush anything remaining in the stream
		FlushStream();

		// write the footer
		unsigned char gzip_footer[MiniZStream::GZIP_FOOTER_SIZE];
		MiniZStream::InitializeGZIPFooter(gzip_footer, crc, total_size);
		file->child_handle->Write(file->context, gzip_footer, MiniZStream::GZIP_FOOTER_SIZE);
	}
	AbortWrite();
}

void MiniZStreamWrapper::AbortWrite() {
	if (!mz_stream_ptr) {
		return;
	}
	if (writing) {
		duckdb_miniz::mz_deflateEnd(mz_stream_ptr.get());
	} else {
		duckdb_miniz::mz_inflateEnd(mz_stream_ptr.get());
	}
	mz_stream_ptr = nullptr;
	file = nullptr;
}

struct GZipFileSystemHolder {
	GZipFileSystem gzip_fs;
};

class GZipFile : private GZipFileSystemHolder, public CompressedFile {
public:
	GZipFile(QueryContext context, unique_ptr<FileHandle> child_handle_p, const string &path, bool write)
	    : CompressedFile(gzip_fs, std::move(child_handle_p), path) {
		Initialize(context, write);
	}
	FileCompressionType GetFileCompressionType() override {
		return FileCompressionType::GZIP;
	}
};

} // namespace

void GZipFileSystem::VerifyGZIPHeader(uint8_t gzip_hdr[], idx_t read_count, optional_ptr<CompressedFile> source_file) {
	// include the filename in the error message if known
	string file_info = source_file ? ": " + source_file->path : "";

	// check for incorrectly formatted files
	if (read_count != GZIP_HEADER_MINSIZE) {
		throw IOException("Input is not a GZIP stream" + file_info);
	}
	if (gzip_hdr[0] != 0x1F || gzip_hdr[1] != 0x8B) { // magic header
		throw IOException("Input is not a GZIP stream" + file_info);
	}
	if (gzip_hdr[2] != GZIP_COMPRESSION_DEFLATE) { // compression method
		throw IOException("Unsupported GZIP compression method" + file_info);
	}
	if (gzip_hdr[3] & GZIP_FLAG_UNSUPPORTED) {
		throw IOException("Unsupported GZIP archive" + file_info);
	}
}

bool GZipFileSystem::CheckIsZip(const char *data, duckdb::idx_t size) {
	if (size < GZIP_HEADER_MINSIZE) {
		return false;
	}

	auto data_ptr = reinterpret_cast<const uint8_t *>(data);
	if (data_ptr[0] != 0x1F || data_ptr[1] != 0x8B) {
		return false;
	}

	if (data_ptr[2] != GZIP_COMPRESSION_DEFLATE) {
		return false;
	}

	return true;
}

string GZipFileSystem::UncompressGZIPString(const string &in) {
	return UncompressGZIPString(in.data(), in.size());
}

string GZipFileSystem::UncompressGZIPString(const char *data, idx_t size) {
	// decompress file
	auto body_ptr = data;

	auto mz_stream_ptr = make_uniq<duckdb_miniz::mz_stream>();
	memset(mz_stream_ptr.get(), 0, sizeof(duckdb_miniz::mz_stream));

	uint8_t gzip_hdr[GZIP_HEADER_MINSIZE];

	// check for incorrectly formatted files

	// TODO this is mostly the same as gzip_file_system.cpp
	if (size < GZIP_HEADER_MINSIZE) {
		throw IOException("Input is not a GZIP stream");
	}
	memcpy(gzip_hdr, body_ptr, GZIP_HEADER_MINSIZE);
	body_ptr += GZIP_HEADER_MINSIZE;
	GZipFileSystem::VerifyGZIPHeader(gzip_hdr, GZIP_HEADER_MINSIZE, nullptr);

	if (gzip_hdr[3] & GZIP_FLAG_EXTRA) {
		throw IOException("Extra field in a GZIP stream unsupported");
	}

	if (gzip_hdr[3] & GZIP_FLAG_NAME) {
		auto end_ptr = data + size;
		while (body_ptr < end_ptr) {
			if (*body_ptr++ == '\0') {
				break;
			}
		}
	}

	// stream is now set to beginning of payload data
	auto status = duckdb_miniz::mz_inflateInit2(mz_stream_ptr.get(), -MZ_DEFAULT_WINDOW_BITS);
	if (status != duckdb_miniz::MZ_OK) {
		throw InternalException("Failed to initialize miniz");
	}

	auto bytes_remaining = size - NumericCast<idx_t>(body_ptr - data);
	mz_stream_ptr->next_in = const_uchar_ptr_cast(body_ptr);
	mz_stream_ptr->avail_in = NumericCast<unsigned int>(bytes_remaining);

	string decompressed;

	while (status == duckdb_miniz::MZ_OK) {
		unsigned char decompress_buffer[BUFSIZ];
		mz_stream_ptr->next_out = decompress_buffer;
		mz_stream_ptr->avail_out = sizeof(decompress_buffer);
		status = mz_inflate(mz_stream_ptr.get(), duckdb_miniz::MZ_NO_FLUSH);
		if (status != duckdb_miniz::MZ_STREAM_END && status != duckdb_miniz::MZ_OK) {
			duckdb_miniz::mz_inflateEnd(mz_stream_ptr.get());
			throw IOException("Failed to uncompress");
		}
		decompressed.append(char_ptr_cast(decompress_buffer), mz_stream_ptr->total_out - decompressed.size());
	}
	duckdb_miniz::mz_inflateEnd(mz_stream_ptr.get());

	if (decompressed.empty()) {
		throw IOException("Failed to uncompress");
	}
	return decompressed;
}

unique_ptr<FileHandle> GZipFileSystem::OpenCompressedFile(QueryContext context, unique_ptr<FileHandle> handle,
                                                          bool write) {
	try {
		auto path = handle->path;
		return make_uniq<GZipFile>(context, std::move(handle), path, write);
	} catch (...) {
		auto error = std::current_exception();
		if (handle) {
			try {
				handle->AbortWrite();
			} catch (...) { // NOLINT
			}
		}
		std::rethrow_exception(error);
	}
}

unique_ptr<StreamWrapper> GZipFileSystem::CreateStream() {
	return make_uniq<MiniZStreamWrapper>();
}

idx_t GZipFileSystem::InBufferSize() {
	return BUFFER_SIZE;
}

idx_t GZipFileSystem::OutBufferSize() {
	return BUFFER_SIZE;
}

} // namespace duckdb
