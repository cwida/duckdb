#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/planner/logical_plan_sql_exporter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "logical_plan_sql_export_test_helpers.hpp"

using namespace duckdb;

namespace logical_plan_sql_export_test {

// The sensitive flag only exists at C++ registration time and SQL cannot register a table function,
// a scalar function, an aggregate or a copy format, so every refusal case needs a C++ probe.
// The SQL-reachable protection coverage (ordinary parameterized scans keep exporting, e.g. read_csv
// with named options) lives in test/sql/sql_export/source_invocation_results.test

static unique_ptr<FunctionData> SensitiveProbeBind(ClientContext &, TableFunctionBindInput &,
                                                   vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.push_back(LogicalType::BIGINT);
	names.emplace_back("i");
	return make_uniq<TableFunctionData>();
}

static void SensitiveProbeScan(ClientContext &, TableFunctionInput &, DataChunk &output) {
	output.SetChildCardinality(0);
}

static unique_ptr<FunctionData> SensitiveCopyBind(ClientContext &, CopyFunctionBindInput &, const vector<Identifier> &,
                                                  const vector<LogicalType> &) {
	return make_uniq<TableFunctionData>();
}

static void SensitiveCopyOptions(ClientContext &, CopyOptionsInput &input) {
	input.options[Identifier("plain")] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	CopyOption key_material(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	key_material.sensitive = true;
	input.options[Identifier("key_material")] = key_material;
	// a sensitive option that is valid without a value
	CopyOption key_flag(LogicalType::BOOLEAN, CopyOptionMode::WRITE_ONLY);
	key_flag.sensitive = true;
	input.options[Identifier("key_flag")] = key_flag;
	// a sensitive option whose name generic COPY binding consumes into resolved sink state
	CopyOption batch_size(LogicalType::UBIGINT, CopyOptionMode::WRITE_ONLY);
	batch_size.sensitive = true;
	input.options[Identifier("batch_size")] = batch_size;
}

static void SensitiveScalarImpl(DataChunk &, ExpressionState &, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
}

static unique_ptr<ParsedExpression> SensitiveScalarUnbind(FunctionUnbindInput &input) {
	return make_uniq<FunctionExpression>(input.expression.Function().GetQualifiedName(), std::move(input.children));
}

static OperatorResultType SensitiveProbeInOut(ExecutionContext &, TableFunctionInput &, DataChunk &, DataChunk &) {
	return OperatorResultType::FINISHED;
}

static void RegisterSensitiveProbes(DuckDB &db) {
	ExtensionLoader loader(*db.instance, "sensitive_export_probe");
	// a scan with a sensitive keyword-only parameter next to benign parameters
	auto signature = FunctionSignature()
	                     .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	                     .AddKeywordOnly(Identifier("api_key"), LogicalType::VARCHAR, Value(""))
	                     .Sensitive()
	                     .AddKeywordOnly(Identifier("region"), LogicalType::VARCHAR, Value("local"));
	TableFunction scan(Identifier("sensitive_probe_scan"), signature, SensitiveProbeScan, SensitiveProbeBind);
	loader.RegisterFunction(std::move(scan));
	// a scan whose typed "**kwargs" schema marks one option sensitive
	auto kwargs_schema = TypedKwargs()
	                         .Add(Identifier("plain"), LogicalType::VARCHAR)
	                         .Add(Identifier("secret_token"), LogicalType::VARCHAR)
	                         .Alias(Identifier("token"))
	                         .Sensitive();
	auto kwargs_signature = FunctionSignature()
	                            .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	                            .AddTypedKwargs(Identifier("options"), std::move(kwargs_schema));
	TableFunction kwargs_scan(Identifier("sensitive_probe_options_scan"), kwargs_signature, SensitiveProbeScan,
	                          SensitiveProbeBind);
	loader.RegisterFunction(std::move(kwargs_scan));
	// a scan with a sensitive positional parameter
	auto positional_signature = FunctionSignature()
	                                .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	                                .AddParameter(Identifier("credential"), LogicalType::VARCHAR)
	                                .Sensitive();
	TableFunction positional_scan(Identifier("sensitive_probe_positional_scan"), positional_signature,
	                              SensitiveProbeScan, SensitiveProbeBind);
	loader.RegisterFunction(std::move(positional_scan));
	// a scan whose sensitive parameter accepts any type next to a typed default, where a lossy comparison
	// against the default must not count as the default
	auto any_signature = FunctionSignature()
	                         .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	                         .AddKeywordOnly(Identifier("level"), LogicalType::ANY, Value::DOUBLE(9007199254740992.0))
	                         .Sensitive();
	TableFunction any_scan(Identifier("sensitive_probe_any_scan"), any_signature, SensitiveProbeScan,
	                       SensitiveProbeBind);
	loader.RegisterFunction(std::move(any_scan));
	// an in-out scan whose sensitive argument is computed by the exported input subquery
	auto inout_signature = FunctionSignature()
	                           .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	                           .AddParameter(Identifier("credential"), LogicalType::VARCHAR)
	                           .Sensitive();
	TableFunction inout_scan(Identifier("sensitive_probe_inout_scan"), inout_signature, nullptr, SensitiveProbeBind);
	inout_scan.in_out_function = SensitiveProbeInOut;
	loader.RegisterFunction(std::move(inout_scan));
	// a scan whose sensitive argument is the TABLE parameter itself, reconstructed as the input subquery
	auto table_signature = FunctionSignature().AddParameter(Identifier("input"), LogicalType::TABLE).Sensitive();
	TableFunction table_scan(Identifier("sensitive_probe_table_input"), table_signature, nullptr, SensitiveProbeBind);
	table_scan.in_out_function = SensitiveProbeInOut;
	loader.RegisterFunction(std::move(table_scan));
	// a scan whose VARIANT default hides the payload types from a coercing comparison
	auto variant_signature =
	    FunctionSignature()
	        .AddParameter(Identifier("endpoint"), LogicalType::VARCHAR)
	        .AddKeywordOnly(Identifier("level"), LogicalType::VARIANT(),
	                        Value::DOUBLE(9007199254740992.0).DefaultCastAs(LogicalType::VARIANT()))
	        .Sensitive();
	TableFunction variant_scan(Identifier("sensitive_probe_variant_scan"), variant_signature, SensitiveProbeScan,
	                           SensitiveProbeBind);
	loader.RegisterFunction(std::move(variant_scan));
	// a sensitive parameter with an empty declared name must still refuse
	auto unnamed_signature = FunctionSignature().AddParameter(Identifier(), LogicalType::VARCHAR).Sensitive();
	TableFunction unnamed_scan(Identifier("sensitive_probe_unnamed_scan"), unnamed_signature, SensitiveProbeScan,
	                           SensitiveProbeBind);
	loader.RegisterFunction(std::move(unnamed_scan));
	// a volatile scalar function with a sensitive parameter, volatility retains the invocation
	auto scalar_signature =
	    FunctionSignature().AddParameter(Identifier("credential"), LogicalType::VARCHAR).Sensitive();
	scalar_signature.SetReturnType(LogicalType::BIGINT);
	ScalarFunction sensitive_scalar(Identifier("sensitive_probe_scalar"), std::move(scalar_signature),
	                                SensitiveScalarImpl);
	sensitive_scalar.SetVolatile();
	loader.RegisterFunction(std::move(sensitive_scalar));
	// the same shape with an unbind callback, whose reconstruction freedom forces a blanket refusal
	auto unbind_signature =
	    FunctionSignature().AddParameter(Identifier("credential"), LogicalType::VARCHAR).Sensitive();
	unbind_signature.SetReturnType(LogicalType::BIGINT);
	ScalarFunction unbind_scalar(Identifier("sensitive_probe_unbind_scalar"), std::move(unbind_signature),
	                             SensitiveScalarImpl);
	unbind_scalar.SetVolatile();
	unbind_scalar.SetUnbindCallback(SensitiveScalarUnbind);
	loader.RegisterFunction(std::move(unbind_scalar));
	// a volatile scalar whose sensitive parameter has a default of a narrower type, so an omitted
	// argument is materialized as a cast of the declared default
	auto default_signature =
	    FunctionSignature().AddParameter(Identifier("credential"), LogicalType::BIGINT, Value::INTEGER(7)).Sensitive();
	default_signature.SetReturnType(LogicalType::BIGINT);
	ScalarFunction default_scalar(Identifier("sensitive_probe_default"), std::move(default_signature),
	                              SensitiveScalarImpl);
	default_scalar.SetVolatile();
	loader.RegisterFunction(std::move(default_scalar));
	// an aggregate with a sensitive parameter, implemented as a renamed copy of the built-in "first"
	{
		Connection lookup(db);
		lookup.BeginTransaction();
		auto &first_entry =
		    Catalog::GetEntry<AggregateFunctionCatalogEntry>(*lookup.context, QualifiedName("system", "main", "first"));
		auto first_implementation =
		    first_entry.functions.GetFunctionByArguments(*lookup.context, {LogicalType::VARCHAR});
		lookup.Rollback();
		REQUIRE(first_implementation);
		AggregateFunction sensitive_agg = *first_implementation;
		sensitive_agg.SetName(Identifier("sensitive_probe_agg"));
		auto agg_signature =
		    FunctionSignature().AddParameter(Identifier("credential"), LogicalType::VARCHAR).Sensitive();
		agg_signature.SetReturnType(sensitive_agg.GetReturnType());
		sensitive_agg.GetSignature() = std::move(agg_signature);
		loader.RegisterFunction(std::move(sensitive_agg));
	}
	// renamed copies of string_agg whose separator is sensitive, their exported states retain the separator
	{
		Connection lookup(db);
		lookup.BeginTransaction();
		auto &string_agg_entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(
		    *lookup.context, QualifiedName("system", "main", "string_agg"));
		auto string_agg_implementation = string_agg_entry.functions.GetFunctionByArguments(
		    *lookup.context, {LogicalType::VARCHAR, LogicalType::VARCHAR});
		lookup.Rollback();
		REQUIRE(string_agg_implementation);
		// the positional separator
		AggregateFunction sensitive_string_agg = *string_agg_implementation;
		sensitive_string_agg.SetName(Identifier("sensitive_string_agg"));
		sensitive_string_agg.GetSignature().GetParameter(0).SetName(Identifier("value"));
		sensitive_string_agg.GetSignature().GetParameter(1).SetName(Identifier("separator"));
		sensitive_string_agg.GetSignature().GetParameter(1).SetSensitive();
		loader.RegisterFunction(std::move(sensitive_string_agg));
		// the separator as a keyword-only parameter, recorded behind the positional slots
		AggregateFunction keyword_agg = *string_agg_implementation;
		keyword_agg.SetName(Identifier("sensitive_keyword_agg"));
		auto keyword_signature = FunctionSignature()
		                             .AddParameter(Identifier("value"), LogicalType::VARCHAR)
		                             .AddKeywordOnly(Identifier("separator"), LogicalType::VARCHAR)
		                             .Sensitive();
		keyword_signature.SetReturnType(keyword_agg.GetReturnType());
		keyword_agg.GetSignature() = std::move(keyword_signature);
		loader.RegisterFunction(std::move(keyword_agg));
		// the separator with a declared default, whose materialized value is registration metadata
		AggregateFunction default_agg = *string_agg_implementation;
		default_agg.SetName(Identifier("sensitive_default_agg"));
		auto default_agg_signature = FunctionSignature()
		                                 .AddParameter(Identifier("value"), LogicalType::VARCHAR)
		                                 .AddParameter(Identifier("separator"), LogicalType::VARCHAR, Value(","))
		                                 .Sensitive();
		default_agg_signature.SetReturnType(default_agg.GetReturnType());
		default_agg.GetSignature() = std::move(default_agg_signature);
		loader.RegisterFunction(std::move(default_agg));
	}
	// a renamed copy of list_transform whose lambda parameter is sensitive
	{
		Connection lookup(db);
		lookup.BeginTransaction();
		auto &transform_entry = Catalog::GetEntry<ScalarFunctionCatalogEntry>(
		    *lookup.context, QualifiedName("system", "main", "list_transform"));
		REQUIRE(transform_entry.functions.Size() >= 1);
		ScalarFunction sensitive_transform = *transform_entry.functions.GetFunctionByOffset(0);
		lookup.Rollback();
		sensitive_transform.SetName(Identifier("sensitive_transform"));
		sensitive_transform.SetVolatile();
		REQUIRE(sensitive_transform.GetSignature().GetParameterCount() == 2);
		sensitive_transform.GetSignature().GetParameter(1).SetName(Identifier("transformation"));
		sensitive_transform.GetSignature().GetParameter(1).SetSensitive();
		loader.RegisterFunction(std::move(sensitive_transform));
	}
	// a copy format with a sensitive option next to a benign option
	CopyFunction copy_probe(Identifier("sensitive_copy_probe"));
	copy_probe.copy_to_bind = SensitiveCopyBind;
	copy_probe.copy_options = SensitiveCopyOptions;
	copy_probe.extension = "sensitive_copy_probe";
	loader.RegisterFunction(std::move(copy_probe));
}

static void RequireSensitiveParameterIssue(const PlanExportResult &result, const string &function_name,
                                           const string &parameter_name) {
	RequirePlanExportIssue(result, LogicalPlanVerificationIssueCode::UNSUPPORTED_EXPORT_FEATURE);
	auto &issue = result.GetIssues()[0];
	REQUIRE(*issue.construct == LogicalPlanVerificationConstructIdentity::ExportFeature("sensitive_parameter"));
	REQUIRE(issue.facts.size() == 2);
	REQUIRE((issue.facts[0] == pair<string, Value> {"function", Value(function_name)}));
	REQUIRE((issue.facts[1] == pair<string, Value> {"parameter", Value(parameter_name)}));
}

TEST_CASE("Sensitive table function parameters refuse SQL export",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);
	connection.BeginTransaction();

	SECTION("sensitive keyword-only parameter refuses with function and parameter facts") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_scan('http://endpoint', api_key := 'credential-value')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_scan", "api_key");
		REQUIRE(!StringUtil::Contains(exported.GetIssues()[0].message, "credential-value"));
	}
	SECTION("the same scan without the sensitive argument exports") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_scan('http://endpoint', region := 'eu')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		REQUIRE(exported.IsSuccess());
		auto text = exported.GetValue().query->ToString();
		REQUIRE(StringUtil::Contains(text, "sensitive_probe_scan"));
		REQUIRE(StringUtil::Contains(text, "region"));
		// the materialized default of the sensitive parameter is registration metadata and renders
		REQUIRE(StringUtil::Contains(text, "api_key"));
	}
	SECTION("an explicit value equal to the declared default renders") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_scan('http://endpoint', api_key := '')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		REQUIRE(exported.IsSuccess());
		REQUIRE(StringUtil::Contains(exported.GetValue().query->ToString(), "api_key"));
	}
	SECTION("a value that only compares equal to the default after a lossy cast refuses") {
		// BIGINT 9007199254740993 converts to the declared DOUBLE default under a coercing comparison
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_any_scan('http://endpoint', level := 9007199254740993)");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_any_scan", "level");
	}
	SECTION("a value of the default's own type equal to the default renders") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection,
		    "SELECT * FROM sensitive_probe_any_scan('http://endpoint', level := CAST(9007199254740992 AS DOUBLE))");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		REQUIRE(exported.IsSuccess());
	}
	SECTION("a sensitive argument computed by the exported input subquery refuses") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_inout_scan('http://endpoint', (SELECT 'swordfish'))");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_inout_scan", "credential");
	}
	SECTION("a sensitive TABLE parameter refuses the reconstructed input subquery") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_table_input((SELECT 'swordfish' AS credential))");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_table_input", "input");
	}
	SECTION("identical outer types do not exempt a lossy VARIANT payload comparison") {
		auto plan =
		    OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM sensitive_probe_variant_scan('http://endpoint', "
		                                               "level := CAST(9007199254740993 AS VARIANT))");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_variant_scan", "level");
	}
	SECTION("a sensitive parameter with an empty declared name still refuses") {
		auto plan =
		    OptimizeLogicalPlanExportQuery(connection, "SELECT * FROM sensitive_probe_unnamed_scan('swordfish')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_unnamed_scan", "");
	}
	SECTION("a sensitive typed kwargs option refuses under its canonical name") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_options_scan('http://endpoint', token := 'credential-value')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_options_scan", "secret_token");
	}
	SECTION("the kwargs scan without the sensitive option exports") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_options_scan('http://endpoint', plain := 'visible')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		REQUIRE(exported.IsSuccess());
		auto text = exported.GetValue().query->ToString();
		REQUIRE(StringUtil::Contains(text, "sensitive_probe_options_scan"));
		REQUIRE(StringUtil::Contains(text, "visible"));
	}
	SECTION("a sensitive positional parameter refuses") {
		auto plan = OptimizeLogicalPlanExportQuery(
		    connection, "SELECT * FROM sensitive_probe_positional_scan('http://endpoint', 'credential-value')");
		auto get = FindLogicalPlanExportOperator(*plan, LogicalOperatorType::LOGICAL_GET);
		REQUIRE(get);
		auto exported = LogicalPlanSQLExporter::Export(*connection.context, *get);
		RequireSensitiveParameterIssue(exported, "sensitive_probe_positional_scan", "credential");
	}
	connection.Rollback();
}

