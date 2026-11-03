#include "json_reader.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/query_profiler.hpp"

#include <utility>

#include "duckdb/common/file_opener.hpp"
#include "duckdb/common/file_open_flags.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/storage/caching_mode.hpp"
#include "json_scan.hpp"

namespace duckdb {

JSONBufferHandle::JSONBufferHandle(JSONReader &reader, idx_t buffer_index_p, idx_t readers_p, AllocatedData buffer_p,
                                   idx_t buffer_size_p, idx_t buffer_start_p)
    : reader(reader), buffer_index(buffer_index_p), readers(readers_p), buffer(std::move(buffer_p)),
      buffer_size(buffer_size_p), buffer_start(buffer_start_p) {
}

JSONFileHandle::JSONFileHandle(QueryContext context_p, unique_ptr<FileHandle> file_handle_p, Allocator &allocator_p)
    : context(context_p), file_handle(std::move(file_handle_p)), allocator(allocator_p),
      can_seek(file_handle->CanSeek()), file_size(file_handle->GetFileSize()),
      compressed(file_handle->GetFileCompressionType() != FileCompressionType::UNCOMPRESSED), read_position(0),
      requested_reads(0), actual_reads(0), last_read_requested(false), cached_size(0) {
}

bool JSONFileHandle::IsOpen() const {
	return file_handle != nullptr;
}

void JSONFileHandle::Close() {
	if (IsOpen() && !file_handle->IsPipe()) {
		file_handle->Close();
		file_handle = nullptr;
	}
}

void JSONFileHandle::Reset() {
	D_ASSERT(RequestedReadsComplete());
	read_position = 0;
	requested_reads = 0;
	actual_reads = 0;
	last_read_requested = false;
	if (IsOpen() && !IsPipe()) {
		file_handle->Reset();
		compressed_bytes_counted = 0;
		// the file is read again from the start
		stored_bytes_untaken = 0;
	}
}

bool JSONFileHandle::RequestedReadsComplete() {
	return requested_reads == actual_reads;
}

bool JSONFileHandle::LastReadRequested() const {
	return last_read_requested;
}

idx_t JSONFileHandle::FileSize() const {
	return file_size;
}

idx_t JSONFileHandle::Remaining() const {
	return file_size - read_position;
}

double JSONFileHandle::GetProgress() const {
	if (file_size == 0) {
		return 0;
	}
	idx_t position;
	if (compressed) {
		if (!IsOpen()) {
			return last_read_requested ? 1 : 0;
		}
		// the progress of a compressed file is the position in the compressed stream
		position = file_handle->GetProgress();
	} else {
		position = read_position;
	}
	return MinValue<double>(static_cast<double>(position) / static_cast<double>(file_size), 1.0);
}

bool JSONFileHandle::CanSeek() const {
	return can_seek;
}

bool JSONFileHandle::IsPipe() const {
	return file_handle->IsPipe();
}

FileHandle &JSONFileHandle::GetHandle() {
	return *file_handle;
}

bool JSONFileHandle::GetPositionAndSize(idx_t &position, idx_t &size, idx_t requested_size) {
	D_ASSERT(requested_size != 0);
	if (last_read_requested) {
		return false;
	}

	position = read_position;
	size = MinValue<idx_t>(requested_size, Remaining());
	read_position += size;

	requested_reads++;
	if (size == 0) {
		last_read_requested = true;
	}

	return true;
}

void JSONFileHandle::ReadAtPosition(char *pointer, idx_t size, idx_t position,
                                    optional_ptr<FileHandle> override_handle) {
	if (IsPipe()) {
		throw InternalException("ReadAtPosition is not supported for pipes");
	}
	if (size != 0) {
		auto &handle = override_handle ? *override_handle.get() : *file_handle.get();
		handle.Read(context, pointer, size, position);
		stored_bytes_untaken += size;
	}

	const auto incremented_actual_reads = ++actual_reads;
	if (incremented_actual_reads > requested_reads) {
		throw InternalException("JSONFileHandle performed more actual reads than requested reads");
	}

	if (last_read_requested && incremented_actual_reads == requested_reads) {
		Close();
	}
}

bool JSONFileHandle::Read(char *pointer, idx_t &read_size, idx_t requested_size) {
	D_ASSERT(requested_size != 0);
	if (last_read_requested) {
		return false;
	}

	read_size = 0;
	if (!cached_buffers.empty() || read_position < cached_size) {
		read_size += ReadFromCache(pointer, requested_size, read_position);
	}

	auto temp_read_size = ReadInternal(pointer, requested_size);
	if (IsPipe() && temp_read_size != 0) { // Cache the buffer
		cached_buffers.emplace_back(allocator.Allocate(temp_read_size));
		memcpy(cached_buffers.back().get(), pointer, temp_read_size);
		cached_size += temp_read_size;
	}
	read_position += temp_read_size;
	read_size += temp_read_size;

	if (read_size == 0) {
		last_read_requested = true;
	}

	return true;
}

idx_t JSONFileHandle::TakeStoredBytesRead() {
	return stored_bytes_untaken.exchange(0);
}

idx_t JSONFileHandle::ReadInternal(char *pointer, const idx_t requested_size) {
	// Deal with reading from pipes
	idx_t total_read_size = 0;
	while (total_read_size < requested_size) {
		auto read_size = file_handle->Read(context, pointer + total_read_size, requested_size - total_read_size);
		if (read_size == 0) {
			break;
		}
		total_read_size += read_size;
	}
	if (compressed) {
		// measured from what was counted before rather than from before this read, so the bytes consumed when the
		// file was opened (e.g. a gzip header) count towards the first read
		const auto stored_position = file_handle->GetProgress();
		stored_bytes_untaken += stored_position - compressed_bytes_counted;
		compressed_bytes_counted = stored_position;
	} else {
		stored_bytes_untaken += total_read_size;
	}
	return total_read_size;
}

idx_t JSONFileHandle::ReadFromCache(char *&pointer, idx_t &size, atomic<idx_t> &position) {
	idx_t read_size = 0;
	idx_t total_offset = 0;

	idx_t cached_buffer_idx;
	for (cached_buffer_idx = 0; cached_buffer_idx < cached_buffers.size(); cached_buffer_idx++) {
		auto &cached_buffer = cached_buffers[cached_buffer_idx];
		if (size == 0) {
			break;
		}
		if (position < total_offset + cached_buffer.GetSize()) {
			idx_t within_buffer_offset = position - total_offset;
			idx_t copy_size = MinValue<idx_t>(size, cached_buffer.GetSize() - within_buffer_offset);
			memcpy(pointer, cached_buffer.get() + within_buffer_offset, copy_size);

			read_size += copy_size;
			pointer += copy_size;
			size -= copy_size;
			position += copy_size;
		}
		total_offset += cached_buffer.GetSize();
	}

	return read_size;
}

JSONReader::JSONReader(ClientContext &context, JSONReaderOptions options_p, OpenFileInfo file_p)
    : file(std::move(file_p)), context(context), options(std::move(options_p)),
      read_flag(JSONCommon::GetReadFlag(JSONCommon::READ_STOP_FLAG, JSONCommon::NumbersAsRaw(options.sql_type_list))),
      initialized(false), next_buffer_index(0), thrown(false) {
}

void JSONReader::OpenJSONFile() {
	lock_guard<mutex> guard(lock);
	if (!IsOpen()) {
		auto &fs = FileSystem::GetFileSystem(context);
		FileOpenFlags flags = FileFlags::FILE_FLAGS_READ | options.compression;
		flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
		auto regular_file_handle = fs.OpenFile(file, flags);
		file_handle = make_uniq<JSONFileHandle>(context, std::move(regular_file_handle), BufferAllocator::Get(context));
	}
	Reset();
}

void JSONReader::CloseHandle() {
	lock_guard<mutex> guard(lock);
	if (IsOpen()) {
		file_handle->Close();
	}
}

void JSONReader::Reset() {
	initialized = false;
	next_buffer_index = 0;
	buffer_map.clear();
	buffer_line_or_object_counts.clear();
	auto_detect_data.Reset();
	auto_detect_data_size = 0;
	if (HasFileHandle()) {
		file_handle->Reset();
	}
}

bool JSONReader::HasFileHandle() const {
	return file_handle != nullptr;
}

bool JSONReader::IsOpen() const {
	if (HasFileHandle()) {
		return file_handle->IsOpen();
	}
	return false;
}

JSONReaderOptions &JSONReader::GetOptions() {
	return options;
}

JSONFormat JSONReader::GetFormat() const {
	return options.format;
}

void JSONReader::SetFormat(JSONFormat format) {
	D_ASSERT(options.format == JSONFormat::AUTO_DETECT);
	options.format = format;
}

JSONRecordType JSONReader::GetRecordType() const {
	return options.record_type;
}

void JSONReader::SetRecordType(duckdb::JSONRecordType type) {
	D_ASSERT(options.record_type == JSONRecordType::AUTO_DETECT);
	options.record_type = type;
}

const string &JSONReader::GetFileName() const {
	return file.path;
}

JSONFileHandle &JSONReader::GetFileHandle() const {
	D_ASSERT(HasFileHandle());
	return *file_handle;
}

void JSONReader::InsertBuffer(idx_t buffer_idx, unique_ptr<JSONBufferHandle> buffer) {
	lock_guard<mutex> guard(lock);
	D_ASSERT(buffer_map.find(buffer_idx) == buffer_map.end());
	buffer_map.insert(make_pair(buffer_idx, std::move(buffer)));
}

optional_ptr<JSONBufferHandle> JSONReader::GetBuffer(idx_t buffer_idx) {
	lock_guard<mutex> guard(lock);
	auto it = buffer_map.find(buffer_idx);
	return it == buffer_map.end() ? nullptr : it->second.get();
}

AllocatedData JSONReader::RemoveBuffer(JSONBufferHandle &handle) {
	lock_guard<mutex> guard(lock);
	auto it = buffer_map.find(handle.buffer_index);
	D_ASSERT(it != buffer_map.end());
	D_ASSERT(RefersToSameObject(handle, *it->second));
	auto result = std::move(it->second->buffer);
	buffer_map.erase(it);
	return result;
}

idx_t JSONReader::GetBufferIndex() {
	buffer_line_or_object_counts.push_back(-1);
	return next_buffer_index++;
}

void JSONReader::SetBufferLineOrObjectCount(JSONBufferHandle &handle, idx_t count) {
	lock_guard<mutex> guard(lock);
	D_ASSERT(buffer_map.find(handle.buffer_index) != buffer_map.end());
	D_ASSERT(RefersToSameObject(handle, *buffer_map.find(handle.buffer_index)->second));
	D_ASSERT(buffer_line_or_object_counts[handle.buffer_index] == -1);
	buffer_line_or_object_counts[handle.buffer_index] = NumericCast<int64_t>(count);
	// if we have any errors - try to report them after finishing a buffer
	ThrowErrorsIfPossible();
}

void JSONReader::AddParseError(JSONReaderScanState &scan_state, idx_t line_or_object_in_buf, yyjson_read_err &err,
                               const string &extra) {
	string unit = options.format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
	auto error_msg = StringUtil::Format("Malformed JSON in file \"%s\", at byte %llu in %s {line}: %s. %s",
	                                    GetFileName(), err.pos + 1, unit, err.msg, extra);
	lock_guard<mutex> guard(lock);
	AddError(scan_state.current_buffer_handle->buffer_index, line_or_object_in_buf + 1, error_msg);
	ThrowErrorsIfPossible();
	// if we could not throw immediately - finish processing this buffer
	scan_state.buffer_offset = 0;
	scan_state.scan_count = 0;
}

void JSONReader::AddTransformError(JSONReaderScanState &scan_state, idx_t object_index, const string &error_message) {
	D_ASSERT(scan_state.current_buffer_handle);
	D_ASSERT(object_index != DConstants::INVALID_INDEX);
	auto line_or_object_in_buffer = scan_state.lines_or_objects_in_buffer - scan_state.scan_count + object_index;
	string unit = options.format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
	auto error_msg =
	    StringUtil::Format("JSON transform error in file \"%s\", in %s {line}: %s", GetFileName(), unit, error_message);
	lock_guard<mutex> guard(lock);
	AddError(scan_state.current_buffer_handle->buffer_index, line_or_object_in_buffer, error_msg);
	ThrowErrorsIfPossible();
	// if we could not throw immediately - finish processing this buffer
	scan_state.buffer_offset = scan_state.buffer_size;
	scan_state.scan_count = 0;
}

void JSONReader::AddError(idx_t buf_index, idx_t line_or_object_in_buf, const string &error_msg) {
	if (error) {
		// we already have an error - check if it happened before this error
		if (error->buf_index < buf_index ||
		    (error->buf_index == buf_index && error->line_or_object_in_buf < line_or_object_in_buf)) {
			// it did! don't record this error
			return;
		}
	} else {
		error = make_uniq<JSONError>();
	}
	error->buf_index = buf_index;
	error->line_or_object_in_buf = line_or_object_in_buf;
	error->error_msg = error_msg;
}

optional_idx JSONReader::TryGetLineNumber(idx_t buf_index, idx_t line_or_object_in_buf) {
	idx_t line = line_or_object_in_buf;
	for (idx_t b_idx = 0; b_idx < buf_index; b_idx++) {
		if (buffer_line_or_object_counts[b_idx] == -1) {
			// this buffer has not been parsed yet - we cannot throw
			return optional_idx();
		}
		line += buffer_line_or_object_counts[b_idx];
	}
	return line;
}

void JSONReader::ThrowErrorsIfPossible() {
	if (!error) {
		return;
	}
	// check if we finished all buffers before the error buffer
	auto line = TryGetLineNumber(error->buf_index, error->line_or_object_in_buf);
	if (!line.IsValid()) {
		return;
	}
	// we can throw!
	thrown = true;
	auto formatted_error = StringUtil::Replace(error->error_msg, "{line}", to_string(line.GetIndex() + 1));
	throw InvalidInputException(formatted_error);
}

bool JSONReader::HasThrown() {
	if (context.GetExecutor().HasError()) {
		return true;
	}
	lock_guard<mutex> guard(lock);
	return thrown;
}

double JSONReader::GetProgress() const {
	lock_guard<mutex> guard(lock);
	if (!HasFileHandle()) {
		return 0;
	}
	return 100.0 * file_handle->GetProgress();
}

static inline void TrimWhitespace(JSONString &line) {
	while (line.size != 0 && StringUtil::CharacterIsSpace(line[0])) {
		line.pointer++;
		line.size--;
	}
	while (line.size != 0 && StringUtil::CharacterIsSpace(line[line.size - 1])) {
		line.size--;
	}
}

JSONReaderScanState::JSONReaderScanState(ClientContext &context, Allocator &global_allocator, idx_t buffer_capacity)
    : fs(FileSystem::GetFileSystem(context)), global_allocator(global_allocator),
      allocator(BufferAllocator::Get(context)), buffer_capacity(buffer_capacity) {
}

void JSONReaderScanState::ResetForNextParse() {
	allocator.Reset();
	scan_count = 0;
}

void JSONReaderScanState::ClearBufferHandle() {
	if (!current_buffer_handle) {
		return;
	}
	// Free up the current buffer - if any
	auto &handle = *current_buffer_handle;
	if (!RefersToSameObject(handle.reader, *current_reader)) {
		throw InternalException("Handle reader and current reader are unaligned");
	}
	handle.reader.DecrementBufferUsage(*current_buffer_handle, lines_or_objects_in_buffer, read_buffer);
	current_buffer_handle = nullptr;
}

void JSONReaderScanState::ResetForNextBuffer() {
	ClearBufferHandle();
	buffer_index = optional_idx();
	buffer_size = 0;
	scan_count = 0;
	is_last = false;
	file_read_type = JSONFileReadType::SCAN_PARTIAL;
}

static inline void SkipWhitespace(const char *buffer_ptr, idx_t &buffer_offset, const idx_t &buffer_size) {
	for (; buffer_offset != buffer_size; buffer_offset++) {
		if (!StringUtil::CharacterIsSpace(buffer_ptr[buffer_offset])) {
			break;
		}
	}
}

static inline const char *NextNewline(const char *ptr, const idx_t size) {
	return const_char_ptr_cast(memchr(ptr, '\n', size));
}

static inline const char *PreviousNewline(const char *ptr, const idx_t size) {
	const auto end = ptr - size;
	for (ptr--; ptr != end; ptr--) {
		if (*ptr == '\n') {
			break;
		}
	}
	return ptr;
}

static inline const char *NextJSONDefault(const char *ptr, const char *const end) {
	idx_t parents = 0;
	while (ptr != end) {
		switch (*ptr++) {
		case '{':
		case '[':
			parents++;
			continue;
		case '}':
		case ']':
			parents--;
			break;
		case '"':
			while (ptr != end) {
				auto string_char = *ptr++;
				if (string_char == '"') {
					break;
				} else if (string_char == '\\') {
					if (ptr != end) {
						ptr++; // Skip the escaped char
					}
				}
			}
			break;
		default:
			continue;
		}

		if (parents == 0) {
			break;
		}
	}

	return ptr;
}

static inline const char *NextJSON(const char *ptr, const idx_t size) {
	D_ASSERT(!StringUtil::CharacterIsSpace(*ptr)); // Should be handled before

	const char *const end = ptr + size;
	switch (*ptr) {
	case '{':
	case '[':
	case '"':
		ptr = NextJSONDefault(ptr, end);
		break;
	default:
		// Special case: JSON array containing JSON without clear "parents", i.e., not obj/arr/str
		while (ptr != end) {
			switch (*ptr++) {
			case ',':
			case ']':
				ptr--;
				break;
			default:
				continue;
			}
			break;
		}
	}

	return ptr == end ? nullptr : ptr;
}

//! Skip over a single JSON value, leaving pos just after it
static bool SkipJSONValue(const char *buffer_ptr, const idx_t buffer_size, idx_t &pos) {
	const auto skip_string = [&]() {
		pos++; // opening quote
		while (pos < buffer_size && buffer_ptr[pos] != '"') {
			pos += buffer_ptr[pos] == '\\' ? 2 : 1;
		}
		if (pos >= buffer_size) {
			return false;
		}
		pos++; // closing quote
		return true;
	};

	if (pos >= buffer_size) {
		return false;
	}
	if (buffer_ptr[pos] == '"') {
		return skip_string();
	}
	if (buffer_ptr[pos] == '{' || buffer_ptr[pos] == '[') {
		idx_t depth = 0;
		while (pos < buffer_size) {
			const auto c = buffer_ptr[pos];
			if (c == '"') {
				if (!skip_string()) {
					return false;
				}
				continue;
			}
			if (c == '{' || c == '[') {
				depth++;
			} else if (c == '}' || c == ']') {
				if (--depth == 0) {
					pos++;
					return true;
				}
			}
			pos++;
		}
		return false;
	}
	// A number, or one of true/false/null
	while (pos < buffer_size && buffer_ptr[pos] != ',' && buffer_ptr[pos] != '}' &&
	       !StringUtil::CharacterIsSpace(buffer_ptr[pos])) {
		pos++;
	}
	return true;
}

//! Recognize a GeoJSON FeatureCollection and locate the '[' that opens its "features" array. This is a textual
//! scan rather than a parse because the array is usually far bigger than the buffer we have here.
static bool FindFeatureCollectionArray(const char *buffer_ptr, const idx_t buffer_size, idx_t &features_offset) {
	idx_t pos = 0;
	SkipWhitespace(buffer_ptr, pos, buffer_size);
	if (pos >= buffer_size || buffer_ptr[pos] != '{') {
		return false;
	}
	pos++;

	while (pos < buffer_size) {
		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size || buffer_ptr[pos] == '}') {
			return false;
		}
		if (buffer_ptr[pos] == ',') {
			pos++;
			continue;
		}
		if (buffer_ptr[pos] != '"') {
			return false;
		}

		const auto key_start = ++pos;
		while (pos < buffer_size && buffer_ptr[pos] != '"') {
			pos += buffer_ptr[pos] == '\\' ? 2 : 1;
		}
		if (pos >= buffer_size) {
			return false;
		}
		const auto key_size = pos++ - key_start;

		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size || buffer_ptr[pos] != ':') {
			return false;
		}
		pos++;
		SkipWhitespace(buffer_ptr, pos, buffer_size);
		if (pos >= buffer_size) {
			return false;
		}

