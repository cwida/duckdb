#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/decimal_cast_operators.hpp"
#include "duckdb/common/operator/double_cast_operator.hpp"
#include "duckdb/common/operator/integer_cast_operator.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_casting.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/skip_scanner.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/client_data.hpp"
#include "utf8proc_wrapper.hpp"

#include <algorithm>

namespace duckdb {

constexpr idx_t StringValueScanner::LINE_FINDER_ID;

StringValueResult::StringValueResult(CSVStates &states, CSVStateMachine &state_machine,
                                     const shared_ptr<CSVBufferHandle> &buffer_handle, Allocator &buffer_allocator,
                                     idx_t result_size_p, idx_t buffer_position, CSVErrorHandler &error_handler_p,
                                     CSVIterator &iterator_p, shared_ptr<CSVFileScan> csv_file_scan_p,
                                     idx_t &lines_read_p, bool sniffing_p, const string &path_p, idx_t scan_id,
                                     bool &used_unstrictness)
    : ScannerResult(states, state_machine, result_size_p),
      number_of_columns(NumericCast<uint32_t>(state_machine.dialect_options.num_cols)),
      null_padding(state_machine.options.null_padding), ignore_errors(state_machine.options.ignore_errors.GetValue()),
      extra_delimiter_bytes(state_machine.dialect_options.state_machine_options.delimiter.GetValue().empty()
                                ? 0
                                : state_machine.dialect_options.state_machine_options.delimiter.GetValue().size() - 1),
      error_handler(error_handler_p), iterator(iterator_p), csv_file_scan(std::move(csv_file_scan_p)),
      lines_read(lines_read_p), used_unstrictness(used_unstrictness),
      current_errors(scan_id, state_machine.options.IgnoreErrors()), sniffing(sniffing_p), path(path_p) {
	// Vector information
	D_ASSERT(number_of_columns > 0);
	if (!buffer_handle) {
		// It Was Over Before It Even Began
		D_ASSERT(iterator.done);
		return;
	}
	buffer_handles[buffer_handle->buffer_idx] = buffer_handle;
	// Buffer Information
	buffer_ptr = buffer_handle->Ptr();
	buffer_size = buffer_handle->actual_size;
	last_position = {buffer_handle->buffer_idx, buffer_position, buffer_size};
	requested_size = buffer_handle->requested_size;
	// Current Result information
	current_line_position.begin = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, buffer_handle->actual_size};
	current_line_position.end = current_line_position.begin;
	// Fill out Parse Types
	vector<LogicalType> logical_types;
	parse_types = make_unsafe_uniq_array<ParseTypeInfo>(number_of_columns);
	LogicalType varchar_type = LogicalType::VARCHAR;
	if (!csv_file_scan) {
		for (idx_t i = 0; i < number_of_columns; i++) {
			parse_types[i] = ParseTypeInfo(varchar_type, true);
			logical_types.emplace_back(LogicalType::VARCHAR);
			string name = "Column_" + to_string(i);
			names.emplace_back(name);
		}
	} else {
		if (csv_file_scan->file_types.size() > number_of_columns) {
			throw InvalidInputException(
			    "Mismatch between the number of columns (%d) in the CSV file and what is expected in the scanner (%d).",
			    number_of_columns, csv_file_scan->file_types.size());
		}
		icu_loaded = csv_file_scan->buffer_manager->context.db->ExtensionIsLoaded("icu");
		for (idx_t i = 0; i < csv_file_scan->file_types.size(); i++) {
			auto type = csv_file_scan->file_types[i];
			if (type.IsJSONType()) {
				type = LogicalType::VARCHAR;
			}
			if (StringValueScanner::CanDirectlyCast(type, icu_loaded)) {
				parse_types[i] = ParseTypeInfo(type, true);
				logical_types.emplace_back(type);
			} else {
				parse_types[i] = ParseTypeInfo(varchar_type, type.id() == LogicalTypeId::VARCHAR || type.IsNested());
				logical_types.emplace_back(LogicalType::VARCHAR);
			}
		}
		names = csv_file_scan->GetNames();
		if (!csv_file_scan->projected_columns.empty()) {
			projecting_columns = false;
			projected_columns = make_unsafe_uniq_array<bool>(number_of_columns);
			for (idx_t col_idx = 0; col_idx < number_of_columns; col_idx++) {
				if (csv_file_scan->projected_columns.find(col_idx) == csv_file_scan->projected_columns.end()) {
					// Column is not projected
					projecting_columns = true;
					projected_columns[col_idx] = false;
				} else {
					projected_columns[col_idx] = true;
				}
			}
		}
		if (!projecting_columns) {
			for (idx_t j = logical_types.size(); j < number_of_columns; j++) {
				// This can happen if we have sneaky null columns at the end that we wish to ignore
				parse_types[j] = ParseTypeInfo(varchar_type, true);
				logical_types.emplace_back(LogicalType::VARCHAR);
			}
		}
	}

	// Initialize Parse Chunk
	parse_chunk.Initialize(buffer_allocator, logical_types, result_size);
	for (auto &col : parse_chunk.data) {
		vector_ptr.push_back(FlatVector::GetDataMutable(col));
		validity_mask.push_back(&FlatVector::ValidityMutable(col));
	}

	// Setup the NullStr information
	null_str_count = state_machine.options.null_str.size();
	null_str_ptr = make_unsafe_uniq_array_uninitialized<const char *>(null_str_count);
	null_str_size = make_unsafe_uniq_array_uninitialized<idx_t>(null_str_count);
	for (idx_t i = 0; i < null_str_count; i++) {
		null_str_ptr[i] = state_machine.options.null_str[i].c_str();
		null_str_size[i] = state_machine.options.null_str[i].size();
	}
	date_format = state_machine.options.dialect_options.date_format.at(LogicalTypeId::DATE).GetValue();
	timestamp_format = state_machine.options.dialect_options.date_format.at(LogicalTypeId::TIMESTAMP).GetValue();
	decimal_separator = state_machine.options.decimal_separator[0];

	if (iterator.first_one) {
		lines_read +=
		    state_machine.dialect_options.skip_rows.GetValue() + state_machine.dialect_options.header.GetValue();
		if (lines_read == 0) {
			SkipBOM();
		}
	}
	ignore_empty_values = state_machine.dialect_options.state_machine_options.delimiter.GetValue()[0] != ' ' &&
	                      state_machine.dialect_options.state_machine_options.quote != ' ' &&
	                      state_machine.dialect_options.state_machine_options.escape != ' ' &&
	                      state_machine.dialect_options.state_machine_options.comment != ' ';
}

StringValueResult::~StringValueResult() {
	// We have to insert the lines read by this scanner
	error_handler.Insert(iterator.GetBoundaryIdx(), lines_read);
	if (!iterator.done) {
		// Some operators, like Limit, might cause a future error to incorrectly report the wrong error line
		// Better to print nothing to print something wrong
		error_handler.DontPrintErrorLine();
	}
}

inline bool IsValueNull(const char *null_str_ptr, const char *value_ptr, const idx_t size) {
	for (idx_t i = 0; i < size; i++) {
		if (null_str_ptr[i] != value_ptr[i]) {
			return false;
		}
	}
	return true;
}

bool StringValueResult::HandleTooManyColumnsError(const char *value_ptr, const idx_t size) {
	if (cur_col_id >= number_of_columns) {
		if (state_machine.state_machine_options.strict_mode.GetValue()) {
			bool error = true;
			if (cur_col_id == number_of_columns && ((quoted && state_machine.options.allow_quoted_nulls) || !quoted)) {
				// we make an exception if the first over-value is null
				bool is_value_null = false;
				for (idx_t i = 0; i < null_str_count; i++) {
					is_value_null =
					    is_value_null || (size == null_str_size[i] && IsValueNull(null_str_ptr[i], value_ptr, size));
				}
				error = !is_value_null;
			}
			if (error) {
				// We error pointing to the current value error.
				current_errors.Insert(TOO_MANY_COLUMNS, cur_col_id, chunk_col_id, last_position);
				cur_col_id++;
			}
			// We had an error
			return true;
		}
		used_unstrictness = true;
	}
	return false;
}

void StringValueResult::SetComment(StringValueResult &result, idx_t buffer_pos) {
	if (!result.comment) {
		result.position_before_comment = buffer_pos;
		result.comment = true;
	}
}

bool StringValueResult::UnsetComment(StringValueResult &result, idx_t buffer_pos) {
	bool done = false;
	if (result.last_position.buffer_pos < result.position_before_comment) {
		bool all_empty = true;
		for (idx_t i = result.last_position.buffer_pos; i < result.position_before_comment; i++) {
			if (result.buffer_ptr[i] != ' ') {
				all_empty = false;
				break;
			}
		}
		if (!all_empty) {
			done = AddRow(result, result.position_before_comment);
		}
	} else {
		if (result.cur_col_id != 0) {
			done = AddRow(result, result.position_before_comment);
		}
	}
	if (result.number_of_rows == 0) {
		result.first_line_is_comment = true;
	}
	result.comment = false;
	if (result.state_machine.dialect_options.state_machine_options.new_line.GetValue() != NewLineIdentifier::CARRY_ON) {
		result.last_position.buffer_pos = buffer_pos + 1;
	} else {
		result.last_position.buffer_pos = buffer_pos + 2;
	}
	LinePosition current_line_start = {result.iterator.pos.buffer_idx, result.iterator.pos.buffer_pos,
	                                   result.buffer_size};
	result.current_line_position.begin = result.current_line_position.end;
	result.current_line_position.end = current_line_start;
	result.cur_col_id = 0;
	result.chunk_col_id = 0;
	return done;
}

void FullLinePosition::SanitizeError(string &value) {
	std::vector<char> char_array(value.begin(), value.end());
	char_array.push_back('\0'); // Null-terminate the character array
	Utf8Proc::MakeValid(&char_array[0], char_array.size());
	value = {char_array.begin(), char_array.end() - 1};
}

