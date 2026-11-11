#include "duckdb/planner/expression/bound_columnref_expression.hpp"

#include "duckdb/common/types/hash.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

BoundColumnRefExpression::BoundColumnRefExpression(Identifier alias_p, LogicalType type, ColumnBinding binding,
                                                   idx_t depth, QueryLocation location)
    : Expression(ExpressionType::BOUND_COLUMN_REF, ExpressionClass::BOUND_COLUMN_REF, std::move(type)),
      binding(binding), depth(depth) {
	this->alias = std::move(alias_p);
	query_location = location;
}

BoundColumnRefExpression::BoundColumnRefExpression(LogicalType type, ColumnBinding binding, idx_t depth)
    : BoundColumnRefExpression(Identifier(), std::move(type), binding, depth) {
}

unique_ptr<Expression> BoundColumnRefExpression::Copy() const {
	return make_uniq<BoundColumnRefExpression>(alias, return_type, binding, depth);
}

hash_t BoundColumnRefExpression::Hash() const {
	auto result = Expression::Hash();
	result = CombineHash(result, duckdb::Hash<ProjectionIndex>(binding.column_index));
	result = CombineHash(result, duckdb::Hash<TableIndex>(binding.table_index));
	return CombineHash(result, duckdb::Hash<uint64_t>(depth));
}

bool BoundColumnRefExpression::Equals(const BaseExpression &other_p) const {
	if (!Expression::Equals(other_p)) {
		return false;
	}
	auto &other = other_p.Cast<BoundColumnRefExpression>();
	return other.binding == binding && other.depth == depth;
}

Identifier BoundColumnRefExpression::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return Identifier(StringUtil::Format("%s (%s)", binding.ToString(), return_type.ToString()));
	}
#endif
	return Expression::GetName();
}

string BoundColumnRefExpression::ToString() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return binding.ToString();
	}
#endif
	if (!alias.empty()) {
		return alias.GetIdentifierName();
	}
	return binding.ToString();
}

} // namespace duckdb
