//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/matcher/expression_matcher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/optimizer/matcher/expression_type_matcher.hpp"
#include "duckdb/optimizer/matcher/set_matcher.hpp"
#include "duckdb/optimizer/matcher/type_matcher.hpp"
#include "duckdb/optimizer/matcher/function_matcher.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

//! The ExpressionMatcher class contains a set of matchers that can be used to pattern match Expressions
class ExpressionMatcher {
public:
	explicit ExpressionMatcher(ExpressionClass type = ExpressionClass::INVALID) : expr_class(type) {
	}
	virtual ~ExpressionMatcher() {
	}

	//! Checks if the given expression matches this ExpressionMatcher. If it does, the expression is appended to the
	//! bindings list and true is returned. Otherwise, false is returned.
	virtual bool Match(Expression *expr, vector<Expression *> &bindings);

	//! The ExpressionClass of the to-be-matched expression. ExpressionClass::INVALID for ANY.
	ExpressionClass expr_class;
	//! Matcher for the ExpressionType of the operator (nullptr for ANY)
	unique_ptr<ExpressionTypeMatcher> expr_type;
	//! Matcher for the return_type of the expression (nullptr for ANY)
	unique_ptr<TypeMatcher> type;
};

//! The ExpressionEqualityMatcher matches on equality with another (given) expression
class ExpressionEqualityMatcher : public ExpressionMatcher {
public:
	explicit ExpressionEqualityMatcher(Expression *expr)
	    : ExpressionMatcher(ExpressionClass::INVALID), expression(expr) {
	}

	bool Match(Expression *expr, vector<Expression *> &bindings) override;

private:
	Expression *expression;
};

class ConstantExpressionMatcher : public ExpressionMatcher {
public:
	ConstantExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_CONSTANT) {
	}
};

class CaseExpressionMatcher : public ExpressionMatcher {
public:
	CaseExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_CASE) {
	}
	//! The check expression to match (if any)
	unique_ptr<ExpressionMatcher> check;
	//! The result_if_true expression to match (if any)
	unique_ptr<ExpressionMatcher> result_if_true;
	//! The result_if_false expression to match (if any)
	unique_ptr<ExpressionMatcher> result_if_false;

	bool Match(Expression *expr_, vector<Expression *> &bindings) override;
};

class ComparisonExpressionMatcher : public ExpressionMatcher {
public:
	ComparisonExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_COMPARISON) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression *expr_, vector<Expression *> &bindings) override;
};

class InClauseExpressionMatcher : public ExpressionMatcher {
public:
	InClauseExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_OPERATOR) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression *expr_, vector<Expression *> &bindings) override;
};

class ConjunctionExpressionMatcher : public ExpressionMatcher {
public:
	ConjunctionExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_CONJUNCTION) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression *expr_, vector<Expression *> &bindings) override;
};

class FunctionExpressionMatcher : public ExpressionMatcher {
public:
	FunctionExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_FUNCTION) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;
	//! The function name to match
	unique_ptr<FunctionMatcher> function;

	bool Match(Expression *expr_, vector<Expression *> &bindings) override;
};

//! The FoldableConstant matcher matches any expression that is foldable into a constant by the ExpressionExecutor (i.e.
//! scalar but not aggregate/window/parameter)
class FoldableConstantMatcher : public ExpressionMatcher {
public:
	FoldableConstantMatcher() : ExpressionMatcher(ExpressionClass::INVALID) {
	}

	bool Match(Expression *expr, vector<Expression *> &bindings) override;
};

} // namespace duckdb
