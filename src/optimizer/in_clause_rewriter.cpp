#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

bool InClauseRewriter::HasRewritableInClause(const Expression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    (expr.GetExpressionType() == ExpressionType::COMPARE_IN ||
	     expr.GetExpressionType() == ExpressionType::COMPARE_NOT_IN)) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.GetChildren().size() >= InClauseRewriter::IN_CLAUSE_REWRITE_THRESHOLD) {
			bool all_foldable = true;
			for (idx_t child_idx = 1; child_idx < op.GetChildren().size(); child_idx++) {
				if (!op.GetChildren()[child_idx]->IsFoldable()) {
					all_foldable = false;
					break;
				}
			}
			if (all_foldable) {
				return true;
			}
		}
	}
	bool result = false;
	ExpressionIterator::EnumerateChildren(expr, [&](const Expression &child) {
		if (!result && HasRewritableInClause(child)) {
			result = true;
		}
	});
	return result;
}

unique_ptr<LogicalOperator> InClauseRewriter::Rewrite(unique_ptr<LogicalOperator> op) {
	switch (op->type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER: {
		current_op = op.get();
		root = std::move(op->children[0]);
		VisitOperatorExpressions(*op);
		op->children[0] = std::move(root);
		break;
	}
	default:
		break;
	}

	for (auto &child : op->children) {
		child = Rewrite(std::move(child));
	}
	return op;
}

unique_ptr<Expression> InClauseRewriter::VisitReplace(BoundOperatorExpression &expr, unique_ptr<Expression> *expr_ptr) {
	if (expr.GetExpressionType() != ExpressionType::COMPARE_IN &&
	    expr.GetExpressionType() != ExpressionType::COMPARE_NOT_IN) {
		return nullptr;
	}
	D_ASSERT(root);
	auto in_type = expr.GetChildrenMutable()[0]->GetReturnType();
	bool is_regular_in = expr.GetExpressionType() == ExpressionType::COMPARE_IN;
	bool all_scalar = true;
	// IN clause with many children: try to generate a mark join that replaces this IN expression
	// we can only do this if the expressions in the expression list are scalar
	for (idx_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
		if (!expr.GetChildrenMutable()[i]->IsFoldable()) {
			// non-scalar expression
			all_scalar = false;
		}
	}
	const bool rewrite_to_join = expr.GetChildrenMutable().size() >= IN_CLAUSE_REWRITE_THRESHOLD && all_scalar;
	if (!rewrite_to_join) {
		VisitExpressionChildren(expr);
	}
	if (expr.GetChildrenMutable().size() == 2) {
		// only one child
		// IN: turn into X = 1
		// NOT IN: turn into X <> 1
		return BoundComparisonExpression::Create(
		    is_regular_in ? ExpressionType::COMPARE_EQUAL : ExpressionType::COMPARE_NOTEQUAL,
		    std::move(expr.GetChildrenMutable()[0]), std::move(expr.GetChildrenMutable()[1]));
	}
	if (!rewrite_to_join) {
		if (expr.GetChildrenMutable()[0]->IsVolatile()) {
			return nullptr;
		}
		// low amount of children or not all scalar
		// IN: turn into (X = 1 OR X = 2 OR X = 3...)
		// NOT IN: turn into (X <> 1 AND X <> 2 AND X <> 3 ...)
		auto conjunction = make_uniq<BoundConjunctionExpression>(is_regular_in ? ExpressionType::CONJUNCTION_OR
		                                                                       : ExpressionType::CONJUNCTION_AND);
		for (idx_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
			conjunction->GetChildrenMutable().push_back(BoundComparisonExpression::Create(
			    is_regular_in ? ExpressionType::COMPARE_EQUAL : ExpressionType::COMPARE_NOTEQUAL,
			    expr.GetChildrenMutable()[0]->Copy(), std::move(expr.GetChildrenMutable()[i])));
		}
		return std::move(conjunction);
	}
	// IN clause with many constant children
	// generate a mark join that replaces this IN expression
	// first generate a ColumnDataCollection from the set of expressions
	vector<LogicalType> types = {in_type};
	auto collection = make_uniq<ColumnDataCollection>(context, types);
	ColumnDataAppendState append_state;
	collection->InitializeAppend(append_state);

	DataChunk chunk;
	chunk.Initialize(context, types);
	for (idx_t i = 1; i < expr.GetChildrenMutable().size(); i++) {
		// resolve this expression to a constant
		Value value;
		if (!ExpressionExecutor::TryEvaluateScalar(context, *expr.GetChildrenMutable()[i], value)) {
			// error while evaluating scalar
			return nullptr;
		}
		chunk.data[0].Append(value);
		if (chunk.size() == STANDARD_VECTOR_SIZE || i + 1 == expr.GetChildrenMutable().size()) {
			// chunk full: append to chunk collection
			collection->Append(append_state, chunk);
			chunk.Reset();
		}
	}
	// now generate a ChunkGet that scans this collection
	auto chunk_index = optimizer.binder.GenerateTableIndex();
	auto chunk_scan = make_uniq<LogicalColumnDataGet>(chunk_index, types, std::move(collection));

	// then we generate the MARK join with the chunk scan on the RHS
	auto mark_index = optimizer.binder.GenerateTableIndex();
	auto join = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
	join->mark_index = mark_index;
	join->AddChild(std::move(root));
	join->AddChild(std::move(chunk_scan));
	// create the JOIN condition
	JoinCondition cond(std::move(expr.GetChildrenMutable()[0]),
	                   make_uniq<BoundColumnRefExpression>(in_type, ColumnBinding(chunk_index, ProjectionIndex(0))),
	                   ExpressionType::COMPARE_EQUAL);
	join->conditions.push_back(std::move(cond));
	root = std::move(join);

	if (current_op->type == LogicalOperatorType::LOGICAL_FILTER) {
		// project out the mark index again
		auto &filter = current_op->Cast<LogicalFilter>();
		if (filter.projection_map.empty()) {
			auto child_bindings = root->GetColumnBindings();
			for (idx_t i = 0; i < child_bindings.size(); i++) {
				if (child_bindings[i].table_index != mark_index) {
					filter.projection_map.emplace_back(i);
				}
			}
		}
	}

	// we replace the original subquery with a BoundColumnRefExpression referring to the mark column
	unique_ptr<Expression> result = make_uniq<BoundColumnRefExpression>("IN (...)", LogicalType::BOOLEAN,
	                                                                    ColumnBinding(mark_index, ProjectionIndex(0)));
	if (!is_regular_in) {
		// NOT IN: invert
		auto invert = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
		invert->GetChildrenMutable().push_back(std::move(result));
		result = std::move(invert);
	}
	return result;
}

} // namespace duckdb