TEST_CASE("Sensitive parameter refusal surfaces through EXPLAIN (SQL)",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);

	auto refused = connection.Query(
	    "EXPLAIN (SQL) SELECT * FROM sensitive_probe_scan('http://endpoint', api_key := 'credential-value')");
	REQUIRE(refused->HasError());
	REQUIRE(StringUtil::Contains(refused->GetError(), "sensitive_probe_scan"));
	REQUIRE(StringUtil::Contains(refused->GetError(), "api_key"));
	REQUIRE(!StringUtil::Contains(refused->GetError(), "credential-value"));

	auto allowed =
	    connection.Query("EXPLAIN (SQL) SELECT * FROM sensitive_probe_scan('http://endpoint', region := 'eu')");
	REQUIRE_NO_FAIL(*allowed);
	auto sql = allowed->Collection().GetValue(1, 0).ToString();
	REQUIRE(StringUtil::Contains(sql, "sensitive_probe_scan"));
}

TEST_CASE("Sensitive scalar, aggregate and window parameters refuse SQL export",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);

	auto require_refused = [&](const string &query, const string &function_name) {
		auto refused = connection.Query(query);
		REQUIRE(refused->HasError());
		REQUIRE(StringUtil::Contains(refused->GetError(), function_name));
		REQUIRE(StringUtil::Contains(refused->GetError(), "credential"));
		REQUIRE(!StringUtil::Contains(refused->GetError(), "swordfish"));
	};
	require_refused("EXPLAIN (SQL) SELECT sensitive_probe_scalar('swordfish')", "sensitive_probe_scalar");
	require_refused("EXPLAIN (SQL) SELECT sensitive_probe_agg('swordfish') FROM range(2)", "sensitive_probe_agg");
	require_refused("EXPLAIN (SQL) SELECT sensitive_probe_agg('swordfish') OVER () FROM range(2)",
	                "sensitive_probe_agg");

	// a non-constant argument also refuses - the exporter cannot prove the generated text excludes the value
	REQUIRE_NO_FAIL(connection.Query("CREATE TABLE sensitive_scalar_inputs AS SELECT 'swordfish' AS s"));
	require_refused("EXPLAIN (SQL) SELECT sensitive_probe_scalar(s) FROM sensitive_scalar_inputs",
	                "sensitive_probe_scalar");

	// an unbind callback reconstructs with bind-time freedom, so a sensitive declaration refuses outright
	require_refused("EXPLAIN (SQL) SELECT sensitive_probe_unbind_scalar(s) FROM sensitive_scalar_inputs",
	                "sensitive_probe_unbind_scalar");

	// the lambda slot is a declared argument, its reconstructed body renders
	auto lambda_refused = connection.Query("EXPLAIN (SQL) SELECT sensitive_transform([1], lambda x: 'swordfish')");
	REQUIRE(lambda_refused->HasError());
	REQUIRE(StringUtil::Contains(lambda_refused->GetError(), "sensitive_transform"));
	REQUIRE(!StringUtil::Contains(lambda_refused->GetError(), "swordfish"));

	// the unmarked built-in the aggregate probe copies keeps exporting
	auto allowed = connection.Query("EXPLAIN (SQL) SELECT first('swordfish') FROM range(2)");
	REQUIRE_NO_FAIL(*allowed);
	REQUIRE(StringUtil::Contains(allowed->Collection().GetValue(1, 0).ToString(), "first"));
}

