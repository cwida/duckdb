//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_match_recognize.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/window/window_executor.hpp"
#include "duckdb/function/window_function.hpp"

namespace duckdb {

struct WindowMatchRecognizeExecutor {
	static unique_ptr<FunctionData> Bind(BindWindowFunctionInput &input);
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const BoundWindowFunction &function);
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, BoundWindowFunction &function);
	static void GetBounds(WindowBoundsSet &required, const BoundWindowExpression &wexpr);
	static void GetSharing(WindowExecutor &executor, WindowSharedExpressions &shared);
	static unique_ptr<GlobalSinkState> GetGlobal(ClientContext &client, const WindowExecutor &executor,
	                                             const idx_t payload_count, const ValidityMask &partition_mask,
	                                             const ValidityMask &order_mask);
	static unique_ptr<LocalSinkState> GetLocal(ExecutionContext &context, const GlobalSinkState &gstate);
	static void Sink(ExecutionContext &context, DataChunk &sink_chunk, DataChunk &coll_chunk, idx_t input_idx,
	                 OperatorSinkInput &sink);
	static void Finalize(ExecutionContext &context, optional_ptr<WindowCollection> collection, OperatorSinkInput &sink);
	static void GetData(ExecutionContext &context, DataChunk &eval_chunk, DataChunk &bounds, Vector &result,
	                    idx_t row_idx, OperatorSinkInput &sink);

	//! The result type produced by the pattern matching window
	static LogicalType ResultType();
};

} // namespace duckdb
