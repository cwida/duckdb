//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/literal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/hash.hpp"

namespace duckdb {

class Serializer;
class Deserializer;
class Value;

//! The syntactic kind of a literal atom in the query text
enum class LiteralKind : uint8_t {
	//! Default-constructed; never produced by the parser
	INVALID = 0,
	//! NULL
	NULL_LITERAL = 1,
	//! TRUE / FALSE - text is "true" or "false"
	BOOLEAN = 2,
	//! Digits with an optional leading '-' and '_' separators; no '.' and no exponent
	INTEGER = 3,
	//! Number text with a '.' and/or an exponent
	NUMERIC = 4,
	//! Decoded string text: quotes stripped, '' collapsed, E'' escapes decoded
	STRING = 5,
	//! X'..' - even-length hex digits without the prefix and quotes
	HEX = 6,
	//! B'..' - validated bit digits without the prefix and quotes
	BIT = 7,
	//! An address supplied by the host program as 0x-prefixed hex; never produced by the parser
	POINTER = 8
};

//! A literal atom as written in the query text. Holds no type information: the binder turns it into a Value.
struct Literal {
public:
	Literal() = default;
	Literal(LiteralKind kind, string text);

	static Literal Null();
	static Literal Boolean(bool value);
	static Literal Integer(int64_t value);
	//! Classifies the number text as INTEGER or NUMERIC
	static Literal Number(string text);
	static Literal String(string text);
	//! Validates the hex digits (throws ParserException)
	static Literal Hex(string text);
	//! Validates the bit digits (throws ParserException)
	static Literal Bit(string text);
	//! Wraps a host-program address; only reachable programmatically
	static Literal Pointer(uintptr_t address);

public:
	bool IsNumeric() const;
	bool IsNull() const;
	bool IsPointer() const;
	//! Whether this is an INTEGER literal that fits in an int64_t
	bool TryGetInt64(int64_t &result) const;
	//! Negates a numeric literal by toggling the leading '-'
	Literal Negate() const;

	//! Throws if text does not conform to kind (used to validate deserialized literals)
	void Verify() const;
	//! Converts the literal into a Value
	Value ToValue() const;
	//! Renders the literal as SQL text that parses back to the same literal
	string ToString() const;

	hash_t Hash() const;
	bool operator==(const Literal &other) const;
	bool operator!=(const Literal &other) const;

	void Serialize(Serializer &serializer) const;
	static Literal Deserialize(Deserializer &deserializer);

public:
	LiteralKind kind = LiteralKind::INVALID;
	string text;
};

} // namespace duckdb
