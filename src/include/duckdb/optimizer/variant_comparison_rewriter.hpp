//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/variant_comparison_rewriter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
class BaseStatistics;
class BoundColumnRefExpression;
class BoundFunctionExpression;
class ClientContext;
class Expression;
class LogicalGet;
class LogicalOperator;
struct ColumnBinding;

//! Whether the expression is a wrapper that orders VARIANT values (variant_comparator / variant_normalize)
bool IsVariantWrapperFunction(const BoundFunctionExpression &expr);
//! If the expression is a wrapper around a single VARIANT column reference, returns that reference
optional_ptr<const BoundColumnRefExpression> TryGetVariantWrapperColumnRef(const Expression &expr);
//! If the expression is a wrapper with a single child, returns that child
optional_ptr<const Expression> TryGetVariantWrapperChild(const Expression &expr);

//! If the statistics describe a fully shredded VARIANT with integral values, returns the shredded type
bool TryGetFullyShreddedIntegralType(const BaseStatistics &stats, LogicalType &result);
//! Casts a VARIANT expression to its shredded type, and then to target_type if that type is wider
unique_ptr<Expression> CreateVariantShreddedCast(ClientContext &context, unique_ptr<Expression> input,
                                                 const LogicalType &shredded_type, const LogicalType &target_type);
//! Unifies the shredded type of a fully shredded VARIANT with the type of a constant being compared against it
bool TryGetVariantIntegralComparisonType(const LogicalType &shredded_type, const LogicalType &constant_type,
                                         LogicalType &result);

//! Replaces comparisons of fully shredded integral VARIANT columns with comparisons of the shredded values,
//! which avoids routing the filter through variant_comparator
class VariantComparisonRewriter {
public:
	explicit VariantComparisonRewriter(ClientContext &context);

public:
	void Rewrite(unique_ptr<LogicalOperator> &op);

private:
	void RewriteOperator(unique_ptr<LogicalOperator> &op);
	void RewriteFilter(LogicalOperator &op);
	bool RewriteExpression(unique_ptr<Expression> &expr);
	bool RewriteComparison(unique_ptr<Expression> &expr);
	bool RewriteBetween(unique_ptr<Expression> &expr);
	bool TryGetIntegralType(const BoundColumnRefExpression &colref, LogicalType &result);
	//! Resolves a column binding to the scan it originates from, and to the binding used by that scan
	optional_ptr<LogicalGet> ResolveSourceColumn(LogicalOperator &op, ColumnBinding &binding);

private:
	ClientContext &context;
	//! The operator below the filter that is currently being rewritten
	optional_ptr<LogicalOperator> source;
};
} // namespace duckdb
