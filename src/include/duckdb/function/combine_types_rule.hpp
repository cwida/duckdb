//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/combine_types_rule.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {
class ClientContext;
class DecimalWidthLimit;

class LogicalTypeResolver {
public:
	explicit LogicalTypeResolver(optional_ptr<ClientContext> context_p,
	                             optional_ptr<const DecimalWidthLimit> decimal_width_limit_p = nullptr)
	    : context(context_p), decimal_width_limit(decimal_width_limit_p) {
	}
	virtual ~LogicalTypeResolver() = default;

	virtual bool Operation(const LogicalType &left, const LogicalType &right, LogicalType &result) = 0;
	//! The widest DECIMAL combining constructs: the limit, or the max_decimal_width setting without one
	DecimalWidthLimit GetDecimalWidthLimit() const;

public:
	optional_ptr<ClientContext> context;
	//! The limit of the DECIMAL widths the binder constructs, if one is given
	optional_ptr<const DecimalWidthLimit> decimal_width_limit;
};

typedef bool (*combine_types_rule_function_t)(LogicalTypeResolver &resolver, const LogicalType &left,
                                              const LogicalType &right, LogicalType &result);

struct CombineTypesRule {
	bool (*matches)(const LogicalType &left, const LogicalType &right); // order-insensitive in (left, right)
	combine_types_rule_function_t function;
};

//! The built-in combine rules, used to seed CastFunctionSet and as the fallback when no context is available
const vector<CombineTypesRule> &DefaultCombineTypesRules();

} // namespace duckdb
