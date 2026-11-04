#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/function/table/direct_file_reader.hpp"

#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/function/table/read_file.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system_wrapper.hpp"
#include "duckdb/storage/caching_mode.hpp"

namespace duckdb {

DirectFileReader::DirectFileReader(OpenFileInfo file_p, const LogicalType &type)
    : BaseFileReader(std::move(file_p)), done(false), type(type) {
	columns.push_back(MultiFileColumnDefinition("filename", LogicalType::VARCHAR));
	columns.push_back(MultiFileColumnDefinition("content", type));
	columns.push_back(MultiFileColumnDefinition("size", LogicalType::BIGINT));
	columns.push_back(MultiFileColumnDefinition("last_modified", LogicalType::TIMESTAMP_TZ));
}

DirectFileReader::~DirectFileReader() {
}

unique_ptr<BaseStatistics> DirectFileReader::GetStatistics(ClientContext &context, const Identifier &name) {
	return nullptr;
}

bool DirectFileReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
                                         LocalTableFunctionState &lstate) {
	auto &state = gstate.Cast<ReadFileGlobalState>();
	return file_list_idx.GetIndex() < state.file_list->GetTotalFileCount() && !done.load(std::memory_order_relaxed);
}

static void AssertMaxFileSize(const string &file_name, idx_t file_size) {
	const auto max_file_size = NumericLimits<uint32_t>::Maximum();
	if (file_size > max_file_size) {
		auto max_byte_size_format = StringUtil::BytesToHumanReadableString(max_file_size);
		auto file_byte_size_format = StringUtil::BytesToHumanReadableString(file_size);
		auto error_msg = StringUtil::Format("File '%s' size (%s) exceeds maximum allowed file (%s)", file_name.c_str(),
		                                    file_byte_size_format, max_byte_size_format);
		throw InvalidInputException(error_msg);
	}
}

static inline void VERIFY(const string &filename, const string_t &content) {
	if (Utf8Proc::Analyze(content.GetData(), content.GetSize()) == UnicodeType::INVALID) {
		throw InvalidInputException("read_text: could not read content of file '%s' as valid UTF-8 encoded text. You "
		                            "may want to use read_blob instead.",
		                            filename);
	}
}

