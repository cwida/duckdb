//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/bound_expression_sql_exporter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/logical_plan_verification_result.hpp"

#include <functional>

namespace duckdb {

class Expression;
class ClientContext;
class BoundWindowExpression;
class BoundAggregateExpression;
class BoundUnnestExpression;

struct ResolvedSQLColumnReference {
	vector<Identifier> names;
	//! Type represented by the exported SQL expression.
	LogicalType type;
	//! Optimizer-selected type accepted from the bound plan, when different.
	optional<LogicalType> optimizer_type = {};
};

using BoundExpressionSQLBindingResolver =
    std::function<optional<ResolvedSQLColumnReference>(const ColumnBinding &binding)>;

struct BoundExpressionSQLExportContext {
	BoundExpressionSQLBindingResolver resolve_binding;
	//! The context that will bind the exported expression, when known
	optional_ptr<ClientContext> client_context;
	//! Internal pruning expressions may be discarded only while exporting their complete logical plan.
	bool discard_optimizer_metadata = false;
};

//! Reconstructs SQL from bound/optimized logical expressions before physical planning lowers their structure.
//! Optimizer specializations are supported when the retained logical definition, arguments and modifiers survive.
class BoundExpressionSQLExporter {
public:
	DUCKDB_API static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	Export(const Expression &expression, const BoundExpressionSQLExportContext &context);

	DUCKDB_API static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	ExportAtPath(const Expression &expression, const BoundExpressionSQLExportContext &context,
	             const LogicalPlanVerificationPath &path);

	//! Only for root expressions placed by their owning logical relation.
	DUCKDB_API static LogicalPlanVerificationResult<unique_ptr<FunctionExpression>>
	ExportAggregateCallAtPath(const BoundAggregateExpression &expression,
	                          const BoundExpressionSQLExportContext &context, const LogicalPlanVerificationPath &path);

	DUCKDB_API static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	ExportWindowAtPath(const BoundWindowExpression &expression, const BoundExpressionSQLExportContext &context,
	                   const LogicalPlanVerificationPath &path);

	DUCKDB_API static LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>
	ExportUnnestAtPath(const BoundUnnestExpression &expression, const BoundExpressionSQLExportContext &context,
	                   const LogicalPlanVerificationPath &path);
};

} // namespace duckdb
