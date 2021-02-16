//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_reservoir_sample.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_sink.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"

namespace duckdb {

//! PhysicalReservoirSample represents a sample taken using reservoir sampling, which is a blocking sampling method
class PhysicalReservoirSample : public PhysicalSink {
public:
	PhysicalReservoirSample(vector<LogicalType> types, unique_ptr<SampleOptions> options)
	    : PhysicalSink(PhysicalOperatorType::RESERVOIR_SAMPLE, move(types)), options(move(options)) {
	}

	unique_ptr<SampleOptions> options;

public:
	void Sink(ExecutionContext &context, GlobalOperatorState &state, LocalSinkState &lstate, DataChunk &input) override;
	unique_ptr<GlobalOperatorState> GetGlobalState(ClientContext &context) override;

	void GetChunkInternal(ExecutionContext &context, DataChunk &chunk, PhysicalOperatorState *state) override;
	unique_ptr<PhysicalOperatorState> GetOperatorState(ExecutionContext &execution_context) override;

	string ParamsToString() const override;
};

} // namespace duckdb
