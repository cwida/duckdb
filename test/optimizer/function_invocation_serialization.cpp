#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/parser/parsed_data/create_window_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/planner/logical_operator_visitor.hpp"

#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

using namespace duckdb;

TEST_CASE("Serialize-only table functions retain rebinding inputs in legacy plans",
          "[serialization][function_invocation][logical_get_bwc]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	TableFunction function("legacy_rebind_inputs", {LogicalType::VARCHAR}, nullptr);
	function.bind = [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
	                   vector<Identifier> &names) -> unique_ptr<FunctionData> {
		REQUIRE(input.inputs.size() == 1);
		REQUIRE(input.inputs[0] == Value("scan_source"));
		REQUIRE(input.named_parameters.size() == 1);
		REQUIRE(input.named_parameters.at("option") == Value::INTEGER(42));
		REQUIRE(input.input_table_types == vector<LogicalType> {LogicalType::BIGINT});
		REQUIRE(input.input_table_names == vector<Identifier> {Identifier("input_column")});
		types.emplace_back(LogicalType::BIGINT);
		names.emplace_back("result");
		return make_uniq<TableFunctionData>();
	};
	SECTION("Serialize callback without deserialize callback") {
		function.serialize = [](Serializer &, const optional_ptr<FunctionData>, const BoundTableFunction &) {
			throw NotImplementedException("This scan cannot serialize its bind data");
		};
	}
	SECTION("No serialization callbacks") {
	}
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);
	LogicalGet get(TableIndex(0), BoundTableFunction(function), make_uniq<TableFunctionData>(), {LogicalType::BIGINT},
	               {Identifier("result")});
	get.parameters = {Value("scan_source")};
	get.named_parameters["option"] = Value::INTEGER(42);
	get.input_table_types = {LogicalType::BIGINT};
	get.input_table_names = {Identifier("input_column")};

	for (const auto &version : {"v1.3.0", "v1.4.0", "v1.5.0", "latest"}) {
		CAPTURE(version);
		SerializationOptions options;
		options.storage_compatibility = StorageCompatibility::FromString(version);
		MemoryStream stream(Allocator::Get(context));
		BinarySerializer::Serialize(get, stream, options);
		stream.Rewind();
		bound_parameter_map_t parameters;
		auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
		auto &copied_get = copy->Cast<LogicalGet>();
		REQUIRE(copied_get.bind_data);
		REQUIRE(copied_get.parameters == get.parameters);
		REQUIRE(copied_get.named_parameters == get.named_parameters);
		REQUIRE(copied_get.input_table_types == get.input_table_types);
		REQUIRE(copied_get.input_table_names == get.input_table_names);
	}
	connection.Rollback();
}

namespace {

struct LegacyScanBindData : public TableFunctionData {
	explicit LegacyScanBindData(int64_t value_p) : value(value_p) {
	}
	int64_t value;
};

static void RequireSameValues(QueryResult &expected, QueryResult &actual) {
	REQUIRE_NO_FAIL(actual);
	REQUIRE(expected.GetTypes() == actual.GetTypes());
	REQUIRE(expected.RowCount() == actual.RowCount());
	auto expected_rows = expected.Collection().GetRows();
	auto actual_rows = actual.Collection().GetRows();
	for (idx_t row = 0; row < expected_rows.size(); row++) {
		for (idx_t col = 0; col < expected.ColumnCount(); col++) {
			REQUIRE(Value::NotDistinctFrom(expected_rows.GetValue(col, row), actual_rows.GetValue(col, row)));
		}
	}
}

} // namespace

