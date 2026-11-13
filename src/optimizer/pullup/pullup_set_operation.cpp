#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"

namespace duckdb {

static void ReplaceFilterTableIndex(unique_ptr<Expression> &expr, LogicalSetOperation &setop) {
	expr = ExpressionIterator::PreorderReplace<BoundColumnRefExpression>(
	    std::move(expr), [&](const BoundColumnRefExpression &colref) {
		    D_ASSERT(colref.Depth() == 0);
		    return colref.WithBinding(ColumnBinding(setop.table_index, colref.Binding().column_index));
	    });
}

unique_ptr<LogicalOperator> FilterPullup::PullupSetOperation(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_INTERSECT || op->type == LogicalOperatorType::LOGICAL_EXCEPT);
	can_add_column = false;
	can_pullup = true;
	// EXCEPT and INTERSECT return rows of the left child, so a filter of the right child does not apply to them
	op = PullupFromLeft(std::move(op));
	if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op->Cast<LogicalFilter>();
		auto &setop = filter.children[0]->Cast<LogicalSetOperation>();
		for (idx_t i = 0; i < filter.expressions.size(); ++i) {
			ReplaceFilterTableIndex(filter.expressions[i], setop);
		}
	}
	return op;
}

} // namespace duckdb
