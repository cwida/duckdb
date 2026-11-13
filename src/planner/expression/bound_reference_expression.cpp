#include "duckdb/planner/expression/bound_reference_expression.hpp"

#include "duckdb/common/to_string.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

BoundReferenceExpression::BoundReferenceExpression(Identifier alias, LogicalType type, idx_t index)
    : Expression(ExpressionType::BOUND_REF, ExpressionClass::BOUND_REF, std::move(type)), index(index) {
	this->alias = std::move(alias);
}
BoundReferenceExpression::BoundReferenceExpression(LogicalType type, storage_t index)
    : BoundReferenceExpression(Identifier(), std::move(type), index) {
}

string BoundReferenceExpression::ToString() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return StringUtil::Format("#%llu (%s)", index, return_type.ToString());
	}
#endif
	if (!alias.empty()) {
		return alias.GetIdentifierName();
	}
	return "#" + to_string(index);
}

bool BoundReferenceExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundReferenceExpression>();
	return other.index == index;
}

hash_t BoundReferenceExpression::Hash() const {
	return CombineHash(Expression::Hash(), duckdb::Hash<idx_t>(index));
}

unique_ptr<Expression> BoundReferenceExpression::WithIndex(idx_t new_index) const {
	auto result = make_uniq<BoundReferenceExpression>(alias, return_type, new_index);
	result->query_location = query_location;
	return std::move(result);
}

unique_ptr<Expression> BoundReferenceExpression::Copy() const {
	return make_uniq<BoundReferenceExpression>(alias, return_type, index);
}

} // namespace duckdb
