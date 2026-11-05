#pragma once

#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include <stdexcept>
#include <type_traits>

namespace logical_plan_sql_export_test {

using namespace duckdb;

using PlanExportResult = LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>;

static_assert(!std::is_copy_constructible<LogicalPlanSQLExportRelation>::value,
              "Logical plan SQL export relations must remain move-only");

unique_ptr<LogicalOperator> OptimizeLogicalPlanExportQuery(Connection &connection, const string &query);

optional_ptr<LogicalOperator> FindLogicalPlanExportOperator(LogicalOperator &op, LogicalOperatorType type);

void RequireLogicalPlanExportIssue(const PlanExportResult &result, LogicalPlanVerificationIssueCode code,
                                   LogicalPlanVerificationPhase phase, const LogicalPlanVerificationPath &path);

void RequirePlanExportIssue(const PlanExportResult &result, LogicalPlanVerificationIssueCode code,
                            const LogicalPlanVerificationPath &path = {});

unique_ptr<Expression> PlanIntegerConstant(int32_t value);

unique_ptr<LogicalExpressionGet> IntegerValues(TableIndex table_index,
                                               std::initializer_list<std::initializer_list<int32_t>> input_rows);

unique_ptr<LogicalProjection> PlanProjection(TableIndex table_index, unique_ptr<LogicalOperator> child,
                                             vector<unique_ptr<Expression>> expressions);

class SQLExportExtensionOperator : public LogicalExtensionOperator {
public:
	SQLExportExtensionOperator(string name_p, vector<ColumnBinding> bindings_p, vector<LogicalType> types_p,
	                           vector<TableIndex> table_indexes_p = {},
	                           vector<unique_ptr<Expression>> expressions_p = {}, string verification_name_p = {})
	    : LogicalExtensionOperator(std::move(expressions_p)), name(std::move(name_p)),
	      verification_name(verification_name_p.empty() ? name : std::move(verification_name_p)),
	      bindings(std::move(bindings_p)), resolved_types(std::move(types_p)),
	      table_indexes(std::move(table_indexes_p)) {
	}

	std::function<PlanExportResult(SQLExportExtensionOperator &, LogicalPlanSQLExportContext &,
	                               const LogicalPlanVerificationPath &)>
	    export_sql;

	PlanExportResult ToSQL(LogicalPlanSQLExportContext &context, const LogicalPlanVerificationPath &path) override {
		if (!export_sql) {
			return LogicalExtensionOperator::ToSQL(context, path);
		}
		return export_sql(*this, context, path);
	}

	PlanExportResult ExportQuery(unique_ptr<QueryNode> query, const LogicalPlanVerificationPath &path) {
		auto fields = LogicalPlanSQLExportHelpers::CreateFields(*this, path);
		if (fields.HasError()) {
			return PlanExportResult::Failure(fields.GetIssues());
		}
		return PlanExportResult::Success({std::move(query), std::move(fields.GetValue())});
	}

	vector<ColumnBinding> GetColumnBindings() override {
		return bindings;
	}

	vector<TableIndex> GetTableIndex() const override {
		return table_indexes;
	}

	optional_ptr<const string> GetTypeBindingVerificationIdentifier() const noexcept override {
		return verification_name;
	}

	string GetExtensionName() const override {
		return name;
	}

	PhysicalOperator &CreatePlan(ClientContext &, PhysicalPlanGenerator &) override {
		throw NotImplementedException("Synthetic SQL export operator cannot create a physical plan");
	}

protected:
	void ResolveTypes() override {
		types = resolved_types;
	}

private:
	string name;
	string verification_name;
	vector<ColumnBinding> bindings;
	vector<LogicalType> resolved_types;
	vector<TableIndex> table_indexes;
};

vector<string> SQLExportRows(QueryResult &result, bool ordered);

} // namespace logical_plan_sql_export_test
