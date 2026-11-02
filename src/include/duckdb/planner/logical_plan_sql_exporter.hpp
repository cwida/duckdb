//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/logical_plan_sql_exporter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/cte_materialize.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/logical_plan_verification_result.hpp"

namespace duckdb {

class ClientContext;
class LogicalOperator;
struct LogicalExtensionOperator;

struct LogicalPlanSQLExportField {
	ColumnBinding source_binding;
	//! Type represented by the exported SQL expression.
	LogicalType type;
	//! Optimizer-selected type accepted from the bound plan, when different.
	optional<LogicalType> optimizer_type = {};
};

struct LogicalPlanSQLExportRelation {
	unique_ptr<QueryNode> query;
	//! A zero-column relation has a SQL placeholder column that is absent from fields.
	vector<LogicalPlanSQLExportField> fields;
};

using LogicalPlanSQLExportResult = LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>;

struct LogicalPlanSQLExportOptions {
	optional<vector<Identifier>> output_names;
};

struct LogicalPlanSQLExportReplacement {
	reference<LogicalOperator> op;
	LogicalPlanSQLExportRelation relation;
	//! Use the normal CTE optimizer rules by default. Callers requiring a materialization boundary
	//! must select ALWAYS. DEFAULT and NEVER may allow repeated or skipped evaluation.
	CTEMaterialize materialized = CTEMaterialize::CTE_MATERIALIZE_DEFAULT;
};

class LogicalPlanSQLExporter {
public:
	//! Export a verified plan to an owned query with positional binding/type fields.
	DUCKDB_API static LogicalPlanVerificationResult<LogicalPlanSQLExportRelation>
	Export(ClientContext &context, LogicalOperator &root, const LogicalPlanSQLExportOptions &options = {});

	//! Export an already resolved plan using owned SQL relations at explicit subtree boundaries.
	//! Checks existing binding/type metadata without re-resolving the input. Each replacement preserves
	//! the replaced subtree's visible bindings and types. Its materialization policy applies only to the
	//! generated source CTE, leaving CTEs inside the relation or the input plan unchanged.
	//! Replacements must be disjoint, reachable subtrees. Neither the plan nor replacement ASTs are consumed.
	DUCKDB_API static LogicalPlanSQLExportResult
	ExportWithSources(ClientContext &context, LogicalOperator &root,
	                  const vector<LogicalPlanSQLExportReplacement> &replacements,
	                  const LogicalPlanSQLExportOptions &options = {});
};

} // namespace duckdb
