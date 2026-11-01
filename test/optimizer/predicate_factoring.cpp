#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/optimizer/expression_rewriter.hpp"
#include "duckdb/optimizer/rule/predicate_factoring.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/planner.hpp"

using namespace duckdb;

static optional_ptr<LogicalFilter> FindFilter(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return op.Cast<LogicalFilter>();
	}
	for (auto &child : op.children) {
		auto result = FindFilter(*child);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

TEST_CASE("Predicate factoring creates flat conjunctions", "[optimizer][predicate_factoring]") {
	constexpr idx_t predicate_count = 100;

	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE t(a INTEGER, b INTEGER)"));
	connection.BeginTransaction();

	string query = "SELECT * FROM t WHERE ";
	for (idx_t i = 0; i < predicate_count; i++) {
		if (i > 0) {
			query += " OR ";
		}
		query += StringUtil::Format("(a = %llu AND b = %llu)", i, i);
	}

	Parser parser(connection.context->GetParserOptions());
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));

	auto filter = FindFilter(*planner.plan);
	REQUIRE(filter);
	REQUIRE(filter->expressions.size() == 1);

	ExpressionRewriter rewriter(*connection.context);
	PredicateFactoringRule rule(rewriter);
	vector<reference<Expression>> bindings;
	bindings.push_back(*filter->expressions[0]);
	bool changes_made = false;
	auto result = rule.Apply(*filter, bindings, changes_made, true);
	REQUIRE(result);

	auto &derived_filter =
	    result->Cast<BoundConjunctionExpression>().GetChildren()[1]->Cast<BoundConjunctionExpression>();
	REQUIRE(derived_filter.GetExpressionType() == ExpressionType::CONJUNCTION_AND);
	REQUIRE(derived_filter.GetChildren().size() == 2);
	for (auto &child : derived_filter.GetChildren()) {
		auto &disjunction = child->Cast<BoundConjunctionExpression>();
		REQUIRE(disjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR);
		REQUIRE(disjunction.GetChildren().size() == predicate_count);
	}
}
