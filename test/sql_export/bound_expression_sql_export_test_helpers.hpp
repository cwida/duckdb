#pragma once

#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include <cmath>
#include <cstring>

namespace bound_expression_sql_export_test {

using namespace duckdb;

using ExportResult = LogicalPlanVerificationResult<unique_ptr<ParsedExpression>>;

unique_ptr<LogicalOperator> OptimizeExportQuery(Connection &connection, const string &query);

unique_ptr<LogicalOperator> BindExportQuery(Connection &connection, const string &query);

optional_ptr<const Expression> FindExpression(const Expression &expression,
                                              const std::function<bool(const Expression &)> &matches);

optional_ptr<const Expression> FindExpression(const LogicalOperator &op,
                                              const std::function<bool(const Expression &)> &matches);

BoundExpressionSQLExportContext ResolveBinding(ColumnBinding target, vector<Identifier> names, LogicalType type);

void RequireRoundTrip(Connection &connection, const Expression &expression,
                      const BoundExpressionSQLExportContext &context, const string &from_clause,
                      const string &oracle_expression);

void RequireIssue(const ExportResult &result, LogicalPlanVerificationIssueCode code,
                  const LogicalPlanVerificationPath &path);

unique_ptr<Expression> Constant(Value value);

unique_ptr<Expression> BinaryRoundTrip(ClientContext &context, const Expression &expression,
                                       const StorageCompatibility &compatibility = StorageCompatibility::Latest());

struct SQLBindingEntry {
	ColumnBinding binding;
	LogicalType type;
	Identifier name;
};

void CollectSQLBindings(const Expression &expression, vector<SQLBindingEntry> &entries);

} // namespace bound_expression_sql_export_test
