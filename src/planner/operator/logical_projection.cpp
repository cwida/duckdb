#include "duckdb/planner/operator/logical_projection.hpp"

#include "duckdb/main/config.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {

LogicalProjection::LogicalProjection(TableIndex table_index, vector<unique_ptr<Expression>> select_list)
    : LogicalOperator(LogicalOperatorType::LOGICAL_PROJECTION, std::move(select_list)), table_index(table_index) {
}

unique_ptr<LogicalProjection> LogicalProjection::CreateIdentity(TableIndex table_index,
                                                                unique_ptr<LogicalOperator> child) {
	child->ResolveOperatorTypes();
	auto bindings = child->GetColumnBindings();
	vector<unique_ptr<Expression>> expressions;
	expressions.reserve(bindings.size());
	for (idx_t i = 0; i < bindings.size(); i++) {
		expressions.push_back(make_uniq<BoundColumnRefExpression>(child->types[i], bindings[i]));
	}
	auto projection = make_uniq<LogicalProjection>(table_index, std::move(expressions));
	projection->children.push_back(std::move(child));
	return projection;
}

vector<ColumnBinding> LogicalProjection::GetColumnBindings() {
	return GenerateColumnBindings(table_index, expressions.size());
}

void LogicalProjection::ResolveTypes() {
	for (auto &expr : expressions) {
		types.push_back(expr->GetReturnType());
	}
}

vector<TableIndex> LogicalProjection::GetTableIndex() const {
	return vector<TableIndex> {table_index};
}

string LogicalProjection::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", table_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

const Expression &LogicalProjection::GetExpression(ColumnBinding binding) const {
	if (binding.table_index != table_index) {
		throw InternalException("LogicalProjection::GetExpression - table index mismatch");
	}
	return *expressions[binding.column_index];
}

const Expression &LogicalProjection::GetExpression(ProjectionIndex proj_index) const {
	return GetExpression(ColumnBinding(table_index, proj_index));
}

} // namespace duckdb
