//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/sql_export/logical_plan_sql_exporter_internal.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/table_column.hpp"
#include "duckdb/planner/logical_plan_sql_export_context.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/parser/query_node/select_node.hpp"

namespace duckdb {

class Expression;
class LogicalOperator;
class LogicalMaterializedCTE;
class LogicalCTERef;
class LogicalRecursiveCTE;
class LogicalComparisonJoin;
class LogicalColumnDataGet;
class LogicalGet;
class LogicalExpressionGet;
class LogicalCopyToFile;
class LogicalCreateTable;
class LogicalFilter;
class LogicalProjection;
class LogicalSecureView;
class LogicalSample;
class LogicalPivot;
class LogicalLimit;
class LogicalSetOperation;
class LogicalAggregate;
struct LogicalExtensionOperator;
class ClientContext;

using LogicalPlanSQLFieldResult = LogicalPlanVerificationResult<vector<LogicalPlanSQLExportField>>;
struct SQLSourceQueryResult {
	unique_ptr<QueryNode> query;
	string unsupported_reason;
};

//! Helpers shared by the logical operators' SQL reconstruction
struct LogicalPlanSQLExportHelpers {
	static SQLSourceQueryResult ReconstructSQLSource(ClientContext &context, const LogicalGet &get,
	                                                 unique_ptr<TableRef> input, const Identifier &relation_alias,
	                                                 bool source_ordinality);

	static LogicalPlanVerificationPath PlanChildPath(const LogicalPlanVerificationPath &path, idx_t ordinal);

	static LogicalPlanVerificationPath PlanExpressionPath(const LogicalPlanVerificationPath &path, idx_t ordinal);

	static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	ExportTypedNull(const LogicalType &type, const LogicalPlanVerificationPath &path);

	static LogicalPlanVerificationIssue PlanUnsupportedFeature(const LogicalPlanVerificationPath &path, string feature,
	                                                           string message);

	static LogicalPlanVerificationFunctionIdentity LogicalSourceIdentity();

	static LogicalPlanVerificationFunctionIdentity LogicalSourceIdentity(const LogicalGet &get);

	static LogicalPlanVerificationIssue UnsupportedSource(const LogicalPlanVerificationPath &path,
	                                                      LogicalPlanVerificationFunctionIdentity source, string guard);

	static Identifier FieldIdentifier(idx_t ordinal);

	static LogicalPlanSQLFieldResult CreateFields(LogicalOperator &op, const LogicalPlanVerificationPath &path);

	static BoundExpressionSQLExportContext
	CreateBindingContext(ClientContext &context, const vector<reference<const LogicalPlanSQLExportedChild>> &children,
	                     const vector<optional_ptr<const SelectNode>> &plain_scopes = {});

	static void PropagateSemanticTypes(vector<LogicalPlanSQLExportField> &fields,
	                                   const vector<reference<const LogicalPlanSQLExportedChild>> &children);

	static unique_ptr<TableRef> CreateSubquery(LogicalPlanSQLExportedChild child);

	static unique_ptr<ParsedExpression> ChildColumn(const LogicalPlanSQLExportedChild &child, idx_t field_index,
	                                                optional_ptr<const SelectNode> plain = nullptr);

	static vector<reference<const Expression>> CollectExpressions(const LogicalOperator &op);

	static bool HasEffectfulExpressions(const LogicalOperator &op);

	static bool CollectScopeAliases(const TableRef &table, identifier_set_t &aliases);

	static optional_ptr<const SelectNode> PlainScope(const QueryNode &query);

	static void SetChildScope(SelectNode &select, LogicalPlanSQLExportedChild child,
	                          optional_ptr<const SelectNode> plain);

	static optional_ptr<const SelectNode> ComposeInput(LogicalOperator &op, const LogicalPlanSQLExportedChild &child,
	                                                   BoundExpressionSQLExportState &state);

	static bool HasSimpleGroups(const LogicalAggregate &aggregate);

	static bool ComposeProjection(LogicalProjection &projection, LogicalPlanSQLExportedChild &child, SelectNode &select,
	                              const BoundExpressionSQLExportContext &context,
	                              const LogicalPlanVerificationPath &path);

	static bool ComposeOrder(LogicalOperator &op, LogicalPlanSQLExportedChild &child, SelectNode &select,
	                         const BoundExpressionSQLExportContext &context, const LogicalPlanVerificationPath &path);

	static bool IsIdentityProjection(const LogicalProjection &projection,
	                                 const vector<LogicalPlanSQLExportField> &fields);

	//! Alias the relation's visible columns with the statement's output names.
	static LogicalPlanSQLExportResult ApplyOutputNames(LogicalPlanSQLExportResult result,
	                                                   const vector<Identifier> &output_names);
};

//! Export of write statements with a query source (sql_export_statements.cpp)
struct SQLExportStatementHelpers {
	static LogicalPlanSQLExportResult ExportCopyToNode(LogicalCopyToFile &op, LogicalPlanSQLExportContext &context,
	                                                   const LogicalPlanVerificationPath &path);
	static LogicalPlanSQLExportResult ExportCreateTableAsNode(LogicalCreateTable &op,
	                                                          LogicalPlanSQLExportContext &context,
	                                                          const LogicalPlanVerificationPath &path);
	//! The query node that carries WITH definitions for this exported relation.
	static QueryNode &SourceDefinitionNode(QueryNode &query);
};

//! Export of data-modifying plan roots and CTE bodies (sql_export_dml.cpp)
struct SQLExportDMLHelpers {
	//! The RETURNING projection over a data-modifying operator, when the root has that shape.
	static optional_ptr<LogicalProjection> MatchReturningRoot(LogicalOperator &root);
	//! Export a RETURNING root; output names become the RETURNING aliases.
	static LogicalPlanSQLExportResult ExportReturning(LogicalPlanSQLExportContext &context, LogicalProjection &root,
	                                                  const optional<vector<Identifier>> &output_names,
	                                                  const LogicalPlanVerificationPath &path);
	//! Whether this operator is a data-modifying statement body: a DML operator or its RETURNING projection.
	static bool MatchDMLBody(LogicalOperator &op);
	//! Whether the statement-body chain itself crosses a replaced source boundary, in which case
	//! the ordinary relational export must handle it instead of the statement export.
	static bool ContainsReplacedBoundary(const LogicalPlanSQLExportContext &context, LogicalOperator &op);
	//! Export a data-modifying statement body outside the plan root (e.g. a materialized CTE side).
	static LogicalPlanSQLExportResult ExportBody(LogicalPlanSQLExportContext &context, LogicalOperator &op,
	                                             const LogicalPlanVerificationPath &path);
	//! Whether this exported query is a data-modifying statement node.
	static bool IsDMLNode(const QueryNode &query);
};
} // namespace duckdb