		if (key_size == 8 && memcmp(buffer_ptr + key_start, "features", 8) == 0) {
			if (buffer_ptr[pos] != '[') {
				return false;
			}
			// "type" usually precedes "features", but accept the other order as long as nothing contradicts us
			features_offset = pos;
			return true;
		}
		if (key_size == 4 && memcmp(buffer_ptr + key_start, "type", 4) == 0) {
			static constexpr auto FEATURE_COLLECTION = "\"FeatureCollection\"";
			static constexpr idx_t FEATURE_COLLECTION_SIZE = 19;
			if (pos + FEATURE_COLLECTION_SIZE > buffer_size ||
			    memcmp(buffer_ptr + pos, FEATURE_COLLECTION, FEATURE_COLLECTION_SIZE) != 0) {
				return false; // Some other kind of object
			}
		}
		if (!SkipJSONValue(buffer_ptr, buffer_size, pos)) {
			return false;
		}
	}
	return false;
}

void JSONReader::SkipOverArrayStart(JSONReaderScanState &scan_state) {
	// First read of this buffer, check if it's actually an array and skip over the bytes
	auto &buffer_ptr = scan_state.buffer_ptr;
	auto &buffer_offset = scan_state.buffer_offset;
	auto &buffer_size = scan_state.buffer_size;
	SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	if (buffer_offset == buffer_size) {
		return; // Empty file
	}
	if (skip_feature_collection_prefix) {
		// Position ourselves on the '[' of the "features" array, then continue as a regular array
		idx_t features_offset;
		if (!FindFeatureCollectionArray(buffer_ptr, buffer_size, features_offset)) {
			throw InvalidInputException("Expected a GeoJSON FeatureCollection in file \"%s\"", GetFileName());
		}
		buffer_offset = features_offset;
	}
	if (buffer_ptr[buffer_offset] != '[') {
		throw InvalidInputException(
		    "Expected top-level JSON array with format='array', but first character is '%c' in file \"%s\"."
		    "\n Try setting format='auto' or format='newline_delimited'.",
		    buffer_ptr[buffer_offset], GetFileName());
	}
	SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
	if (buffer_offset >= buffer_size) {
		throw InvalidInputException("Missing closing brace ']' in JSON array with format='array' in file \"%s\"",
		                            GetFileName());
	}
	if (buffer_ptr[buffer_offset] == ']') {
		// Empty array
		SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
		if (buffer_offset != buffer_size && !skip_feature_collection_prefix) {
			throw InvalidInputException(
			    "Empty array with trailing data when parsing JSON array with format='array' in file \"%s\"",
			    GetFileName());
		}
		// The rest of the FeatureCollection is not ours to read
		buffer_offset = buffer_size;
		scan_state.skip_remainder_of_file = skip_feature_collection_prefix;
	}
}

