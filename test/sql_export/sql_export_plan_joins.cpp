#include "sql_export_test_helpers.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/sql_export_helpers.hpp"
#include "duckdb/planner/operator/logical_cross_product.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_empty_result.hpp"
#include <stdexcept>
#include <type_traits>
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

TEST_CASE("SQL export inlines plain join sources without alias capture", "[sql_export][logical_plan_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	for (auto aliases : {pair<string, string> {"v", "w"}, {"v", "v"}, {"r1", "v"}, {"v", "r0"}, {"r1", "r0"}}) {
		for (bool filtered : {false, true}) {
			if (filtered && (aliases.first != "v" || aliases.second == "v")) {
				continue;
			}
			CAPTURE(aliases.first, aliases.second, filtered);
			auto leaf = [](const string &name, idx_t index) {
				return make_uniq<SQLExportExtensionOperator>(
				    name, vector<ColumnBinding> {{TableIndex(index), ProjectionIndex(0)}},
				    vector<LogicalType> {LogicalType::INTEGER}, vector<TableIndex> {TableIndex(index)});
			};
			auto plan = make_uniq<LogicalCrossProduct>(leaf("left", 800), leaf("right", 801));
			plan->ResolveOperatorTypes();
			auto export_source = [&](SQLExportExtensionOperator &input, LogicalPlanSQLExportContext &,
			                         const LogicalPlanVerificationPath &path) {
				auto is_left = input.GetExtensionName() == "left";
				auto alias = is_left ? aliases.first : aliases.second;
				auto values = is_left ? "(VALUES(1),(2))" : "(VALUES(10),(20))";
				auto sql = "SELECT " + alias + ".x FROM " + values + " " + alias + "(x)";
				if (is_left && filtered) {
					sql += " WHERE " + alias + ".x=1";
				}
				Parser parser(*connection.context);
				parser.ParseQuery(sql);
				return input.ExportQuery(std::move(parser.statements[0]->Cast<SelectStatement>().node), path);
			};
			for (auto &child : plan->children) {
				child->Cast<SQLExportExtensionOperator>().export_sql = export_source;
			}
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *plan);
			INFO((exported.HasError() ? exported.GetIssues()[0].message : string()));
			REQUIRE(exported.IsSuccess());
			if (aliases.first == "v" && aliases.second == "w" && !filtered) {
				auto &join = exported.GetValue().query->Cast<SelectNode>().from_table->Cast<JoinRef>();
				REQUIRE(join.left->alias == Identifier("v"));
				REQUIRE(join.right->alias == Identifier("w"));
			}
			auto sql = exported.GetValue().query->ToString() + " ORDER BY 1,2";
			CAPTURE(sql);
			auto generated = connection.Query(sql);
			REQUIRE_NO_FAIL(*generated);
			if (filtered) {
				REQUIRE(CHECK_COLUMN(generated, 0, {1, 1}));
				REQUIRE(CHECK_COLUMN(generated, 1, {10, 20}));
			} else {
				REQUIRE(CHECK_COLUMN(generated, 0, {1, 1, 2, 2}));
				REQUIRE(CHECK_COLUMN(generated, 1, {10, 20, 10, 20}));
			}
		}
	}
	connection.Rollback();
}