void StringValueResult::AddValueToVector(const char *value_ptr, idx_t size, bool allocate) {
	if (HandleTooManyColumnsError(value_ptr, size)) {
		return;
	}
	if (cur_col_id >= number_of_columns) {
		if (!state_machine.state_machine_options.strict_mode.GetValue()) {
			used_unstrictness = true;
			return;
		}
		bool error = true;
		if (cur_col_id == number_of_columns && ((quoted && state_machine.options.allow_quoted_nulls) || !quoted)) {
			// we make an exception if the first over-value is null
			bool is_value_null = false;
			for (idx_t i = 0; i < null_str_count; i++) {
				is_value_null =
				    is_value_null || (size == null_str_size[i] && IsValueNull(null_str_ptr[i], value_ptr, size));
			}
			error = !is_value_null;
		}
		if (error) {
			// We error pointing to the current value error.
			current_errors.Insert(TOO_MANY_COLUMNS, cur_col_id, chunk_col_id, last_position);
			cur_col_id++;
		}
		return;
	}

	if (projecting_columns) {
		if (!projected_columns[cur_col_id]) {
			cur_col_id++;
			return;
		}
	}

	if (((quoted && state_machine.options.allow_quoted_nulls) || !quoted)) {
		// Check for the occurrence of escaped null string like \N only if strict_mode is disabled
		const bool check_unquoted_escaped_null =
		    state_machine.state_machine_options.strict_mode.GetValue() == false && escaped && !quoted && size == 1;
		for (idx_t i = 0; i < null_str_count; i++) {
			bool is_null = false;
			if (null_str_size[i] == 2 && null_str_ptr[i][0] == state_machine.state_machine_options.escape.GetValue()) {
				is_null = check_unquoted_escaped_null && null_str_ptr[i][1] == value_ptr[0];
			} else if (size == null_str_size[i] && !check_unquoted_escaped_null) {
				is_null = IsValueNull(null_str_ptr[i], value_ptr, size);
			}
			if (is_null) {
				bool empty = false;
				if (chunk_col_id < state_machine.options.force_not_null.size()) {
					empty = state_machine.options.force_not_null[chunk_col_id];
				}
				if (empty) {
					if (parse_types[chunk_col_id].type_id != LogicalTypeId::VARCHAR) {
						// If it is not a varchar, empty values are not accepted, we must error.
						current_errors.Insert(CAST_ERROR, cur_col_id, chunk_col_id, last_position);
					} else {
						static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] = string_t();
					}
				} else {
					if (chunk_col_id == number_of_columns) {
						// We check for a weird case, where we ignore an extra value, if it is a null value
						return;
					}
					validity_mask[chunk_col_id]->SetInvalid(static_cast<idx_t>(number_of_rows));
				}
				cur_col_id++;
				chunk_col_id++;
				return;
			}
		}
	}
	bool success = true;
	string strip_thousands;
	// Whether current type is logically numeric (BIGNUM is physically stored as VARCHAR instead of numerical value).
	if ((LogicalType::IsNumeric(parse_types[chunk_col_id].type_id) ||
	     parse_types[chunk_col_id].type_id == LogicalTypeId::BIGNUM) &&
	    state_machine.options.thousands_separator != '\0') {
		// If we have a thousands separator we should try to use that
		strip_thousands = BaseScanner::RemoveSeparator(value_ptr, size, state_machine.options.thousands_separator);
		value_ptr = strip_thousands.c_str();
		size = strip_thousands.size();
	}
	switch (parse_types[chunk_col_id].type_id) {
	case LogicalTypeId::BOOLEAN:
		success =
		    TryCastStringBool(value_ptr, size, static_cast<bool *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::TINYINT:
		success = TrySimpleIntegerCast(value_ptr, size, static_cast<int8_t *>(vector_ptr[chunk_col_id])[number_of_rows],
		                               false);
		break;
	case LogicalTypeId::SMALLINT:
		success = TrySimpleIntegerCast(value_ptr, size,
		                               static_cast<int16_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::INTEGER:
		success = TrySimpleIntegerCast(value_ptr, size,
		                               static_cast<int32_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::BIGINT:
		success = TrySimpleIntegerCast(value_ptr, size,
		                               static_cast<int64_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::HUGEINT: {
		auto &result_value = static_cast<hugeint_t *>(vector_ptr[chunk_col_id])[number_of_rows];
		success = TryCast::Operation<string_t, hugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(size)),
		                                                  result_value, false);
		break;
	}
	case LogicalTypeId::UTINYINT:
		success = TrySimpleIntegerCast<uint8_t, false>(
		    value_ptr, size, static_cast<uint8_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::USMALLINT:
		success = TrySimpleIntegerCast<uint16_t, false>(
		    value_ptr, size, static_cast<uint16_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::UINTEGER:
		success = TrySimpleIntegerCast<uint32_t, false>(
		    value_ptr, size, static_cast<uint32_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::UBIGINT:
		success = TrySimpleIntegerCast<uint64_t, false>(
		    value_ptr, size, static_cast<uint64_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	case LogicalTypeId::UHUGEINT: {
		auto &result_value = static_cast<uhugeint_t *>(vector_ptr[chunk_col_id])[number_of_rows];
		success = TryCast::Operation<string_t, uhugeint_t>(string_t(value_ptr, NumericCast<uint32_t>(size)),
		                                                   result_value, false);
		break;
	}
	case LogicalTypeId::BIGNUM: {
		try {
			while (size > 0 && StringUtil::CharacterIsSpace(*value_ptr)) {
				value_ptr++;
				size--;
			}
			while (size > 0 && StringUtil::CharacterIsSpace(value_ptr[size - 1])) {
				size--;
			}
			auto bignum = Bignum::VarcharToBignum(string_t(value_ptr, NumericCast<uint32_t>(size)));
			static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] =
			    StringVector::AddStringOrBlob(parse_chunk.data[chunk_col_id], bignum.data(), bignum.size());
		} catch (const ConversionException &) {
			success = false;
		}
		break;
	}
	case LogicalTypeId::DOUBLE:
		success =
		    TryDoubleCast<double>(value_ptr, size, static_cast<double *>(vector_ptr[chunk_col_id])[number_of_rows],
		                          false, state_machine.options.decimal_separator[0]);
		break;
	case LogicalTypeId::FLOAT:
		success = TryDoubleCast<float>(value_ptr, size, static_cast<float *>(vector_ptr[chunk_col_id])[number_of_rows],
		                               false, state_machine.options.decimal_separator[0]);
		break;
	case LogicalTypeId::DATE: {
		if (!date_format.Empty()) {
			success = date_format.TryParseDate(value_ptr, size,
			                                   static_cast<date_t *>(vector_ptr[chunk_col_id])[number_of_rows]);
		} else {
			idx_t pos;
			bool special;
			success = Date::TryConvertDate(value_ptr, size, pos,
			                               static_cast<date_t *>(vector_ptr[chunk_col_id])[number_of_rows], special,
			                               false) == DateCastResult::SUCCESS;
		}
		break;
	}
	case LogicalTypeId::TIME: {
		idx_t pos;
		success = Time::TryConvertTime(value_ptr, size, pos,
		                               static_cast<dtime_t *>(vector_ptr[chunk_col_id])[number_of_rows], false);
		break;
	}
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ: {
		if (!timestamp_format.Empty()) {
			success = timestamp_format.TryParseTimestamp(
			    value_ptr, size, static_cast<timestamp_t *>(vector_ptr[chunk_col_id])[number_of_rows]);
		} else {
			const bool use_offset = (parse_types[chunk_col_id].type_id == LogicalTypeId::TIMESTAMP_TZ);
			success = Timestamp::TryConvertTimestamp(
			              value_ptr, size, static_cast<timestamp_t *>(vector_ptr[chunk_col_id])[number_of_rows],
			              use_offset) == TimestampCastResult::SUCCESS;
		}
		break;
	}
	case LogicalTypeId::DECIMAL: {
		if (decimal_separator == ',') {
			switch (parse_types[chunk_col_id].internal_type) {
			case PhysicalType::INT16:
				success = TryDecimalStringCast<int16_t, ','>(
				    value_ptr, size, static_cast<int16_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				    parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT32:
				success = TryDecimalStringCast<int32_t, ','>(
				    value_ptr, size, static_cast<int32_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				    parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT64:
				success = TryDecimalStringCast<int64_t, ','>(
				    value_ptr, size, static_cast<int64_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				    parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT128:
				success = TryDecimalStringCast<hugeint_t, ','>(
				    value_ptr, size, static_cast<hugeint_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				    parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(parse_types[chunk_col_id].internal_type));
			}

		} else if (decimal_separator == '.') {
			switch (parse_types[chunk_col_id].internal_type) {
			case PhysicalType::INT16:
				success = TryDecimalStringCast(value_ptr, size,
				                               static_cast<int16_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				                               parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT32:
				success = TryDecimalStringCast(value_ptr, size,
				                               static_cast<int32_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				                               parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT64:
				success = TryDecimalStringCast(value_ptr, size,
				                               static_cast<int64_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				                               parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			case PhysicalType::INT128:
				success = TryDecimalStringCast(value_ptr, size,
				                               static_cast<hugeint_t *>(vector_ptr[chunk_col_id])[number_of_rows],
				                               parse_types[chunk_col_id].width, parse_types[chunk_col_id].scale);
				break;
			default:
				throw InternalException("Invalid Physical Type for Decimal Value. Physical Type: " +
				                        TypeIdToString(parse_types[chunk_col_id].internal_type));
			}
		} else {
			throw InvalidInputException("Decimals can only have ',' and '.' as decimal separators");
		}
		break;
	}
	default: {
		// By default, we add a string
		// We only evaluate if a string is utf8 valid, if it's actually a varchar
		if (parse_types[chunk_col_id].validate_utf8 &&
		    !Utf8Proc::IsValid(value_ptr, UnsafeNumericCast<uint32_t>(size))) {
			bool force_error = !state_machine.options.ignore_errors.GetValue() && sniffing;
			// Invalid unicode, we must error
			if (force_error) {
				HandleUnicodeError(cur_col_id, last_position);
			}
			// If we got here, we are ignoring errors, hence we must ignore this line.
			current_errors.Insert(INVALID_ENCODING, cur_col_id, chunk_col_id, last_position);
			static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] = StringVector::AddStringOrBlob(
			    parse_chunk.data[chunk_col_id], string_t(value_ptr, UnsafeNumericCast<uint32_t>(0)));
			break;
		}
		if (allocate) {
			// If it's a value produced over multiple buffers, we must allocate
			static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] = StringVector::AddStringOrBlob(
			    parse_chunk.data[chunk_col_id], string_t(value_ptr, UnsafeNumericCast<uint32_t>(size)));
		} else {
			static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] =
			    string_t(value_ptr, UnsafeNumericCast<uint32_t>(size));
		}
		break;
	}
	}
	if (!success) {
		current_errors.Insert(CAST_ERROR, cur_col_id, chunk_col_id, last_position);
		if (!state_machine.options.IgnoreErrors()) {
			// We have to write the cast error message.
			std::ostringstream error;
			// Casting Error Message
			error << "Could not convert string \"" << std::string(value_ptr, size) << "\" to \'"
			      << LogicalTypeIdToString(parse_types[chunk_col_id].type_id) << "\'";
			auto error_string = error.str();
			FullLinePosition::SanitizeError(error_string);

			current_errors.ModifyErrorMessageOfLastError(error_string);
		}
	}
	cur_col_id++;
	chunk_col_id++;
}

DataChunk &StringValueResult::ToChunk() {
	if (number_of_rows < 0) {
		throw InternalException("CSVScanner: ToChunk() function. Has a negative number of rows, this indicates an "
		                        "issue with the error handler.");
	}
	const idx_t n = static_cast<idx_t>(number_of_rows);
	if (parse_chunk.size() != n) {
		parse_chunk.SetChildCardinality(n);
	}
	return parse_chunk;
}

void StringValueResult::Reset() {
	if (number_of_rows == 0) {
		return;
	}
	number_of_rows = 0;
	cur_col_id = 0;
	chunk_col_id = 0;
	for (auto &v : validity_mask) {
		v->SetAllValid(result_size);
	}
	// We keep a reference to the buffer from our current iteration if it already exists
	shared_ptr<CSVBufferHandle> cur_buffer;
	auto handle_iter = buffer_handles.find(iterator.GetBufferIdx());
	if (handle_iter != buffer_handles.end()) {
		cur_buffer = handle_iter->second;
	}
	buffer_handles.clear();
	idx_t actual_size = 0;
	if (cur_buffer) {
		buffer_handles[cur_buffer->buffer_idx] = cur_buffer;
		actual_size = cur_buffer->actual_size;
	}
	current_errors.Reset();
	borked_rows.clear();
	current_line_position.begin = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, actual_size};
	current_line_position.end = current_line_position.begin;
}

void StringValueResult::AddQuotedValue(StringValueResult &result, const idx_t buffer_pos) {
	if (!result.unquoted) {
		result.current_errors.Insert(UNTERMINATED_QUOTES, result.cur_col_id, result.chunk_col_id, result.last_position);
	}
	if (buffer_pos <= result.quoted_position + 1) {
		AddPossiblyEscapedValue(result, buffer_pos, result.buffer_ptr + result.quoted_position + 1, 0,
		                        buffer_pos < result.last_position.buffer_pos + 2);
		result.quoted = false;
		return;
	}
	// remove potential empty values
	idx_t length = buffer_pos - result.quoted_position - 1;
	while (length > 0 && result.ignore_empty_values &&
	       result.buffer_ptr[result.quoted_position + 1 + length - 1] == ' ') {
		length--;
	}
	if (length == 0) {
		// All content was stripped as empty/space values
		AddPossiblyEscapedValue(result, buffer_pos, result.buffer_ptr + result.quoted_position + 1, 0, true);
		result.quoted = false;
		return;
	}
	length--;
	AddPossiblyEscapedValue(result, buffer_pos, result.buffer_ptr + result.quoted_position + 1, length,
	                        buffer_pos < result.last_position.buffer_pos + 2);
	result.quoted = false;
}

void StringValueResult::AddPossiblyEscapedValue(StringValueResult &result, const idx_t buffer_pos,
                                                const char *value_ptr, const idx_t length, const bool empty) {
	if (result.escaped) {
		if (result.projecting_columns && result.cur_col_id < result.number_of_columns) {
			if (!result.projected_columns[result.cur_col_id]) {
				result.cur_col_id++;
				result.escaped = false;
				return;
			}
		}
		if (result.cur_col_id >= result.number_of_columns &&
		    !result.state_machine.state_machine_options.strict_mode.GetValue()) {
			result.used_unstrictness = true;
			return;
		}
		if (!result.HandleTooManyColumnsError(value_ptr, length)) {
			// If it's an escaped value we have to remove all the escapes, this is not really great
			// If we are going to escape, this vector must be a varchar vector
			if (result.parse_chunk.data[result.chunk_col_id].GetType() != LogicalType::VARCHAR) {
				result.current_errors.Insert(CAST_ERROR, result.cur_col_id, result.chunk_col_id, result.last_position);
				if (!result.state_machine.options.IgnoreErrors()) {
					// We have to write the cast error message.
					std::ostringstream error;
					// Casting Error Message
					error << "Could not convert string \"" << std::string(value_ptr, length) << "\" to \'"
					      << LogicalTypeIdToString(result.parse_types[result.chunk_col_id].type_id) << "\'";
					auto error_string = error.str();
					FullLinePosition::SanitizeError(error_string);
					result.current_errors.ModifyErrorMessageOfLastError(error_string);
				}
				result.cur_col_id++;
				result.chunk_col_id++;
			} else {
				auto value = StringValueScanner::RemoveEscape(
				    value_ptr, length, result.state_machine.dialect_options.state_machine_options.escape.GetValue(),
				    result.state_machine.dialect_options.state_machine_options.quote.GetValue(),
				    result.state_machine.dialect_options.state_machine_options.strict_mode.GetValue(),
				    result.parse_chunk.data[result.chunk_col_id]);
				result.AddValueToVector(value.GetData(), value.GetSize());
			}
		}
	} else {
		if (empty) {
			// empty value
			auto value = string_t();
			result.AddValueToVector(value.GetData(), value.GetSize());
		} else {
			result.AddValueToVector(value_ptr, length);
		}
	}
	result.escaped = false;
}

inline idx_t StringValueResult::HandleMultiDelimiter(const idx_t buffer_pos) const {
	idx_t size = buffer_pos - last_position.buffer_pos - extra_delimiter_bytes;
	if (buffer_pos < last_position.buffer_pos + extra_delimiter_bytes) {
		// If this is a scenario where the value is null, that is fine (e.g., delim = '||' and line is: A||)
		if (buffer_pos == last_position.buffer_pos) {
			size = 0;
		} else {
			// Otherwise something went wrong.
			throw InternalException(
			    "Value size is lower than the number of extra delimiter bytes in the HandleMultiDelimiter(). "
			    "buffer_pos = %d, last_position.buffer_pos = %d, extra_delimiter_bytes = %d",
			    buffer_pos, last_position.buffer_pos, extra_delimiter_bytes);
		}
	}
	return size;
}

void StringValueResult::AddValue(StringValueResult &result, const idx_t buffer_pos) {
	if (result.last_position.buffer_pos > buffer_pos) {
		return;
	}
	if (result.quoted) {
		AddQuotedValue(result, buffer_pos - result.extra_delimiter_bytes);
	} else if (result.escaped) {
		AddPossiblyEscapedValue(result, buffer_pos, result.buffer_ptr + result.last_position.buffer_pos,
		                        result.HandleMultiDelimiter(buffer_pos), false);
	} else {
		result.AddValueToVector(result.buffer_ptr + result.last_position.buffer_pos,
		                        result.HandleMultiDelimiter(buffer_pos));
	}
	result.last_position.buffer_pos = buffer_pos + 1;
}

void StringValueResult::HandleUnicodeError(idx_t col_idx, LinePosition &error_position) {
	bool first_nl = false;
	auto borked_line = current_line_position.ReconstructCurrentLine(first_nl, buffer_handles, PrintErrorLine());
	LinesPerBoundary lines_per_batch(iterator.GetBoundaryIdx(), lines_read);
	if (current_line_position.begin == error_position) {
		auto csv_error = CSVError::InvalidUTF8(state_machine.options, col_idx, lines_per_batch, borked_line,
		                                       current_line_position.begin.GetGlobalPosition(requested_size, first_nl),
		                                       error_position.GetGlobalPosition(requested_size, first_nl), path);
		error_handler.Error(csv_error, true);
	} else {
		auto csv_error = CSVError::InvalidUTF8(state_machine.options, col_idx, lines_per_batch, borked_line,
		                                       current_line_position.begin.GetGlobalPosition(requested_size, first_nl),
		                                       error_position.GetGlobalPosition(requested_size), path);
		error_handler.Error(csv_error, true);
	}
}

bool LineError::HandleErrors(StringValueResult &result) {
	bool skip_sniffing = false;
	for (auto &cur_error : current_errors) {
		if (cur_error.type == CSVErrorType::INVALID_ENCODING) {
			skip_sniffing = true;
		}
	}
	skip_sniffing = result.sniffing && skip_sniffing;

	if ((ignore_errors || skip_sniffing) && is_error_in_line && !result.figure_out_new_line) {
		result.RemoveLastLine();
		Reset();
		return true;
	}
	// Reconstruct CSV Line
	for (auto &cur_error : current_errors) {
		LinesPerBoundary lines_per_batch(result.iterator.GetBoundaryIdx(), result.lines_read);
		bool first_nl = false;
		auto borked_line = result.current_line_position.ReconstructCurrentLine(first_nl, result.buffer_handles,
		                                                                       result.PrintErrorLine());
		CSVError csv_error;
		auto col_idx = cur_error.col_idx;
		auto &line_pos = cur_error.error_position;

		switch (cur_error.type) {
		case TOO_MANY_COLUMNS:
		case TOO_FEW_COLUMNS:
			if (result.current_line_position.begin == line_pos) {
				csv_error = CSVError::IncorrectColumnAmountError(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size, first_nl), result.path);
			} else {
				csv_error = CSVError::IncorrectColumnAmountError(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size), result.path);
			}
			break;
		case INVALID_ENCODING: {
			if (result.current_line_position.begin == line_pos) {
				csv_error = CSVError::InvalidUTF8(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size, first_nl), result.path);
			} else {
				csv_error = CSVError::InvalidUTF8(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size), result.path);
			}
			// the file types are those of the projected columns
			auto &file_types = result.csv_file_scan->file_types;
			if (cur_error.chunk_idx < file_types.size() &&
			    !StringValueScanner::CanDirectlyCast(file_types[cur_error.chunk_idx], result.icu_loaded)) {
				result.number_of_rows--;
			}
			break;
		}
		case UNTERMINATED_QUOTES:
			if (result.current_line_position.begin == line_pos) {
				csv_error = CSVError::UnterminatedQuotesError(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size, first_nl), result.path);
			} else {
				csv_error = CSVError::UnterminatedQuotesError(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size), result.path);
			}
			break;
		case CAST_ERROR: {
			string column_name;
			LogicalTypeId type_id = LogicalTypeId::INVALID;
			if (cur_error.col_idx < result.names.size()) {
				column_name = result.names[cur_error.col_idx];
			}
			if (cur_error.col_idx < result.number_of_columns) {
				type_id = result.parse_types[cur_error.chunk_idx].type_id;
			}
			if (result.current_line_position.begin == line_pos) {
				csv_error = CSVError::CastError(
				    result.state_machine.options, column_name, cur_error.error_message, cur_error.col_idx, borked_line,
				    lines_per_batch,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size, first_nl), type_id, result.path);
			} else {
				csv_error = CSVError::CastError(
				    result.state_machine.options, column_name, cur_error.error_message, cur_error.col_idx, borked_line,
				    lines_per_batch,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size), type_id, result.path);
			}
		} break;
		case MAXIMUM_LINE_SIZE:
			csv_error = CSVError::LineSizeError(
			    result.state_machine.options, lines_per_batch, borked_line,
			    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl), result.path);
			break;
		case INVALID_STATE:
			if (result.current_line_position.begin == line_pos) {
				csv_error = CSVError::InvalidState(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size, first_nl), result.path);
			} else {
				csv_error = CSVError::InvalidState(
				    result.state_machine.options, col_idx, lines_per_batch, borked_line,
				    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
				    line_pos.GetGlobalPosition(result.requested_size), result.path);
			}
			break;
		default:
			throw InvalidInputException("CSV Error not allowed when inserting row");
		}
		result.error_handler.Error(csv_error, result.try_row);
	}
	if (is_error_in_line && scan_id != StringValueScanner::LINE_FINDER_ID) {
		if (result.sniffing) {
			// If we are sniffing we just remove the line
			result.RemoveLastLine();
		} else {
			// Otherwise, we add it to the borked rows to remove it later and just cleanup the column variables.
			result.borked_rows.insert(static_cast<idx_t>(result.number_of_rows));
			result.cur_col_id = 0;
			result.chunk_col_id = 0;
		}
		Reset();
		return true;
	}
	return false;
}

