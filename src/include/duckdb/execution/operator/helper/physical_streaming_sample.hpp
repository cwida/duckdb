//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_streaming_sample.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"

namespace duckdb {

//! PhysicalStreamingSample represents a streaming sample using either system or bernoulli sampling
class PhysicalStreamingSample : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::STREAMING_SAMPLE;

public:
	PhysicalStreamingSample(PhysicalPlan &physical_plan, vector<LogicalType> types, unique_ptr<SampleOptions> options,
	                        idx_t estimated_cardinality);

	unique_ptr<SampleOptions> sample_options;
	double percentage;
	double system_sample_phase;
	idx_t rows;

public:
	// Operator interface
	void InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const override;
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const override;

	bool ParallelOperator() const override;

	InsertionOrderPreservingMap<string> ParamsToString() const override;

private:
	void SystemSamplePercent(DataChunk &input, DataChunk &result, OperatorState &state) const;
	void SystemSampleRows(DataChunk &input, DataChunk &result, OperatorState &state) const;
	void BernoulliSample(DataChunk &input, DataChunk &result, OperatorState &state) const;
};

} // namespace duckdb