TEST_CASE("Grouped MARK export requires reconstructible retained metadata",
          "[sql_export][logical_plan_sql_export][join_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	const vector<LogicalType> types {LogicalType::VARCHAR_COLLATION("nocase"), LogicalType::BIGINT,
	                                 LogicalType::INTEGER};
	auto join = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
	join->mark_index = TableIndex(1102);
	for (idx_t side = 0; side < 2; side++) {
		vector<ColumnBinding> bindings;
		for (idx_t i = 0; i < types.size(); i++) {
			bindings.emplace_back(TableIndex(1100 + side), ProjectionIndex(i));
		}
		join->children.push_back(make_uniq<LogicalEmptyResult>(types, std::move(bindings)));
	}
	for (idx_t i = 0; i < types.size(); i++) {
		join->conditions.emplace_back(
		    make_uniq<BoundColumnRefExpression>(types[i], ColumnBinding(TableIndex(1100), ProjectionIndex(i))),
		    make_uniq<BoundColumnRefExpression>(types[i], ColumnBinding(TableIndex(1101), ProjectionIndex(i))),
		    i + 1 == types.size() ? ExpressionType::COMPARE_EQUAL : ExpressionType::COMPARE_NOT_DISTINCT_FROM);
	}
	join->ResolveOperatorTypes();
	const vector<LogicalType> groups {types[0], types[1]};
	join->mark_types = groups;
	REQUIRE(LogicalPlanSQLExporter::Export(*connection.context, *join).IsSuccess());
	for (auto &metadata : vector<vector<LogicalType>> {{},
	                                                   {types[0]},
	                                                   {types[1], types[0]},
	                                                   {LogicalType::VARCHAR, types[1]},
	                                                   {LogicalType::VARCHAR_COLLATION("noaccent"), types[1]}}) {
		join->mark_types = metadata;
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *join);
		RequirePlanExportIssue(exported, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	}
	join->mark_types = groups;
	std::swap(join->conditions[0], join->conditions[2]);
	RequirePlanExportIssue(LogicalPlanSQLExporter::Export(*connection.context, *join),
	                       LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	connection.Rollback();
}

TEST_CASE("Logical plan SQL export preserves join output maps and empty sides",
          "[sql_export][logical_plan_sql_export][join_sql_export]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	for (auto type : {JoinType::INNER, JoinType::LEFT, JoinType::RIGHT, JoinType::OUTER, JoinType::SEMI, JoinType::ANTI,
	                  JoinType::RIGHT_SEMI, JoinType::RIGHT_ANTI, JoinType::MARK, JoinType::SINGLE}) {
		for (idx_t empty_mask : {0, 1, 2, 3}) {
			CAPTURE(type, empty_mask);
			unique_ptr<LogicalOperator> left = IntegerValues(TableIndex(1010), {{1, 10}, {2, 20}, {2, 21}});
			unique_ptr<LogicalOperator> right = IntegerValues(TableIndex(1011), {{2, 100}, {3, 200}});
			if (empty_mask & 1) {
				left = make_uniq<LogicalEmptyResult>(std::move(left));
			}
			if (empty_mask & 2) {
				right = make_uniq<LogicalEmptyResult>(std::move(right));
			}
			auto join = make_uniq<LogicalComparisonJoin>(type);
			join->mark_index = TableIndex(1012);
			join->left_projection_map = {ProjectionIndex(1)};
			join->right_projection_map = {ProjectionIndex(1), ProjectionIndex(0)};
			join->conditions.emplace_back(
			    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER,
			                                        ColumnBinding(TableIndex(1010), ProjectionIndex(0))),
			    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER,
			                                        ColumnBinding(TableIndex(1011), ProjectionIndex(0))),
			    ExpressionType::COMPARE_EQUAL);
			join->children.push_back(std::move(left));
			join->children.push_back(std::move(right));
			join->ResolveOperatorTypes();
			auto exported = LogicalPlanSQLExporter::Export(*connection.context, *join);
			REQUIRE(exported.IsSuccess());
			REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
			auto direct = connection.Query(make_uniq<LogicalPlanStatement>(std::move(join)));
			REQUIRE_NO_FAIL(connection.Query("PRAGMA enable_optimizer"));
			auto generated = connection.Query(exported.GetValue().query->ToString());
			REQUIRE_NO_FAIL(*direct);
			REQUIRE_NO_FAIL(*generated);
			REQUIRE(generated->GetTypes() == direct->GetTypes());
			REQUIRE(SQLExportRows(*generated, false) == SQLExportRows(*direct, false));
		}
	}
	connection.Rollback();
}

} // namespace logical_plan_sql_export_test