static pair<JSONFormat, JSONRecordType> DetectFormatAndRecordType(char *const buffer_ptr, const idx_t buffer_size,
                                                                  yyjson_alc *alc) {
	// First we do the easy check whether it's NEWLINE_DELIMITED
	auto line_end = NextNewline(buffer_ptr, buffer_size);
	if (line_end != nullptr) {
		idx_t line_size = line_end - buffer_ptr;
		SkipWhitespace(buffer_ptr, line_size, buffer_size);

		yyjson_read_err error;
		auto doc = JSONCommon::ReadDocumentUnsafe(buffer_ptr, line_size, JSONCommon::READ_FLAG, alc, &error);
		if (error.code == YYJSON_READ_SUCCESS) { // We successfully read the line
			if (yyjson_is_arr(doc->root) && line_size == buffer_size) {
				// It's just one array, let's actually assume ARRAY, not NEWLINE_DELIMITED
				if (yyjson_arr_size(doc->root) == 0 || yyjson_is_obj(yyjson_arr_get(doc->root, 0))) {
					// Either an empty array (assume records), or an array of objects
					return make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
				} else {
					return make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
				}
			} else if (yyjson_is_obj(doc->root)) {
				return make_pair(JSONFormat::NEWLINE_DELIMITED, JSONRecordType::RECORDS);
			} else {
				return make_pair(JSONFormat::NEWLINE_DELIMITED, JSONRecordType::VALUES);
			}
		}
	}

	// Skip whitespace
	idx_t buffer_offset = 0;
	SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	auto remaining = buffer_size - buffer_offset;

	// We know it's not NEWLINE_DELIMITED at this point, if there's a '{', we know it's not ARRAY either
	// Also if it's fully whitespace we just return something because we don't know
	if (remaining == 0 || buffer_ptr[buffer_offset] == '{') {
		return make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::RECORDS);
	}

	// We know it's not top-level records, if it's not '[', it's not ARRAY either
	if (buffer_ptr[buffer_offset] != '[') {
		return make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::VALUES);
	}

	// It's definitely an ARRAY, but now we have to figure out if there's more than one top-level array
	yyjson_read_err error;
	auto doc =
	    JSONCommon::ReadDocumentUnsafe(buffer_ptr + buffer_offset, remaining, JSONCommon::READ_STOP_FLAG, alc, &error);
	if (error.code == YYJSON_READ_SUCCESS) {
		D_ASSERT(yyjson_is_arr(doc->root));

		// We successfully read something!
		buffer_offset += yyjson_doc_get_read_size(doc);
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
		remaining = buffer_size - buffer_offset;

		if (remaining != 0) { // There's more
			return make_pair(JSONFormat::UNSTRUCTURED, JSONRecordType::VALUES);
		}

		// Just one array, check what's in there
		if (yyjson_arr_size(doc->root) == 0 || yyjson_is_obj(yyjson_arr_get(doc->root, 0))) {
			// Either an empty array (assume records), or an array of objects
			return make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
		} else {
			return make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
		}
	}

	// We weren't able to parse an array, could be broken or an array larger than our buffer size, let's skip over '['
	SkipWhitespace(buffer_ptr, ++buffer_offset, buffer_size);
	remaining = buffer_size - buffer_offset;

	// If it's '{' we know there's RECORDS in the ARRAY, else it's VALUES
	if (remaining == 0 || buffer_ptr[buffer_offset] == '{') {
		return make_pair(JSONFormat::ARRAY, JSONRecordType::RECORDS);
	}

	// It's not RECORDS, so it must be VALUES
	return make_pair(JSONFormat::ARRAY, JSONRecordType::VALUES);
}