TEST_CASE("Table function bind data remains readable after removing its serialize callback",
          "[serialization][function_invocation][deserialize_only]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	TableFunction writer("legacy_scan", {}, nullptr);
	writer.bind = [](ClientContext &, TableFunctionBindInput &, vector<LogicalType> &,
	                 vector<Identifier> &) -> unique_ptr<FunctionData> {
		throw InternalException("Serialized bind data must not be rebound");
	};
	writer.serialize = [](Serializer &serializer, const optional_ptr<FunctionData> data, const BoundTableFunction &) {
		serializer.WriteProperty(100, "value", data->Cast<LegacyScanBindData>().value);
	};
	writer.deserialize = [](Deserializer &deserializer, BoundTableFunction &) -> unique_ptr<FunctionData> {
		return make_uniq<LegacyScanBindData>(deserializer.ReadProperty<int64_t>(100, "value"));
	};
	auto reader = writer;
	bool can_deserialize = true;
	SECTION("Reader retains only the deserialize callback") {
		reader.serialize = nullptr;
	}
	SECTION("Reader retains both callbacks") {
	}
	SECTION("Reader lacks the deserialize callback") {
		reader.deserialize = nullptr;
		can_deserialize = false;
	}
	CreateTableFunctionInfo info(reader);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);
	LogicalGet get(TableIndex(0), BoundTableFunction(writer), make_uniq<LegacyScanBindData>(42), {LogicalType::BIGINT},
	               {Identifier("result")});
	for (const auto &version : {"v1.4.0", "v1.5.0", "latest"}) {
		CAPTURE(version);
		SerializationOptions options;
		options.storage_compatibility = StorageCompatibility::FromString(version);
		MemoryStream stream(Allocator::Get(context));
		BinarySerializer::Serialize(get, stream, options);
		stream.Rewind();
		bound_parameter_map_t parameters;
		if (!can_deserialize) {
			REQUIRE_THROWS_WITH(BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters),
			                    Catch::Matchers::Contains("no deserialization function for"));
			continue;
		}
		auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
		auto &copied_get = copy->Cast<LogicalGet>();
		REQUIRE(copied_get.bind_data);
		REQUIRE(copied_get.bind_data->Cast<LegacyScanBindData>().value == 42);
	}
	connection.Rollback();
}

TEST_CASE("Current list serialization preserves bound ordering across repeated copies",
          "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	bool optimize = false;
	SECTION("Bound plan") {
	}
	SECTION("Optimized plan") {
		optimize = true;
	}
	REQUIRE_NO_FAIL(connection.Query("SET threads=1; SET debug_disable_optimizer=true; "
	                                 "CREATE TABLE ordering_input(l VARCHAR[]); "
	                                 "INSERT INTO ordering_input VALUES (['a','B',NULL,'a']),([]),(NULL)"));
	connection.BeginTransaction();
	for (const auto &expression : {"list_sort(l)", "list_sort(l, 'DESC')", "list_sort(l, 'DESC', 'NULLS LAST')",
	                               "list_reverse_sort(l)", "list_reverse_sort(l, 'NULLS LAST')", "list_grade_up(l)",
	                               "list_grade_up(l, 'DESC')", "list_grade_up(l, 'DESC', 'NULLS LAST')"}) {
		INFO(expression);
		REQUIRE_NO_FAIL(connection.Query("SET default_collation='nocase'; SET default_order='ASC'; "
		                                 "SET default_null_order='NULLS FIRST'"));
		auto sql = "SELECT " + string(expression) + " FROM ordering_input";
		auto expected = connection.Query(sql);
		REQUIRE_NO_FAIL(*expected);
		Parser parser(*connection.context);
		parser.ParseQuery(sql);
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		if (optimize) {
			planner.plan->ResolveOperatorTypes();
			Optimizer optimizer(*planner.binder, *connection.context);
			planner.plan = optimizer.Optimize(std::move(planner.plan));
		}
		REQUIRE_NO_FAIL(connection.Query("SET default_collation=''; SET default_order='DESC'; "
		                                 "SET default_null_order='NULLS LAST'"));
		auto plan = std::move(planner.plan);
		for (idx_t copy = 0; copy < 2; copy++) {
			plan = plan->Copy(*connection.context);
		}
		plan->ResolveOperatorTypes();
		auto actual = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
		RequireSameValues(*expected, *actual);
	}
	connection.Rollback();
}

