//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/aggregate_rewrite_helper.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {
class LogicalAggregate;
class Optimizer;

struct AggregateRewriteHelper {
	static optional_idx GetDirectReferenceIndex(const Expression &expression, LogicalOperator &input);
	static vector<Identifier> GenerateColumnNames(const string &prefix, idx_t column_count);
	static unique_ptr<Expression> CopyAndRebind(const Expression &expr,
	                                            const column_binding_map_t<ColumnBinding> &replacement_map);
	static void StageVolatileAggregateInputs(Optimizer &optimizer, LogicalAggregate &aggr,
	                                         unique_ptr<LogicalOperator> &child);
	static unique_ptr<LogicalOperator> CreateCTERef(Optimizer &optimizer, TableIndex cte_index,
	                                                const vector<LogicalType> &input_types,
	                                                const vector<Identifier> &input_names,
	                                                const vector<ColumnBinding> &input_bindings,
	                                                column_binding_map_t<ColumnBinding> &replacement_map);
};

} // namespace duckdb
