#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/planner.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

using namespace duckdb;

namespace {

//! One bound function found in an optimized plan
struct BoundFunctionInfo {
	Identifier catalog_name;
	Identifier schema_name;
	Identifier name;
	vector<LogicalType> arguments;
	LogicalType return_type;
	bool is_aggregate;
};

unique_ptr<LogicalOperator> OptimizeIdentityQuery(Connection &con, const string &query) {
	Parser parser(*con.context);
	parser.ParseQuery(query);
	REQUIRE(parser.statements.size() == 1);
	Planner planner(*con.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	Optimizer optimizer(*planner.binder, *con.context);
	return optimizer.Optimize(std::move(planner.plan));
}

void CollectIdentityFunctions(const Expression &expr, vector<BoundFunctionInfo> &result) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &fun = expr.Cast<BoundFunctionExpression>().Function();
		result.push_back(
		    {fun.GetCatalogName(), fun.GetSchemaName(), fun.GetName(), fun.GetArguments(), fun.GetReturnType(), false});
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_AGGREGATE) {
		auto &fun = expr.Cast<BoundAggregateExpression>().Function();
		result.push_back(
		    {fun.GetCatalogName(), fun.GetSchemaName(), fun.GetName(), fun.GetArguments(), fun.GetReturnType(), true});
	} else if (expr.GetExpressionClass() == ExpressionClass::BOUND_WINDOW) {
		// a window expression holds either an aggregate (sum(x) OVER ()) or a true window function (row_number())
		auto &window = expr.Cast<BoundWindowExpression>();
		if (window.AggregateFunction()) {
			auto &fun = *window.AggregateFunction();
			result.push_back({fun.GetCatalogName(), fun.GetSchemaName(), fun.GetName(), fun.GetArguments(),
			                  fun.GetReturnType(), true});
		}
		if (window.WindowFunction()) {
			auto &fun = *window.WindowFunction();
			result.push_back({fun.GetCatalogName(), fun.GetSchemaName(), fun.GetName(), fun.GetArguments(),
			                  fun.GetReturnType(), false});
		}
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](const Expression &child) { CollectIdentityFunctions(child, result); });
}

void CollectIdentityFunctions(const LogicalOperator &op, vector<BoundFunctionInfo> &result) {
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](const unique_ptr<Expression> *expr) { CollectIdentityFunctions(**expr, result); });
	for (auto &child : op.children) {
		CollectIdentityFunctions(*child, result);
	}
}

//! The join filter pushdown aggregates hang off the join rather than being plan expressions, so
//! EnumerateExpressions does not reach them
void CollectJoinFilterAggregates(const LogicalOperator &op, vector<BoundFunctionInfo> &result) {
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.filter_pushdown) {
			for (auto &aggregate : join.filter_pushdown->min_max_aggregates) {
				CollectIdentityFunctions(*aggregate, result);
			}
		}
	}
	for (auto &child : op.children) {
		CollectJoinFilterAggregates(*child, result);
	}
}

vector<BoundFunctionInfo> PlanIdentityFunctions(Connection &con, const string &query) {
	vector<BoundFunctionInfo> result;
	auto plan = OptimizeIdentityQuery(con, query);
	CollectIdentityFunctions(*plan, result);
	return result;
}

//! Compressed materialization functions are registered so that plans containing them can be (de)serialized, but
//! their bind throws - they are constructed directly, and so carry no qualification.
bool IsInternalCompressionFunction(const Identifier &name) {
	return StringUtil::StartsWith(name.GetIdentifierName(), "__internal_compress_") ||
	       StringUtil::StartsWith(name.GetIdentifierName(), "__internal_decompress_");
}

optional_idx FindIdentityFunction(const vector<BoundFunctionInfo> &functions, const Identifier &name) {
	for (idx_t i = 0; i < functions.size(); i++) {
		if (functions[i].name == name) {
			return i;
		}
	}
	return optional_idx();
}

//! Assert that the named function was introduced, and that every instance of it kept the catalog and schema name
//! of its definition - the same qualification the binder gives the function when it is written in SQL
const BoundFunctionInfo &RequireIdentityFunction(const vector<BoundFunctionInfo> &functions, const Identifier &name) {
	auto index = FindIdentityFunction(functions, name);
	INFO("expected function " << name.GetIdentifierName() << " in the optimized plan");
	REQUIRE(index.IsValid());
	for (auto &fun : functions) {
		if (fun.name != name) {
			continue;
		}
		REQUIRE(fun.catalog_name == Identifier::SystemCatalog());
		REQUIRE(fun.schema_name == Identifier::DefaultSchema());
	}
	return functions[index.GetIndex()];
}

} // namespace