static unique_ptr<LogicalOperator> PlanAndOptimize(Connection &connection, const string &sql) {
	Parser parser(*connection.context);
	parser.ParseQuery(sql);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	planner.plan->ResolveOperatorTypes();
	Optimizer optimizer(*planner.binder, *connection.context);
	auto plan = optimizer.Optimize(std::move(planner.plan));
	plan->ResolveOperatorTypes();
	return plan;
}

static void CheckCompressionOrigins(ClientContext &context, LogicalOperator &plan, vector<bool> &seen) {
	LogicalOperatorVisitor::EnumerateExpressions(plan, [&](unique_ptr<Expression> *root) {
		ExpressionIterator::VisitExpression<BoundFunctionExpression>(
		    **root, [&](const BoundFunctionExpression &expression) {
			    seen[idx_t(CMUtils::GetExpressionType(expression))] = true;
			    MemoryStream stream(Allocator::Get(context));
			    BinarySerializer::Serialize(expression, stream);
			    stream.Rewind();
			    bound_parameter_map_t parameters;
			    auto copy = BinaryDeserializer::Deserialize<Expression>(stream, context, parameters);
			    REQUIRE(CMUtils::GetExpressionType(copy->Cast<BoundFunctionExpression>()) ==
			            CMUtils::GetExpressionType(expression));
		    });
	});
	for (auto &child : plan.children) {
		CheckCompressionOrigins(context, *child, seen);
	}
}

TEST_CASE("Compressed materialization functions retain their identity across copies and serialization",
          "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("SET threads=1; CREATE TABLE compression_input AS "
	                                 "SELECT i::BIGINT i, i::VARCHAR s FROM range(100) r(i)"));
	connection.BeginTransaction();
	vector<bool> seen(4, false);
	for (const auto &sql :
	     {"SELECT i, count(*) FROM compression_input GROUP BY i",
	      "SELECT s, count(*) FROM compression_input GROUP BY s", "SELECT upper(s) FROM compression_input"}) {
		auto plan = PlanAndOptimize(connection, sql);
		CheckCompressionOrigins(*connection.context, *plan, seen);
	}
	for (auto found : seen) {
		REQUIRE(found);
	}
	connection.Rollback();
}

static optional_ptr<LogicalSecureView> FindSecureView(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_SECURE_VIEW) {
		return op.Cast<LogicalSecureView>();
	}
	for (auto &child : op.children) {
		auto view = FindSecureView(*child);
		if (view) {
			return view;
		}
	}
	return nullptr;
}

TEST_CASE("Secure-view caller predicates retain source positions across pruning and serialization",
          "[serialization][secure_view]") {
	DuckDB db(nullptr);
	Connection connection(db);
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE filter_input(i INTEGER, p VARCHAR); "
	                                 "INSERT INTO filter_input VALUES (1,'a'),(2,'b'),(NULL,'n'),(1,'a'); "
	                                 "CREATE SECURE VIEW filter_view AS SELECT * FROM filter_input"));
	connection.BeginTransaction();
	auto plan = PlanAndOptimize(connection, "SELECT p FROM filter_view WHERE i=1 ORDER BY p");
	for (idx_t copy = 0; copy < 2; copy++) {
		auto view = FindSecureView(*plan);
		REQUIRE(view);
		REQUIRE(view->source_filters.size() == 1);
		REQUIRE(view->source_filters.size() == view->pushed_filters.size());
		REQUIRE(view->source_filters[0]);
		idx_t references = 0;
		ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
		    *view->source_filters[0], [&](const BoundColumnRefExpression &ref) {
			    REQUIRE(ref.Depth() == 0);
			    REQUIRE(ref.Binding() == ColumnBinding(TableIndex(0), ProjectionIndex(0)));
			    references++;
		    });
		REQUIRE(references == 1);
		plan = plan->Copy(*connection.context);
		plan->ResolveOperatorTypes();
	}
	REQUIRE_NO_FAIL(connection.Query("SET debug_disable_optimizer=true"));
	auto result = connection.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {"a", "a"}));
	connection.Rollback();
}

