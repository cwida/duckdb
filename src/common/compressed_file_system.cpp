#include "duckdb/common/compressed_file_system.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

//! Compressed data of remote files is read in chunks of this size, so they take few requests
static constexpr idx_t REMOTE_READ_IN_BUF_SIZE = 16777216;

StreamWrapper::~StreamWrapper() {
}

void StreamWrapper::FinalizeRead(StreamData &) {
}

void StreamWrapper::AbortWrite() {
	Close();
}

bool CompressedFileSystem::CanHandleFile(const string &fpath) {
	return false;
}

CompressedFile::CompressedFile(CompressedFileSystem &fs, unique_ptr<FileHandle> child_handle_p, const string &path) try
    : FileHandle(fs, path, child_handle_p->GetFlags()), compressed_fs(fs), child_handle(std::move(child_handle_p)) {
	// The real on-disk I/O happens on the (compressed) child handle; attribute the bytes there instead of
	// double-counting the uncompressed bytes that pass through this wrapper handle.
	track_io = false;

} catch (...) {
	auto error = std::current_exception();
	if (child_handle_p) {
		try {
			child_handle_p->AbortWrite();
		} catch (...) { // NOLINT
		}
	}
	std::rethrow_exception(error);
}

CompressedFile::~CompressedFile() {
	try {
		if (write && !initialized) {
			AbortCompressedWrite();
		} else {
			Close();
		}
	} catch (std::exception &ex) {
		if (child_handle) {
			// FIXME: Make any log context available here.
			ErrorData data(ex);
			try {
				const auto logger = child_handle->logger;
				if (logger) {
					DUCKDB_LOG_ERROR(logger, "CompressedFile::~CompressedFile()\t\t" + data.Message());
				}
			} catch (...) { // NOLINT
			}
		}
	} catch (...) { // NOLINT
	}
}

void CompressedFile::Initialize(QueryContext context, bool write) {
	Clear();
	initialized = false;

	this->context = context;
	this->write = write;
	stream_data.in_buf_size = compressed_fs.InBufferSize();
	if (!write && FileSystem::IsRemoteFile(child_handle->path)) {
		stream_data.in_buf_size = MaxValue<idx_t>(
		    stream_data.in_buf_size, MinValue<idx_t>(REMOTE_READ_IN_BUF_SIZE, child_handle->GetFileSize()));
	}
	stream_data.out_buf_size = compressed_fs.OutBufferSize();
	auto &allocator =
	    context.Valid() ? BufferAllocator::Get(*context.GetClientContext()) : Allocator::DefaultAllocator();
	stream_data.in_buff = allocator.Allocate(stream_data.in_buf_size);
	stream_data.in_buff_start = stream_data.in_buff.get();
	stream_data.in_buff_end = stream_data.in_buff.get();
	stream_data.out_buff = allocator.Allocate(stream_data.out_buf_size);
	stream_data.out_buff_start = stream_data.out_buff.get();
	stream_data.out_buff_end = stream_data.out_buff.get();

	compressed_bytes_read = 0;
	compressed_bytes_consumed.store(0, std::memory_order_relaxed);

	stream_wrapper = compressed_fs.CreateStream();
	stream_wrapper->Initialize(context, *this, write);
	initialized = true;
}

void CompressedFile::AddConsumedBytes(idx_t bytes) {
	compressed_bytes_read += bytes;
	compressed_bytes_consumed.fetch_add(bytes, std::memory_order_relaxed);
}

idx_t CompressedFile::GetProgress() {
	return compressed_bytes_consumed.load(std::memory_order_relaxed);
}