TEST_CASE("Sensitive parameter defaults materialized by the binder keep exporting",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);

	// without the optimizer the materialized default survives as a cast of the declared constant
	REQUIRE_NO_FAIL(connection.Query("PRAGMA disable_optimizer"));
	auto defaulted = connection.Query("EXPLAIN (SQL) SELECT sensitive_probe_default()");
	REQUIRE_NO_FAIL(*defaulted);
	REQUIRE(StringUtil::Contains(defaulted->Collection().GetValue(1, 0).ToString(), "sensitive_probe_default"));

	auto explicit_default = connection.Query("EXPLAIN (SQL) SELECT sensitive_probe_default(7)");
	REQUIRE_NO_FAIL(*explicit_default);

	auto refused = connection.Query("EXPLAIN (SQL) SELECT sensitive_probe_default(8)");
	REQUIRE(refused->HasError());
	REQUIRE(StringUtil::Contains(refused->GetError(), "credential"));

	// a single explicit cast of the default to the declared type renders nothing but registration metadata
	auto cast_default = connection.Query("EXPLAIN (SQL) SELECT sensitive_probe_default(CAST(7 AS BIGINT))");
	REQUIRE_NO_FAIL(*cast_default);

	// deeper cast chains render their own material and may change the value, so they refuse
	auto chained = connection.Query("EXPLAIN (SQL) SELECT sensitive_probe_default(CAST(CAST(7 AS BOOLEAN) AS BIGINT))");
	REQUIRE(chained->HasError());
	REQUIRE(StringUtil::Contains(chained->GetError(), "credential"));
}

