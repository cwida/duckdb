//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_aggregate_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window/window_aggregator.hpp"
#include "duckdb/common/enums/window_aggregation_mode.hpp"

namespace duckdb {

class WindowAggregateExecutor : public WindowExecutor {
public:
	WindowAggregateExecutor(BoundWindowExpression &wexpr, ClientContext &client, WindowSharedExpressions &shared);

	void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, const idx_t input_idx,
	          OperatorSinkInput &sink) const override;
	SourceResultType Finalize(ExecutionContext &context, CollectionPtr collection,
	                          OperatorSinkInput &sink) const override;

	unique_ptr<GlobalSinkState> GetGlobalState(ClientContext &client, const idx_t payload_count,
	                                           const ValidityMask &partition_mask,
	                                           const ValidityMask &order_mask) const override;
	unique_ptr<LocalSinkState> GetLocalState(ExecutionContext &context, const GlobalSinkState &gstate) const override;

	const WindowAggregationMode mode;

	// aggregate computation algorithm
	unique_ptr<WindowAggregator> aggregator;

	// FILTER reference expression in sink_chunk
	unique_ptr<Expression> filter_ref;

protected:
	void EvaluateInternal(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                      idx_t row_idx, OperatorSinkInput &sink) const override;
};

} // namespace duckdb
