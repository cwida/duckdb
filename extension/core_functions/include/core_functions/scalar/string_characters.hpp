//===----------------------------------------------------------------------===//
//                         DuckDB
//
// core_functions/scalar/string_characters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

//! Whether the string only consists of ASCII characters, i.e. every byte is a character
inline bool StringIsAscii(const string_t &str) {
	auto data = str.GetData();
	for (idx_t i = 0; i < str.GetSize(); i++) {
		if (static_cast<unsigned char>(data[i]) >= 0x80) {
			return false;
		}
	}
	return true;
}

//! The characters (code points) of a UTF-8 string - a byte that is not valid UTF-8 is a character of its own
inline vector<int32_t> GetStringCharacters(const string_t &str) {
	vector<int32_t> result;
	auto data = str.GetData();
	auto len = str.GetSize();
	result.reserve(len);
	idx_t pos = 0;
	while (pos < len) {
		auto byte = static_cast<unsigned char>(data[pos]);
		if (byte < 0x80) {
			result.push_back(byte);
			pos++;
			continue;
		}
		int sz = 1;
		auto codepoint = Utf8Proc::UTF8ToCodepoint(data + pos, sz, len - pos);
		if (codepoint < 0 || sz <= 0) {
			result.push_back(byte);
			pos++;
			continue;
		}
		result.push_back(codepoint);
		pos += UnsafeNumericCast<idx_t>(sz);
	}
	return result;
}

} // namespace duckdb
