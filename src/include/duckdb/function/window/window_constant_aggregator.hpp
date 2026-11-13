//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_constant_aggregator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/window/window_aggregator.hpp"

namespace duckdb {

class WindowConstantAggregator : public WindowAggregator {
public:
	static bool CanAggregate(const BoundWindowExpression &wexpr);

	static BoundWindowExpression &RebindAggregate(ClientContext &client, BoundWindowExpression &wexpr);

	WindowConstantAggregator(BoundWindowExpression &wexpr, WindowSharedExpressions &shared, ClientContext &context);
	~WindowConstantAggregator() override {
	}

	unique_ptr<GlobalSinkState> GetGlobalState(ClientContext &context, idx_t group_count,
	                                           const ValidityMask &partition_mask) const override;
	void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	          optional_ptr<SelectionVector> filter_sel, idx_t filtered, OperatorSinkInput &sink) override;
	SourceResultType Finalize(ExecutionContext &context, CollectionPtr collection, const FrameStats &stats,
	                          OperatorSinkInput &sink) override;

	unique_ptr<LocalSinkState> GetLocalState(ExecutionContext &context, const GlobalSinkState &gstate) const override;
	void Evaluate(ExecutionContext &context, const DataChunk &bounds, Vector &result, idx_t count, idx_t row_idx,
	              OperatorSinkInput &sink) const override;
};

} // namespace duckdb
