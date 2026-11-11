//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/string_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include <string>

#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {
class ParquetReader;
class Vector;
struct ParquetColumnSchema;
struct SelectionVector;

class StringColumnReader : public ColumnReader {
public:
	enum class StringColumnType : uint8_t { VARCHAR, JSON, OTHER };

	enum class Utf8ValidationOption : uint8_t { STRICT_UTF8 = 1, REPLACE_UTF8 = 2, IGNORE_UTF8 = 3 };

	static StringColumnType GetStringColumnType(const LogicalType &type) {
		if (type.IsJSONType()) {
			return StringColumnType::JSON;
		}
		if (type.id() == LogicalTypeId::VARCHAR) {
			return StringColumnType::VARCHAR;
		}
		return StringColumnType::OTHER;
	}

	static Utf8ValidationOption GetUtf8ValidationOption(const string &value) {
		if (StringUtil::CIEquals(value, "strict")) {
			return Utf8ValidationOption::STRICT_UTF8;
		} else if (StringUtil::CIEquals(value, "replace")) {
			return Utf8ValidationOption::REPLACE_UTF8;
		} else if (StringUtil::CIEquals(value, "ignore")) {
			return Utf8ValidationOption::IGNORE_UTF8;
		}
		throw BinderException(
		    "utf8_validation option \"%s\" not recognized, must be one of 'strict', 'replace', 'ignore'", value);
	}

public:
	static constexpr const PhysicalType TYPE = PhysicalType::VARCHAR;
	void SetCurrentResult(Vector &result) const {
		current_plain_result = &result;
	}

public:
	StringColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema);
	idx_t fixed_width_string_length;
	const StringColumnType string_column_type;

public:
	static bool IsValid(const char *str_data, uint32_t str_len, bool is_varchar);
	static bool IsValid(const string &str, bool is_varchar);
	string_t VerifyString(const char *str_data, uint32_t str_len, bool is_varchar) const;
	string_t VerifyString(const char *str_data, uint32_t str_len) const;

	static void ReferenceBlock(Vector &result, shared_ptr<ResizeableBuffer> &block, BufferManager &buffer_manager);

protected:
	void Plain(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset,
	           Vector &result) override {
		throw NotImplementedException("StringColumnReader can only read plain data from a shared buffer");
	}
	void Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset,
	           Vector &result) override;
	void PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) override;
	void PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values, Vector &result,
	                 const SelectionVector &sel, idx_t count) override;

	bool SupportsDirectFilter() const override {
		return true;
	}
	bool SupportsDirectSelect() const override {
		return true;
	}

private:
	mutable optional_ptr<Vector> current_plain_result;
};

struct StringParquetValueConversion {
	template <bool CHECKED>
	static string_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		auto &scr = reader.Cast<StringColumnReader>();
		uint32_t str_len =
		    scr.fixed_width_string_length == 0 ? plain_data.Read<uint32_t>() : scr.fixed_width_string_length;
		plain_data.Available(str_len);
		auto plain_str = char_ptr_cast(plain_data.GetCurrentLoc());
		auto ret_str = scr.VerifyString(plain_str, str_len);
		plain_data.Inc(str_len);
		return ret_str;
	}
	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		auto &scr = reader.Cast<StringColumnReader>();
		uint32_t str_len =
		    scr.fixed_width_string_length == 0 ? plain_data.Read<uint32_t>() : scr.fixed_width_string_length;
		plain_data.Inc(str_len);
	}
	static bool PlainAvailable(const ByteBuffer &plain_data, const idx_t count) {
		return false;
	}

	static idx_t PlainConstantSize() {
		return 0;
	}
};

} // namespace duckdb
