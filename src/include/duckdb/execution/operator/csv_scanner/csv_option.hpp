//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_option.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/scalar/strftime_format.hpp"

namespace duckdb {

enum class NewLineIdentifier : uint8_t {
	SINGLE_N = 1, // \n
	CARRY_ON = 2, // \r\n
	NOT_SET = 3,
	SINGLE_R = 4 // \r

};

class Serializer;
class Deserializer;

//! Wrapper for CSV Options that can be manually set by the user
//! It is important to make this difference for options that can be automatically sniffed AND manually set.
template <typename T>
struct CSVOption { // NOLINT: work-around bug in clang-tidy
public:
	CSVOption(T value_p) : value(value_p) { // NOLINT: allow implicit conversion from value
	}
	CSVOption(T value_p, bool set_by_user_p) : set_by_user(set_by_user_p), value(value_p) {
	}

	CSVOption() {};

	//! Sets value.
	//! If by user it also toggles the set_by user flag
	void Set(T value_p, bool by_user = true) {
		D_ASSERT(!(by_user && set_by_user));
		if (!set_by_user) {
			// If it's not set by user we can change the value
			value = std::move(value_p); // NOLINT(performance-move-const-arg)
			set_by_user = by_user;
		}
	}

	//! Sets value.
	//! If by user it also toggles the set_by user flag
	void Set(CSVOption value_p, bool by_user = true) {
		D_ASSERT(!(by_user && set_by_user));
		if (!set_by_user) {
			// If it's not set by user we can change the value
			value = value_p;
			set_by_user = by_user;
		}
	}

	// this is due to a hacky implementation in the read csv relation
	void ChangeSetByUserTrue() {
		set_by_user = true;
	}

	bool operator==(const CSVOption &other) const {
		return value == other.value;
	}

	bool operator!=(const CSVOption &other) const {
		return value != other.value;
	}

	bool operator==(const T &other) const {
		return value == other;
	}

	bool operator!=(const T &other) const {
		return value != other;
	}
	//! Returns CSV Option value
	inline const T &GetValue() const {
		return value;
	}
	bool IsSetByUser() const {
		return set_by_user;
	}

	//! Returns a formatted string with information regarding how this option was set
	string FormatSet() const {
		if (set_by_user) {
			return "(Set By User)";
		}
		return "(Auto-Detected)";
	}

	//! Returns a formatted string with the actual value of this option
	string FormatValue() const {
		return FormatValueInternal(value);
	}

	//! Serializes CSV Option
	DUCKDB_API void Serialize(Serializer &serializer) const;

	//! Deserializes CSV Option
	DUCKDB_API static CSVOption<T> Deserialize(Deserializer &deserializer);

private:
	//! If this option was manually set by the user
	bool set_by_user = false;
	T value;

	//! --------------------------------------------------- //
	//! Functions used to convert a value to a string
	//! --------------------------------------------------- //

	template <typename U>
	std::string FormatValueInternal(const U &val) const {
		throw InternalException("Type not accepted as CSV Option.");
	}

	std::string FormatValueInternal(const std::string &val) const {
		return val;
	}

	std::string FormatValueInternal(const idx_t &val) const {
		return to_string(val);
	}

	std::string FormatValueInternal(const char &val) const {
		string char_val;
		if (val == '\0') {
			char_val = "(empty)";
			return char_val;
		}
		char_val += val;
		return char_val;
	}

	std::string FormatValueInternal(const NewLineIdentifier &val) const {
		switch (val) {
		case NewLineIdentifier::SINGLE_N:
			return "\\n";
		case NewLineIdentifier::SINGLE_R:
			return "\\r";
		case NewLineIdentifier::CARRY_ON:
			return "\\r\\n";
		case NewLineIdentifier::NOT_SET:
			return "Single-Line File";
		default:
			throw InternalException("Invalid Newline Detected.");
		}
	}

	std::string FormatValueInternal(const StrpTimeFormat &val) const {
		return val.format_specifier;
	}

	std::string FormatValueInternal(const bool &val) const {
		if (val) {
			return "true";
		}
		return "false";
	}
};

} // namespace duckdb