int64_t CompressedFile::ReadData(void *buffer, int64_t remaining) {
	idx_t total_read = 0;
	while (true) {
		// first check if there are input bytes available in the output buffers
		if (stream_data.out_buff_start != stream_data.out_buff_end) {
			// there is! copy it into the output buffer
			auto available =
			    MinValue<idx_t>(UnsafeNumericCast<idx_t>(remaining),
			                    UnsafeNumericCast<idx_t>(stream_data.out_buff_end - stream_data.out_buff_start));
			memcpy(static_cast<data_ptr_t>(buffer) + total_read, stream_data.out_buff_start, available);

			// increment the total read variables as required
			stream_data.out_buff_start += available;
			total_read += available;
			remaining = UnsafeNumericCast<int64_t>(UnsafeNumericCast<idx_t>(remaining) - available);
			if (remaining == 0) {
				// done! read enough
				return UnsafeNumericCast<int64_t>(total_read);
			}
		}
		if (!stream_wrapper) {
			return UnsafeNumericCast<int64_t>(total_read);
		}
		// ran out of buffer: read more data from the child stream
		stream_data.out_buff_start = stream_data.out_buff.get();
		stream_data.out_buff_end = stream_data.out_buff.get();
		D_ASSERT(stream_data.in_buff_start <= stream_data.in_buff_end);
		D_ASSERT(stream_data.in_buff_end <= stream_data.in_buff_start + stream_data.in_buf_size);

		// read more input if none available
		if (stream_data.in_buff_start == stream_data.in_buff_end) {
			// empty input buffer: refill from the start
			stream_data.in_buff_start = stream_data.in_buff.get();
			stream_data.in_buff_end = stream_data.in_buff_start;
			auto sz = child_handle->Read(context, stream_data.in_buff.get(), stream_data.in_buf_size);
			if (sz <= 0) {
				stream_wrapper->FinalizeRead(stream_data);
				stream_wrapper.reset();
				compressed_bytes_consumed.store(compressed_bytes_read, std::memory_order_relaxed);
				break;
			} else {
				stream_data.in_buff_end = stream_data.in_buff_start + sz;
				compressed_bytes_read += UnsafeNumericCast<idx_t>(sz);
			}
		}

		auto finished = stream_wrapper->Read(stream_data);
		// the input that remains in the buffer has not been consumed by the decompressor yet
		auto unconsumed = static_cast<idx_t>(stream_data.in_buff_end - stream_data.in_buff_start);
		compressed_bytes_consumed.store(compressed_bytes_read - MinValue(unconsumed, compressed_bytes_read),
		                                std::memory_order_relaxed);
		if (finished) {
			stream_wrapper.reset();
		}
	}
	return UnsafeNumericCast<int64_t>(total_read);
}

int64_t CompressedFile::WriteData(data_ptr_t buffer, int64_t nr_bytes) {
	stream_wrapper->Write(*this, stream_data, buffer, nr_bytes);
	return nr_bytes;
}

// Clear does most of the heavy lifting of a close, but leaves the child_handle intact. Specifically it is separated out
// to support upstream Reset() calls from the FileSystem, which should tear down / reset ephemeral state but leave
// persistent state (child_handle) intact.
void CompressedFile::Clear() {
	if (stream_wrapper) {
		stream_wrapper->Close();
		stream_wrapper.reset();
	}
	ResetStreamData();
}

void CompressedFile::ResetStreamData() {
	stream_data.in_buff.Reset();
	stream_data.out_buff.Reset();
	stream_data.out_buff_start = nullptr;
	stream_data.out_buff_end = nullptr;
	stream_data.in_buff_start = nullptr;
	stream_data.in_buff_end = nullptr;
	stream_data.in_buf_size = 0;
	stream_data.out_buf_size = 0;
	stream_data.refresh = false;
}

void CompressedFile::Close() {
	try {
		Clear();
	} catch (...) {
		auto error = std::current_exception();
		try {
			AbortCompressedWrite();
		} catch (...) { // NOLINT
		}
		std::rethrow_exception(error);
	}

	auto child = std::move(child_handle);
	if (child) {
		child->Close();
	}
}

void CompressedFile::AbortCompressedWrite() {
	initialized = false;
	std::exception_ptr error;
	auto wrapper = std::move(stream_wrapper);
	if (wrapper) {
		try {
			wrapper->AbortWrite();
		} catch (...) {
			error = std::current_exception();
		}
	}
	ResetStreamData();
	auto child = std::move(child_handle);
	if (child) {
		try {
			child->AbortWrite();
		} catch (...) {
			if (!error) {
				error = std::current_exception();
			}
		}
	}
	if (error) {
		std::rethrow_exception(error);
	}
}

int64_t CompressedFileSystem::Read(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.ReadData(buffer, nr_bytes);
}

int64_t CompressedFileSystem::Write(FileHandle &handle, void *buffer, int64_t nr_bytes) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.WriteData(data_ptr_cast(buffer), nr_bytes);
}

void CompressedFileSystem::Reset(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	compressed_file.child_handle->Reset();
	// Preserve the query context across a reset so re-reads (e.g. the scan after the CSV sniffer) stay attributed.
	compressed_file.Initialize(compressed_file.context, compressed_file.write);
}

int64_t CompressedFileSystem::GetFileSize(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return NumericCast<int64_t>(compressed_file.child_handle->GetFileSize());
}

bool CompressedFileSystem::OnDiskFile(FileHandle &handle) {
	auto &compressed_file = handle.Cast<CompressedFile>();
	return compressed_file.child_handle->OnDiskFile();
}

bool CompressedFileSystem::CanSeek() {
	return false;
}

void CompressedFileSystem::AbortFileWrite(FileHandle &handle) {
	handle.Cast<CompressedFile>().AbortCompressedWrite();
}

} // namespace duckdb