bool JSONReader::ParseJSON(JSONReaderScanState &scan_state, char *const json_start, const idx_t json_size,
                           const idx_t remaining) {
	yyjson_doc *doc;
	yyjson_read_err err;
	// If we return strings, we cannot parse INSITU. If we ignore errors, we cannot parse INSITU either: a malformed
	// document could extend into (and modify) the next one
	const bool ignorable_errors = options.ignore_errors && GetFormat() == JSONFormat::NEWLINE_DELIMITED;
	// A number that does not fit is read as a raw string, which INSITU parsing null-terminates in place - at the root
	// that overwrites the delimiter that follows the document
	idx_t first_char = 0;
	while (first_char < json_size && StringUtil::CharacterIsSpace(json_start[first_char])) {
		first_char++;
	}
	const bool number_root = first_char < json_size &&
	                         (StringUtil::CharacterIsDigit(json_start[first_char]) || json_start[first_char] == '-');
	if (options.type == JSONScanType::READ_JSON_OBJECTS || ignorable_errors || number_root) {
		doc = JSONCommon::ReadDocumentUnsafe(json_start, json_size, read_flag, scan_state.allocator.GetYYAlc(), &err);
	} else {
		doc = JSONCommon::ReadDocumentUnsafe(json_start, remaining, read_flag | YYJSON_READ_INSITU,
		                                     scan_state.allocator.GetYYAlc(), &err);
	}
	if (err.code != YYJSON_READ_SUCCESS) {
		auto can_ignore_this_error = options.ignore_errors;
		string extra;
		if (GetFormat() != JSONFormat::NEWLINE_DELIMITED) {
			can_ignore_this_error = false;
			extra = options.ignore_errors
			            ? "Parse errors cannot be ignored for JSON formats other than 'newline_delimited'"
			            : "";
		}
		if (!can_ignore_this_error) {
			AddParseError(scan_state, scan_state.lines_or_objects_in_buffer, err, extra);
			return false;
		}
	}

	// We parse with YYJSON_STOP_WHEN_DONE, so we need to check this by hand
	const auto read_size = yyjson_doc_get_read_size(doc);
	if (read_size > json_size) {
		// Can't go past the boundary, even with ignore_errors
		err.code = YYJSON_READ_ERROR_UNEXPECTED_END;
		err.msg = "unexpected end of data";
		err.pos = json_size;
		AddParseError(scan_state, scan_state.lines_or_objects_in_buffer, err, "Try auto-detecting the JSON format");
		return false;
	}
	if (read_size < json_size) {
		idx_t off = read_size;
		idx_t rem = json_size;
		SkipWhitespace(json_start, off, rem);
		if (off != rem) { // Between end of document and boundary should be whitespace only
			if (!options.ignore_errors) {
				err.code = YYJSON_READ_ERROR_UNEXPECTED_CONTENT;
				err.msg = "unexpected content after document";
				err.pos = read_size;
				AddParseError(scan_state, scan_state.lines_or_objects_in_buffer, err,
				              "Try auto-detecting the JSON format");
				return false;
			}
			doc = nullptr;
		}
	}

	scan_state.lines_or_objects_in_buffer++;
	if (!doc) {
		scan_state.values[scan_state.scan_count] = nullptr;
		return true;
	}

	// Set the JSONLine and trim
	scan_state.units[scan_state.scan_count] = JSONString(json_start, json_size);
	TrimWhitespace(scan_state.units[scan_state.scan_count]);
	scan_state.values[scan_state.scan_count] = doc->root;
	return true;
}

