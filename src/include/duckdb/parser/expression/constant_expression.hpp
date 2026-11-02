//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/constant_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/literal.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

class Value;

//! ConstantExpression represents a literal atom in the query text
class ConstantExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::CONSTANT;

public:
	DUCKDB_API explicit ConstantExpression(Literal literal);
	//! Values are not literals - use ConstantExpression::FromValue
	explicit ConstantExpression(const Value &value) = delete;

	const Literal &GetLiteral() const {
		return literal;
	}

public:
	DUCKDB_API static unique_ptr<ConstantExpression> Null();
	DUCKDB_API static unique_ptr<ConstantExpression> Boolean(bool value);
	DUCKDB_API static unique_ptr<ConstantExpression> Integer(int64_t value);
	DUCKDB_API static unique_ptr<ConstantExpression> Number(string text);
	DUCKDB_API static unique_ptr<ConstantExpression> String(string text);
	DUCKDB_API static unique_ptr<ConstantExpression> Hex(string text);
	DUCKDB_API static unique_ptr<ConstantExpression> Bit(string text);
	DUCKDB_API static unique_ptr<ConstantExpression> FromLiteral(Literal literal);

	//! Builds the parsed expression for a value: a literal when it re-binds to the same value, the constructor
	//! call the parser produces for a nested value, or a cast otherwise
	DUCKDB_API static unique_ptr<ParsedExpression> FromValue(const Value &value);
	//! Whether SQL needs a value constructor to preserve the type's metadata
	DUCKDB_API static bool RequiresTypeWitness(const LogicalType &type);

public:
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

private:
	ConstantExpression();

	//! The value written when serializing for storage versions that predate literals
	Value GetValueForSerialization() const;
	//! Rebuilds the expression from either the literal or, for older storage versions, the value
	static unique_ptr<ParsedExpression> DeserializeConstant(const Value &value, Literal literal);

	//! The literal as written in the query
	Literal literal;
};

} // namespace duckdb
