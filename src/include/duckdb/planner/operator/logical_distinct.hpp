//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_distinct.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/bound_result_modifier.hpp"

namespace duckdb {

//! LogicalDistinct filters duplicate entries from its child operator
class LogicalDistinct : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_DISTINCT;

public:
	explicit LogicalDistinct(DistinctType distinct_type);
	explicit LogicalDistinct(vector<unique_ptr<Expression>> targets, DistinctType distinct_type);

	//! Whether or not this is a DISTINCT or DISTINCT ON
	DistinctType distinct_type;
	//! The set of distinct targets
	vector<unique_ptr<Expression>> distinct_targets;
	//! The order by modifier (optional, only for distinct on)
	unique_ptr<BoundOrderModifier> order_by;

public:
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	vector<ColumnBinding> GetColumnBindings() override {
		return children[0]->GetColumnBindings();
	}

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	void ResolveTypes() override;
};
} // namespace duckdb