AsyncResult DirectFileReader::Scan(ClientContext &context, GlobalTableFunctionState &global_state,
                                   LocalTableFunctionState &local_state, DataChunk &output) {
	auto &state = global_state.Cast<ReadFileGlobalState>();
	if (done.load(std::memory_order_relaxed) || file_list_idx.GetIndex() >= state.file_list->GetTotalFileCount()) {
		return AsyncResult(SourceResultType::FINISHED);
	}

	auto files = state.file_list;

	auto &fs = FileSystem::GetFileSystem(context);
	const idx_t out_idx = 0;

	unique_ptr<FileHandle> file_handle = nullptr;
	optional<FileMetadata> file_metadata;
	const bool is_remote = FileSystem::IsRemoteFile(file.path);

	// Remote metadata is obtained from the opened handle so it can use the remote file cache and validators.
	if (state.requires_file_open || (state.requires_file_metadata && is_remote)) {
		auto flags = FileFlags::FILE_FLAGS_READ;
		if (is_remote) {
			flags |= FileFlags::FILE_FLAGS_DIRECT_IO;
		}
		flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
		file_handle = fs.OpenFile(file, flags);
	} else if (state.requires_file_metadata) {
		file_metadata = fs.GetStatsIfExists(file);
		if (!file_metadata) {
			done.store(true, std::memory_order_relaxed);
			return SourceResultType::FINISHED;
		}
	} else {
		// At least verify that the file exist
		// The globbing behavior in remote filesystems can lead to files being listed that do not actually exist
		if (is_remote && !fs.FileExists(file.path)) {
			done.store(true, std::memory_order_relaxed);
			return SourceResultType::FINISHED;
		}
	}

	for (idx_t col_idx = 0; col_idx < state.column_ids.size(); col_idx++) {
		// We utilize projection pushdown to avoid potentially expensive fs operations.
		auto proj_idx = state.column_ids[col_idx];
		if (proj_idx == COLUMN_IDENTIFIER_ROW_ID) {
			continue;
		}
		try {
			switch (proj_idx) {
			case ReadFileBindData::FILE_NAME_COLUMN: {
				auto &file_name_vector = output.data[col_idx];
				auto file_name_string = StringVector::AddString(file_name_vector, file.path);
				FlatVector::GetDataMutable<string_t>(file_name_vector)[out_idx] = file_name_string;
			} break;
			case ReadFileBindData::FILE_CONTENT_COLUMN: {
				const auto file_size = file_handle->GetFileSize();
				AssertMaxFileSize(file.path, file_size);
				// Zero-sized files could be a special file, so read until EOF.
				const bool read_until_eof = file_handle->IsPipe() || file_size == 0;

				// Initialize write stream if not yet done
				if (!state.stream) {
					state.stream = make_uniq<MemoryStream>(BufferAllocator::Get(context), NextPowerOfTwo(file_size));
				}
				state.stream->Rewind();

				// Read in batches of 128mb
				constexpr idx_t MAX_READ_SIZE = 128LL * 1024 * 1024;
				constexpr idx_t UNKNOWN_FILE_READ_SIZE = 64LL * 1024;
				const auto read_size = read_until_eof ? UNKNOWN_FILE_READ_SIZE : MAX_READ_SIZE;
				auto remaining_bytes = read_until_eof ? read_size : file_size;
				while (remaining_bytes > 0) {
					const auto bytes_to_read = MinValue(remaining_bytes, read_size);
					state.stream->GrowCapacity(bytes_to_read);
					idx_t actually_read = NumericCast<idx_t>(file_handle->Read(
					    context, state.stream->GetData() + state.stream->GetPosition(), bytes_to_read));
					state.stream->SetPosition(state.stream->GetPosition() + actually_read);
					AssertMaxFileSize(file.path, state.stream->GetPosition());

					if (read_until_eof) {
						if (actually_read == 0) {
							remaining_bytes = 0;
						}
						continue;
					}

					if (actually_read == 0) {
						// Uh oh, random EOF?
						throw IOException("Failed to read file '%s' at offset %lu, unexpected EOF", file.path,
						                  file_size - remaining_bytes);
					}
					remaining_bytes -= actually_read;
				}

				auto &file_content_vector = output.data[col_idx];
				auto &content_string = FlatVector::GetDataMutable<string_t>(file_content_vector)[out_idx];
				content_string = string_t(char_ptr_cast(state.stream->GetData()),
				                          NumericCast<uint32_t>(state.stream->GetPosition()));

				if (type == LogicalType::VARCHAR) {
					VERIFY(file.path, content_string);
				}
			} break;
			case ReadFileBindData::FILE_SIZE_COLUMN: {
				auto &file_size_vector = output.data[col_idx];
				D_ASSERT(file_metadata.has_value() || file_handle);
				auto file_size =
				    file_metadata ? file_metadata->file_size : NumericCast<int64_t>(file_handle->GetFileSize());
				FlatVector::GetDataMutable<int64_t>(file_size_vector)[out_idx] = file_size;
			} break;
			case ReadFileBindData::FILE_LAST_MODIFIED_COLUMN: {
				auto &last_modified_vector = output.data[col_idx];
				// This can sometimes fail (e.g. httpfs file system cant always parse the last modified time
				// correctly)
				try {
					D_ASSERT(file_metadata.has_value() || file_handle);
					const auto timestamp_seconds =
					    file_metadata ? file_metadata->last_modification_time : fs.GetLastModifiedTime(*file_handle);
					FlatVector::GetDataMutable<timestamp_tz_t>(last_modified_vector)[out_idx] =
					    timestamp_tz_t(timestamp_seconds);
				} catch (std::exception &ex) {
					ErrorData error(ex);
					if (error.Type() == ExceptionType::CONVERSION) {
						FlatVector::SetNull(last_modified_vector, out_idx, true);
					} else {
						throw;
					}
				}
			} break;
			default:
				break;
			}
		}
		// Filesystems are not required to support all operations, so we just set the column to NULL if not
		// implemented
		catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::NOT_IMPLEMENTED) {
				FlatVector::SetNull(output.data[col_idx], out_idx, true);
			} else {
				throw;
			}
		}
	}
	output.SetChildCardinality(1);
	done.store(true, std::memory_order_relaxed);
	return AsyncResult(SourceResultType::HAVE_MORE_OUTPUT);
}

void DirectFileReader::FinishFile(ClientContext &context, GlobalTableFunctionState &gstate) {
	return;
}

double DirectFileReader::GetProgressInFile(ClientContext &context) {
	return done.load(std::memory_order_relaxed) ? 100.0 : 0.0;
}

} // namespace duckdb