void JSONReader::AutoDetect(Allocator &allocator, idx_t buffer_capacity) {
	// read the first buffer from the file
	auto read_buffer = allocator.Allocate(buffer_capacity);
	idx_t read_size = 0;
	auto buffer_ptr = char_ptr_cast(read_buffer.get());
	if (!file_handle->Read(buffer_ptr, read_size, buffer_capacity - YYJSON_PADDING_SIZE)) {
		// could not read anything
		return;
	}
	if (read_size == 0) {
		// file is empty - skip
		return;
	}
	// perform auto-detection over the data we just read
	JSONAllocator json_allocator(allocator);
	idx_t features_offset;
	if (options.geojson.value_or(false) && GetFormat() == JSONFormat::AUTO_DETECT &&
	    FindFeatureCollectionArray(buffer_ptr, read_size, features_offset)) {
		// A FeatureCollection is one big object whose "features" array holds the rows, so read it as an array
		skip_feature_collection_prefix = true;
		SetFormat(JSONFormat::ARRAY);
		if (GetRecordType() == JSONRecordType::AUTO_DETECT) {
			SetRecordType(JSONRecordType::RECORDS);
		}
	} else {
		auto format_and_record_type = DetectFormatAndRecordType(buffer_ptr, read_size, json_allocator.GetYYAlc());
		if (GetFormat() == JSONFormat::AUTO_DETECT) {
			SetFormat(format_and_record_type.first);
		}
		if (GetRecordType() == JSONRecordType::AUTO_DETECT) {
			SetRecordType(format_and_record_type.second);
		}
	}
	if (!options.ignore_errors && options.record_type == JSONRecordType::RECORDS &&
	    GetRecordType() != JSONRecordType::RECORDS) {
		// LCOV_EXCL_START
		string unit = options.format == JSONFormat::NEWLINE_DELIMITED ? "line" : "record/value";
		throw InvalidInputException(
		    "JSON auto-detection error in file \"%s\": Expected records, detected non-record JSON instead",
		    GetFileName());
		// LCOV_EXCL_STOP
	}
	// store the buffer in the file so it can be re-used by the first reader of the file
	if (!file_handle->IsPipe()) {
		// a file that is much smaller than the buffer only keeps what was read - with many files (e.g. union_by_name)
		// one full-sized buffer per file adds up to far more than the data
		const idx_t required = read_size + YYJSON_PADDING_SIZE;
		if (required * 2 <= buffer_capacity) {
			auto shrunk = allocator.Allocate(required);
			memcpy(shrunk.get(), read_buffer.get(), required);
			read_buffer = std::move(shrunk);
		}
		auto_detect_data = std::move(read_buffer);
		auto_detect_data_size = read_size;
	} else {
		file_handle->Reset();
	}
}