void StringValueResult::QuotedNewLine(StringValueResult &result) {
	result.quoted_new_line = true;
}

void StringValueResult::NullPaddingQuotedNewlineCheck() const {
	// We do some checks for null_padding correctness
	if (state_machine.options.null_padding && iterator.IsBoundarySet() && quoted_new_line) {
		// If we have null_padding set, we found a quoted new line, we are scanning the file in parallel; We error.
		LinesPerBoundary lines_per_batch(iterator.GetBoundaryIdx(), lines_read);
		auto csv_error = CSVError::NullPaddingFail(state_machine.options, lines_per_batch, path);
		error_handler.Error(csv_error, true);
	}
}

void StringValueResult::InvalidateUnwrittenColumns(idx_t first_unwritten_col) {
	// A borked row is sliced out of the chunk later on, but the slice still spans it - the columns we never got around
	// to writing must not be left as uninitialized string_t values with their valid bit set
	if (borked_rows.find(static_cast<idx_t>(number_of_rows)) == borked_rows.end()) {
		return;
	}
	for (idx_t col_idx = first_unwritten_col; col_idx < validity_mask.size(); col_idx++) {
		validity_mask[col_idx]->SetInvalid(static_cast<idx_t>(number_of_rows));
	}
}

bool StringValueResult::AddRowInternal() {
	LinePosition current_line_start = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, buffer_size};
	idx_t current_line_size = current_line_start - current_line_position.end;
	current_line_position.begin = current_line_position.end;
	current_line_position.end = current_line_start;
	if (current_line_size > state_machine.options.maximum_line_size.GetValue()) {
		current_errors.Insert(MAXIMUM_LINE_SIZE, 1, chunk_col_id, last_position, current_line_size);
	}
	if (!state_machine.options.null_padding) {
		for (idx_t col_idx = cur_col_id; col_idx < number_of_columns; col_idx++) {
			current_errors.Insert(TOO_FEW_COLUMNS, col_idx - 1, chunk_col_id, last_position);
		}
	}

	const auto chunk_col_id_before = chunk_col_id;
	if (current_errors.HandleErrors(*this)) {
		InvalidateUnwrittenColumns(chunk_col_id_before);

		D_ASSERT(buffer_handles.find(current_line_position.begin.buffer_idx) != buffer_handles.end());
		D_ASSERT(buffer_handles.find(current_line_position.end.buffer_idx) != buffer_handles.end());
		line_positions_per_row[static_cast<idx_t>(number_of_rows)] = current_line_position;
		number_of_rows++;
		if (static_cast<idx_t>(number_of_rows) >= result_size) {
			// We have a full chunk
			return true;
		}
		return false;
	}
	NullPaddingQuotedNewlineCheck();
	quoted_new_line = false;
	// We need to check if we are getting the correct number of columns here.
	// If columns are correct, we add it, and that's it.
	if (cur_col_id < number_of_columns) {
		// We have too few columns:
		if (null_padding) {
			while (cur_col_id < number_of_columns) {
				bool empty = false;
				if (cur_col_id < state_machine.options.force_not_null.size()) {
					empty = state_machine.options.force_not_null[cur_col_id];
				}
				if (projecting_columns) {
					if (!projected_columns[cur_col_id]) {
						cur_col_id++;
						continue;
					}
				}
				if (empty) {
					static_cast<string_t *>(vector_ptr[chunk_col_id])[number_of_rows] = string_t();
				} else {
					validity_mask[chunk_col_id]->SetInvalid(static_cast<idx_t>(number_of_rows));
				}
				cur_col_id++;
				chunk_col_id++;
			}
		} else {
			// If we are not null-padding this is an error
			if (!state_machine.options.IgnoreErrors()) {
				bool first_nl = false;
				auto borked_line =
				    current_line_position.ReconstructCurrentLine(first_nl, buffer_handles, PrintErrorLine());
				LinesPerBoundary lines_per_batch(iterator.GetBoundaryIdx(), lines_read);
				if (current_line_position.begin == last_position) {
					auto csv_error = CSVError::IncorrectColumnAmountError(
					    state_machine.options, cur_col_id - 1, lines_per_batch, borked_line,
					    current_line_position.begin.GetGlobalPosition(requested_size, first_nl),
					    last_position.GetGlobalPosition(requested_size, first_nl), path);
					error_handler.Error(csv_error, try_row);
				} else {
					auto csv_error = CSVError::IncorrectColumnAmountError(
					    state_machine.options, cur_col_id - 1, lines_per_batch, borked_line,
					    current_line_position.begin.GetGlobalPosition(requested_size, first_nl),
					    last_position.GetGlobalPosition(requested_size), path);
					error_handler.Error(csv_error, try_row);
				}
			}
			// If we are here we ignore_errors, so we delete this line
			RemoveLastLine();
		}
	}
	D_ASSERT(buffer_handles.find(current_line_position.begin.buffer_idx) != buffer_handles.end());
	D_ASSERT(buffer_handles.find(current_line_position.end.buffer_idx) != buffer_handles.end());
	line_positions_per_row[static_cast<idx_t>(number_of_rows)] = current_line_position;
	cur_col_id = 0;
	chunk_col_id = 0;
	number_of_rows++;
	if (static_cast<idx_t>(number_of_rows) >= result_size) {
		// We have a full chunk
		return true;
	}
	return false;
}

