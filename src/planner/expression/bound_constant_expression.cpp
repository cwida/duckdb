#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

BoundConstantExpression::BoundConstantExpression(Value value_p)
    : Expression(ExpressionType::VALUE_CONSTANT, ExpressionClass::BOUND_CONSTANT, value_p.type()),
      value(std::move(value_p)) {
}

void BoundConstantExpression::SetValue(Value value_p) {
	return_type = value_p.type();
	value = std::move(value_p);
}

Value BoundConstantExpression::TakeValue() {
	return std::move(value);
}

void BoundConstantExpression::SetReturnType(LogicalType type) {
	D_ASSERT(value.type() == type);
	value = value.WithType(type);
	return_type = std::move(type);
}

string BoundConstantExpression::ToString() const {
	return value.ToSQLString();
}

//! Two constants are the same expression if their values have the same representation. VARIANT values compare by
//! meaning - 1::TINYINT and 1::INTEGER are equal - but as constants they are different: variant_typeof tells them
//! apart, and they hash differently. Vectors compare a VARIANT as the STRUCT it is stored as.
static bool SameRepresentation(const Value &left, const Value &right) {
	if (!TypeVisitor::Contains(left.type(), LogicalTypeId::VARIANT)) {
		return !ValueOperations::DistinctFrom(left, right);
	}
	Vector left_vector(left, count_t(1));
	Vector right_vector(right, count_t(1));
	return VectorOperations::NotDistinctFrom(left_vector, right_vector, nullptr, 1, nullptr, nullptr) == 1;
}

bool BoundConstantExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundConstantExpression>();
	return value.type() == other.value.type() && SameRepresentation(value, other.value);
}

hash_t BoundConstantExpression::Hash() const {
	hash_t result = Expression::Hash();
	return CombineHash(value.Hash(), result);
}

unique_ptr<Expression> BoundConstantExpression::Copy() const {
	auto copy = make_uniq<BoundConstantExpression>(value);
	copy->CopyProperties(*this);
	return std::move(copy);
}

} // namespace duckdb
