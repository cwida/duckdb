#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/planner/bound_expression_sql_exporter.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include <cmath>
#include <cstring>
#include "bound_expression_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace bound_expression_sql_export_test {

static const Value &GetIssueFact(const LogicalPlanVerificationIssue &issue, const string &name) {
	for (auto &fact : issue.facts) {
		if (fact.first == name) {
			return fact.second;
		}
	}
	throw InternalException("Missing logical plan SQL export issue fact");
}

TEST_CASE("Bound constant SQL export discards display metadata", "[sql_export][bound_expression_sql_export]") {
	BoundExpressionSQLExportContext context;
	BoundConstantExpression metadata_expression(Value::INTEGER(1));
	metadata_expression.SetAlias(Identifier("display_only"));
	metadata_expression.SetQueryLocation(optional_idx(17));
	auto metadata_result = BoundExpressionSQLExporter::Export(metadata_expression, context);
	REQUIRE(metadata_result.IsSuccess());
	REQUIRE_FALSE(metadata_result.GetValue()->HasAlias());
	REQUIRE_FALSE(metadata_result.GetValue()->HasQueryLocation());
	REQUIRE_FALSE(metadata_result.GetValue()->Cast<CastExpression>().Child().HasAlias());
	REQUIRE_FALSE(metadata_result.GetValue()->Cast<CastExpression>().Child().HasQueryLocation());
}

TEST_CASE("SQL export type admission follows DuckDB value types", "[sql_export][bound_expression_sql_export]") {
	auto types = LogicalType::AllTypes();
	for (idx_t value = 0; value <= NumericLimits<uint8_t>::Maximum(); value++) {
		auto id = static_cast<LogicalTypeId>(value);
		bool admitted = id == LogicalTypeId::SQLNULL || id == LogicalTypeId::TYPE;
		for (auto &type : types) {
			admitted |= type.id() == id;
		}
		REQUIRE(SQLExportHelpers::IsSQLExportType(id) == admitted);
	}
	types.push_back(LogicalType::SQLNULL);
	types.push_back(LogicalType::TYPE());
	types.push_back(LogicalType::POINTER);
	types.push_back(LogicalType::ANY);
	types.push_back(LogicalType::INVALID);
	auto binding = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	for (auto &type : types) {
		const bool admitted = SQLExportHelpers::IsSQLExportType(type.id()) && type.IsComplete();
		for (auto &candidate : vector<LogicalType> {type, LogicalType::LIST(type)}) {
			REQUIRE(SQLExportHelpers::IsSQLRepresentableType(candidate) == admitted);
			REQUIRE(SQLExportHelpers::IsSQLValueType(candidate) == admitted);
			BoundColumnRefExpression expression(candidate, binding);
			auto context = ResolveBinding(binding, {Identifier("value")}, candidate);
			auto result = BoundExpressionSQLExporter::Export(expression, context);
			if (admitted) {
				REQUIRE(result.IsValid());
				REQUIRE(result.IsSuccess());
				REQUIRE(result.GetValue());
			} else {
				RequireIssue(result, LogicalPlanVerificationIssueCode::INTERNAL_INVARIANT, path);
			}
		}
	}
	auto tuple = LogicalType::TUPLE(vector<LogicalType> {LogicalType::INTEGER});
	REQUIRE(SQLExportHelpers::IsSQLRepresentableType(tuple));
	REQUIRE_FALSE(SQLExportHelpers::IsSQLRepresentableType(LogicalType::TUPLE(vector<LogicalType> {})));
	REQUIRE(SQLExportHelpers::IsSQLValueType(tuple));
	REQUIRE(SQLExportHelpers::IsSQLValueType(LogicalType::LIST(tuple)));
	REQUIRE_FALSE(SQLExportHelpers::IsSQLValueType(LogicalType::TUPLE(vector<LogicalType> {LogicalType::POINTER})));
}

