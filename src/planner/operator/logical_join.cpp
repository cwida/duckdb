#include "duckdb/planner/operator/logical_join.hpp"

#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/main/config.hpp"

namespace duckdb {

LogicalJoin::LogicalJoin(JoinType join_type, LogicalOperatorType logical_type)
    : LogicalOperator(logical_type), join_type(join_type) {
}

vector<ColumnBinding> LogicalJoin::GetColumnBindings() {
	auto left_bindings = MapBindings(children[0]->GetColumnBindings(), left_projection_map);
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		// for SEMI and ANTI join we only project the left hand side
		return left_bindings;
	}

	if (join_type == JoinType::MARK) {
		// for MARK join we project the left hand side plus the MARK column
		left_bindings.emplace_back(mark_index, ProjectionIndex(0));
		return left_bindings;
	}
	// for other join types we project both the LHS and the RHS
	auto right_bindings = MapBindings(children[1]->GetColumnBindings(), right_projection_map);
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		return right_bindings;
	}
	left_bindings.insert(left_bindings.end(), right_bindings.begin(), right_bindings.end());
	return left_bindings;
}

vector<TableIndex> LogicalJoin::GetTableIndex() const {
	vector<TableIndex> result;
	if (join_type == JoinType::MARK) {
		result.emplace_back(mark_index);
	}
	return result;
}

string LogicalJoin::GetName() const {
#ifdef DEBUG
	if (DBConfigOptions::debug_print_bindings && join_type == JoinType::MARK) {
		return LogicalOperator::GetName() + StringUtil::Format(" #%llu", mark_index.index);
	}
#endif
	return LogicalOperator::GetName();
}

void LogicalJoin::ResolveTypes() {
	types = MapTypes(children[0]->types, left_projection_map);
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		// for SEMI and ANTI join we only project the left hand side
		return;
	}
	if (join_type == JoinType::MARK) {
		// for MARK join we project the left hand side, plus a BOOLEAN column indicating the MARK
		types.emplace_back(LogicalType::BOOLEAN);
		return;
	}
	// for any other join we project both sides
	auto right_types = MapTypes(children[1]->types, right_projection_map);
	if (join_type == JoinType::RIGHT_SEMI || join_type == JoinType::RIGHT_ANTI) {
		types = right_types;
		return;
	}
	types.insert(types.end(), right_types.begin(), right_types.end());
}

void LogicalJoin::GetTableReferences(LogicalOperator &op, unordered_set<TableIndex> &bindings) {
	auto column_bindings = op.GetColumnBindings();
	for (auto binding : column_bindings) {
		bindings.insert(binding.table_index);
	}
}

void LogicalJoin::GetExpressionBindings(const Expression &root_expr, unordered_set<TableIndex> &bindings) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(root_expr,
	                                                              [&](const BoundColumnRefExpression &colref) {
		                                                              D_ASSERT(colref.Depth() == 0);
		                                                              bindings.insert(colref.Binding().table_index);
	                                                              });
}

unique_ptr<LogicalOperator> LogicalJoin::PromoteJoinSide(unique_ptr<LogicalOperator> child,
                                                         vector<ProjectionIndex> projection_map) {
	if (projection_map.empty()) {
		return child;
	}
	auto filter = make_uniq<LogicalFilter>();
	filter->projection_map = std::move(projection_map);
	filter->children.push_back(std::move(child));
	if (filter->children[0]->has_estimated_cardinality) {
		filter->SetEstimatedCardinality(filter->children[0]->estimated_cardinality);
	}
	filter->ResolveOperatorTypes();
	return std::move(filter);
}

void LogicalJoin::MoveJoinState(LogicalJoin &source, LogicalJoin &target) {
	D_ASSERT(source.join_type == target.join_type);
	target.mark_index = source.mark_index;
	target.left_projection_map = std::move(source.left_projection_map);
	target.right_projection_map = std::move(source.right_projection_map);
	if (source.has_estimated_cardinality) {
		target.SetEstimatedCardinality(source.estimated_cardinality);
	}
}

} // namespace duckdb
