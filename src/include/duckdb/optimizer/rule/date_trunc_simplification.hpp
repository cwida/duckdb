//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/rule/date_trunc_simplification.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enum_util.hpp"
#include "duckdb/optimizer/rule.hpp"

namespace duckdb {

// DateTruncSimplificationRule rewrites an expression of the form
//
//    date_trunc(part, column) <op> const_rhs
//
// such that the date_trunc is instead applied to the constant RHS and then simplified further.
// The rules applied are as follows:
//
//   date_trunc(part, column) >= const_rhs   -->   column >= date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//    - but if date_trunc(const_rhs) = const_rhs, then we can do column >= const_rhs
//
//   date_trunc(part, column) <= const_rhs   -->   column <= date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//
//   date_trunc(part, column) >  const_rhs   -->   column >= date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//    - note the change from > to >=!
//
//   date_trunc(part, column) <  const_rhs   -->   column <  date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//    - but if date_trunc(const_rhs) = const_rhs, then we can do column < const_rhs
//
//   date_trunc(part, column) == const_rhs   -->   column >= date_trunc(part, const_rhs) AND
//                                                    column <  date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//    - but if date_trunc(const_rhs) != const_rhs, then this one is unsatisfiable
//
//   date_trunc(part, column) <> const_rhs   -->   column <  date_trunc(part, const_rhs) OR
//                                                    column >= date_trunc(part, date_add(const_rhs, INTERVAL 1 part))
//    - but if date_trunc(const_rhs) != const_rhs, then this is always satisfied
//
//   date_trunc(part, column) IS NOT DISTINCT FROM const_rhs  --> (column >= date_trunc(part, const_rhs) AND
//                                                                 column < date_trunc(part,
//                                                                     date_add(const_rhs, INTERVAL 1 part)) AND
//                                                                 column IS NOT NULL)
//    - but if const_rhs is NULL, then this is just 'column IS NULL'
//
//   date_trunc(part, column) IS DISTINCT FROM const_rhs  -->  (column < date_trunc(part, const_rhs) OR
//                                                              column >= date_trunc(part,
//                                                                  date_add(const_rhs, INTERVAL 1 part)) OR
//                                                              column IS NULL)
//    - but if const_rhs is NULL, then this is just 'column IS NOT NULL'
//
class DateTruncSimplificationRule : public Rule {
public:
	explicit DateTruncSimplificationRule(ExpressionRewriter &rewriter);

	unique_ptr<Expression> Apply(LogicalOperator &op, vector<reference<Expression>> &bindings, bool &changes_made,
	                             bool is_root) override;

	static string DatePartToFunc(const DatePartSpecifier &date_part);

	unique_ptr<Expression> CreateTrunc(const BoundConstantExpression &date_part, const BoundConstantExpression &rhs,
	                                   const LogicalType &return_type);
	unique_ptr<Expression> CreateTruncAdd(const BoundConstantExpression &date_part, const BoundConstantExpression &rhs,
	                                      const LogicalType &return_type);

	//! Whether the truncation check could run - on success, is_truncated holds the outcome
	bool DateIsTruncated(const BoundConstantExpression &date_part, const BoundConstantExpression &rhs,
	                     bool &is_truncated);
	bool IsInfinity(const Value &value);
	void ReplaceDateTruncWithColumn(unique_ptr<Expression> &column_side, unique_ptr<Expression> &constant_side,
	                                const BoundColumnRefExpression &column_part);

	unique_ptr<Expression> CastAndEvaluate(unique_ptr<Expression> rhs, const LogicalType &return_type);
};

} // namespace duckdb