void JSONReader::ThrowObjectSizeError(const idx_t object_size) {
	// LCOV_EXCL_START
	throw InvalidInputException(
	    "\"maximum_object_size\" of %llu bytes exceeded while reading file \"%s\" (>%llu bytes)."
	    "\n Try increasing \"maximum_object_size\".",
	    options.maximum_object_size, GetFileName(), object_size);
	// LCOV_EXCL_STOP
}

bool JSONReader::CopyRemainderFromPreviousBuffer(JSONReaderScanState &scan_state) {
	D_ASSERT(scan_state.buffer_index.GetIndex() != 0);
	D_ASSERT(GetFormat() == JSONFormat::NEWLINE_DELIMITED);

	// Spinlock until the previous batch index has also read its buffer
	optional_ptr<JSONBufferHandle> previous_buffer_handle;
	while (!previous_buffer_handle) {
		if (HasThrown()) {
			return false;
		}
		previous_buffer_handle = GetBuffer(scan_state.buffer_index.GetIndex() - 1);
	}

	// First we find the newline in the previous block
	idx_t prev_buffer_size = previous_buffer_handle->buffer_size - previous_buffer_handle->buffer_start;
	auto prev_buffer_ptr = char_ptr_cast(previous_buffer_handle->buffer.get()) + previous_buffer_handle->buffer_size;
	// the first buffer of a file that is only a byte order mark holds nothing past it
	auto prev_object_start =
	    prev_buffer_size == 0 ? prev_buffer_ptr : PreviousNewline(prev_buffer_ptr, prev_buffer_size);
	auto prev_object_size = NumericCast<idx_t>(prev_buffer_ptr - prev_object_start);

	D_ASSERT(scan_state.buffer_offset == options.maximum_object_size);
	if (prev_object_size > scan_state.buffer_offset) {
		ThrowObjectSizeError(prev_object_size);
	}
	// Now copy the data to our reconstruct buffer
	memcpy(scan_state.buffer_ptr + scan_state.buffer_offset - prev_object_size, prev_object_start, prev_object_size);

	// Just a newline, or nothing. A single byte without a newline before it is a value, such as a 1 at the end of a
	// file. This is checked before the previous buffer is released
	const bool no_remainder = prev_object_size == 0 || (prev_object_size == 1 && *prev_object_start == '\n');

	// We copied the object, so we are no longer reading the previous buffer
	if (--previous_buffer_handle->readers == 0) {
		RemoveBuffer(*previous_buffer_handle);
	}

	if (no_remainder) {
		return false;
	}
	scan_state.buffer_offset -= prev_object_size;
	return true;
}

bool JSONReader::ParseNextChunk(JSONReaderScanState &scan_state) {
	const auto format = GetFormat();
	auto &buffer_ptr = scan_state.buffer_ptr;
	auto &buffer_offset = scan_state.buffer_offset;
	auto &buffer_size = scan_state.buffer_size;
	auto &scan_count = scan_state.scan_count;
	for (; scan_count < STANDARD_VECTOR_SIZE; scan_count++) {
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
		auto json_start = buffer_ptr + buffer_offset;
		idx_t remaining = buffer_size - buffer_offset;
		if (remaining == 0) {
			break;
		}
		D_ASSERT(format != JSONFormat::AUTO_DETECT);
		const char *json_end = format == JSONFormat::NEWLINE_DELIMITED ? NextNewline(json_start, remaining)
		                                                               : NextJSON(json_start, remaining);
		if (json_end == nullptr) {
			if (remaining > options.maximum_object_size) {
				ThrowObjectSizeError(remaining);
			}
			// We reached the end of the buffer
			if (!scan_state.is_last) {
				// Last bit of data belongs to the next batch
				if (scan_state.file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
					scan_state.prev_buffer_remainder = remaining;
					scan_state.prev_buffer_offset = json_start - buffer_ptr;
				}
				buffer_offset = buffer_size;
				break;
			}
			json_end = json_start + remaining;
		}

		idx_t json_size = json_end - json_start;
		if (!ParseJSON(scan_state, json_start, json_size, remaining)) {
			return false;
		}
		buffer_offset += json_size;

		if (format == JSONFormat::ARRAY) {
			SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
			if (buffer_ptr[buffer_offset] == ']' && skip_feature_collection_prefix) {
				// Anything after the "features" array (the closing brace, "bbox", ...) is not ours to read.
				// Count the value we just parsed, since breaking here skips the loop's increment.
				buffer_offset = buffer_size;
				scan_state.skip_remainder_of_file = true;
				scan_count++;
				break;
			}
			if (buffer_ptr[buffer_offset] == ',' || buffer_ptr[buffer_offset] == ']') {
				buffer_offset++;
			} else { // We can't ignore this error, even with 'ignore_errors'
				yyjson_read_err err;
				err.code = YYJSON_READ_ERROR_UNEXPECTED_CHARACTER;
				err.msg = "unexpected character";
				err.pos = json_size;
				AddParseError(scan_state, scan_state.lines_or_objects_in_buffer, err);
				return false;
			}
		}
		SkipWhitespace(buffer_ptr, buffer_offset, buffer_size);
	}
	return true;
}

void JSONReader::Initialize(Allocator &allocator, idx_t buffer_size) {
	if (initialized) {
		throw InternalException("JSON InitializeScan called twice on the same reader without resetting");
	}
	// Open the file if it is not yet open
	if (!IsOpen()) {
		OpenJSONFile();
	}
	initialized = true;
	// Auto-detect if we haven't yet done this during the bind
	if (options.record_type == JSONRecordType::AUTO_DETECT || GetFormat() == JSONFormat::AUTO_DETECT) {
		// We have to detect the JSON format
		AutoDetect(allocator, buffer_size);
	}
}