bool StringValueResult::AddRow(StringValueResult &result, const idx_t buffer_pos) {
	if (result.last_position.buffer_pos <= buffer_pos) {
		// We add the value
		if (result.quoted) {
			AddQuotedValue(result, buffer_pos);
		} else {
			const char *value_ptr = result.buffer_ptr + result.last_position.buffer_pos;
			idx_t size = buffer_pos - result.last_position.buffer_pos;
			if (result.escaped) {
				AddPossiblyEscapedValue(result, buffer_pos, value_ptr, size, size == 0);
			} else {
				result.AddValueToVector(value_ptr, size);
			}
		}
		if (result.state_machine.dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON) {
			if (result.states.states[1] == CSVState::RECORD_SEPARATOR) {
				// Even though this is marked as a carry on, this is a hippie mixie
				result.last_position.buffer_pos = buffer_pos + 1;
			} else {
				result.last_position.buffer_pos = buffer_pos + 2;
			}
		} else {
			result.last_position.buffer_pos = buffer_pos + 1;
		}
	}

	// We add the value
	return result.AddRowInternal();
}

void StringValueResult::InvalidState(StringValueResult &result) {
	if (result.quoted) {
		result.current_errors.Insert(UNTERMINATED_QUOTES, result.cur_col_id, result.chunk_col_id, result.last_position);
	} else {
		LinesPerBoundary lines_per_batch(result.iterator.GetBoundaryIdx(), result.lines_read);
		bool first_nl = false;
		auto borked_line = result.current_line_position.ReconstructCurrentLine(first_nl, result.buffer_handles,
		                                                                       result.PrintErrorLine());
		CSVError csv_error;
		auto error = CSVError::InvalidState(
		    result.state_machine.options, result.cur_col_id, lines_per_batch, borked_line,
		    result.current_line_position.begin.GetGlobalPosition(result.requested_size, first_nl),
		    result.last_position.GetGlobalPosition(result.requested_size, first_nl), result.path);
		result.error_handler.Error(error, true);
	}
}

bool StringValueResult::EmptyLine(StringValueResult &result, const idx_t buffer_pos) {
	// We care about empty lines if this is a single column csv file
	result.last_position = {result.iterator.pos.buffer_idx, result.iterator.pos.buffer_pos + 1, result.buffer_size};
	if (result.states.IsCarriageReturn() &&
	    result.state_machine.dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON) {
		result.last_position.buffer_pos++;
	}
	if (result.number_of_columns == 1) {
		bool empty_is_null = false;
		for (idx_t i = 0; i < result.null_str_count && !empty_is_null; i++) {
			empty_is_null = result.null_str_size[i] == 0;
		}
		if (empty_is_null) {
			bool empty = false;
			if (!result.state_machine.options.force_not_null.empty()) {
				empty = result.state_machine.options.force_not_null[0];
			}
			if (empty) {
				static_cast<string_t *>(result.vector_ptr[0])[result.number_of_rows] = string_t();
			} else {
				result.validity_mask[0]->SetInvalid(static_cast<idx_t>(result.number_of_rows));
			}
			result.number_of_rows++;
		}
		if (static_cast<idx_t>(result.number_of_rows) >= result.result_size) {
			// We have a full chunk
			return true;
		}
	}
	return false;
}

StringValueScanner::StringValueScanner(idx_t scanner_idx_p, const shared_ptr<CSVBufferManager> &buffer_manager,
                                       const shared_ptr<CSVStateMachine> &state_machine,
                                       const shared_ptr<CSVErrorHandler> &error_handler,
                                       const shared_ptr<CSVFileScan> &csv_file_scan, bool sniffing,
                                       const CSVIterator &boundary, idx_t result_size, bool can_suspend_p)
    : BaseScanner(buffer_manager, state_machine, error_handler, sniffing, csv_file_scan, boundary),
      scanner_idx(scanner_idx_p),
      result(states, *state_machine, cur_buffer_handle, BufferAllocator::Get(buffer_manager->context), result_size,
             iterator.pos.buffer_pos, *error_handler, iterator, csv_file_scan, lines_read, sniffing,
             buffer_manager->GetFilePath(), scanner_idx_p, used_unstrictness),
      start_pos(0), can_suspend(can_suspend_p) {
	if (scanner_idx == 0 && csv_file_scan) {
		lines_read += csv_file_scan->skipped_rows;
	}
	iterator.buffer_size = state_machine->options.buffer_size_option.GetValue();
	result.try_row = scanner_idx == LINE_FINDER_ID;
}

