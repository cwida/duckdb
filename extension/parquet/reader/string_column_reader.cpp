#include "reader/string_column_reader.hpp"

#include <stddef.h>
#include <utility>

#include "utf8proc_wrapper.hpp"
#include "parquet_reader.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/vector_buffer.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_types.h"
#include "parquet_timestamp.hpp"

namespace duckdb {
class Vector;
struct SelectionVector;

//===--------------------------------------------------------------------===//
// String Column Reader
//===--------------------------------------------------------------------===//
StringColumnReader::StringColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
    : ColumnReader(reader, schema), string_column_type(GetStringColumnType(Type())) {
	fixed_width_string_length = 0;
	if (schema.parquet_type == Type::FIXED_LEN_BYTE_ARRAY) {
		fixed_width_string_length = schema.type_length;
	}
	if (schema.parquet_type == Type::INT96) {
		// INT96 is read as a raw 12-byte blob for int96_as='struct' (which converts it to STRUCT(date, time))
		fixed_width_string_length = sizeof(Int96);
	}
}

bool StringColumnReader::IsValid(const char *str_data, uint32_t str_len, const bool is_varchar) {
	if (!is_varchar) {
		return true;
	}
	// verify if a string is actually UTF8, and if there are no null bytes in the middle of the string
	// technically Parquet should guarantee this, but reality is often disappointing
	UnicodeInvalidReason reason;
	size_t pos;
	auto utf_type = Utf8Proc::Analyze(str_data, str_len, &reason, &pos);
	return utf_type != UnicodeType::INVALID;
}

bool StringColumnReader::IsValid(const string &str, bool is_varchar) {
	return IsValid(str.c_str(), str.size(), is_varchar);
}

string_t StringColumnReader::VerifyString(const char *str_data, uint32_t str_len, const bool is_varchar) const {
	if (!is_varchar) {
		return string_t(str_data, str_len);
	}
	if (Utf8Proc::Analyze(str_data, str_len) != UnicodeType::INVALID) {
		return string_t(str_data, str_len);
	}

	switch (reader.parquet_options.utf8_validation_option) {
	case StringColumnReader::Utf8ValidationOption::STRICT_UTF8:
		throw InvalidInputException(
		    "Invalid string encoding found in Parquet file \"%s\": value \"%s\" is not valid UTF8!",
		    reader.GetFileName(), Blob::ToString(string_t(str_data, str_len)));

	case StringColumnReader::Utf8ValidationOption::REPLACE_UTF8: {
		if (!current_plain_result) {
			throw InternalException("VerifyString: REPLACE mode requires current_plain_result to be set");
		}
		auto target = StringVector::EmptyString(*current_plain_result, str_len);
		auto output = target.GetDataWriteable();
		memcpy(output, str_data, str_len);
		Utf8Proc::MakeValid(output, str_len);
		target.Finalize();
		return target;
	}

	case StringColumnReader::Utf8ValidationOption::IGNORE_UTF8: {
		if (!current_plain_result) {
			throw InternalException("VerifyString: IGNORE mode requires current_plain_result to be set");
		}
		auto new_str = Utf8Proc::RemoveInvalid(str_data, str_len);
		auto target = StringVector::EmptyString(*current_plain_result, new_str.size());
		auto output = target.GetDataWriteable();
		memcpy(output, new_str.data(), new_str.size()); // NOLINT(bugprone-not-null-terminated-result)
		target.Finalize();
		return target;
	}

	default:
		throw InternalException("Unimplemented Utf8ValidationOption");
	}
}

string_t StringColumnReader::VerifyString(const char *str_data, uint32_t str_len) const {
	switch (string_column_type) {
	case StringColumnType::VARCHAR:
		return VerifyString(str_data, str_len, true);
	case StringColumnType::JSON: {
		const auto error = StringUtil::ValidateJSON(str_data, str_len);
		if (!error.empty()) {
			throw InvalidInputException("Invalid JSON found in Parquet file: %s", error);
		}
		return string_t(str_data, str_len);
	}
	case StringColumnType::OTHER:
		return string_t(str_data, str_len);
	default:
		throw InternalException("Unimplemented StringColumnType");
	}
}

class ParquetStringVectorBuffer : public AuxiliaryDataHolder {
public:
	explicit ParquetStringVectorBuffer(shared_ptr<ResizeableBuffer> buffer_p) : buffer(std::move(buffer_p)) {
	}

private:
	shared_ptr<ResizeableBuffer> buffer;
};

void StringColumnReader::ReferenceBlock(Vector &result, shared_ptr<ResizeableBuffer> &block) {
	StringVector::AddAuxiliaryData(result, make_uniq<ParquetStringVectorBuffer>(block));
}

void StringColumnReader::Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                               idx_t result_offset, Vector &result) {
	ReferenceBlock(result, plain_data);
	current_plain_result = &result;
	PlainTemplated<string_t, StringParquetValueConversion>(*plain_data, defines, num_values, result_offset, result);
	current_plain_result = nullptr;
}

void StringColumnReader::PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values,
                                     Vector &result, const SelectionVector &sel, idx_t count) {
	ReferenceBlock(result, plain_data);
	current_plain_result = &result;
	PlainSelectTemplated<string_t, StringParquetValueConversion>(*plain_data, defines, num_values, result, sel, count);
	current_plain_result = nullptr;
}

void StringColumnReader::PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) {
	PlainSkipTemplated<StringParquetValueConversion>(plain_data, defines, num_values);
}

} // namespace duckdb