TEST_CASE("Secure-view source positions survive window column pruning", "[serialization][secure_view]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	for (idx_t retained_window = 0; retained_window < 2; retained_window++) {
		CAPTURE(retained_window);
		Parser parser(*connection.context);
		parser.ParseQuery("SELECT row_number() OVER (), rank() OVER ()");
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		planner.plan->ResolveOperatorTypes();
		auto window = std::move(planner.plan->children[0]);
		REQUIRE(window->type == LogicalOperatorType::LOGICAL_WINDOW);
		REQUIRE(window->expressions.size() == 2);
		auto bindings = window->GetColumnBindings();
		auto types = window->types;
		auto source_position = bindings.size() - 2 + retained_window;
		vector<unique_ptr<Expression>> outputs;
		outputs.push_back(make_uniq<BoundColumnRefExpression>(types[source_position], bindings[source_position]));
		unique_ptr<LogicalOperator> plan =
		    make_uniq<LogicalProjection>(planner.binder->GenerateTableIndex(), std::move(outputs));
		plan->children.push_back(make_uniq<LogicalSecureView>(
		    "window_view", QualifiedName("memory", "main", "window_view"), types, nullptr, std::move(window)));
		Optimizer optimizer(*planner.binder, *connection.context);
		RemoveUnusedColumns remove(optimizer);
		remove.VisitOperator(plan);
		plan->ResolveOperatorTypes();
		for (idx_t copy = 0; copy < 2; copy++) {
			auto view = FindSecureView(*plan);
			REQUIRE(view);
			REQUIRE(view->children[0]->type == LogicalOperatorType::LOGICAL_WINDOW);
			REQUIRE(view->children[0]->expressions.size() == 1);
			auto selected = plan->expressions[0]->Cast<BoundColumnRefExpression>().Binding();
			idx_t matches = 0;
			for (idx_t i = 0; i < view->output_bindings.size(); i++) {
				if (view->output_bindings[i] != selected) {
					continue;
				}
				auto &source = view->output_expressions[i]->Cast<BoundColumnRefExpression>();
				REQUIRE(source.Binding() == ColumnBinding(TableIndex(0), ProjectionIndex(source_position)));
				matches++;
			}
			REQUIRE(matches == 1);
			plan = plan->Copy(*connection.context);
			plan->ResolveOperatorTypes();
		}
	}
	connection.Rollback();
}

