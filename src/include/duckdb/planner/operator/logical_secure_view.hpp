//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/operator/logical_secure_view.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class BoundAtClause;

//! LogicalSecureView wraps the expanded plan of a secure view. It does not alter the result of its child - it only
//! acts as an optimization barrier that prevents the optimizer from pushing anything into the view.
class LogicalSecureView : public LogicalOperator {
public:
	LogicalPlanSQLExportResult ToSQL(LogicalPlanSQLExportContext &context,
	                                 const LogicalPlanVerificationPath &path) override;

	static constexpr const LogicalOperatorType TYPE = LogicalOperatorType::LOGICAL_SECURE_VIEW;

public:
	LogicalSecureView(string view_name, unique_ptr<LogicalOperator> child);
	LogicalSecureView(string view_name, QualifiedName source_name, vector<LogicalType> source_types,
	                  optional_ptr<BoundAtClause> at_clause, unique_ptr<LogicalOperator> child);

	//! The name of the view - used for printing the plan
	string view_name;
	//! The filters that the optimizer pushed into the view - these are the caller's own expressions, and they are
	//! reported as part of the boundary node because the operators inside the view are never shown
	vector<string> pushed_filters;
	//! Whether the statistics of the columns the view emits may escape the boundary. Set by AnalyzeStatistics
	bool propagate_statistics = false;
	//! Qualified source identity and the original positional schema used by SQL export
	bool has_source = false;
	QualifiedName source_name;
	vector<LogicalType> source_types;
	bool has_at_clause = false;
	Identifier at_unit;
	Value at_value;
	//! Current child bindings and their expressions over the original positional schema
	vector<ColumnBinding> output_bindings;
	vector<unique_ptr<Expression>> output_expressions;
	//! Caller predicates expressed over the original positional view schema
	vector<unique_ptr<Expression>> source_filters;

public:
	//! Determine for every secure view in the plan whether the statistics of the columns it emits may escape it.
	//! This must run before anything is pushed into the views - the filters that the optimizer pushes into a view
	//! are the caller's own, and must not be mistaken for the view restricting the rows it emits
	static void AnalyzeStatistics(LogicalOperator &plan);

	vector<ColumnBinding> GetColumnBindings() override;
	idx_t EstimateCardinality(ClientContext &context) override;
	InsertionOrderPreservingMap<string> ParamsToString() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<LogicalOperator> Deserialize(Deserializer &deserializer);

protected:
	void ResolveTypes() override;

private:
	LogicalSecureView();
};

} // namespace duckdb