TEST_CASE("SQL export distinguishes inherited and unrepresented collations",
          "[sql_export][bound_expression_sql_export]") {
	const auto collated = LogicalType::VARCHAR_COLLATION("nocase");
	const ColumnBinding binding(TableIndex(1), ProjectionIndex(0));
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	for (const auto &type :
	     {collated, LogicalType::LIST(collated), LogicalType::ARRAY(collated, 2),
	      LogicalType::LIST(LogicalType::LIST(collated)), LogicalType::STRUCT({{Identifier("s"), collated}}),
	      LogicalType::MAP(LogicalType::INTEGER, collated)}) {
		CAPTURE(type.ToString());
		auto plain = TypeVisitor::VisitReplace(type, [](const LogicalType &child) {
			return child.id() == LogicalTypeId::VARCHAR ? LogicalType::VARCHAR : child;
		});
		REQUIRE(type == plain);
		REQUIRE_FALSE(type.EqualsIncludingCollation(plain));
		REQUIRE_FALSE(plain.EqualsIncludingCollation(type));
		REQUIRE(type.EqualsIncludingCollation(type));
		BoundColumnRefExpression column(type, binding);
		auto matching = ResolveBinding(binding, {Identifier("v")}, type);
		auto exported = BoundExpressionSQLExporter::Export(column, matching);
		REQUIRE(exported.IsSuccess());
		REQUIRE(exported.GetValue()->GetExpressionClass() == ExpressionClass::COLUMN_REF);
		auto changed = ResolveBinding(binding, {Identifier("v")}, plain);
		auto replacement = BoundExpressionSQLExporter::Export(column, changed);
		if (type.id() == LogicalTypeId::VARCHAR) {
			REQUIRE(replacement.IsSuccess());
			REQUIRE(replacement.GetValue()->GetExpressionClass() == ExpressionClass::COLLATE);
		} else {
			RequireIssue(replacement, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, path);
			REQUIRE(replacement.GetIssues()[0].construct ==
			        LogicalPlanVerificationConstructIdentity::ExportFeature("nested_result_collation"));
			auto &replacement_issue = replacement.GetIssues()[0];
			REQUIRE(GetIssueFact(replacement_issue, "logical_type") == Value(type.ToString()));
			REQUIRE(GetIssueFact(replacement_issue, "input_logical_type") == Value(plain.ToString()));
			REQUIRE(GetIssueFact(replacement_issue, "varchar_collations") ==
			        Value(SQLExportHelpers::TypeCollationSignature(type)));
			REQUIRE(GetIssueFact(replacement_issue, "input_varchar_collations") ==
			        Value(SQLExportHelpers::TypeCollationSignature(plain)));
		}
		BoundColumnRefExpression reset(plain, binding);
		auto reset_result = BoundExpressionSQLExporter::Export(reset, matching);
		RequireIssue(reset_result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, path);
		REQUIRE(reset_result.GetIssues()[0].construct ==
		        LogicalPlanVerificationConstructIdentity::ExportFeature(
		            type.id() == LogicalTypeId::VARCHAR ? "column_collation_reset" : "nested_result_collation"));
		if (type.id() != LogicalTypeId::VARCHAR) {
			auto &reset_issue = reset_result.GetIssues()[0];
			REQUIRE(GetIssueFact(reset_issue, "logical_type") == Value(plain.ToString()));
			REQUIRE(GetIssueFact(reset_issue, "input_logical_type") == Value(type.ToString()));
			REQUIRE(GetIssueFact(reset_issue, "varchar_collations") ==
			        Value(SQLExportHelpers::TypeCollationSignature(plain)));
			REQUIRE(GetIssueFact(reset_issue, "input_varchar_collations") ==
			        Value(SQLExportHelpers::TypeCollationSignature(type)));
		}
	}
}

TEST_CASE("Nested SQL cast reconstruction distinguishes collation and error semantics",
          "[sql_export][bound_expression_sql_export]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	BoundExpressionSQLExportContext context;
	context.client_context = connection.context.get();
	LogicalPlanVerificationPath path;
	path.root = LogicalPlanVerificationPathRoot::STANDALONE_EXPRESSION;
	const auto target = LogicalType::LIST(LogicalType::VARCHAR_COLLATION("nocase"));
	GetCastFunctionInput input(*connection.context);
	for (bool same_type : {false, true}) {
		for (bool try_cast : {false, true}) {
			CAPTURE(same_type, try_cast);
			const auto source =
			    same_type ? LogicalType::LIST(LogicalType::VARCHAR) : LogicalType::LIST(LogicalType::INTEGER);
			auto cast_info = CastFunctionSet::Get(*connection.context).GetCastFunction(source, target, input);
			auto expression = BoundCastExpression::Create(make_uniq<BoundConstantExpression>(Value(source)), target,
			                                              std::move(cast_info), try_cast);
			vector<unique_ptr<Expression>> variants;
			variants.push_back(expression->Copy());
			variants.push_back(BinaryRoundTrip(*connection.context, *expression));
			variants.push_back(std::move(expression));
			for (auto &variant : variants) {
				auto exported = BoundExpressionSQLExporter::Export(*variant, context);
				if (same_type || try_cast) {
					RequireIssue(exported, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE, path);
					REQUIRE(exported.GetIssues()[0].construct ==
					        LogicalPlanVerificationConstructIdentity::ExportFeature("nested_result_collation"));
				} else {
					REQUIRE(exported.IsSuccess());
					auto rebound = connection.Query("SELECT " + exported.GetValue()->ToString());
					REQUIRE_NO_FAIL(*rebound);
					REQUIRE(rebound->GetTypes()[0].EqualsIncludingCollation(target));
					REQUIRE(rebound->Collection().GetValue(0, 0).IsNull());
				}
			}
		}
	}
	connection.Rollback();
}

} // namespace bound_expression_sql_export_test
