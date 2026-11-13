//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sql_identifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include <ostream>

namespace duckdb {

class String;
class Identifier;

// Helper class to support custom overloading
// Optionally escaping " and quoting the value with " if required
class SQLIdentifier {
public:
	explicit SQLIdentifier(const string &raw_string) : raw_string(raw_string) {
	}
	explicit SQLIdentifier(const char *raw_string) : raw_string(raw_string) {
	}
	explicit SQLIdentifier(const Identifier &id);

	//! Emits an optionally quoted identifier including required escapes (i.e. ident -> ident, table -> "table")
	static string ToString(const string &identifier);
	static string ToString(const Identifier &identifier);
	//! Needed to disambiguate between the string and Identifier overloads, as string literals convert to both
	static string ToString(const char *identifier);

public:
	string raw_string;
};

inline string operator+(const SQLIdentifier &lhs, const string &rhs) {
	return SQLIdentifier::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const SQLIdentifier &lhs, const char *rhs) {
	return SQLIdentifier::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const string &lhs, const SQLIdentifier &rhs) {
	return lhs + SQLIdentifier::ToString(rhs.raw_string);
}
inline string operator+(const char *lhs, const SQLIdentifier &rhs) {
	return lhs + SQLIdentifier::ToString(rhs.raw_string);
}
inline string &operator+=(string &lhs, const SQLIdentifier &rhs) {
	return lhs += SQLIdentifier::ToString(rhs.raw_string);
}
inline std::ostream &operator<<(std::ostream &os, const SQLIdentifier &val) {
	return os << SQLIdentifier::ToString(val.raw_string);
}

// Helper class to support custom overloading
// Escaping " and quoting the value with " if required
class SQLQuotedIdentifier {
public:
	explicit SQLQuotedIdentifier(const string &raw_string) : raw_string(raw_string) {
	}
	explicit SQLQuotedIdentifier(const char *raw_string) : raw_string(raw_string) {
	}
	explicit SQLQuotedIdentifier(const Identifier &id);

	//! Emits an optionally quoted identifier including required escapes (i.e. ident -> ident, table -> "table")
	static string ToString(const string &identifier);
	static string ToString(const Identifier &identifier);
	//! Needed to disambiguate between the string and Identifier overloads, as string literals convert to both
	static string ToString(const char *identifier);

public:
	string raw_string;
};

inline string operator+(const SQLQuotedIdentifier &lhs, const string &rhs) {
	return SQLQuotedIdentifier::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const SQLQuotedIdentifier &lhs, const char *rhs) {
	return SQLQuotedIdentifier::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const string &lhs, const SQLQuotedIdentifier &rhs) {
	return lhs + SQLQuotedIdentifier::ToString(rhs.raw_string);
}
inline string operator+(const char *lhs, const SQLQuotedIdentifier &rhs) {
	return lhs + SQLQuotedIdentifier::ToString(rhs.raw_string);
}
inline string &operator+=(string &lhs, const SQLQuotedIdentifier &rhs) {
	return lhs += SQLQuotedIdentifier::ToString(rhs.raw_string);
}
inline std::ostream &operator<<(std::ostream &os, const SQLQuotedIdentifier &val) {
	return os << SQLQuotedIdentifier::ToString(val.raw_string);
}

// Helper class to support custom overloading
// Escaping ' and quoting the value with '
class SQLString {
public:
	explicit SQLString(const string &raw_string) : raw_string(raw_string) {
	}
	explicit SQLString(const char *raw_string) : raw_string(raw_string) {
	}
	explicit SQLString(const Identifier &id);

	//! Emits a quoted string literal including required escapes (i.e. ident -> 'ident')
	static string ToString(const string &literal);
	static string ToString(const Identifier &literal);
	//! Needed to disambiguate between the string and Identifier overloads, as string literals convert to both
	static string ToString(const char *literal);

public:
	string raw_string;
};

inline string operator+(const SQLString &lhs, const string &rhs) {
	return SQLString::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const SQLString &lhs, const char *rhs) {
	return SQLString::ToString(lhs.raw_string) + rhs;
}
inline string operator+(const string &lhs, const SQLString &rhs) {
	return lhs + SQLString::ToString(rhs.raw_string);
}
inline string operator+(const char *lhs, const SQLString &rhs) {
	return lhs + SQLString::ToString(rhs.raw_string);
}
inline string &operator+=(string &lhs, const SQLString &rhs) {
	return lhs += SQLString::ToString(rhs.raw_string);
}
inline std::ostream &operator<<(std::ostream &os, const SQLString &val) {
	return os << SQLString::ToString(val.raw_string);
}

} // namespace duckdb