TEST_CASE("Function unbind callbacks reconstruct retained invocation data", "[function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	for (const auto &sql : {"SELECT struct_insert({'a': 1}, \"new field\" := 2)", "SELECT alias(42)",
	                        "SELECT ([1,2,3])[:2]", "SELECT ([1,2,3])[2:]"}) {
		CAPTURE(sql);
		Parser parser(*connection.context);
		parser.ParseQuery(sql);
		Planner planner(*connection.context);
		planner.CreatePlan(std::move(parser.statements[0]));
		auto &expression = *planner.plan->expressions[0];
		REQUIRE(expression.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION);
		auto &function = expression.Cast<BoundFunctionExpression>();
		auto &definition = *function.Function().GetDefinition();
		REQUIRE(definition.HasUnbindCallback());
		vector<unique_ptr<ParsedExpression>> children;
		for (auto &child : function.GetChildren()) {
			children.push_back(
			    ConstantExpression::FromValue(ExpressionExecutor::EvaluateScalar(*connection.context, *child)));
		}
		FunctionUnbindInput input(function, std::move(children));
		auto parsed = definition.GetUnbindCallback()(input);
		REQUIRE(parsed);
		auto expected = connection.Query(sql);
		auto actual = connection.Query("SELECT " + parsed->ToString());
		REQUIRE_NO_FAIL(*expected);
		RequireSameValues(*expected, *actual);
	}
	connection.Rollback();
}
TEST_CASE("Nested function qualification survives plan copies", "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	REQUIRE_NO_FAIL(connection.Query("CREATE SCHEMA parent; CREATE SCHEMA parent.child"));
	auto &context = *connection.context;
	auto &catalog = Catalog::GetCatalog(context, Identifier("memory"));
	auto install = [&](CreateFunctionInfo &info) {
		info.internal = false;
		info.SetQualifiedName(QualifiedName({catalog.GetName(), Identifier("parent"), Identifier("child")},
		                                    info.GetQualifiedName().Name()));
		catalog.CreateFunction(context, info);
	};
	auto &scalar = Catalog::GetEntry<ScalarFunctionCatalogEntry>(context, QualifiedName("system", "main", "abs"));
	CreateScalarFunctionInfo scalar_info(scalar.functions);
	install(scalar_info);
	auto &aggregate = Catalog::GetEntry<AggregateFunctionCatalogEntry>(context, QualifiedName("system", "main", "min"));
	CreateAggregateFunctionInfo aggregate_info(aggregate.functions);
	install(aggregate_info);
	auto &window =
	    Catalog::GetEntry<WindowFunctionCatalogEntry>(context, QualifiedName("system", "main", "row_number"));
	CreateWindowFunctionInfo window_info(window.functions);
	install(window_info);
	auto &table = Catalog::GetEntry<TableFunctionCatalogEntry>(context, QualifiedName("system", "main", "range"));
	CreateTableFunctionInfo table_info(table.functions);
	install(table_info);
	const string sql = "SELECT memory.parent.child.abs(i), memory.parent.child.min(i) OVER (), "
	                   "memory.parent.child.row_number() OVER () FROM memory.parent.child.range(3) r(i)";
	Parser parser(context);
	parser.ParseQuery(sql);
	Planner planner(context);
	planner.CreatePlan(std::move(parser.statements[0]));
	auto copy = planner.plan->Copy(context);
	copy = copy->Copy(context);
	copy->ResolveOperatorTypes();
	auto expected = connection.Query(sql);
	auto actual = connection.Query(make_uniq<LogicalPlanStatement>(std::move(copy)));
	REQUIRE_NO_FAIL(*expected);
	RequireSameValues(*expected, *actual);
	connection.Rollback();
}

TEST_CASE("Custom scan serialization retains invocation inputs", "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	auto file = TestCreatePath("retained_scan_arguments.parquet");
	REQUIRE_NO_FAIL(connection.Query("COPY (SELECT 42 AS i) TO " + Value(file).ToSQLString() + " (FORMAT PARQUET)"));
	connection.BeginTransaction();
	auto plan =
	    PlanAndOptimize(connection, "SELECT * FROM read_parquet(" + Value(file).ToSQLString() + ", filename := true)");
	std::function<void(LogicalOperator &)> check = [&](LogicalOperator &op) {
		if (op.type == LogicalOperatorType::LOGICAL_GET) {
			auto &get = op.Cast<LogicalGet>();
			REQUIRE(get.parameters.size() == 1);
			REQUIRE(get.parameters[0] == Value(file));
			REQUIRE(get.named_parameters.at("filename") == Value::BOOLEAN(true));
		}
		for (auto &child : op.children) {
			check(*child);
		}
	};
	check(*plan);
	for (idx_t i = 0; i < 2; i++) {
		plan = plan->Copy(*connection.context);
		check(*plan);
	}
	connection.Rollback();
}

