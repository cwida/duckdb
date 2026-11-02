#include "duckdb/common/serializer/async_file_writer.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <cstring>

namespace duckdb {

const FileOpenFlags AsyncFileWriter::DEFAULT_OPEN_FLAGS =
    FileOpenFlags(FileOpenFlags::FILE_FLAGS_WRITE | FileOpenFlags::FILE_FLAGS_FILE_CREATE);

class CopiedAsyncWriteBuffer : public AsyncWriteBuffer {
public:
	CopiedAsyncWriteBuffer(ClientContext &context, idx_t capacity_p)
	    : data(BufferAllocator::Get(context).Allocate(capacity_p)), capacity(capacity_p) {
	}

	data_ptr_t Ptr() override {
		return data.get();
	}

	idx_t Size() const override {
		return size;
	}

	idx_t Remaining() const {
		return capacity - size;
	}

	void Append(const_data_ptr_t buffer, idx_t append_size) {
		D_ASSERT(append_size <= Remaining());
		memcpy(data.get() + size, buffer, append_size);
		size += append_size;
	}

private:
	AllocatedData data;
	idx_t capacity;
	idx_t size = 0;
};

static ClientContext &RequireClientContext(QueryContext context) {
	auto client_context = context.GetClientContext();
	if (!client_context) {
		throw InvalidInputException("AsyncFileWriter requires a ClientContext");
	}
	return *client_context;
}

AsyncFileWriter::AsyncFileWriter(QueryContext context_p, FileSystem &fs_p, const string &path_p,
                                 const FileOpenFlags &open_flags)
    : context(context_p), client_context(RequireClientContext(context_p)), fs(fs_p), path(path_p) {
	handle = fs.OpenFile(path, open_flags | FileLockType::WRITE_LOCK);

	try {
		ManagedAsyncWriteStreamTarget &target = *this;
		write_queue = make_uniq<ManagedAsyncWriteStreamQueue>(client_context, target);
	} catch (...) {
		auto error = std::current_exception();
		try {
			handle->AbortWrite();
		} catch (...) { // NOLINT
		}
		handle.reset();
		std::rethrow_exception(error);
	}
}

AsyncFileWriter::~AsyncFileWriter() {
	if (!finished && handle) {
		try {
			AbortWrite();
		} catch (...) {
		}
	}
}

AsyncFileWriter::BatchGuard::BatchGuard(AsyncFileWriter &writer_p) : writer(writer_p) {
	writer->BeginBatch();
}

AsyncFileWriter::BatchGuard::BatchGuard(BatchGuard &&other) noexcept : writer(other.writer) {
	other.writer = nullptr;
}

AsyncFileWriter::BatchGuard::~BatchGuard() {
	// We would call Finish() here, but that can throw, instead we assert it has been called.
	D_ASSERT(Exception::UncaughtException() || !writer);
	if (writer) {
		writer->LeaveBatch();
	}
}

void AsyncFileWriter::BatchGuard::Finish() {
	if (!writer) {
		return;
	}
	auto &writer_ref = *writer;
	writer = nullptr;
	auto apply_backpressure = !writer_ref.finished;
	writer_ref.LeaveBatch();
	if (apply_backpressure) {
		writer_ref.ApplyBackpressure();
	}
}

idx_t AsyncFileWriter::GetFileSize() {
	return GetTotalWritten();
}

idx_t AsyncFileWriter::GetPhysicalFileSize() {
	WaitAll();
	return handle->GetFileSize();
}

FileCompressionType AsyncFileWriter::GetFileCompressionType() {
	return handle->GetFileCompressionType();
}

idx_t AsyncFileWriter::GetTotalWritten() const {
	return total_written;
}

void AsyncFileWriter::WriteData(const_data_ptr_t buffer, idx_t write_size) {
	RethrowError();
	try {
		WriteDataInternal(buffer, write_size);
	} catch (...) {
		first_error = std::current_exception();
		std::rethrow_exception(first_error);
	}
}

void AsyncFileWriter::WriteDataInternal(const_data_ptr_t buffer, idx_t write_size) {
	if (write_size == 0) {
		return;
	}
	RethrowTaskError();
	if (finished) {
		throw IOException("Cannot write to closed file \"%s\"", path);
	}

	// Caller-owned memory cannot outlive async scheduling, so even large const inputs are copied before registration.
	if (write_size >= AsyncWriteConfig::COPIED_BUFFER_CAPACITY) {
		SealCopiedBuffer(ScheduleMode::DEFER);
		auto owned_buffer = make_uniq<CopiedAsyncWriteBuffer>(client_context, write_size);
		owned_buffer->Append(buffer, write_size);
		RegisterWrite(std::move(owned_buffer));
		return;
	}

	idx_t offset = 0;
	while (offset < write_size) {
		unique_ptr<CopiedAsyncWriteBuffer> sealed_buffer;
		idx_t sealed_buffer_offset = 0;
		if (!copied_buffer) {
			copied_buffer_offset = total_written;
			copied_buffer = make_uniq<CopiedAsyncWriteBuffer>(client_context, AsyncWriteConfig::COPIED_BUFFER_CAPACITY);
		}
		auto append_size = MinValue(write_size - offset, copied_buffer->Remaining());
		copied_buffer->Append(buffer + offset, append_size);
		total_written += append_size;
		offset += append_size;
		if (copied_buffer->Remaining() == 0) {
			sealed_buffer_offset = copied_buffer_offset;
			sealed_buffer = std::move(copied_buffer);
		}
		if (sealed_buffer) {
			RegisterStagedWrite(std::move(sealed_buffer), sealed_buffer_offset);
		}
	}
}

void AsyncFileWriter::WriteData(unique_ptr<AsyncWriteBuffer> buffer) {
	RethrowError();
	try {
		WriteDataInternal(std::move(buffer));
	} catch (...) {
		first_error = std::current_exception();
		std::rethrow_exception(first_error);
	}
}

void AsyncFileWriter::WriteDataInternal(unique_ptr<AsyncWriteBuffer> buffer) {
	if (!buffer || buffer->Size() == 0) {
		return;
	}
	RethrowTaskError();
	if (finished) {
		throw IOException("Cannot write to closed file \"%s\"", path);
	}
	if (!write_queue->IsAsync()) {
		// Keep the no-async path buffered like BufferedFileWriter instead of turning every owned buffer into a syscall.
		WriteDataSynchronously(buffer->Ptr(), buffer->Size());
		return;
	}
	SealCopiedBuffer(ScheduleMode::DEFER);
	RegisterWrite(std::move(buffer));
}

void AsyncFileWriter::RegisterWrite(unique_ptr<AsyncWriteBuffer> buffer, ScheduleMode schedule_mode) {
	RethrowTaskError();
	if (finished) {
		throw IOException("Cannot write to closed file \"%s\"", path);
	}

	auto write_size = buffer->Size();
	auto offset = total_written;
	total_written += write_size;
	RegisterWriteInternal(std::move(buffer), offset, schedule_mode);
}

void AsyncFileWriter::RegisterStagedWrite(unique_ptr<AsyncWriteBuffer> buffer, idx_t offset,
                                          ScheduleMode schedule_mode) {
	RethrowTaskError();
	if (finished) {
		throw IOException("Cannot write to closed file \"%s\"", path);
	}
	RegisterWriteInternal(std::move(buffer), offset, schedule_mode);
}

void AsyncFileWriter::RegisterWriteInternal(unique_ptr<AsyncWriteBuffer> buffer, idx_t offset,
                                            ScheduleMode schedule_mode) {
	write_queue->RegisterWrite(std::move(buffer), offset, schedule_mode);
}

void AsyncFileWriter::WriteDataSynchronously(data_ptr_t buffer, idx_t write_size) {
	auto copied_size = copied_buffer ? copied_buffer->Size() : 0;
	if (write_size >= 2 * AsyncWriteConfig::COPIED_BUFFER_CAPACITY - copied_size) {
		idx_t copied_prefix = 0;
		if (copied_size > 0) {
			copied_prefix = copied_buffer->Remaining();
			D_ASSERT(copied_prefix <= write_size);
			copied_buffer->Append(buffer, copied_prefix);
			total_written += copied_prefix;
			SealCopiedBuffer(ScheduleMode::DEFER);
		}
		auto remaining_size = write_size - copied_prefix;
		if (remaining_size > 0) {
			auto remaining_offset = total_written;
			total_written += remaining_size;
			if (GetWriteMode() != FileWriteMode::SEQUENTIAL) {
				Write(buffer + copied_prefix, remaining_size, remaining_offset);
			} else {
				Write(buffer + copied_prefix, remaining_size);
			}
			write_queue->ResetNextOffset(total_written);
		}
		return;
	}

	idx_t input_offset = 0;
	while (input_offset < write_size) {
		unique_ptr<CopiedAsyncWriteBuffer> sealed_buffer;
		idx_t sealed_buffer_offset = 0;
		if (!copied_buffer) {
			copied_buffer_offset = total_written;
			copied_buffer = make_uniq<CopiedAsyncWriteBuffer>(client_context, AsyncWriteConfig::COPIED_BUFFER_CAPACITY);
		}
		auto append_size = MinValue(write_size - input_offset, copied_buffer->Remaining());
		copied_buffer->Append(buffer + input_offset, append_size);
		total_written += append_size;
		input_offset += append_size;
		if (copied_buffer->Remaining() == 0) {
			sealed_buffer_offset = copied_buffer_offset;
			sealed_buffer = std::move(copied_buffer);
		}
		if (sealed_buffer) {
			RegisterStagedWrite(std::move(sealed_buffer), sealed_buffer_offset);
		}
	}
}

void AsyncFileWriter::SealCopiedBuffer(ScheduleMode schedule_mode) {
	if (!copied_buffer || copied_buffer->Size() == 0) {
		return;
	}
	auto sealed_buffer_offset = copied_buffer_offset;
	auto sealed_buffer = std::move(copied_buffer);
	RegisterStagedWrite(std::move(sealed_buffer), sealed_buffer_offset, schedule_mode);
}

AsyncFileWriter::BatchGuard AsyncFileWriter::StartBatch() {
	return BatchGuard(*this);
}

void AsyncFileWriter::SchedulePendingWrites(SchedulePolicy policy) {
	if (!write_queue->IsAsync()) {
		return;
	}
	SealCopiedBuffer(ScheduleMode::DEFER);
	write_queue->SchedulePendingWrites(policy);
}

void AsyncFileWriter::BeginBatch() {
	write_queue->BeginBatch();
}

void AsyncFileWriter::LeaveBatch() noexcept {
	write_queue->LeaveBatch();
}

FileWriteMode AsyncFileWriter::GetWriteMode() {
	return handle->GetWriteMode();
}

bool AsyncFileWriter::IsLocalFile() {
	auto local_file = fs.IsLocalFileSystem();
	if (!local_file && handle) {
		try {
			local_file = handle->OnDiskFile();
		} catch (...) {
			local_file = false;
		}
	}
	return local_file;
}

void AsyncFileWriter::Write(data_ptr_t buffer, idx_t size, idx_t offset) {
	if (size == 0) {
		return;
	}
	handle->Write(context, buffer, size, offset);
}

void AsyncFileWriter::Write(data_ptr_t buffer, idx_t size) {
	while (size > 0) {
		auto written = handle->Write(context, buffer, size);
		if (written <= 0) {
			throw IOException("Failed to write to file \"%s\"", path);
		}
		auto written_size = NumericCast<idx_t>(written);
		if (written_size > size) {
			throw IOException("File write exceeded the requested size for \"%s\"", path);
		}
		buffer += written_size;
		size -= written_size;
	}
}

void AsyncFileWriter::RethrowTaskError() {
	if (write_queue) {
		write_queue->RethrowTaskError();
	}
}

void AsyncFileWriter::RethrowError() const {
	if (first_error) {
		std::rethrow_exception(first_error);
	}
}

void AsyncFileWriter::Flush() {
	WaitAll();
}

void AsyncFileWriter::ApplyBackpressure() {
	if (!write_queue->IsAsync()) {
		return;
	}
	RethrowTaskError();
	if (write_queue->HasOpenBatch()) {
		return;
	}
	SealCopiedBuffer(ScheduleMode::DEFER);
	write_queue->ApplyBackpressure();
}

void AsyncFileWriter::WaitAll() {
	WaitAllInternal(BatchDrainMode::PRESERVE_BATCH);
}

void AsyncFileWriter::WaitAllInternal(BatchDrainMode batch_drain_mode) {
	if (!write_queue->IsAsync()) {
		SealCopiedBuffer(ScheduleMode::DEFER);
		RethrowTaskError();
		return;
	}

	if (!write_queue->HasError()) {
		SealCopiedBuffer(ScheduleMode::DEFER);
	}
	write_queue->WaitAll(batch_drain_mode);
}

void AsyncFileWriter::Close() {
	if (finished) {
		RethrowError();
		return;
	}
	if (first_error) {
		auto error = first_error;
		try {
			AbortWrite();
		} catch (...) { // NOLINT
		}
		std::rethrow_exception(error);
	}
	try {
		if (!write_queue->HasError()) {
			SealCopiedBuffer(ScheduleMode::DEFER);
		}
		write_queue->Close();
		if (handle->file_system.IsLocalFileSystem() && handle->GetFileCompressionType().IsUncompressed()) {
			auto metadata = handle->Stats();
			if (metadata.file_type == FileType::FILE_TYPE_REGULAR &&
			    metadata.file_size != NumericCast<int64_t>(total_written)) {
				throw IOException("File size mismatch for file \"%s\": expected %llu bytes, found %lld bytes", path,
				                  total_written, metadata.file_size);
			}
		}
	} catch (...) {
		auto error = std::current_exception();
		if (!first_error) {
			first_error = error;
		}
		try {
			AbortWrite();
		} catch (...) { // NOLINT
		}
		std::rethrow_exception(error);
	}
	try {
		handle->Close();
	} catch (...) {
		first_error = std::current_exception();
		publication_state = FileWritePublicationState::UNKNOWN;
		finished = true;
		auto failed_handle = std::move(handle);
		std::rethrow_exception(first_error);
	}
	publication_state = FileWritePublicationState::PUBLISHED;
	handle.reset();
	finished = true;
}

void AsyncFileWriter::AbortWrite() {
	if (finished) {
		RethrowError();
		return;
	}
	finished = true;
	copied_buffer.reset();

	auto error = first_error;
	try {
		write_queue->AbortWrites();
	} catch (...) {
		if (!error) {
			error = std::current_exception();
		}
	}
	auto abort_handle = std::move(handle);
	if (abort_handle) {
		try {
			abort_handle->AbortWrite();
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

void AsyncFileWriter::Sync() {
	WaitAll();
	handle->Sync();
}

void AsyncFileWriter::Truncate(idx_t size) {
	WaitAll();
	handle->Truncate(NumericCast<int64_t>(size));
	total_written = size;
	write_queue->ResetNextOffset(total_written);
	if (handle->CanSeek() && handle->SeekPosition() != size) {
		handle->Seek(size);
	}
}

} // namespace duckdb
