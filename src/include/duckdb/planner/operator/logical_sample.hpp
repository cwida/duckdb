//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_sample.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/parser/parsed_data/sample_options.hpp"

namespace duckdb {

//! LogicalSample represents a SAMPLE clause
class LogicalSample : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_SAMPLE;

public:
	LogicalSample(unique_ptr<SampleOptions> sample_options_p, unique_ptr<LogicalOperator> child);

	//! The sample options
	unique_ptr<SampleOptions> sample_options;

public:
	vector<ColumnBinding> GetColumnBindings() override;
	idx_t EstimateCardinality(ClientContext &context) override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	void ResolveTypes() override;

private:
	LogicalSample();
};

} // namespace duckdb
