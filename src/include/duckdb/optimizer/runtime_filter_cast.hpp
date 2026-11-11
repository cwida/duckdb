//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/runtime_filter_cast.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"

namespace duckdb {

class ClientContext;
class Expression;
struct JoinFilterPushdownColumn;

//! Utilities for expressions recorded on pushed runtime filter columns.
//! Shared by the join filter pushdown (physical_hash_join) and the TopN dynamic filter pushdown.
struct RuntimeFilterCastUtil {
	//! Whether a cast from source_type to target_type can fail at runtime
	static bool RuntimeFilterCastCanFail(const LogicalType &source_type, const LogicalType &target_type);
	//! Whether a bound cast uses the built-in implementation without custom bind data
	static bool IsBuiltinCast(const Expression &expr);
	//! Whether the expression contains (or requires) a try cast
	static bool RuntimeFilterUsesTryCast(const JoinFilterPushdownColumn &column);
	//! Whether the expression preserves the ordering of the raw projected column
	static bool PreservesStorageOrdering(const JoinFilterPushdownColumn &column);
	//! The type that CreateRuntimeFilterInputExpression evaluates to for this column
	static LogicalType GetRuntimeFilterInputType(const JoinFilterPushdownColumn &column,
	                                             const LogicalType &runtime_type);
	//! Whether evaluating the pushed runtime filter requires reconstructing the pushed expression
	//! on top of the projected scan value
	static bool RequiresRuntimeFilterExpressionReconstruction(const JoinFilterPushdownColumn &column,
	                                                          const LogicalType &runtime_type);
	//! Build the input expression for evaluating a pushed runtime filter on top of the raw scan column:
	//! a copy of the bound input expression, with potentially failing casts made non-throwing.
	//! Set `preserves_cast_errors` when a default (non-try) cast in the chain can fail.
	static unique_ptr<Expression> CreateRuntimeFilterInputExpression(ClientContext &context,
	                                                                 const JoinFilterPushdownColumn &column,
	                                                                 bool &preserves_cast_errors);
};

} // namespace duckdb
