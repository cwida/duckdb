#include "duckdb/planner/operator/logical_explain.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/parser/peg/sql_formatter.hpp"

namespace duckdb {

LogicalExplain::LogicalExplain(unique_ptr<LogicalOperator> plan, ExplainType explain_type,
                               const ProfilerPrintFormat &format)
    : LogicalOperator(LogicalOperatorType::LOGICAL_EXPLAIN), explain_type(explain_type), format(format) {
	children.push_back(std::move(plan));
}

unique_ptr<LogicalOperator> LogicalExplain::CreateSQLResult(ClientContext &context, TableIndex table_index) {
	D_ASSERT(explain_type == ExplainType::EXPLAIN_SQL);
	D_ASSERT(children.size() == 1);
	LogicalPlanSQLExportOptions options;
	switch (children[0]->type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
	case LogicalOperatorType::LOGICAL_CREATE_TABLE:
	case LogicalOperatorType::LOGICAL_COPY_TO_FILE:
		// a data-modifying statement reports a change count; that is not a projection of the generated SQL
		break;
	default:
		options.output_names = sql_output_names;
		break;
	}
	auto exported = LogicalPlanSQLExporter::Export(context, *children[0], options);
	if (exported.HasError()) {
		auto &issue = exported.GetIssues()[0];
		auto message = "EXPLAIN (SQL) cannot render this query: " + issue.message;
		if (issue.construct && issue.construct->type == LogicalPlanVerificationConstructType::SOURCE_FUNCTION) {
			auto &construct = *issue.construct;
			if (construct.function) {
				auto &function = *construct.function;
				if (function.name != "logical_source") {
					message = StringUtil::Format("EXPLAIN (SQL) cannot render table function \"%s\".", function.name);
				}
			}
		}
		for (auto &entry : exported.GetIssues()) {
			switch (entry.code) {
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_OPERATOR:
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPRESSION:
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_FUNCTION:
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_SOURCE:
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXTENSION:
			case LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE:
				break;
			default:
				throw InternalException("EXPLAIN (SQL) cannot render this query: " + entry.message);
			}
		}
		if (!allow_unsupported_sql) {
			throw NotImplementedException({{"sql_export_unsupported", "true"}}, message);
		}
	}
	vector<LogicalType> result_types {LogicalType::VARCHAR, LogicalType::VARCHAR};
	auto collection =
	    make_uniq<ColumnDataCollection>(context, result_types, ColumnDataAllocatorType::IN_MEMORY_ALLOCATOR);
	if (exported.IsSuccess()) {
		auto sql = FormatSQL(exported.GetValue().query->ToString());
		DataChunk chunk;
		chunk.Initialize(Allocator::Get(context), result_types);
		chunk.data[0].Append(Value("sql"));
		chunk.data[1].Append(Value(sql));
		collection->Append(chunk);
	}
	return make_uniq<LogicalColumnDataGet>(table_index, std::move(result_types), std::move(collection));
}

idx_t LogicalExplain::EstimateCardinality(ClientContext &context) {
	return 3;
}

bool LogicalExplain::SupportSerialization() const {
	//! Skips the serialization check in VerifyPlan
	return false;
}

void LogicalExplain::ResolveTypes() {
	types = {LogicalType::VARCHAR, LogicalType::VARCHAR};
}
vector<ColumnBinding> LogicalExplain::GetColumnBindings() {
	vector<ColumnBinding> result;
	TableIndex explain_tbl_idx(0);
	for (auto explain_col_idx : ProjectionIndex::GetIndexes(2)) {
		result.emplace_back(explain_tbl_idx, explain_col_idx);
	}
	return result;
}

} // namespace duckdb