bool JSONReader::InitializeScan(JSONReaderScanState &scan_state, JSONFileReadType file_read_type) {
	if (file_read_type == JSONFileReadType::SCAN_PARTIAL && GetFormat() != JSONFormat::NEWLINE_DELIMITED) {
		throw InternalException("JSON Partial scans are only possible on ND json");
	}
	scan_state.current_reader = this;
	scan_state.is_first_scan = true;
	scan_state.skip_remainder_of_file = false;
	scan_state.file_read_type = file_read_type;
	if (file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
		// when initializing a single-file scan we don't need to read anything yet
		return true;
	}
	// partial read
	// we need to check if there is data available within our current reader
	if (PrepareBufferForRead(scan_state)) {
		// there is data available! return
		return true;
	}
	return false;
}

idx_t JSONReader::Scan(JSONReaderScanState &scan_state) {
	PrepareForScan(scan_state);
	while (scan_state.scan_count == 0) {
		while (scan_state.buffer_offset >= scan_state.buffer_size) {
			// we have exhausted the current buffer
			if (scan_state.file_read_type == JSONFileReadType::SCAN_PARTIAL) {
				// we are not scanning the entire file
				// return and fetch the next buffer from the global state
				return 0;
			}
			// read the next buffer
			if (scan_state.skip_remainder_of_file || !ReadNextBuffer(scan_state)) {
				// we have exhausted the file
				return 0;
			}
		}
		if (!ParseNextChunk(scan_state)) {
			// found an error but we can't handle it - return
			return 0;
		}
	}
	return scan_state.scan_count;
}

void JSONReader::PrepareForScan(JSONReaderScanState &scan_state) {
	if (!scan_state.is_first_scan) {
		// we have already scanned from this buffer before - just reset and scan the next batch
		scan_state.ResetForNextParse();
		return;
	}
	scan_state.is_first_scan = false;
	if (scan_state.file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
		// first time scanning from this reader while scanning the entire file
		// we need to initialize the reader
		if (!scan_state.current_reader->IsInitialized()) {
			scan_state.current_reader->Initialize(scan_state.global_allocator, scan_state.buffer_capacity);
		}
		return;
	}
	if (!scan_state.needs_to_read && !scan_state.read_buffer.IsSet()) {
		// we have already read (because we auto-detected) - skip
		return;
	}
	// we are scanning only a buffer - finalize it so we can start reading
	FinalizeBuffer(scan_state);
}

void JSONReader::FinalizeBuffer(JSONReaderScanState &scan_state) {
	if (scan_state.needs_to_read) {
		// we haven't read into the buffer yet - this can only happen if we are reading a file we can seek into
		// read into the buffer
		ReadNextBufferSeek(scan_state);
		scan_state.needs_to_read = false;
	}

	// we read something
	// skip over the array start if required
	if (!scan_state.is_last) {
		if (scan_state.buffer_index.GetIndex() == 0) {
			// the file starts at the buffer offset, which leaves room for the end of a previous buffer when the file
			// is read in parallel
			idx_t bom_size = 0;
			StringUtil::SkipBOM(scan_state.buffer_ptr + scan_state.buffer_offset,
			                    scan_state.buffer_size - scan_state.buffer_offset, bom_size);
			scan_state.buffer_offset += bom_size;
			if (GetFormat() == JSONFormat::ARRAY) {
				SkipOverArrayStart(scan_state);
			}
		}
	}
	// then finalize the buffer
	FinalizeBufferInternal(scan_state, scan_state.read_buffer, scan_state.buffer_index.GetIndex());
}

void JSONReader::CountBytesScanned() {
	if (!scanning || !HasFileHandle()) {
		return;
	}
	auto bytes = GetFileHandle().TakeStoredBytesRead();
	if (bytes == 0) {
		return;
	}
	bytes_scanned_unreported += bytes;
	QueryProfiler::Get(context).TrackBytesScanned(bytes);
}

bool JSONReader::ReadNextBuffer(JSONReaderScanState &scan_state) {
	if (!PrepareBufferForRead(scan_state)) {
		return false;
	}
	// finalize the buffer
	FinalizeBuffer(scan_state);
	return true;
}

void JSONReader::FinalizeBufferInternal(JSONReaderScanState &scan_state, AllocatedData &buffer, idx_t buffer_index) {
	idx_t readers = 1;
	if (scan_state.file_read_type == JSONFileReadType::SCAN_PARTIAL) {
		readers = scan_state.is_last ? 1 : 2;
	}

	// Create an entry and insert it into the map
	auto json_buffer_handle = make_uniq<JSONBufferHandle>(*this, buffer_index, readers, std::move(buffer),
	                                                      scan_state.buffer_size, scan_state.buffer_offset);
	scan_state.current_buffer_handle = json_buffer_handle.get();
	InsertBuffer(buffer_index, std::move(json_buffer_handle));

	if (scan_state.file_read_type == JSONFileReadType::SCAN_PARTIAL) {
		// if we are not scanning the entire file - copy the remainder of the previous buffer into this buffer
		// we don't need to do this for the first buffer
		// we do this after inserting the buffer in the map to ensure we can still read in parallel
		if (scan_state.buffer_index.GetIndex() != 0) {
			CopyRemainderFromPreviousBuffer(scan_state);
		}
	}

	scan_state.prev_buffer_remainder = 0;
	scan_state.lines_or_objects_in_buffer = 0;

	// YYJSON needs this
	memset(scan_state.buffer_ptr + scan_state.buffer_size, 0, YYJSON_PADDING_SIZE);
}

void JSONReader::DecrementBufferUsage(JSONBufferHandle &handle, idx_t lines_or_object_in_buffer,
                                      AllocatedData &buffer) {
	SetBufferLineOrObjectCount(handle, lines_or_object_in_buffer);
	if (--handle.readers == 0) {
		buffer = RemoveBuffer(handle);
	}
}