namespace {

struct KeywordScanBindData : public TableFunctionData {
	explicit KeywordScanBindData(Value value_p) : value(std::move(value_p)) {
	}
	Value value;
};

//! Returns the option it was called with, typed as the overload it was bound to declares it
static unique_ptr<FunctionData> KeywordScanBind(ClientContext &, TableFunctionBindInput &input,
                                                vector<LogicalType> &types, vector<Identifier> &names) {
	// "opt" is a keyword-only parameter, or an option of the overload's "**kwargs"
	auto &signature = input.table_function.GetSignature();
	auto param_idx = signature.GetParameterIndexByName("opt");
	auto option = signature.GetTypedKwargs() ? signature.GetTypedKwargs()->Find("opt") : nullptr;
	REQUIRE((param_idx.IsValid() || option));
	auto &type = param_idx.IsValid() ? signature.GetParameter(param_idx.GetIndex()).GetType() : option->type;
	auto entry = input.named_parameters.find("opt");
	types.push_back(type);
	names.emplace_back("opt");
	return make_uniq<KeywordScanBindData>(entry == input.named_parameters.end() ? Value(type) : entry->second);
}

static void KeywordScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> data,
                                 const BoundTableFunction &) {
	serializer.WriteProperty(100, "value", data->Cast<KeywordScanBindData>().value);
}

static unique_ptr<FunctionData> KeywordScanDeserialize(Deserializer &deserializer, BoundTableFunction &) {
	return make_uniq<KeywordScanBindData>(deserializer.ReadProperty<Value>(100, "value"));
}

static TableFunction KeywordScan(FunctionSignature signature, bool serialize_bind_data) {
	TableFunction function("keyword_scan", std::move(signature), nullptr, KeywordScanBind);
	if (serialize_bind_data) {
		function.serialize = KeywordScanSerialize;
		function.deserialize = KeywordScanDeserialize;
	}
	return function;
}

static LogicalGet &FindGet(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return op.Cast<LogicalGet>();
	}
	REQUIRE(op.children.size() == 1);
	return FindGet(*op.children[0]);
}

} // namespace

TEST_CASE("Table function overloads selected by named arguments survive serialization",
          "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	bool serialize_bind_data = false;
	SECTION("Rebound on deserialization") {
	}
	SECTION("Bind data serialized") {
		serialize_bind_data = true;
	}
	// one overload requires an INTEGER option, the other two take a positional argument and differ only in the type
	// of an optional one
	TableFunctionSet set("keyword_scan");
	set.AddFunction(KeywordScan(FunctionSignature().AddKeywordOnly("opt", LogicalType::INTEGER), serialize_bind_data));
	// keyword-only parameters take part in overload selection - options would not
	set.AddFunction(KeywordScan(FunctionSignature()
	                                .AddParameter(LogicalType::VARCHAR)
	                                .AddKeywordOnly("opt", LogicalType::DOUBLE, Value(LogicalType::DOUBLE)),
	                            serialize_bind_data));
	set.AddFunction(KeywordScan(FunctionSignature()
	                                .AddParameter(LogicalType::VARCHAR)
	                                .AddKeywordOnly("opt", LogicalType::DATE, Value(LogicalType::DATE)),
	                            serialize_bind_data));
	CreateTableFunctionInfo info(set);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	struct Case {
		string sql;
		LogicalType type;
		Value value;
	};
	vector<Case> cases {{"SELECT * FROM keyword_scan(opt := 42)", LogicalType::INTEGER, Value::INTEGER(42)},
	                    {"SELECT * FROM keyword_scan('a', opt := 1.5)", LogicalType::DOUBLE, Value::DOUBLE(1.5)},
	                    {"SELECT * FROM keyword_scan('a', opt := DATE '2024-01-02')", LogicalType::DATE,
	                     Value::DATE(date_t(Date::FromDate(2024, 1, 2)))}};
	for (auto &test_case : cases) {
		CAPTURE(test_case.sql);
		Parser parser(context);
		parser.ParseQuery(test_case.sql);
		Planner planner(context);
		planner.CreatePlan(std::move(parser.statements[0]));
		auto &get = FindGet(*planner.plan);
		REQUIRE(get.returned_types == vector<LogicalType> {test_case.type});

		MemoryStream stream(Allocator::Get(context));
		BinarySerializer::Serialize(get, stream, SerializationOptions());
		stream.Rewind();
		bound_parameter_map_t parameters;
		auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
		auto &copied_get = copy->Cast<LogicalGet>();
		REQUIRE(copied_get.function.GetDefinition() == get.function.GetDefinition());
		REQUIRE(copied_get.function.GetArguments() == get.function.GetArguments());
		REQUIRE(copied_get.function.GetNamedArguments() == get.function.GetNamedArguments());
		REQUIRE(copied_get.returned_types == vector<LogicalType> {test_case.type});
		REQUIRE(copied_get.bind_data->Cast<KeywordScanBindData>().value == test_case.value);
	}

	// a plan for an older version records only the positional arguments, which cannot select the first overload
	Parser parser(context);
	parser.ParseQuery(cases[0].sql);
	Planner planner(context);
	planner.CreatePlan(std::move(parser.statements[0]));
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::FromString("v1.5.0");
	MemoryStream stream(Allocator::Get(context));
	REQUIRE_THROWS_WITH(BinarySerializer::Serialize(FindGet(*planner.plan), stream, options),
	                    Catch::Matchers::Contains("required keyword-only parameter"));
	connection.Rollback();
}

