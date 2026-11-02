#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {

CastExpression::CastExpression(unique_ptr<TypeExpression> target, unique_ptr<ParsedExpression> child_p, bool try_cast_p)
    : ParsedExpression(ExpressionType::OPERATOR_CAST, ExpressionClass::CAST), cast_type(std::move(target)),
      try_cast(try_cast_p) {
	D_ASSERT(child_p);
	D_ASSERT(cast_type);
	this->child = std::move(child_p);
}

CastExpression::CastExpression(const LogicalType &target, unique_ptr<ParsedExpression> child_p, bool try_cast_p)
    : CastExpression(TypeExpression::FromLogicalType(target), std::move(child_p), try_cast_p) {
}

CastExpression::CastExpression() : ParsedExpression(ExpressionType::OPERATOR_CAST, ExpressionClass::CAST) {
}

void CastExpression::SetTargetType(unique_ptr<TypeExpression> target) {
	D_ASSERT(target);
	cast_type = std::move(target);
}

string CastExpression::ToString() const {
	return ToString<CastExpression, ParsedExpression>(*this);
}

void CastExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child", child);
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		// Older versions store the cast target as a LogicalType. Resolve built-in types rather than writing an
		// unbound type: below v1.5.0 an unbound type degrades to the legacy USER type, whose modifiers are bare
		// values, so a named modifier such as a VARCHAR collation would be lost.
		D_ASSERT(cast_type);
		serializer.WriteProperty<LogicalType>(201, "cast_type", UnboundType::TryDefaultBind(*cast_type));
	}
	serializer.WritePropertyWithDefault<bool>(202, "try_cast", try_cast);
	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<unique_ptr<TypeExpression>>(203, "type_expr", cast_type);
	}
}

unique_ptr<ParsedExpression> CastExpression::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<CastExpression>(new CastExpression());
	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(200, "child", result->child);
	auto cast_type = deserializer.ReadPropertyWithExplicitDefault<LogicalType>(201, "cast_type", LogicalType::INVALID);
	deserializer.ReadPropertyWithDefault<bool>(202, "try_cast", result->try_cast);
	auto type_expression =
	    deserializer.ReadPropertyWithExplicitDefault<unique_ptr<ParsedExpression>>(203, "type_expr", nullptr);

	if (type_expression) {
		if (type_expression->GetExpressionClass() != ExpressionClass::TYPE) {
			throw SerializationException("Failed to deserialize CAST expression: the target is not a type expression");
		}
		result->cast_type = unique_ptr_cast<ParsedExpression, TypeExpression>(std::move(type_expression));
	} else {
		// written by a version that stored the target as a LogicalType
		if (cast_type.id() == LogicalTypeId::INVALID) {
			throw SerializationException("Failed to deserialize CAST expression: missing target type");
		}
		result->cast_type = TypeExpression::FromLogicalType(cast_type);
	}
	return std::move(result);
}

} // namespace duckdb