void JSONReader::PrepareForReadInternal(JSONReaderScanState &scan_state) {
	// clear the previous buffer handle
	scan_state.ClearBufferHandle();
	// if we don't have a buffer - allocate it
	if (!scan_state.read_buffer.IsSet()) {
		scan_state.read_buffer = scan_state.global_allocator.Allocate(scan_state.buffer_capacity);
		scan_state.buffer_ptr = char_ptr_cast(scan_state.read_buffer.get());
	} else if (scan_state.read_buffer.GetSize() < scan_state.buffer_capacity) {
		// the retained auto-detect buffer only holds what was read from the file - it cannot take a full read
		auto buffer = scan_state.global_allocator.Allocate(scan_state.buffer_capacity);
		if (scan_state.file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
			memcpy(buffer.get(), scan_state.buffer_ptr + scan_state.prev_buffer_offset,
			       scan_state.prev_buffer_remainder);
		}
		scan_state.read_buffer = std::move(buffer);
		scan_state.buffer_ptr = char_ptr_cast(scan_state.read_buffer.get());
		return;
	}
	if (scan_state.file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
		// Copy last bit of previous buffer to the beginning if we are doing a single-threaded read
		memmove(scan_state.buffer_ptr, scan_state.buffer_ptr + scan_state.prev_buffer_offset,
		        scan_state.prev_buffer_remainder);
	}
}
bool JSONReader::PrepareBufferForRead(JSONReaderScanState &scan_state) {
	if (auto_detect_data.IsSet()) {
		// we have auto-detected data - re-use the buffer
		if (next_buffer_index != 0 || auto_detect_data_size == 0 || scan_state.prev_buffer_remainder != 0) {
			throw InternalException("Invalid re-use of auto-detect data in JSON");
		}
		scan_state.buffer_index = GetBufferIndex();
		scan_state.buffer_size = auto_detect_data_size;
		scan_state.read_buffer = std::move(auto_detect_data);
		scan_state.buffer_ptr = char_ptr_cast(scan_state.read_buffer.get());
		scan_state.prev_buffer_remainder = 0;
		scan_state.needs_to_read = false;
		scan_state.is_last = false;
		scan_state.buffer_offset = 0;
		auto_detect_data.Reset();
		auto_detect_data_size = 0;
		return true;
	}
	if (scan_state.file_read_type == JSONFileReadType::SCAN_PARTIAL && GetFileHandle().CanSeek()) {
		// we can seek and are doing a parallel read - we don't need to read immediately yet
		// we only need to prepare the read now
		if (!PrepareBufferSeek(scan_state)) {
			return false;
		}
	} else {
		// we cannot seek - we need to read immediately here
		if (!ReadNextBufferNoSeek(scan_state)) {
			return false;
		}
	}
	return true;
}

bool JSONReader::PrepareBufferSeek(JSONReaderScanState &scan_state) {
	scan_state.request_size = scan_state.buffer_capacity / 2 - scan_state.prev_buffer_remainder - YYJSON_PADDING_SIZE;
	if (!IsOpen()) {
		return false;
	}
	auto &file_handle = GetFileHandle();

	if (file_handle.LastReadRequested()) {
		return false;
	}
	if (!file_handle.GetPositionAndSize(scan_state.read_position, scan_state.read_size, scan_state.request_size)) {
		return false; // We weren't able to read
	}
	scan_state.buffer_index = GetBufferIndex();
	scan_state.is_last = scan_state.read_size == 0;
	scan_state.needs_to_read = true;
	scan_state.buffer_size = 0;
	return true;
}

void JSONReader::ReadNextBufferSeek(JSONReaderScanState &scan_state) {
	PrepareForReadInternal(scan_state);

	// we start reading at "options.maximum_object_size" to leave space for data from the previous buffer
	idx_t read_offset = options.maximum_object_size;
	if (scan_state.read_size > 0) {
		auto &file_handle = GetFileHandle();
		{
			lock_guard<mutex> reader_guard(lock);
			auto &raw_handle = file_handle.GetHandle();
			// For non-on-disk files, we create a handle per thread: this is faster for e.g. S3Filesystem where
			// throttling per tcp connection can occur meaning that using multiple connections is faster.
			if (!raw_handle.OnDiskFile() && raw_handle.CanSeek()) {
				if (!scan_state.thread_local_filehandle ||
				    scan_state.thread_local_filehandle->GetPath() != raw_handle.GetPath()) {
					FileOpenFlags flags = FileFlags::FILE_FLAGS_READ | FileFlags::FILE_FLAGS_DIRECT_IO;
					flags.SetCachingMode(CachingMode::CACHE_REMOTE_ONLY);
					scan_state.thread_local_filehandle = scan_state.fs.OpenFile(file, flags);
				}
			} else if (scan_state.thread_local_filehandle) {
				scan_state.thread_local_filehandle = nullptr;
			}
		}

		// Now read the file lock-free!
		D_ASSERT(read_offset + scan_state.read_size + YYJSON_PADDING_SIZE <= scan_state.read_buffer.GetSize());
		file_handle.ReadAtPosition(scan_state.buffer_ptr + read_offset, scan_state.read_size, scan_state.read_position,
		                           scan_state.thread_local_filehandle);
	}
	scan_state.buffer_size = read_offset + scan_state.read_size;
	scan_state.buffer_offset = read_offset;
	scan_state.prev_buffer_remainder = 0;
}

bool JSONReader::ReadNextBufferNoSeek(JSONReaderScanState &scan_state) {
	idx_t read_offset;
	if (scan_state.file_read_type == JSONFileReadType::SCAN_ENTIRE_FILE) {
		read_offset = scan_state.prev_buffer_remainder;
	} else {
		// we start reading at "options.maximum_object_size" to leave space for data from the previous buffer
		read_offset = options.maximum_object_size;
	}
	idx_t request_size = scan_state.buffer_capacity - read_offset - YYJSON_PADDING_SIZE;
	idx_t read_size;

	if (!IsOpen()) {
		return false; // Couldn't read anything
	}
	auto &file_handle = GetFileHandle();
	if (file_handle.LastReadRequested()) {
		return false;
	}
	scan_state.buffer_index = GetBufferIndex();
	PrepareForReadInternal(scan_state);
	D_ASSERT(read_offset + request_size + YYJSON_PADDING_SIZE <= scan_state.read_buffer.GetSize());
	if (!file_handle.Read(scan_state.buffer_ptr + read_offset, read_size, request_size)) {
		return false; // Couldn't read anything
	}
	scan_state.is_last = read_size == 0;
	if (scan_state.is_last) {
		file_handle.Close();
	}
	scan_state.buffer_size = read_offset + read_size;
	if (scan_state.file_read_type == JSONFileReadType::SCAN_PARTIAL) {
		// if we are doing a partial read we don't have the reconstruction data yet
		scan_state.buffer_offset = read_offset;
	} else {
		scan_state.buffer_offset = 0;
	}
	scan_state.needs_to_read = false;
	scan_state.prev_buffer_remainder = 0;
	return true;
}

} // namespace duckdb