StringValueScanner::StringValueScanner(const shared_ptr<CSVBufferManager> &buffer_manager,
                                       const shared_ptr<CSVStateMachine> &state_machine,
                                       const shared_ptr<CSVErrorHandler> &error_handler, idx_t result_size,
                                       const CSVIterator &boundary)
    : BaseScanner(buffer_manager, state_machine, error_handler, false, nullptr, boundary), scanner_idx(0),
      result(states, *state_machine, cur_buffer_handle, Allocator::DefaultAllocator(), result_size,
             iterator.pos.buffer_pos, *error_handler, iterator, csv_file_scan, lines_read, sniffing,
             buffer_manager->GetFilePath(), 0, used_unstrictness),
      start_pos(0), can_suspend(false) {
	if (scanner_idx == 0 && csv_file_scan) {
		lines_read += csv_file_scan->skipped_rows;
	}
	iterator.buffer_size = state_machine->options.buffer_size_option.GetValue();
}

unique_ptr<StringValueScanner> StringValueScanner::GetCSVScanner(ClientContext &context, CSVReaderOptions &options,
                                                                 const MultiFileOptions &file_options) {
	auto state_machine = make_shared_ptr<CSVStateMachine>(options, options.dialect_options.state_machine_options,
	                                                      CSVStateMachineCache::Get(context));

	state_machine->dialect_options.num_cols = options.dialect_options.num_cols;
	state_machine->dialect_options.header = options.dialect_options.header;
	auto buffer_manager = CSVBufferManager::Open(context, options, options.file_path, false);
	idx_t rows_to_skip = state_machine->options.GetSkipRows() + state_machine->options.GetHeader();
	rows_to_skip = std::max(rows_to_skip, state_machine->dialect_options.rows_until_header +
	                                          state_machine->dialect_options.header.GetValue());
	auto it = BaseScanner::SkipCSVRows(buffer_manager, state_machine, rows_to_skip);
	auto scanner = make_uniq<StringValueScanner>(buffer_manager, state_machine, make_shared_ptr<CSVErrorHandler>(),
	                                             STANDARD_VECTOR_SIZE, it);
	scanner->csv_file_scan = make_shared_ptr<CSVFileScan>(context, options.file_path, options, file_options);
	scanner->csv_file_scan->InitializeProjection();
	return scanner;
}

bool StringValueScanner::FinishedIterator() const {
	return iterator.done;
}

StringValueResult &StringValueScanner::ParseChunk() {
	if (suspended) {
		// Resuming a chunk that suspended on a buffer load, we keep the parsed rows until here
		ResumeParse();
		return result;
	}
	result.Reset();
	ParseChunkInternal(result);
	return result;
}

void StringValueScanner::ResumeParse() {
	suspended = false;
	if (iterator.IsBoundarySet()) {
		// This got blocked at TryMoveToNextBuffer(); so we gotta finish the boundary scan
		FinishBoundaryScan(TryMoveToNextBuffer() == MoveBufferResult::MOVED);
	} else {
		ProcessRemainingBuffers();
	}
}

void StringValueScanner::Flush(DataChunk &insert_chunk) {
	bool continue_processing;
	do {
		continue_processing = false;
		auto &process_result = ParseChunk();
		if (IsSuspended()) {
			// Suspended on a buffer load
			return;
		}
		// First Get Parsed Chunk
		auto &parse_chunk = process_result.ToChunk();
		insert_chunk.Reset();
		// We have to check if we got to error
		error_handler->ErrorIfNeeded();
		if (parse_chunk.size() == 0) {
			return;
		}
		// convert the columns in the parsed chunk to the types of the table
		insert_chunk.SetChildCardinality(parse_chunk.size());

		// We keep track of the borked lines, in case we are ignoring errors
		D_ASSERT(csv_file_scan);

		auto &names = csv_file_scan->GetNames();
		// Now Do the cast-aroo
		for (idx_t i = 0; i < csv_file_scan->column_ids.size(); i++) {
			idx_t result_idx = i;
			if (!csv_file_scan->projection_ids.empty()) {
				result_idx = csv_file_scan->projection_ids[i].second;
			}
			if (i >= parse_chunk.ColumnCount()) {
				throw InvalidInputException("Mismatch between the schema of different files");
			}
			auto &parse_vector = parse_chunk.data[i];
			auto &result_vector = insert_chunk.data[result_idx];
			auto &type = result_vector.GetType();
			auto &parse_type = parse_vector.GetType();
			if (!type.IsJSONType() && (type == LogicalType::VARCHAR ||
			                           (type != LogicalType::VARCHAR && parse_type != LogicalType::VARCHAR))) {
				// reinterpret rather than reference
				result_vector.Reinterpret(parse_vector);
			} else {
				string error_message;
				idx_t line_error = 0;
				if (VectorOperations::TryCast(buffer_manager->context, parse_vector, result_vector, parse_chunk.size(),
				                              &error_message, false, true)) {
					continue;
				}
				// An error happened, to propagate it we need to figure out the exact line where the casting failed.
				auto inserted_validity = result_vector.Validity();
				auto parse_validity = parse_vector.Validity();

				for (; line_error < parse_chunk.size(); line_error++) {
					if (!inserted_validity.IsValid(line_error) && parse_validity.IsValid(line_error)) {
						break;
					}
				}
				{
					if (state_machine->options.ignore_errors.GetValue()) {
						vector<Value> row;
						for (idx_t col = 0; col < parse_chunk.ColumnCount(); col++) {
							row.push_back(parse_chunk.GetValue(col, line_error));
						}
					}
					if (!state_machine->options.IgnoreErrors()) {
						LinesPerBoundary lines_per_batch(iterator.GetBoundaryIdx(),
						                                 lines_read - parse_chunk.size() + line_error);
						bool first_nl = false;
						auto borked_line = result.line_positions_per_row[line_error].ReconstructCurrentLine(
						    first_nl, result.buffer_handles, result.PrintErrorLine());
						std::ostringstream error;
						error << "Could not convert string \"" << parse_vector.GetValue(line_error) << "\" to \'"
						      << type.ToString() << "\'";
						string error_msg = error.str();
						FullLinePosition::SanitizeError(error_msg);
						idx_t row_byte_pos = 0;
						if (!(result.line_positions_per_row[line_error].begin ==
						      result.line_positions_per_row[line_error].end)) {
							row_byte_pos = result.line_positions_per_row[line_error].begin.GetGlobalPosition(
							    result.result_size, first_nl);
						}
						auto csv_error = CSVError::CastError(
						    state_machine->options, names[i], error_msg, i, borked_line, lines_per_batch, row_byte_pos,
						    optional_idx::Invalid(), result_vector.GetType().id(), result.path);
						error_handler->Error(csv_error);
					}
				}
				result.borked_rows.insert(line_error++);
				D_ASSERT(state_machine->options.ignore_errors.GetValue());
				// We are ignoring errors. We must continue but ignoring borked-rows
				for (; line_error < parse_chunk.size(); line_error++) {
					if (!inserted_validity.IsValid(line_error) && parse_validity.IsValid(line_error)) {
						result.borked_rows.insert(line_error);
						vector<Value> row;
						for (idx_t col = 0; col < parse_chunk.ColumnCount(); col++) {
							row.push_back(parse_chunk.GetValue(col, line_error));
						}
						if (!state_machine->options.IgnoreErrors()) {
							LinesPerBoundary lines_per_batch(iterator.GetBoundaryIdx(),
							                                 lines_read - parse_chunk.size() + line_error);
							bool first_nl = false;
							auto borked_line = result.line_positions_per_row[line_error].ReconstructCurrentLine(
							    first_nl, result.buffer_handles, result.PrintErrorLine());
							std::ostringstream error;
							// Casting Error Message
							error << "Could not convert string \"" << parse_vector.GetValue(line_error) << "\" to \'"
							      << LogicalTypeIdToString(type.id()) << "\'";
							string error_msg = error.str();
							FullLinePosition::SanitizeError(error_msg);
							auto csv_error = CSVError::CastError(
							    state_machine->options, names[i], error_msg, i, borked_line, lines_per_batch,
							    result.line_positions_per_row[line_error].begin.GetGlobalPosition(result.result_size,
							                                                                      first_nl),
							    optional_idx::Invalid(), result_vector.GetType().id(), result.path);
							error_handler->Error(csv_error);
						}
					}
				}
			}
		}
		if (!result.borked_rows.empty()) {
			// We must remove the borked lines from our chunk
			SelectionVector successful_rows(parse_chunk.size());
			idx_t sel_idx = 0;
			for (idx_t row_idx = 0; row_idx < parse_chunk.size(); row_idx++) {
				if (result.borked_rows.find(row_idx) == result.borked_rows.end()) {
					successful_rows.set_index(sel_idx++, row_idx);
				}
			}
			// Now we slice the result
			insert_chunk.Slice(successful_rows, sel_idx);
			result.borked_rows.clear();
		}
		insert_chunk.SetChildCardinality(insert_chunk.size());
		if (insert_chunk.size() == 0 && cur_buffer_handle) {
			idx_t to_pos;
			if (iterator.IsBoundarySet()) {
				to_pos = iterator.GetEndPos();
				if (to_pos > cur_buffer_handle->actual_size) {
					to_pos = cur_buffer_handle->actual_size;
				}
			} else {
				to_pos = cur_buffer_handle->actual_size;
			}
			if (iterator.pos.buffer_pos < to_pos) {
				// If a chunk is complete with errors, we might get to this situation where we must proceed with the
				// scanning
				continue_processing = true;
			}
		}
	} while (continue_processing);
}

void StringValueScanner::Initialize() {
	states.Initialize();

	if (result.result_size != 1 && !(sniffing && state_machine->options.null_padding &&
	                                 !state_machine->options.dialect_options.skip_rows.IsSetByUser())) {
		SetStart();
	} else {
		start_pos = iterator.GetGlobalCurrentPos();
	}

	result.last_position = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, cur_buffer_handle->actual_size};
	result.current_line_position.begin = result.last_position;
	result.current_line_position.end = result.current_line_position.begin;
}

