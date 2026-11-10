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
	virtual bool Match(Expression &expr, vector<reference<Expression>> &bindings);

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
	explicit ExpressionEqualityMatcher(const Expression &expr)
	    : ExpressionMatcher(ExpressionClass::INVALID), expression(expr) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;

private:
	const Expression &expression;
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

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class ComparisonExpressionMatcher : public ExpressionMatcher {
public:
	ComparisonExpressionMatcher()
	    : ExpressionMatcher(ExpressionClass::BOUND_FUNCTION), policy(SetMatcher::Policy::INVALID) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class CastExpressionMatcher : public ExpressionMatcher {
public:
	CastExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_FUNCTION) {
	}
	//! The matcher for the child expressions
	unique_ptr<ExpressionMatcher> matcher;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class NotExpressionMatcher : public ExpressionMatcher {
public:
	NotExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_OPERATOR) {
		expr_type = make_uniq<SpecificExpressionTypeMatcher>(ExpressionType::OPERATOR_NOT);
	}
	//! The matcher for the operand of the NOT (nullptr for ANY)
	unique_ptr<ExpressionMatcher> child;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class InClauseExpressionMatcher : public ExpressionMatcher {
public:
	InClauseExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_OPERATOR) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class InUniformExpressionMatcher : public ExpressionMatcher {
public:
	InUniformExpressionMatcher();

	//! The matchers for the probe and child expressions
	unique_ptr<ExpressionMatcher> probe_matcher;
	unique_ptr<ExpressionMatcher> child_matcher;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class ConjunctionExpressionMatcher : public ExpressionMatcher {
public:
	ConjunctionExpressionMatcher()
	    : ExpressionMatcher(ExpressionClass::BOUND_CONJUNCTION), policy(SetMatcher::Policy::INVALID) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
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

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

class AggregateExpressionMatcher : public ExpressionMatcher {
public:
	AggregateExpressionMatcher() : ExpressionMatcher(ExpressionClass::BOUND_AGGREGATE) {
	}
	//! The matchers for the child expressions
	vector<unique_ptr<ExpressionMatcher>> matchers;
	//! The set matcher matching policy to use
	SetMatcher::Policy policy;
	//! The function name to match
	unique_ptr<FunctionMatcher> function;

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

//! The FoldableConstant matcher matches any expression that is foldable into a constant by the ExpressionExecutor (i.e.
//! scalar but not aggregate/window/parameter)
class FoldableConstantMatcher : public ExpressionMatcher {
public:
	FoldableConstantMatcher() : ExpressionMatcher(ExpressionClass::INVALID) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

//! The stable expression matcher matches only stable expressions (non-volatile)
class StableExpressionMatcher : public ExpressionMatcher {
public:
	StableExpressionMatcher() : ExpressionMatcher(ExpressionClass::INVALID) {
	}

	bool Match(Expression &expr, vector<reference<Expression>> &bindings) override;
};

} // namespace duckdb
