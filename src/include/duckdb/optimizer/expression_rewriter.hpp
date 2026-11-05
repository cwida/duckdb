//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/expression_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/optimizer/rule.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {
class ClientContext;

//! The ExpressionRewriter performs a set of fixed rewrite rules on the expressions that occur in a SQL statement
class ExpressionRewriter : public LogicalOperatorVisitor {
public:
	explicit ExpressionRewriter(ClientContext &context) : context(context) {
	}

public:
	ClientContext &context;

public:
	void VisitOperator(LogicalOperator &op) override;
	void VisitExpression(unique_ptr<Expression> *expression) override;

	//! Adds a custom rule; a rewriter with custom rules skips the default rule set
	void AddRule(unique_ptr<Rule> rule);

	// Generates either a constant_or_null(child) expression
	static unique_ptr<Expression> ConstantOrNull(ClientContext &context, unique_ptr<Expression> child, Value value);
	static unique_ptr<Expression> ConstantOrNull(ClientContext &context, vector<unique_ptr<Expression>> children,
	                                             Value value);

private:
	//! Returns the rewrite rules, constructing them on first access
	const vector<unique_ptr<Rule>> &GetRules();

	//! Apply a set of rules to a specific expression
	static unique_ptr<Expression> ApplyRules(LogicalOperator &op, const vector<reference<Rule>> &rules,
	                                         unique_ptr<Expression> expr, bool &changes_made, bool is_root = false);

	//! The set of rules as known by the Expression Rewriter
	vector<unique_ptr<Rule>> rules;
	optional_ptr<LogicalOperator> op;
	vector<reference<Rule>> to_apply_rules;
};

} // namespace duckdb