void StringValueScanner::ProcessExtraRow() {
	result.NullPaddingQuotedNewlineCheck();
	idx_t to_pos = cur_buffer_handle->actual_size;
	while (iterator.pos.buffer_pos < to_pos) {
		state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos]);
		switch (states.states[1]) {
		case CSVState::INVALID:
			result.InvalidState(result);
			iterator.pos.buffer_pos++;
			return;
		case CSVState::RECORD_SEPARATOR:
			if (states.states[0] == CSVState::RECORD_SEPARATOR) {
				result.EmptyLine(result, iterator.pos.buffer_pos);
				iterator.pos.buffer_pos++;
				lines_read++;
				return;
			} else if (states.states[0] != CSVState::CARRIAGE_RETURN) {
				if (result.IsCommentSet(result)) {
					result.UnsetComment(result, iterator.pos.buffer_pos);
				} else {
					result.AddRow(result, iterator.pos.buffer_pos);
				}
				iterator.pos.buffer_pos++;
				lines_read++;
				return;
			}
			lines_read++;
			iterator.pos.buffer_pos++;
			break;
		case CSVState::CARRIAGE_RETURN:
			if (states.states[0] != CSVState::RECORD_SEPARATOR) {
				if (result.IsCommentSet(result)) {
					result.UnsetComment(result, iterator.pos.buffer_pos);
				} else {
					result.AddRow(result, iterator.pos.buffer_pos);
				}
				iterator.pos.buffer_pos++;
				lines_read++;
				return;
			} else {
				result.EmptyLine(result, iterator.pos.buffer_pos);
				iterator.pos.buffer_pos++;
				lines_read++;
				return;
			}
			break;
		case CSVState::DELIMITER:
			result.AddValue(result, iterator.pos.buffer_pos);
			iterator.pos.buffer_pos++;
			break;
		case CSVState::QUOTED:
			if (states.states[0] == CSVState::UNQUOTED) {
				result.SetEscaped(result);
			}
			result.SetQuoted(result, iterator.pos.buffer_pos);
			iterator.pos.buffer_pos++;
			while (iterator.pos.buffer_pos < to_pos - 1 &&
			       state_machine->transition_array
			           .skip_quoted[static_cast<uint8_t>(buffer_handle_ptr[iterator.pos.buffer_pos])]) {
				iterator.pos.buffer_pos++;
			}
			break;
		case CSVState::ESCAPE:
		case CSVState::UNQUOTED_ESCAPE:
		case CSVState::ESCAPED_RETURN:
			result.SetEscaped(result);
			iterator.pos.buffer_pos++;
			break;
		case CSVState::STANDARD:
			iterator.pos.buffer_pos++;
			while (state_machine->transition_array
			           .skip_standard[static_cast<uint8_t>(buffer_handle_ptr[iterator.pos.buffer_pos])] &&
			       iterator.pos.buffer_pos < to_pos - 1) {
				iterator.pos.buffer_pos++;
			}
			break;
		case CSVState::UNQUOTED: {
			result.SetUnquoted(result);
			iterator.pos.buffer_pos++;
			break;
		}
		case CSVState::COMMENT:
			result.SetComment(result, iterator.pos.buffer_pos);
			iterator.pos.buffer_pos++;
			while (state_machine->transition_array
			           .skip_comment[static_cast<uint8_t>(buffer_handle_ptr[iterator.pos.buffer_pos])] &&
			       iterator.pos.buffer_pos < to_pos - 1) {
				iterator.pos.buffer_pos++;
			}
			break;
		case CSVState::QUOTED_NEW_LINE:
			result.quoted_new_line = true;
			result.NullPaddingQuotedNewlineCheck();
			iterator.pos.buffer_pos++;
			break;
		default:
			iterator.pos.buffer_pos++;
			break;
		}
	}
}

string_t StringValueScanner::RemoveEscape(const char *str_ptr, idx_t end, char escape, char quote, bool strict_mode,
                                          Vector &vector) {
	// Figure out the exact size
	idx_t str_pos = 0;
	bool just_escaped = false;
	for (idx_t cur_pos = 0; cur_pos < end; cur_pos++) {
		if (str_ptr[cur_pos] == escape && !just_escaped) {
			just_escaped = true;
		} else if (str_ptr[cur_pos] == quote) {
			if (just_escaped || !strict_mode) {
				str_pos++;
			}
			just_escaped = false;
		} else {
			just_escaped = false;
			str_pos++;
		}
	}

	auto removed_escapes = StringVector::EmptyString(vector, str_pos);
	auto removed_escapes_ptr = removed_escapes.GetDataWriteable();
	// Allocate string and copy it
	str_pos = 0;
	just_escaped = false;
	for (idx_t cur_pos = 0; cur_pos < end; cur_pos++) {
		const char c = str_ptr[cur_pos];
		if (c == escape && !just_escaped) {
			just_escaped = true;
		} else if (str_ptr[cur_pos] == quote) {
			if (just_escaped || !strict_mode) {
				removed_escapes_ptr[str_pos++] = c;
			}
			just_escaped = false;
		} else {
			just_escaped = false;
			removed_escapes_ptr[str_pos++] = c;
		}
	}
	removed_escapes.Finalize();
	return removed_escapes;
}

void StringValueScanner::ProcessOverBufferValue() {
	// Process first string
	if (result.last_position.buffer_pos != previous_buffer_handle->actual_size) {
		states.Initialize();
	}

	string over_buffer_string;
	auto previous_buffer = previous_buffer_handle->Ptr();
	idx_t j = 0;
	result.quoted = false;
	for (idx_t i = result.last_position.buffer_pos; i < previous_buffer_handle->actual_size; i++) {
		state_machine->Transition(states, previous_buffer[i]);
		if (states.EmptyLine() || states.IsCurrentNewRow()) {
			continue;
		}
		if (states.NewRow() || states.NewValue()) {
			break;
		} else if (!result.comment) {
			over_buffer_string += previous_buffer[i];
		}
		if (states.IsQuoted()) {
			result.SetQuoted(result, j);
		}
		if (states.IsUnquoted()) {
			result.SetUnquoted(result);
		}
		if (states.IsEscaped() && result.state_machine.dialect_options.state_machine_options.escape != '\0') {
			result.escaped = true;
		}
		if (states.IsComment()) {
			result.SetComment(result, j);
		}
		if (states.IsInvalid()) {
			result.InvalidState(result);
		}
		j++;
	}
	if (over_buffer_string.empty() &&
	    state_machine->dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON) {
		if (!iterator.IsBoundarySet()) {
			if (buffer_handle_ptr[iterator.pos.buffer_pos] == '\n') {
				iterator.pos.buffer_pos++;
			}
		} else {
			idx_t pre_carry_pos = iterator.pos.buffer_pos;
			while (iterator.pos.buffer_pos < cur_buffer_handle->actual_size &&
			       (buffer_handle_ptr[iterator.pos.buffer_pos] == '\n' ||
			        buffer_handle_ptr[iterator.pos.buffer_pos] == '\r')) {
				if (buffer_handle_ptr[iterator.pos.buffer_pos] == '\r') {
					if (result.last_position.buffer_pos <= previous_buffer_handle->actual_size) {
						// we add the value
						result.AddValue(result, previous_buffer_handle->actual_size);
						if (result.IsCommentSet(result)) {
							result.UnsetComment(result, iterator.pos.buffer_pos);
						} else {
							result.AddRow(result, previous_buffer_handle->actual_size);
						}
						state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos++]);
						while (iterator.pos.buffer_pos < cur_buffer_handle->actual_size &&
						       (buffer_handle_ptr[iterator.pos.buffer_pos] == '\r' ||
						        buffer_handle_ptr[iterator.pos.buffer_pos] == '\n')) {
							state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos++]);
						}
						return;
					}
				} else {
					if (iterator.pos.buffer_pos + 1 == cur_buffer_handle->actual_size) {
						return;
					}
				}
				state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos]);
				iterator.pos.buffer_pos++;
			} // Avoid double-counting when \r\r\n line endings split across buffer boundary: first \r triggers AddRow
			  // in previous buffer, remaining \r\n is here. Check first consumed char is \r (not \n) to avoid
			  // false-positives.
			if (over_buffer_string.empty() && iterator.pos.buffer_pos > pre_carry_pos &&
			    result.last_position.buffer_pos > previous_buffer_handle->actual_size &&
			    buffer_handle_ptr[pre_carry_pos] == '\r') {
				return;
			}
		}
	}
	// second buffer
	for (; iterator.pos.buffer_pos < cur_buffer_handle->actual_size; iterator.pos.buffer_pos++) {
		state_machine->Transition(states, buffer_handle_ptr[iterator.pos.buffer_pos]);
		if (states.EmptyLine()) {
			if (state_machine->dialect_options.num_cols == 1) {
				break;
			}
			continue;
		}
		if (states.NewRow() || states.NewValue()) {
			break;
		} else if (!result.comment && !states.IsComment()) {
			over_buffer_string += buffer_handle_ptr[iterator.pos.buffer_pos];
		}
		if (states.IsQuoted()) {
			result.SetQuoted(result, j);
		}
		if (states.IsComment()) {
			result.SetComment(result, j);
		}
		if (states.IsEscaped() && result.state_machine.dialect_options.state_machine_options.escape != '\0') {
			result.escaped = true;
		}
		if (states.IsInvalid()) {
			result.InvalidState(result);
		}
		j++;
	}
	bool skip_value = false;
	if (result.projecting_columns) {
		if (result.cur_col_id < result.number_of_columns && !result.projected_columns[result.cur_col_id]) {
			result.cur_col_id++;
			skip_value = true;
		}
	}
	if (!skip_value) {
		string_t value;
		const auto &delimiter = result.state_machine.dialect_options.state_machine_options.delimiter.GetValue();
		const idx_t extra_delimiter_bytes = states.IsDelimiter() && !delimiter.empty() ? delimiter.size() - 1 : 0;
		bool removed_extra_delimiter_bytes = false;
		if (result.quoted && !result.comment) {
			idx_t length = over_buffer_string.size() - 1 - result.quoted_position;
			while (length > 0 && result.ignore_empty_values &&
			       over_buffer_string.c_str()[result.quoted_position + length] == ' ') {
				length--;
			}
			// remove the extra delimiter bytes before removing escapes, which can change the length of the value
			if (extra_delimiter_bytes > length) {
				throw InternalException(
				    "Value size is lower than the number of extra delimiter bytes in the ProcessOverBufferValue()");
			}
			length -= extra_delimiter_bytes;
			removed_extra_delimiter_bytes = true;
			value = string_t(over_buffer_string.c_str() + result.quoted_position, UnsafeNumericCast<uint32_t>(length));
			if (result.escaped) {
				if (result.cur_col_id >= result.number_of_columns &&
				    !result.state_machine.state_machine_options.strict_mode.GetValue()) {
					result.used_unstrictness = true;
				} else if (!result.HandleTooManyColumnsError(over_buffer_string.c_str(), over_buffer_string.size())) {
					const auto str_ptr = over_buffer_string.c_str() + result.quoted_position;
					if (result.parse_chunk.data[result.chunk_col_id].GetType() != LogicalType::VARCHAR) {
						// We cant have escapes on non varchar columns
						result.current_errors.Insert(CAST_ERROR, result.cur_col_id, result.chunk_col_id,
						                             result.last_position);
						if (!result.state_machine.options.IgnoreErrors()) {
							// We have to write the cast error message.
							std::ostringstream error;
							// Casting Error Message
							error << "Could not convert string \""
							      << std::string(over_buffer_string.c_str(), over_buffer_string.size()) << "\" to \'"
							      << LogicalTypeIdToString(result.parse_types[result.chunk_col_id].type_id) << "\'";
							auto error_string = error.str();
							FullLinePosition::SanitizeError(error_string);
							result.current_errors.ModifyErrorMessageOfLastError(error_string);
						}
						return;
					}
					value = RemoveEscape(
					    str_ptr, length, state_machine->dialect_options.state_machine_options.escape.GetValue(),
					    state_machine->dialect_options.state_machine_options.quote.GetValue(),
					    result.state_machine.dialect_options.state_machine_options.strict_mode.GetValue(),
					    result.parse_chunk.data[result.chunk_col_id]);
				}
			}
		} else {
			value = string_t(over_buffer_string.c_str(), UnsafeNumericCast<uint32_t>(over_buffer_string.size()));
			if (result.escaped) {
				if (result.cur_col_id >= result.number_of_columns &&
				    !result.state_machine.state_machine_options.strict_mode.GetValue()) {
					result.used_unstrictness = true;
				} else if (!result.HandleTooManyColumnsError(over_buffer_string.c_str(), over_buffer_string.size())) {
					if (result.parse_chunk.data[result.chunk_col_id].GetType() != LogicalType::VARCHAR) {
						// We cant have escapes on non varchar columns
						result.current_errors.Insert(CAST_ERROR, result.cur_col_id, result.chunk_col_id,
						                             result.last_position);
						if (!result.state_machine.options.IgnoreErrors()) {
							// We have to write the cast error message.
							std::ostringstream error;
							// Casting Error Message
							error << "Could not convert string \""
							      << std::string(over_buffer_string.c_str(), over_buffer_string.size()) << "\" to \'"
							      << LogicalTypeIdToString(result.parse_types[result.chunk_col_id].type_id) << "\'";
							auto error_string = error.str();
							FullLinePosition::SanitizeError(error_string);
							result.current_errors.ModifyErrorMessageOfLastError(error_string);
						}
						return;
					}
					value =
					    RemoveEscape(over_buffer_string.c_str(), over_buffer_string.size(),
					                 state_machine->dialect_options.state_machine_options.escape.GetValue(),
					                 state_machine->dialect_options.state_machine_options.quote.GetValue(),
					                 result.state_machine.dialect_options.state_machine_options.strict_mode.GetValue(),
					                 result.parse_chunk.data[result.chunk_col_id]);
				}
			}
		}
		if (states.EmptyLine() && state_machine->dialect_options.num_cols == 1) {
			result.EmptyLine(result, iterator.pos.buffer_pos);
		} else if (!states.IsNotSet() && (!result.comment || !value.Empty())) {
			idx_t value_size = value.GetSize();
			if (!removed_extra_delimiter_bytes) {
				if (extra_delimiter_bytes > value_size) {
					throw InternalException(
					    "Value size is lower than the number of extra delimiter bytes in the ProcessOverBufferValue()");
				}
				value_size -= extra_delimiter_bytes;
			}
			result.AddValueToVector(value.GetData(), value_size, true);
		}
	} else {
		if (states.EmptyLine() && state_machine->dialect_options.num_cols == 1) {
			result.EmptyLine(result, iterator.pos.buffer_pos);
		}
	}

	if (states.NewRow() && !states.IsNotSet()) {
		if (result.IsCommentSet(result)) {
			result.UnsetComment(result, iterator.pos.buffer_pos);
		} else {
			result.AddRowInternal();
		}
		lines_read++;
	}

	if (iterator.pos.buffer_pos >= cur_buffer_handle->actual_size && cur_buffer_handle->is_last_buffer) {
		result.added_last_line = true;
	}
	if (states.IsCarriageReturn() &&
	    state_machine->dialect_options.state_machine_options.new_line == NewLineIdentifier::CARRY_ON) {
		result.last_position = {iterator.pos.buffer_idx, ++iterator.pos.buffer_pos + 1, result.buffer_size};
	} else {
		result.last_position = {iterator.pos.buffer_idx, ++iterator.pos.buffer_pos, result.buffer_size};
	}
	// Be sure to reset the quoted and escaped variables
	result.quoted = false;
	result.escaped = false;
}