TEST_CASE("LIKE rewrites keep the definition of the built-in they introduce", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE strings(s VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO strings VALUES ('abc'), ('cab'), (NULL)"));

	con.BeginTransaction();
	for (auto &entry : vector<pair<string, string>> {{"SELECT s LIKE 'abc%' FROM strings", "prefix"},
	                                                 {"SELECT s LIKE '%abc' FROM strings", "suffix"},
	                                                 {"SELECT s LIKE '%abc%' FROM strings", "contains"},
	                                                 {"SELECT s NOT LIKE 'abc%' FROM strings", "prefix"},
	                                                 {"SELECT s GLOB 'abc*' FROM strings", "prefix"}}) {
		INFO(entry.first);
		auto functions = PlanIdentityFunctions(con, entry.first);
		auto &fun = RequireIdentityFunction(functions, Identifier(entry.second));
		REQUIRE(fun.arguments == vector<LogicalType> {LogicalType::VARCHAR, LogicalType::VARCHAR});
		REQUIRE(fun.return_type == LogicalType::BOOLEAN);
		REQUIRE(!fun.is_aggregate);
	}
	con.Rollback();
}

TEST_CASE("Regex rewrites keep the definition of the built-in they introduce", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE strings(s VARCHAR)"));

	con.BeginTransaction();
	// a literal regex becomes contains()
	RequireIdentityFunction(PlanIdentityFunctions(con, "SELECT regexp_matches(s, 'abc') FROM strings"), "contains");
	// an anchored regex becomes a LIKE, which the LIKE rule then turns into prefix()
	RequireIdentityFunction(PlanIdentityFunctions(con, "SELECT regexp_matches(s, '^abc') FROM strings"), "prefix");
	con.Rollback();
}

TEST_CASE("String prefix rewrites keep the definition of prefix()", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE strings(s VARCHAR)"));

	con.BeginTransaction();
	RequireIdentityFunction(PlanIdentityFunctions(con, "SELECT left(s, 3) = 'abc' FROM strings"), "prefix");
	RequireIdentityFunction(PlanIdentityFunctions(con, "SELECT instr(s, 'abc') = 1 FROM strings"), "prefix");
	con.Rollback();
}

TEST_CASE("Optimizer-introduced constant_or_null keeps its definition", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(i INTEGER)"));

	con.BeginTransaction();
	auto functions = PlanIdentityFunctions(con, "SELECT i * 0 FROM integers");
	auto &fun = RequireIdentityFunction(functions, "constant_or_null");
	// the return type is the type of the leading constant
	REQUIRE(fun.return_type == LogicalType::INTEGER);
	REQUIRE(fun.arguments.size() == 2);
	con.Rollback();
}

TEST_CASE("Aggregate rewrites keep the definition of the aggregates they introduce", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers(g INTEGER, i INTEGER)"));
	// a NULL keeps the count over i from being turned into a count_star by statistics propagation
	REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES (1, 1), (1, NULL), (2, 3)"));

	con.BeginTransaction();
	// avg(x) is rewritten into sum(x) / count(x)
	{
		auto functions = PlanIdentityFunctions(con, "SELECT avg(i) FROM integers");
		auto &count = RequireIdentityFunction(functions, "count");
		REQUIRE(count.is_aggregate);
		REQUIRE(count.return_type == LogicalType::BIGINT);
		REQUIRE(FindIdentityFunction(functions, "avg").IsValid() == false);
		// statistics propagation may swap sum for its no-overflow implementation
		REQUIRE((FindIdentityFunction(functions, "sum").IsValid() ||
		         FindIdentityFunction(functions, "sum_no_overflow").IsValid()));
	}
	// over a column without NULLs, statistics propagation turns the introduced count into a count_star
	{
		auto functions = PlanIdentityFunctions(con, "SELECT avg(g) FROM integers");
		REQUIRE(FindIdentityFunction(functions, "count_star").IsValid());
	}
	// ROLLUP is cascaded through exported aggregate states combined by combine_aggr()
	{
		auto functions = PlanIdentityFunctions(con, "SELECT g, i, sum(i) FROM integers GROUP BY ROLLUP(g, i)");
		auto &combine = RequireIdentityFunction(functions, "combine_aggr");
		REQUIRE(combine.is_aggregate);
	}
	con.Rollback();
}

TEST_CASE("Column pruning keeps the definition of count_star()", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers AS SELECT i::INTEGER AS i FROM range(100) t(i)"));

	con.BeginTransaction();
	// pruning the unreferenced sum leaves an aggregate with no expressions, which the optimizer replaces with a
	// count_star of its own. The filter keeps the aggregate from being folded away before that happens.
	auto functions = PlanIdentityFunctions(con, "SELECT 1 FROM (SELECT sum(i) FROM integers WHERE random() > 0.5) t");
	auto &count_star = RequireIdentityFunction(functions, "count_star");
	REQUIRE(count_star.is_aggregate);
	REQUIRE(count_star.return_type == LogicalType::BIGINT);
	REQUIRE(count_star.arguments.empty());
	REQUIRE(FindIdentityFunction(functions, "sum").IsValid() == false);
	con.Rollback();
}

TEST_CASE("Aggregate reuse keeps the definition of combine_aggr()", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE reuse_dim(id INTEGER, label VARCHAR)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO reuse_dim VALUES (10, 'x'), (20, 'b'), (30, 'x'), (40, 'd')"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE reuse_owner(k INTEGER, dim_id INTEGER)"));
	REQUIRE_NO_FAIL(con.Query("INSERT INTO reuse_owner VALUES (1, 10), (2, 20), (3, 30), (4, 40)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE reuse_fact(k INTEGER, v INTEGER, w INTEGER)"));
	REQUIRE_NO_FAIL(con.Query(
	    "INSERT INTO reuse_fact VALUES (1, 10, 1), (1, 20, 2), (2, 5, 3), (3, 40, 4), (3, 50, 5), (4, NULL, 6)"));

	con.BeginTransaction();
	auto functions = PlanIdentityFunctions(con, "SELECT o.k, d.label, sum(f.v) "
	                                            "FROM reuse_owner o JOIN reuse_dim d ON o.dim_id = d.id "
	                                            "JOIN reuse_fact f USING (k) "
	                                            "WHERE o.k IN (SELECT k FROM reuse_fact GROUP BY k HAVING sum(v) > 25) "
	                                            "GROUP BY o.k, d.label");
	RequireIdentityFunction(functions, "combine_aggr");
	con.Rollback();
}