TEST_CASE("Exported aggregate state refuses rendering a sensitive constant parameter",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);

	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE saved_state = (SELECT sensitive_string_agg(v, 'swordfish') "
	                                 "EXPORT_STATE FROM (VALUES ('x')) t(v))"));
	auto require_refused = [&](const string &query) {
		auto refused = connection.Query(query);
		REQUIRE(refused->HasError());
		REQUIRE(StringUtil::Contains(refused->GetError(), "separator"));
		REQUIRE(!StringUtil::Contains(refused->GetError(), "swordfish"));
	};
	require_refused("EXPLAIN (SQL) SELECT getvariable('saved_state')");
	// empty-result propagation preserves the state type and its typed NULL still renders the parameters
	require_refused("EXPLAIN (SQL) SELECT getvariable('saved_state') WHERE false");
	// a TYPE value renders its payload type
	require_refused("EXPLAIN (SQL) SELECT get_type(getvariable('saved_state'))");
	// a cast of a non-constant to the state type renders the parameters through the target type
	require_refused("EXPLAIN (SQL) SELECT cast_to_type(v, getvariable('saved_state')) FROM (VALUES ('x')) t(v)");
	// a type nested in a container renders through the constructed type witness
	require_refused("EXPLAIN (SQL) SELECT cast_to_type(v, [getvariable('saved_state')]) FROM (VALUES (['x'])) t(v)");
	// an outer state records the inner state type in its signature, which renders the inner parameters
	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE outer_state = (SELECT min(getvariable('saved_state')) "
	                                 "EXPORT_STATE FROM range(2))"));
	require_refused("EXPLAIN (SQL) SELECT getvariable('outer_state')");

	// recorded indexes follow the bound layout, keyword-only arguments sit behind the positional slots
	REQUIRE_NO_FAIL(
	    connection.Query("SET VARIABLE keyword_state = (SELECT sensitive_keyword_agg(v, separator := 'swordfish') "
	                     "EXPORT_STATE FROM (VALUES ('x')) t(v))"));
	require_refused("EXPLAIN (SQL) SELECT getvariable('keyword_state')");

	// a recorded constant equal to the declared default is registration metadata and keeps exporting
	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE default_state = (SELECT sensitive_default_agg(v) "
	                                 "EXPORT_STATE FROM (VALUES ('x')) t(v))"));
	auto default_state = connection.Query("EXPLAIN (SQL) SELECT getvariable('default_state')");
	REQUIRE_NO_FAIL(*default_state);
	REQUIRE(StringUtil::Contains(default_state->Collection().GetValue(1, 0).ToString(), "to_aggregate_state"));

	// the unmarked built-in state keeps exporting its constant parameters
	REQUIRE_NO_FAIL(connection.Query("SET VARIABLE benign_state = (SELECT string_agg(v, ',') "
	                                 "EXPORT_STATE FROM (VALUES ('x')) t(v))"));
	auto allowed = connection.Query("EXPLAIN (SQL) SELECT getvariable('benign_state')");
	REQUIRE_NO_FAIL(*allowed);
	REQUIRE(StringUtil::Contains(allowed->Collection().GetValue(1, 0).ToString(), "to_aggregate_state"));
}