StringValueScanner::MoveBufferResult StringValueScanner::TryMoveToNextBuffer() {
	if (iterator.pos.buffer_pos < cur_buffer_handle->actual_size) {
		return MoveBufferResult::NOT_MOVED;
	}
	// a value spans at most two buffers, so the buffer before the previous one is never needed again:
	// release it before pinning the next one, keeping the peak at two pinned buffers per scanner
	previous_buffer_handle.reset();
	const idx_t next_buffer_idx = iterator.pos.buffer_idx + 1;
	shared_ptr<CSVBufferHandle> next_buffer;
	if (buffer_manager->GetBufferResidency(next_buffer_idx, next_buffer) == CSVBufferResidency::NEEDS_LOAD) {
		if (can_suspend && pending_buffer_idx != next_buffer_idx) {
			pending_buffer_idx = next_buffer_idx;
			return MoveBufferResult::NOT_IN_MEMORY;
		}
		next_buffer = buffer_manager->GetBuffer(next_buffer_idx);
	}
	if (next_buffer) {
		FinishMoveToNextBuffer(std::move(next_buffer));
		return MoveBufferResult::MOVED;
	}
	previous_buffer_handle = std::move(cur_buffer_handle);
	cur_buffer_handle.reset();
	buffer_handle_ptr = nullptr;
	// We do not care if it's a quoted new line on the last row of our file.
	result.quoted_new_line = false;
	// This means we reached the end of the file, we must add a last line if there is any to be added
	if (states.EmptyLine() || states.NewRow() || result.added_last_line || states.IsCurrentNewRow() ||
	    states.IsNotSet()) {
		if (result.cur_col_id == result.number_of_columns && !result.IsStateCurrent(CSVState::INVALID)) {
			result.number_of_rows++;
		}
		result.cur_col_id = 0;
		result.chunk_col_id = 0;
	} else if (states.NewValue()) {
		// we add the value
		result.AddValue(result, previous_buffer_handle->actual_size);
		// And an extra empty value to represent what comes after the delimiter
		if (result.IsCommentSet(result)) {
			result.UnsetComment(result, iterator.pos.buffer_pos);
		} else {
			result.AddRow(result, previous_buffer_handle->actual_size);
		}
		lines_read++;
	} else if (states.IsQuotedCurrent() &&
	           state_machine->dialect_options.state_machine_options.strict_mode.GetValue()) {
		// Unterminated quote
		LinePosition current_line_start = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, result.buffer_size};
		result.current_line_position.begin = result.current_line_position.end;
		result.current_line_position.end = current_line_start;
		result.InvalidState(result);
	} else {
		if (result.IsCommentSet(result)) {
			result.UnsetComment(result, iterator.pos.buffer_pos);
		} else {
			if (result.quoted && states.IsDelimiterBytes() &&
			    state_machine->dialect_options.state_machine_options.strict_mode.GetValue()) {
				result.current_errors.Insert(UNTERMINATED_QUOTES, result.cur_col_id, result.chunk_col_id,
				                             result.last_position);
			}
			result.AddRow(result, previous_buffer_handle->actual_size);
		}
		lines_read++;
	}
	return MoveBufferResult::NOT_MOVED;
}

void StringValueScanner::FinishMoveToNextBuffer(shared_ptr<CSVBufferHandle> next_buffer) {
	previous_buffer_handle = std::move(cur_buffer_handle);
	cur_buffer_handle = std::move(next_buffer);
	iterator.pos.buffer_idx++;
	result.buffer_handles[cur_buffer_handle->buffer_idx] = cur_buffer_handle;

	iterator.pos.buffer_pos = 0;
	buffer_handle_ptr = cur_buffer_handle->Ptr();
	// Handle over-buffer value
	ProcessOverBufferValue();
	result.buffer_ptr = buffer_handle_ptr;
	result.buffer_size = cur_buffer_handle->actual_size;
}

void StringValueResult::SkipBOM() const {
	StringUtil::SkipBOM(buffer_ptr, buffer_size, iterator.pos.buffer_pos);
}

void StringValueResult::RemoveLastLine() {
	// potentially de-nullify values
	for (idx_t i = 0; i < chunk_col_id; i++) {
		validity_mask[i]->SetValid(static_cast<idx_t>(number_of_rows));
	}
	// reset column trackers
	cur_col_id = 0;
	chunk_col_id = 0;
	// decrement row counter
	number_of_rows--;
}
bool StringValueResult::PrintErrorLine() const {
	// To print a lint, result size must be different, than one (i.e., this is a SetStart() trying to figure out new
	// lines) And must either not be ignoring errors OR must be storing them in a rejects table.
	return result_size != 1 &&
	       (state_machine.options.store_rejects.GetValue() || !state_machine.options.ignore_errors.GetValue());
}

bool StringValueScanner::FirstValueEndsOnQuote(CSVIterator iterator) const {
	CSVStates current_state;
	current_state.Initialize(CSVState::STANDARD);
	const idx_t to_pos = iterator.GetEndPos();
	const auto quote = state_machine->dialect_options.state_machine_options.quote.GetValue();
	const auto start_pos = iterator.pos.buffer_pos;
	while (iterator.pos.buffer_pos < to_pos) {
		state_machine->Transition(current_state, buffer_handle_ptr[iterator.pos.buffer_pos++]);
		if (current_state.IsState(CSVState::DELIMITER) || current_state.IsState(CSVState::CARRIAGE_RETURN) ||
		    current_state.IsState(CSVState::RECORD_SEPARATOR)) {
			// a value that also starts with a quote is a regular quoted value
			const auto value_end = iterator.pos.buffer_pos - 2;
			return value_end > start_pos && buffer_handle_ptr[value_end] == quote &&
			       buffer_handle_ptr[start_pos] != quote;
		}
	}
	return false;
}

bool StringValueScanner::SkipUntilState(CSVState initial_state, CSVState until_state, CSVIterator &current_iterator,
                                        bool &quoted) const {
	CSVStates current_state;
	current_state.Initialize(initial_state);
	bool first_column = true;
	const idx_t to_pos = current_iterator.GetEndPos();
	skipper.SetBuffer(*cur_buffer_handle);
	while (current_iterator.pos.buffer_pos < to_pos) {
		state_machine_strict->Transition(current_state, buffer_handle_ptr[current_iterator.pos.buffer_pos++]);
		if (current_state.IsState(CSVState::STANDARD) || current_state.IsState(CSVState::STANDARD_NEWLINE)) {
			skipper.SkipToStop(state_machine_strict->transition_array.skip_standard, to_pos,
			                   current_iterator.pos.buffer_pos);
		}
		if (current_state.IsState(CSVState::QUOTED)) {
			skipper.SkipToStop(state_machine_strict->transition_array.skip_quoted, to_pos,
			                   current_iterator.pos.buffer_pos);
		}
		if ((current_state.IsState(CSVState::DELIMITER) || current_state.IsState(CSVState::CARRIAGE_RETURN) ||
		     current_state.IsState(CSVState::RECORD_SEPARATOR)) &&
		    first_column) {
			if (buffer_handle_ptr[current_iterator.pos.buffer_pos - 1] ==
			    state_machine_strict->dialect_options.state_machine_options.quote.GetValue()) {
				quoted = true;
			}
		}
		if (current_state.WasState(CSVState::DELIMITER)) {
			first_column = false;
		}
		if (current_state.IsState(until_state)) {
			return true;
		}
		if (current_state.IsState(CSVState::INVALID)) {
			return false;
		}
	}
	return false;
}