TEST_CASE("Table function options survive plans written for older versions", "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	bool serialize_bind_data = false;
	SECTION("Rebound on deserialization") {
	}
	SECTION("Bind data serialized") {
		serialize_bind_data = true;
	}
	TableFunctionSet set("keyword_scan");
	set.AddFunction(KeywordScan(
	    FunctionSignature()
	        .AddParameter(LogicalType::VARCHAR)
	        .WithTypedKwargs("options", [](TypedKwargs &options) { options.Add("opt", LogicalType::DOUBLE); }),
	    serialize_bind_data));
	CreateTableFunctionInfo info(set);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	Parser parser(context);
	parser.ParseQuery("SELECT * FROM keyword_scan('a', opt := 1.5)");
	Planner planner(context);
	planner.CreatePlan(std::move(parser.statements[0]));
	auto &get = FindGet(*planner.plan);

	for (const auto &version : {"v1.3.0", "v1.4.0", "v1.5.0"}) {
		CAPTURE(version);
		SerializationOptions options;
		options.storage_compatibility = StorageCompatibility::FromString(version);
		MemoryStream stream(Allocator::Get(context));
		BinarySerializer::Serialize(get, stream, options);
		stream.Rewind();
		bound_parameter_map_t parameters;
		auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
		auto &copied_get = copy->Cast<LogicalGet>();
		REQUIRE(copied_get.function.GetDefinition() == get.function.GetDefinition());
		if (!serialize_bind_data) {
			// older versions write the options only when the bind is repeated on deserialization
			REQUIRE(copied_get.named_parameters == get.named_parameters);
		}
		REQUIRE(copied_get.returned_types == vector<LogicalType> {LogicalType::DOUBLE});
		REQUIRE(copied_get.bind_data->Cast<KeywordScanBindData>().value == Value::DOUBLE(1.5));
	}
	connection.Rollback();
}