TEST_CASE("Partial aggregate pushdown keeps the definition of combine_aggr()", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE fact AS SELECT i % 1000 AS dim_key, (i * 17) % 100 AS measure "
	                          "FROM range(100000) _(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE dim AS SELECT i AS dim_key, (i % 7) AS d_a, (i % 11) AS d_b, "
	                          "(i % 13) AS d_c, (i % 17) AS d_d FROM range(1000) _(i)"));
	REQUIRE_NO_FAIL(con.Query("ANALYZE"));

	con.BeginTransaction();
	auto functions = PlanIdentityFunctions(con, "SELECT d_a, d_b, d_c, d_d, sum(measure) AS s "
	                                            "FROM fact JOIN dim USING (dim_key) GROUP BY d_a, d_b, d_c, d_d");
	RequireIdentityFunction(functions, "combine_aggr");
	con.Rollback();
}

TEST_CASE("Compressed materialization keeps its internal functions unqualified", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE cm AS SELECT (i % 100) + 1000 AS a, (i % 7)::VARCHAR AS b "
	                          "FROM range(10000) _(i)"));
	REQUIRE_NO_FAIL(con.Query("ANALYZE"));

	con.BeginTransaction();
	auto functions = PlanIdentityFunctions(con, "SELECT a, b, count(*) FROM cm GROUP BY a, b");
	idx_t compression_functions = 0;
	for (auto &fun : functions) {
		if (!IsInternalCompressionFunction(fun.name)) {
			continue;
		}
		compression_functions++;
		// these are not bindable through the catalog, so they stay unqualified
		REQUIRE(fun.catalog_name.empty());
		REQUIRE(fun.schema_name.empty());
	}
	REQUIRE(compression_functions > 0);
	// count_star is introduced by the planner here, and is qualified like any catalog-bound aggregate
	RequireIdentityFunction(functions, "count_star");
	con.Rollback();
}

TEST_CASE("Join filter pushdown keeps the definition of min() and max()", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	// the build side keys are spread out, so the min/max filters cannot be resolved from statistics alone
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE big AS SELECT i::INTEGER AS k FROM range(100000) t(i)"));
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE small AS SELECT (i * 7)::INTEGER AS k FROM range(50) t(i)"));

	con.BeginTransaction();
	auto plan = OptimizeIdentityQuery(con, "SELECT count(*) FROM big JOIN small USING (k)");

	vector<BoundFunctionInfo> functions;
	CollectJoinFilterAggregates(*plan, functions);
	auto &min_function = RequireIdentityFunction(functions, "min");
	REQUIRE(min_function.is_aggregate);
	RequireIdentityFunction(functions, "max");
	con.Rollback();
}