bool StringValueScanner::CanDirectlyCast(const LogicalType &type, bool icu_loaded) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIME:
	case LogicalTypeId::DECIMAL:
	case LogicalType::BOOLEAN:
		return true;
	case LogicalType::TIMESTAMP_TZ:
		// We only try to do direct cast of timestamp tz if the ICU extension is not loaded, otherwise, it needs to go
		// through string -> timestamp_tz casting
		return !icu_loaded;
	case LogicalType::VARCHAR:
		return !type.IsJSONType();
	default:
		return false;
	}
}

bool StringValueScanner::IsRowValid(CSVIterator &current_iterator) const {
	if (iterator.pos.buffer_pos == cur_buffer_handle->actual_size) {
		return false;
	}
	constexpr idx_t result_size = 1;
	auto scan_finder = make_uniq<StringValueScanner>(LINE_FINDER_ID, buffer_manager, state_machine_strict,
	                                                 make_shared_ptr<CSVErrorHandler>(), csv_file_scan, false,
	                                                 current_iterator, result_size);
	try {
		auto &tuples = scan_finder->ParseChunk();
		current_iterator.pos = scan_finder->GetIteratorPosition();
		bool has_error = false;
		if (tuples.current_errors.HasError()) {
			if (tuples.current_errors.Size() != 1 || !tuples.current_errors.HasErrorType(MAXIMUM_LINE_SIZE)) {
				// We ignore maximum line size errors
				has_error = true;
			}
		}
		return (tuples.number_of_rows == 1 || tuples.first_line_is_comment) && !has_error && tuples.borked_rows.empty();
	} catch (const Exception &e) {
		return false;
	}
	return true;
}

ValidRowInfo StringValueScanner::TryRow(CSVState state, idx_t start_pos, idx_t end_pos) const {
	auto current_iterator = iterator;
	current_iterator.SetStart(start_pos);
	current_iterator.SetEnd(end_pos);
	bool quoted = false;
	if (SkipUntilState(state, CSVState::RECORD_SEPARATOR, current_iterator, quoted)) {
		auto iterator_start = current_iterator;
		idx_t current_pos = current_iterator.pos.buffer_pos;
		current_iterator.SetEnd(iterator.GetEndPos());
		if (IsRowValid(current_iterator)) {
			if (!quoted) {
				quoted = FirstValueEndsOnQuote(iterator_start);
			}
			return {true, current_pos, current_iterator.pos.buffer_idx, current_iterator.pos.buffer_pos, quoted};
		}
	}
	return {false, current_iterator.pos.buffer_pos, current_iterator.pos.buffer_idx, current_iterator.pos.buffer_pos,
	        quoted};
}

void StringValueScanner::SetStart() {
	start_pos = iterator.GetGlobalCurrentPos();
	if (iterator.first_one) {
		return;
	}
	if (iterator.GetEndPos() > cur_buffer_handle->actual_size) {
		iterator.SetEnd(cur_buffer_handle->actual_size);
	}
	if (!state_machine_strict) {
		// We need to initialize our strict state machine
		auto &state_machine_cache = CSVStateMachineCache::Get(buffer_manager->context);
		auto state_options = state_machine->state_machine_options;
		// To set the state machine to be strict we ensure that strict_mode is set to true
		if (!state_options.strict_mode.IsSetByUser()) {
			state_options.strict_mode = true;
		}
		state_machine_strict =
		    make_shared_ptr<CSVStateMachine>(state_machine_cache.Get(state_options), state_machine->options);
	}
	// At this point we have 3 options:
	// 1. We are at the start of a valid line
	ValidRowInfo best_row = TryRow(CSVState::STANDARD_NEWLINE, iterator.pos.buffer_pos, iterator.GetEndPos());
	// 2. We are in the middle of a quoted value
	if (state_machine->dialect_options.state_machine_options.quote.GetValue() != '\0') {
		idx_t end_pos = iterator.GetEndPos();
		if (best_row.is_valid && best_row.end_buffer_idx == iterator.pos.buffer_idx) {
			// If we got a valid row from the standard state, we limit our search up to that.
			end_pos = best_row.end_pos;
		}
		auto quoted_row = TryRow(CSVState::QUOTED, iterator.pos.buffer_pos, end_pos);
		if (quoted_row.is_valid && (!best_row.is_valid || best_row.last_state_quote)) {
			best_row = quoted_row;
		}
		if (!best_row.is_valid && !quoted_row.is_valid && best_row.start_pos < quoted_row.start_pos) {
			best_row = quoted_row;
		}
		if (quoted_row.is_valid && quoted_row.start_pos < best_row.start_pos) {
			best_row = quoted_row;
		}
	}
	// 3. We are in an escaped value
	if (!best_row.is_valid && state_machine->dialect_options.state_machine_options.quote.GetValue() != '\0') {
		auto escape_row = TryRow(CSVState::ESCAPE, iterator.pos.buffer_pos, iterator.GetEndPos());
		if (escape_row.is_valid) {
			best_row = escape_row;
		} else {
			if (best_row.start_pos < escape_row.start_pos) {
				best_row = escape_row;
			}
		}
	}
	if (!best_row.is_valid) {
		bool is_this_the_end =
		    best_row.start_pos >= cur_buffer_handle->actual_size && cur_buffer_handle->is_last_buffer;
		if (is_this_the_end) {
			iterator.pos.buffer_pos = best_row.start_pos;
			iterator.done = true;
		} else {
			bool mock;
			if (!SkipUntilState(CSVState::STANDARD_NEWLINE, CSVState::RECORD_SEPARATOR, iterator, mock)) {
				iterator.CheckIfDone();
			}
		}
	} else {
		iterator.pos.buffer_pos = best_row.start_pos;
		bool is_this_the_end =
		    best_row.start_pos >= cur_buffer_handle->actual_size && cur_buffer_handle->is_last_buffer;
		if (is_this_the_end) {
			iterator.done = true;
		}
	}

	// 4. We have an error, if we have an error, we let life go on, the scanner will either ignore it
	// or throw.
	result.last_position = {iterator.pos.buffer_idx, iterator.pos.buffer_pos, result.buffer_size};
	start_pos = iterator.GetGlobalCurrentPos();
}

void StringValueScanner::FinalizeChunkProcess() {
	if (static_cast<idx_t>(result.number_of_rows) >= result.result_size || iterator.done) {
		// We are done
		if (!sniffing) {
			if (csv_file_scan) {
				csv_file_scan->bytes_read += bytes_read;
				bytes_read = 0;
			}
		}
		return;
	}
	// If we are not done we have two options.
	// 1) If a boundary is set.
	if (iterator.IsBoundarySet()) {
		if (!result.current_errors.HasErrorType(UNTERMINATED_QUOTES) &&
		    !result.current_errors.HasErrorType(INVALID_STATE)) {
			iterator.done = true;
		}
		// We read until the next line or until we have nothing else to read.
		// Move to next buffer
		if (!cur_buffer_handle) {
			return;
		}
		const auto move_result = TryMoveToNextBuffer();
		if (move_result == MoveBufferResult::NOT_IN_MEMORY) {
			suspended = true;
			return;
		}
		FinishBoundaryScan(move_result == MoveBufferResult::MOVED);
	} else {
		// 2) If a boundary is not set
		// We read until the chunk is complete, or we have nothing else to read.
		ProcessRemainingBuffers();
	}
}

void StringValueScanner::FinishBoundaryScan(const bool moved) {
	if (cur_buffer_handle) {
		if (moved && result.cur_col_id > 0) {
			ProcessExtraRow();
		} else if (!moved) {
			ProcessExtraRow();
		}
		if (cur_buffer_handle->is_last_buffer && iterator.pos.buffer_pos >= cur_buffer_handle->actual_size) {
			// the last buffer has no next buffer to wait for
			TryMoveToNextBuffer();
		}
	}
	const bool found_error =
	    result.current_errors.HasErrorType(UNTERMINATED_QUOTES) || result.current_errors.HasErrorType(INVALID_STATE);
	auto chunk_col_id_before = result.chunk_col_id;
	if (result.current_errors.HandleErrors(result)) {
		result.InvalidateUnwrittenColumns(chunk_col_id_before);
		result.number_of_rows++;
	}
	if (states.IsQuotedCurrent() && !found_error) {
		if (state_machine->dialect_options.state_machine_options.strict_mode.GetValue()) {
			// If we finish the execution of a buffer, and we end in a quoted state, it means we have unterminated
			// quotes
			result.current_errors.Insert(UNTERMINATED_QUOTES, result.cur_col_id, result.chunk_col_id,
			                             result.last_position);
			chunk_col_id_before = result.chunk_col_id;
			if (result.current_errors.HandleErrors(result)) {
				result.InvalidateUnwrittenColumns(chunk_col_id_before);
				result.number_of_rows++;
			}
		} else {
			used_unstrictness = true;
		}
	}
	if (!iterator.done) {
		if (iterator.pos.buffer_pos >= iterator.GetEndPos() || iterator.pos.buffer_idx > iterator.GetBufferIdx() ||
		    FinishedFile()) {
			iterator.done = true;
		}
	}
}

void StringValueScanner::ProcessRemainingBuffers() {
	while (!FinishedFile() && static_cast<idx_t>(result.number_of_rows) < result.result_size) {
		if (TryMoveToNextBuffer() == MoveBufferResult::NOT_IN_MEMORY) {
			suspended = true;
			return;
		}
		if (static_cast<idx_t>(result.number_of_rows) >= result.result_size) {
			return;
		}
		if (cur_buffer_handle) {
			Process(result);
		}
	}
	iterator.done = FinishedFile();
	if (result.null_padding && result.number_of_rows < STANDARD_VECTOR_SIZE && result.chunk_col_id > 0) {
		while (result.chunk_col_id < result.parse_chunk.ColumnCount()) {
			result.validity_mask[result.chunk_col_id++]->SetInvalid(static_cast<idx_t>(result.number_of_rows));
			result.cur_col_id++;
		}
		result.number_of_rows++;
	}
}

ValidatorLine StringValueScanner::GetValidationLine() {
	return {start_pos, result.iterator.GetGlobalCurrentPos()};
}

} // namespace duckdb
