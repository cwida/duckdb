#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_window.hpp"
#include "duckdb/planner/query_node/bound_set_operation_node.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/function/window/rows_functions.hpp"
#include "duckdb/function/window_function.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

// Optionally push a PROJECTION operator
unique_ptr<LogicalOperator> Binder::CastLogicalOperatorToTypes(const vector<LogicalType> &source_types,
                                                               const vector<LogicalType> &target_types,
                                                               unique_ptr<LogicalOperator> op) {
	D_ASSERT(op);
	D_ASSERT(source_types.size() == target_types.size());
	auto node = op.get();
	if (source_types == target_types) {
		bool has_cast = false;
		if (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
			for (auto &expression : node->expressions) {
				if (BoundCastExpression::IsCast(*expression)) {
					has_cast = true;
					break;
				}
			}
		}
		if (!has_cast) {
			return op;
		}
	}
	if (node->type == LogicalOperatorType::LOGICAL_PROJECTION) {
		D_ASSERT(node->expressions.size() == source_types.size());
		// add the casts to the selection list
		for (idx_t i = 0; i < target_types.size(); i++) {
			if (source_types[i] != target_types[i]) {
				// differing types, have to add a cast
				auto cur_alias = node->expressions[i]->GetAlias();
				node->expressions[i] =
				    BoundCastExpression::AddCastToType(context, std::move(node->expressions[i]), target_types[i]);
				node->expressions[i]->SetAlias(cur_alias);
			}
		}
		return op;
	} else {
		if (source_types == target_types) {
			return op;
		}
		// found a non-projection operator
		// push a new projection containing the casts

		// fetch the set of column bindings
		auto setop_columns = op->GetColumnBindings();
		D_ASSERT(setop_columns.size() == source_types.size());

		// now generate the expression list
		vector<unique_ptr<Expression>> select_list;
		for (idx_t i = 0; i < target_types.size(); i++) {
			unique_ptr<Expression> result = make_uniq<BoundColumnRefExpression>(source_types[i], setop_columns[i]);
			if (source_types[i] != target_types[i]) {
				// add a cast only if the source and target types are not equivalent
				result = BoundCastExpression::AddCastToType(context, std::move(result), target_types[i]);
			}
			select_list.push_back(std::move(result));
		}
		auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(select_list));
		projection->children.push_back(std::move(op));
		return std::move(projection);
	}
}

//! Numbers the rows that are equal in all columns, so that EXCEPT ALL and INTERSECT ALL match the n-th copies of a row
static unique_ptr<LogicalOperator> AddRowNumber(Binder &binder, const vector<LogicalType> &types,
                                                unique_ptr<LogicalOperator> child) {
	auto &context = binder.context;
	auto bindings = child->GetColumnBindings();
	auto row_number = GetBuiltinWindowFunction(context, RowNumberFun::Name, {})->Bind(context);
	row_number->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	row_number->WindowEndMutable() = WindowBoundary::UNBOUNDED_FOLLOWING;
	for (idx_t i = 0; i < types.size(); i++) {
		unique_ptr<Expression> partition = make_uniq<BoundColumnRefExpression>(types[i], bindings[i]);
		ExpressionBinder::PushCollation(context, partition, types[i]);
		row_number->PartitionsMutable().push_back(std::move(partition));
	}
	auto window = make_uniq<LogicalWindow>(binder.GenerateTableIndex());
	window->expressions.push_back(std::move(row_number));
	window->AddChild(std::move(child));
	return std::move(window);
}

//! Plans EXCEPT as an ANTI join and INTERSECT as a SEMI join on all columns, which compares them with the collations
//! of both children and treats NULLs as equal
static unique_ptr<LogicalOperator> CreateSetOperationJoin(Binder &binder, BoundSetOperationNode &node,
                                                          unique_ptr<LogicalOperator> left,
                                                          unique_ptr<LogicalOperator> right) {
	auto &types = node.types;
	auto compared_types = types;
	if (node.setop_all) {
		left = AddRowNumber(binder, types, std::move(left));
		right = AddRowNumber(binder, types, std::move(right));
		compared_types.push_back(LogicalType::BIGINT);
	}
	auto left_bindings = left->GetColumnBindings();
	auto right_bindings = right->GetColumnBindings();
	D_ASSERT(left_bindings.size() == compared_types.size() && right_bindings.size() == compared_types.size());

	auto join_type = node.setop_type == SetOperationType::EXCEPT ? JoinType::ANTI : JoinType::SEMI;
	auto join = make_uniq<LogicalComparisonJoin>(join_type);
	for (idx_t i = 0; i < compared_types.size(); i++) {
		auto &type = compared_types[i];
		JoinCondition condition(make_uniq<BoundColumnRefExpression>(type, left_bindings[i]),
		                        make_uniq<BoundColumnRefExpression>(type, right_bindings[i]),
		                        ExpressionType::COMPARE_NOT_DISTINCT_FROM);
		ExpressionBinder::PushCollation(binder.context, condition.LeftReference(), type);
		ExpressionBinder::PushCollation(binder.context, condition.RightReference(), type);
		join->conditions.push_back(std::move(condition));
	}
	join->AddChild(std::move(left));
	join->AddChild(std::move(right));

	vector<unique_ptr<Expression>> select_list;
	for (idx_t i = 0; i < types.size(); i++) {
		select_list.push_back(make_uniq<BoundColumnRefExpression>(types[i], left_bindings[i]));
	}
	auto projection = make_uniq<LogicalProjection>(node.setop_index, std::move(select_list));
	projection->AddChild(std::move(join));
	return std::move(projection);
}

unique_ptr<LogicalOperator> Binder::CreatePlan(BoundSetOperationNode &node) {
	// Generate the logical plan for the children of the set operation
	D_ASSERT(node.bound_children.size() >= 2);
	vector<unique_ptr<LogicalOperator>> children;
	for (auto &child : node.bound_children) {
		// construct the logical plan for the child node
		auto child_node = std::move(child.plan);
		// push casts for the target types
		child_node = CastLogicalOperatorToTypes(child.types, node.types, std::move(child_node));
		children.push_back(std::move(child_node));
	}
	unique_ptr<LogicalOperator> root;
	switch (node.setop_type) {
	case SetOperationType::UNION:
	case SetOperationType::UNION_BY_NAME:
		root = make_uniq<LogicalSetOperation>(node.setop_index, node.types.size(), std::move(children),
		                                      LogicalOperatorType::LOGICAL_UNION, node.setop_all);
		break;
	case SetOperationType::EXCEPT:
	case SetOperationType::INTERSECT:
		D_ASSERT(children.size() == 2);
		root = CreateSetOperationJoin(*this, node, std::move(children[0]), std::move(children[1]));
		break;
	default:
		throw InternalException("Unsupported logical operator type for set-operation");
	}
	return VisitQueryNode(node, std::move(root));
}

} // namespace duckdb