TEST_CASE("Planner-introduced row_number keeps its definition", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i::INTEGER AS v, (i % 5)::INTEGER AS g FROM range(50) x(i)"));

	con.BeginTransaction();
	// WITH ORDINALITY, decorrelating a correlated subquery with a LIMIT, and a set operation each push a
	// row_number() window of their own
	for (auto &query : vector<string> {"SELECT * FROM range(3) WITH ORDINALITY",
	                                   "SELECT v, (SELECT g FROM t x WHERE x.v = t.v LIMIT 1) FROM t",
	                                   "SELECT v FROM t UNION SELECT v FROM t"}) {
		INFO(query);
		auto functions = PlanIdentityFunctions(con, query);
		if (!FindIdentityFunction(functions, "row_number").IsValid()) {
			continue;
		}
		auto &row_number = RequireIdentityFunction(functions, "row_number");
		REQUIRE(!row_number.is_aggregate);
		REQUIRE(row_number.return_type == LogicalType::BIGINT);
	}
	// WITH ORDINALITY always introduces one
	RequireIdentityFunction(PlanIdentityFunctions(con, "SELECT * FROM range(3) WITH ORDINALITY"), "row_number");
	con.Rollback();
}

TEST_CASE("Specializing a bound aggregate keeps its definition", "[optimizer][function_identity]") {
	DuckDB db(nullptr);
	Connection con(db);
	REQUIRE_NO_FAIL(con.Query("CREATE TABLE t AS SELECT i::INTEGER AS v, (i * 1.5)::DECIMAL(10,2) AS d, "
	                          "(i % 5)::VARCHAR AS s FROM range(50) x(i)"));

	con.BeginTransaction();
	// each of these binds from the catalog and then replaces the implementation with a specialized one - the
	// replacement comes from a factory, so it must not take the qualification with it
	for (auto &entry : vector<pair<string, string>> {{"SELECT sum(v) FROM t", "sum_no_overflow"},
	                                                 {"SELECT min(v) FROM t", "min"},
	                                                 {"SELECT max(v) FROM t", "max"},
	                                                 {"SELECT quantile(v, 0.5) FROM t", "quantile_disc"},
	                                                 {"SELECT first(v) FROM t", "first"},
	                                                 {"SELECT arg_min(v, d) FROM t", "arg_min"}}) {
		INFO(entry.first);
		RequireIdentityFunction(PlanIdentityFunctions(con, entry.first), Identifier(entry.second));
	}
	con.Rollback();
}

TEST_CASE("Exported recursive payload aggregates resolve qualified schemas",
          "[optimizer][function_identity][recursive_cte]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE SCHEMA payload_schema; CREATE SCHEMA outer_schema; "
	                                 "CREATE SCHEMA outer_schema.inner_schema"));
	connection.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*connection.context, Identifier("memory"));
	MetaTransaction::Get(*connection.context)
	    .ModifyDatabase(catalog.GetAttached(), DatabaseModificationType::CREATE_CATALOG_ENTRY);
	for (idx_t schema = 0; schema < 3; schema++) {
		auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(
		    *connection.context, QualifiedName("system", "main", schema == 0 ? "min" : "max"));
		auto functions = entry.functions;
		functions.SetName("payload_choice");
		functions.ApplyToFunctions([](AggregateFunction &function) { function.SetName("payload_choice"); });
		CreateAggregateFunctionInfo info(std::move(functions));
		info.SetQualifiedName(
		    schema == 2 ? QualifiedName(vector<Identifier> {"memory", "outer_schema", "inner_schema"}, "payload_choice")
		                : QualifiedName("memory", schema == 0 ? "main" : "payload_schema", "payload_choice"));
		info.internal = false;
		catalog.CreateFunction(*connection.context, info);
	}
	connection.Commit();
	REQUIRE_NO_FAIL(connection.Query("SET search_path='payload_schema,main'"));
	REQUIRE_NO_FAIL(connection.Query("SET debug_verify_statement='explain_sql_strict'"));
	for (auto name : {"memory.main.payload_choice", "memory.payload_schema.payload_choice",
	                  "memory.outer_schema.inner_schema.payload_choice", "payload_choice"}) {
		CAPTURE(name);
		auto expected = string(name) == "memory.main.payload_choice" ? 1 : 3;
		auto ordinary = connection.Query("SELECT " + string(name) + "(v) FROM (VALUES(1),(3))t(v)");
		REQUIRE_NO_FAIL(*ordinary);
		REQUIRE(ordinary->Collection().GetValue(0, 0) == Value::INTEGER(expected));
		auto recursive = connection.Query("WITH RECURSIVE r(k,v) USING KEY(k," + string(name) +
		                                  "(v)) AS (SELECT * FROM (VALUES(1,1),(1,3))t(k,v) UNION ALL "
		                                  "SELECT k+1,v FROM r WHERE k<2) SELECT k,v FROM r ORDER BY k");
		REQUIRE_NO_FAIL(*recursive);
		REQUIRE(recursive->RowCount() == 2);
		REQUIRE(recursive->Collection().GetValue(1, 0) == Value::INTEGER(expected));
		REQUIRE(recursive->Collection().GetValue(1, 1) == Value::INTEGER(expected));
	}
}