TEST_CASE("Plans written for older versions select overloads with required keyword-only parameters",
          "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	// shaped like repeat_row, whose "num_rows" an older version recorded only as a named parameter - also declared as
	// ANY, a type that has no value of its own
	vector<TableFunction> functions;
	for (auto &type : vector<LogicalType> {LogicalType::BIGINT, LogicalType::ANY}) {
		TableFunction function(Identifier("legacy_required_keyword_" + StringUtil::Lower(type.ToString())),
		                       FunctionSignature().AddArgs("args", LogicalType::ANY).AddKeywordOnly("num_rows", type),
		                       nullptr);
		function.bind = [](ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
		                   vector<Identifier> &names) -> unique_ptr<FunctionData> {
			auto entry = input.named_parameters.find("num_rows");
			if (entry == input.named_parameters.end()) {
				throw BinderException("num_rows is required");
			}
			types.push_back(LogicalType::BIGINT);
			names.emplace_back("num_rows");
			return make_uniq<KeywordScanBindData>(entry->second);
		};
		CreateTableFunctionInfo info(function);
		Catalog::GetSystemCatalog(context).CreateFunction(context, info);
		functions.push_back(std::move(function));
	}

	bool pass_num_rows = true;
	SECTION("Passed") {
	}
	SECTION("Left out") {
		pass_num_rows = false;
	}
	for (auto &function : functions) {
		CAPTURE(function.GetName().GetIdentifierName());
		LogicalGet get(TableIndex(0), BoundTableFunction(function), make_uniq<KeywordScanBindData>(Value::BIGINT(3)),
		               {LogicalType::BIGINT}, {Identifier("num_rows")});
		get.parameters = {Value::INTEGER(1)};
		if (pass_num_rows) {
			get.named_parameters["num_rows"] = Value::BIGINT(3);
		}

		for (const auto &version : {"v1.3.0", "v1.4.0", "v1.5.0"}) {
			CAPTURE(version);
			SerializationOptions options;
			options.storage_compatibility = StorageCompatibility::FromString(version);
			MemoryStream stream(Allocator::Get(context));
			BinarySerializer::Serialize(get, stream, options);
			stream.Rewind();
			bound_parameter_map_t parameters;
			if (!pass_num_rows) {
				// the bind still rejects the call, as it did when the plan was written
				REQUIRE_THROWS_WITH(BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters),
				                    Catch::Matchers::Contains("num_rows is required"));
				continue;
			}
			auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
			auto &copied_get = copy->Cast<LogicalGet>();
			REQUIRE(copied_get.parameters == get.parameters);
			REQUIRE(copied_get.named_parameters == get.named_parameters);
			REQUIRE(copied_get.bind_data->Cast<KeywordScanBindData>().value == Value::BIGINT(3));
		}
	}
	connection.Rollback();
}

TEST_CASE("Table function defaults survive serialization", "[serialization][function_invocation]") {
	DuckDB db(nullptr);
	Connection connection(db);
	connection.BeginTransaction();
	auto &context = *connection.context;
	TableFunctionSet set("keyword_scan");
	set.AddFunction(KeywordScan(FunctionSignature()
	                                .AddParameter(LogicalType::VARCHAR)
	                                .AddKeywordOnly("opt", LogicalType::DOUBLE, Value::DOUBLE(7)),
	                            false));
	CreateTableFunctionInfo info(set);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	struct Case {
		string sql;
		Value value;
		//! Whether to drop the option from the plan before writing it, as a version that did not place defaults did
		bool drop_option;
	};
	vector<Case> cases {{"SELECT * FROM keyword_scan('a')", Value::DOUBLE(7), false},
	                    {"SELECT * FROM keyword_scan('a', opt := NULL)", Value(LogicalType::DOUBLE), false},
	                    {"SELECT * FROM keyword_scan('a', opt := 1)", Value::DOUBLE(1), false},
	                    {"SELECT * FROM keyword_scan('a')", Value::DOUBLE(7), true}};
	for (auto &test_case : cases) {
		CAPTURE(test_case.sql);
		CAPTURE(test_case.drop_option);
		Parser parser(context);
		parser.ParseQuery(test_case.sql);
		Planner planner(context);
		planner.CreatePlan(std::move(parser.statements[0]));
		auto &get = FindGet(*planner.plan);
		REQUIRE(Value::NotDistinctFrom(get.named_parameters.at("opt"), test_case.value));
		if (test_case.drop_option) {
			get.named_parameters.clear();
		}

		MemoryStream stream(Allocator::Get(context));
		BinarySerializer::Serialize(get, stream, SerializationOptions());
		stream.Rewind();
		bound_parameter_map_t parameters;
		auto copy = BinaryDeserializer::Deserialize<LogicalOperator>(stream, context, parameters);
		auto &copied_get = copy->Cast<LogicalGet>();
		REQUIRE(Value::NotDistinctFrom(copied_get.named_parameters.at("opt"), test_case.value));
		REQUIRE(Value::NotDistinctFrom(copied_get.bind_data->Cast<KeywordScanBindData>().value, test_case.value));
	}
	connection.Rollback();
}
