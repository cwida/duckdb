//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/in_clause_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ClientContext;
class LogicalFilter;
class Optimizer;

class InClauseRewriter : public LogicalOperatorVisitor {
public:
	explicit InClauseRewriter(ClientContext &context, Optimizer &optimizer) : context(context), optimizer(optimizer) {
	}

	ClientContext &context;
	Optimizer &optimizer;
	optional_ptr<LogicalOperator> current_op;
	unique_ptr<LogicalOperator> root;

public:
	// Minimum number of values to use a hash join
	static constexpr idx_t IN_CLAUSE_REWRITE_THRESHOLD = 6;
	static bool HasRewritableInClause(const Expression &expr);
	unique_ptr<LogicalOperator> Rewrite(unique_ptr<LogicalOperator> op);
	//! skips error-capture interiors (TRY, has_error) and guarded conjunction/CASE/COALESCE children:
	//! the rewrite would move the probe's evaluation (and its errors) below the operator, outside the
	//! capture or the guard
	void VisitExpression(unique_ptr<Expression> *expression) override;
	unique_ptr<Expression> VisitReplace(BoundOperatorExpression &expr, unique_ptr<Expression> *expr_ptr) override;

private:
	//! Whether VisitExpression reaches a rewritable IN in this expression
	static bool VisitsRewritableInClause(const Expression &expr);
	//! Cut the filter in front of each barred expression holding an IN the rewriter turns into a join
	static void SplitBarrierFilter(LogicalFilter &filter);
};

} // namespace duckdb