TEST_CASE("Sensitive copy options refuse COPY TO export",
          "[sql_export][logical_plan_sql_export][sensitive_parameters]") {
	DuckDB db(nullptr);
	Connection connection(db);
	RegisterSensitiveProbes(db);
	auto out_path = TestCreatePath("sensitive_copy_probe_out");

	auto refused = connection.Query("EXPLAIN (SQL) COPY (SELECT 42 AS i) TO '" + out_path +
	                                "' (FORMAT 'sensitive_copy_probe', key_material 'credential-value')");
	REQUIRE(refused->HasError());
	REQUIRE(StringUtil::Contains(refused->GetError(), "sensitive_copy_probe"));
	REQUIRE(StringUtil::Contains(refused->GetError(), "key_material"));
	REQUIRE(!StringUtil::Contains(refused->GetError(), "credential-value"));

	auto allowed = connection.Query("EXPLAIN (SQL) COPY (SELECT 42 AS i) TO '" + out_path +
	                                "' (FORMAT 'sensitive_copy_probe', plain 'visible')");
	REQUIRE_NO_FAIL(*allowed);
	auto sql = allowed->Collection().GetValue(1, 0).ToString();
	REQUIRE(StringUtil::Contains(sql, "sensitive_copy_probe"));
	REQUIRE(StringUtil::Contains(sql, "plain"));
	REQUIRE(StringUtil::Contains(sql, "visible"));

	// a value-less sensitive option renders as a bare name, there is no value to protect
	auto bare_flag = connection.Query("EXPLAIN (SQL) COPY (SELECT 42 AS i) TO '" + out_path +
	                                  "' (FORMAT 'sensitive_copy_probe', key_flag)");
	REQUIRE_NO_FAIL(*bare_flag);
	REQUIRE(StringUtil::Contains(bare_flag->Collection().GetValue(1, 0).ToString(), "key_flag"));

	auto flag_value = connection.Query("EXPLAIN (SQL) COPY (SELECT 42 AS i) TO '" + out_path +
	                                   "' (FORMAT 'sensitive_copy_probe', key_flag true)");
	REQUIRE(flag_value->HasError());
	REQUIRE(StringUtil::Contains(flag_value->GetError(), "key_flag"));

	// generic binding consumes batch_size into resolved sink state, the sensitive registration must still refuse
	auto consumed = connection.Query("EXPLAIN (SQL) COPY (SELECT 42 AS i) TO '" + out_path +
	                                 "' (FORMAT 'sensitive_copy_probe', batch_size 123456)");
	REQUIRE(consumed->HasError());
	REQUIRE(StringUtil::Contains(consumed->GetError(), "batch_size"));
}

} // namespace logical_plan_sql_export_test
