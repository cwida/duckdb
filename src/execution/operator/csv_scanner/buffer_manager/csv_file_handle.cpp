#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"

namespace duckdb {

CSVFileHandle::CSVFileHandle(ClientContext &context_p, unique_ptr<FileHandle> file_handle_p, const OpenFileInfo &file_p,
                             const CSVReaderOptions &options)
    : compression_type(options.compression), context(context_p), file_handle(std::move(file_handle_p)),
      encoder(context_p, options.encoding, options.buffer_size_option.GetValue()), file(file_p) {
	can_seek = file_handle->CanSeek();
	on_disk_file = file_handle->OnDiskFile();
	file_size = file_handle->GetFileSize();
	is_pipe = file_handle->IsPipe();
	compression_type = file_handle->GetFileCompressionType();
}

unique_ptr<FileHandle> CSVFileHandle::OpenFileHandle(FileSystem &fs, Allocator &allocator, const OpenFileInfo &file,
                                                     FileCompressionType compression) {
	FileOpenFlags flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_PARALLEL_ACCESS | std::move(compression);
	flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
	auto file_handle = fs.OpenFile(file, flags);
	if (file_handle->CanSeek()) {
		file_handle->Reset();
	}
	return file_handle;
}

unique_ptr<CSVFileHandle> CSVFileHandle::OpenFile(ClientContext &context, const OpenFileInfo &file,
                                                  const CSVReaderOptions &options) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto &allocator = BufferAllocator::Get(context);
	auto file_handle = OpenFileHandle(fs, allocator, file, options.compression);
	return make_uniq<CSVFileHandle>(context, std::move(file_handle), file, options);
}

double CSVFileHandle::GetProgress() const {
	return static_cast<double>(file_handle->GetProgress());
}

bool CSVFileHandle::CanSeek() const {
	return can_seek;
}

void CSVFileHandle::Seek(const idx_t position) const {
	if (!can_seek) {
		if (is_pipe) {
			throw InternalException("Trying to seek a piped CSV File.");
		}
		throw InternalException("Trying to seek a compressed CSV File.");
	}
	file_handle->Seek(position);
}

bool CSVFileHandle::OnDiskFile() const {
	return on_disk_file;
}

void CSVFileHandle::Reset() {
	file_handle->Reset();
	encoder.Reset();
	finished = false;
	requested_bytes = 0;
}

bool CSVFileHandle::IsPipe() const {
	return is_pipe;
}

idx_t CSVFileHandle::FileSize() const {
	return file_size;
}

bool CSVFileHandle::HasKnownBufferRanges() const {
	return compression_type == FileCompressionType::UNCOMPRESSED && can_seek && !is_pipe &&
	       encoder.encoding_name == "utf-8";
}

bool CSVFileHandle::FinishedReading() const {
	return finished;
}

idx_t CSVFileHandle::Read(void *buffer, idx_t nr_bytes) {
	// We avoid reading past the original size of the file for uncompressed files in utf-8 encoding. This avoids reading
	// the data that is written after opening the file. This can be useful, for example when reading a duckdb log file
	// in csv format while logging is enabled
	if (file_handle->GetFileCompressionType() == FileCompressionType::UNCOMPRESSED && file_handle->CanSeek() &&
	    encoder.encoding_name == "utf-8") {
		nr_bytes = MinValue<idx_t>(nr_bytes, file_size - file_handle->SeekPosition());
	}

	requested_bytes += nr_bytes;
	// if this is a plain file source OR we can seek we are not caching anything
	idx_t bytes_read = 0;
	if (encoder.encoding_name == "utf-8") {
		bytes_read = static_cast<idx_t>(file_handle->Read(context, buffer, nr_bytes));
	} else {
		bytes_read = encoder.Encode(*file_handle, static_cast<char *>(buffer), nr_bytes);
	}
	if (!finished) {
		finished = bytes_read == 0;
	}
	uncompressed_bytes_read.fetch_add(static_cast<idx_t>(bytes_read), std::memory_order_relaxed);
	return UnsafeNumericCast<idx_t>(bytes_read);
}

void CSVFileHandle::ReadAt(void *buffer, idx_t nr_bytes, idx_t position) {
	D_ASSERT(HasKnownBufferRanges());
	D_ASSERT(position + nr_bytes <= file_size);
	file_handle->Read(context, buffer, nr_bytes, position);
}

string CSVFileHandle::ReadLine() {
	bool carriage_return = false;
	string result;
	char buffer[1];
	while (true) {
		idx_t bytes_read = Read(buffer, 1);
		if (bytes_read == 0) {
			return result;
		}
		if (carriage_return) {
			if (buffer[0] != '\n') {
				if (!file_handle->CanSeek()) {
					throw BinderException(
					    "Carriage return newlines not supported when reading CSV files in which we cannot seek");
				}
				file_handle->Seek(file_handle->SeekPosition() - 1);
				return result;
			}
		}
		if (buffer[0] == '\n') {
			return result;
		}
		if (buffer[0] != '\r') {
			result += buffer[0];
		} else {
			carriage_return = true;
		}
	}
}

string CSVFileHandle::GetFilePath() {
	return file.path;
}

} // namespace duckdb
