#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/function/function_set.hpp"

using namespace duckdb;

static void CountArgumentsExec(DataChunk &args, ExpressionState &state, Vector &result) {
	result.Reference(Value::BIGINT(NumericCast<int64_t>(args.ColumnCount())), count_t(args.size()));
}

TEST_CASE("Keyword-only parameters follow *args", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::INTEGER);
	sig.AddArgs("args", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER);
	sig.AddKwargs("kwargs", LogicalType::ANY);
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());

	REQUIRE(sig.GetParameter(0).GetKind() == FunctionParameterKind::STANDARD);
	REQUIRE(sig.GetParameter(2).GetKind() == FunctionParameterKind::KEYWORD_ONLY);
	REQUIRE(sig.GetPositionalParameterCount() == 1);
	REQUIRE(sig.GetRequiredParameterCount() == 2);
	// the variadic parameters cannot be looked up by name
	REQUIRE(!sig.GetParameterIndexByName("args").IsValid());
	REQUIRE(sig.GetParameterIndexByName("kw").GetIndex() == 2);
	REQUIRE(sig.ToString() == "(a INTEGER, *args INTEGER, kw INTEGER, **kwargs ANY) -> BIGINT");
}

TEST_CASE("A keyword-only parameter closes the positional parameters", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());

	REQUIRE(sig.GetParameter(0).GetKind() == FunctionParameterKind::STANDARD);
	REQUIRE(sig.GetParameter(1).GetKind() == FunctionParameterKind::KEYWORD_ONLY);
	// no "*args" pack is declared, so the closure occupies no parameter slot of its own
	REQUIRE(sig.GetParameterCount() == 2);
	REQUIRE(!sig.GetArgs());
	REQUIRE(sig.GetArgs() == nullptr);
	REQUIRE(sig.GetPositionalParameterCount() == 1);
	REQUIRE(sig.GetRequiredParameterCount() == 1);
	REQUIRE(sig.GetParameterIndexByName("kw").GetIndex() == 1);
	// it still reads as the bare "*" Python spells it with
	REQUIRE(sig.ToString() == "(a INTEGER, *, kw INTEGER := NULL) -> BIGINT");
}

TEST_CASE("A *args pack takes the place of the bare * separator", "[api][scalar_function]") {
	// the keyword-only parameter is closed off by the pack, so no "*" is spelled out on top of it
	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::INTEGER);
	sig.AddArgs("args", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());
	REQUIRE(sig.GetArgs());
	REQUIRE(sig.ToString() == "(a INTEGER, *args INTEGER, kw INTEGER := NULL) -> BIGINT");
}

TEST_CASE("Keyword-only parameters still accept a **kwargs after them", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	sig.AddKwargs("kwargs", LogicalType::ANY);
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());
	REQUIRE(!sig.GetArgs());
	REQUIRE(sig.GetKwargs()->GetType() == LogicalType::ANY);
	REQUIRE(sig.ToString() == "(a INTEGER, *, kw INTEGER := NULL, **kwargs ANY) -> BIGINT");
}

TEST_CASE("A function with only keyword-only parameters takes no positional argument", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());
	REQUIRE(sig.GetPositionalParameterCount() == 0);
	REQUIRE(sig.ToString() == "(*, kw INTEGER := NULL) -> BIGINT");
}

TEST_CASE("A positional-only parameter cannot be passed by name", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddPositionalOnly("a", LogicalType::INTEGER);
	sig.AddParameter("b", LogicalType::INTEGER);
	sig.AddKwargs("kwargs", LogicalType::ANY);
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());

	REQUIRE(sig.GetParameter(0).GetKind() == FunctionParameterKind::POSITIONAL_ONLY);
	REQUIRE(sig.GetParameter(1).GetKind() == FunctionParameterKind::STANDARD);
	// it takes a position like a standard parameter, but its name is invisible to a caller
	REQUIRE(sig.GetPositionalParameterCount() == 2);
	REQUIRE(sig.GetPositionalOnlyParameterCount() == 1);
	REQUIRE(!sig.GetParameterIndexByName("a").IsValid());
	REQUIRE(sig.GetParameterIndexByName("b").GetIndex() == 1);
	REQUIRE(sig.ToString() == "(a INTEGER, /, b INTEGER, **kwargs ANY) -> BIGINT");
}

TEST_CASE("Positional-only parameters combine with the other kinds", "[api][scalar_function]") {
	FunctionSignature sig;
	sig.AddPositionalOnly("a", LogicalType::INTEGER);
	sig.AddParameter("b", LogicalType::INTEGER);
	sig.AddArgs("args", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER, Value(LogicalType::INTEGER));
	sig.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(sig.Verify());
	REQUIRE(sig.ToString() == "(a INTEGER, /, b INTEGER, *args INTEGER, kw INTEGER := NULL) -> BIGINT");

	// a signature of nothing but positional-only parameters still closes them
	FunctionSignature only;
	only.AddPositionalOnly("a", LogicalType::INTEGER);
	only.AddPositionalOnly("b", LogicalType::INTEGER);
	only.SetReturnType(LogicalType::BIGINT);
	REQUIRE_NOTHROW(only.Verify());
	REQUIRE(only.GetPositionalOnlyParameterCount() == 2);
	REQUIRE(only.ToString() == "(a INTEGER, b INTEGER, /) -> BIGINT");

	// parameters are kept in the order they are added, which must put them ahead of the ones passed by name
	FunctionSignature ordered;
	ordered.AddParameter("b", LogicalType::INTEGER);
	ordered.AddPositionalOnly("a", LogicalType::INTEGER);
	REQUIRE(ordered.GetParameter(0).GetName() == "b");
	REQUIRE_THROWS(ordered.Verify());
}

TEST_CASE("Positional-only parameters must come first", "[api][scalar_function]") {
	using Kind = FunctionParameterKind;
	auto i32 = LogicalType::INTEGER;
	auto verify = [&](vector<FunctionParameter> params) {
		FunctionSignature(std::move(params), LogicalType::BIGINT).Verify();
	};

	REQUIRE_NOTHROW(verify({{"a", i32, {}, Kind::POSITIONAL_ONLY}, {"b", i32}}));
	REQUIRE_THROWS(verify({{"b", i32}, {"a", i32, {}, Kind::POSITIONAL_ONLY}}));
	REQUIRE_THROWS(verify({{"args", i32, {}, Kind::VAR_POSITIONAL}, {"a", i32, {}, Kind::POSITIONAL_ONLY}}));
	REQUIRE_THROWS(verify({{"kw", i32, {}, Kind::KEYWORD_ONLY}, {"a", i32, {}, Kind::POSITIONAL_ONLY}}));
}

TEST_CASE("The order of the parameter kinds is verified", "[api][scalar_function]") {
	using Kind = FunctionParameterKind;
	auto i32 = LogicalType::INTEGER;
	auto verify = [&](vector<FunctionParameter> params) {
		FunctionSignature(std::move(params), LogicalType::BIGINT).Verify();
	};

	REQUIRE_NOTHROW(verify({{"a", i32}, {"args", i32, {}, Kind::VAR_POSITIONAL}, {"kw", i32, {}, Kind::KEYWORD_ONLY}}));
	// only one of each variadic parameter
	REQUIRE_THROWS(verify({{"x", i32, {}, Kind::VAR_POSITIONAL}, {"y", i32, {}, Kind::VAR_POSITIONAL}}));
	REQUIRE_THROWS(verify({{"x", i32, {}, Kind::VAR_KEYWORD}, {"y", i32, {}, Kind::VAR_KEYWORD}}));
	// **kwargs is last
	REQUIRE_THROWS(verify({{"x", i32, {}, Kind::VAR_KEYWORD}, {"kw", i32, {}, Kind::KEYWORD_ONLY}}));
	REQUIRE_THROWS(verify({{"x", i32, {}, Kind::VAR_KEYWORD}, {"y", i32, {}, Kind::VAR_POSITIONAL}}));
	// a standard parameter cannot follow *args or a keyword-only parameter
	REQUIRE_THROWS(verify({{"x", i32, {}, Kind::VAR_POSITIONAL}, {"a", i32}}));
	REQUIRE_THROWS(verify({{"kw", i32, {}, Kind::KEYWORD_ONLY}, {"a", i32}}));
	REQUIRE_THROWS(verify({{"kw", i32, {}, Kind::KEYWORD_ONLY}, {"x", i32, {}, Kind::VAR_POSITIONAL}}));
	// variadic parameters have no default value
	REQUIRE_THROWS(verify({{"x", i32, Value::INTEGER(1), Kind::VAR_POSITIONAL}}));

	// keyword-only parameters can mix required and optional parameters, standard parameters cannot
	FunctionSignature sig;
	sig.AddParameter("a", i32, Value::INTEGER(1));
	sig.AddArgs("args", i32);
	sig.AddKeywordOnly("kw", i32, Value::INTEGER(1));
	sig.AddKeywordOnly("kw2", i32);
	REQUIRE_NOTHROW(sig.Verify());

	// the kinds are verified when the function is registered
	DuckDB db(nullptr);
	Connection con(db);
	ScalarFunction fn("bad_kinds", {{"x", i32, {}, Kind::VAR_POSITIONAL}, {"a", i32}}, LogicalType::BIGINT,
	                  CountArgumentsExec);
	REQUIRE_THROWS(CreateScalarFunctionInfo(fn));
}

TEST_CASE("Argument names cannot be serialized to older storage versions", "[api][scalar_function]") {
	DBConfig config;
	config.SetOptionByName("storage_compatibility_version", Value("v1.0.0"));
	DuckDB db(nullptr, &config);
	Connection con(db);

	FunctionSignature sig;
	sig.AddArgs("args", LogicalType::INTEGER);
	sig.AddKeywordOnly("kw", LogicalType::INTEGER);
	sig.SetReturnType(LogicalType::BIGINT);
	CreateScalarFunctionInfo info(ScalarFunction("count_arguments", std::move(sig), CountArgumentsExec));
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });

	REQUIRE_NO_FAIL(con.Query("SET debug_verify_serializer=true"));
	auto result = con.Query("SELECT count_arguments(i::INTEGER, kw := 1) FROM range(1) t(i)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "cannot be serialized to a storage version older than"));
}

namespace {

//! The arguments the last bind of "defaults_probe" received
struct DefaultsProbe {
	vector<Value> inputs;
	named_argument_map_t named_parameters;
};
DefaultsProbe defaults_probe;

unique_ptr<FunctionData> DefaultsProbeBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                           vector<Identifier> &names) {
	defaults_probe.inputs = input.inputs;
	defaults_probe.named_parameters = input.named_parameters;
	types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("ok");
	return make_uniq<TableFunctionData>();
}

void DefaultsProbeScan(ClientContext &, TableFunctionInput &, DataChunk &output) {
	output.SetChildCardinality(0);
}

} // namespace

TEST_CASE("A table function receives the declared default of every parameter", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::INTEGER, Value::INTEGER(42));
	sig.AddKeywordOnly("k", LogicalType::INTEGER, Value::INTEGER(7));
	sig.WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("o", LogicalType::INTEGER).Add("untyped", LogicalType::ANY);
	});
	TableFunction function("defaults_probe", std::move(sig), DefaultsProbeScan, DefaultsProbeBind);
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	// every default is supplied, and an option the call leaves out is absent
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM defaults_probe()"));
	REQUIRE(defaults_probe.inputs == vector<Value> {Value::INTEGER(42)});
	REQUIRE(defaults_probe.named_parameters.size() == 1);
	REQUIRE(defaults_probe.named_parameters.at("k") == Value::INTEGER(7));

	// passed arguments replace the defaults, and a NULL is passed as a value - also in place of a default that is not
	// NULL
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM defaults_probe(1, k := NULL, o := 2, untyped := 'x')"));
	REQUIRE(defaults_probe.inputs == vector<Value> {Value::INTEGER(1)});
	REQUIRE(defaults_probe.named_parameters.size() == 3);
	REQUIRE(defaults_probe.named_parameters.at("k").IsNull());
	REQUIRE(defaults_probe.named_parameters.at("o") == Value::INTEGER(2));
	REQUIRE(defaults_probe.named_parameters.at("untyped") == Value("x"));

	REQUIRE_NO_FAIL(con.Query("SELECT * FROM defaults_probe(a := 3, o := 4)"));
	REQUIRE(defaults_probe.inputs == vector<Value> {Value::INTEGER(3)});
	REQUIRE(defaults_probe.named_parameters.at("k") == Value::INTEGER(7));
	REQUIRE(defaults_probe.named_parameters.at("o") == Value::INTEGER(4));
	con.Rollback();
}

TEST_CASE("A table function receives its defaults cast to the parameter types", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	// defaults of another type than their parameter, as for a scalar function they are cast - unless the
	// parameter is ANY
	auto sig = FunctionSignature()
	               .AddParameter("a", LogicalType::BIGINT, Value::INTEGER(42))
	               .AddParameter("any", LogicalType::ANY, Value::INTEGER(1))
	               .AddKeywordOnly("k", LogicalType::VARCHAR, Value::INTEGER(7));
	TableFunction function("defaults_cast_probe", std::move(sig), DefaultsProbeScan, DefaultsProbeBind);
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	REQUIRE_NO_FAIL(con.Query("SELECT * FROM defaults_cast_probe()"));
	REQUIRE(defaults_probe.inputs.size() == 2);
	REQUIRE(defaults_probe.inputs[0].type() == LogicalType::BIGINT);
	REQUIRE(defaults_probe.inputs[0] == Value::BIGINT(42));
	REQUIRE(defaults_probe.inputs[1].type() == LogicalType::INTEGER);
	REQUIRE(defaults_probe.named_parameters.at("k").type() == LogicalType::VARCHAR);
	REQUIRE(defaults_probe.named_parameters.at("k") == Value("7"));
	con.Rollback();
}

namespace {

//! The named arguments the last bind of "options_probe" received
named_argument_map_t options_probe;

unique_ptr<FunctionData> OptionsProbeBind(ClientContext &, TableFunctionBindInput &input, vector<LogicalType> &types,
                                          vector<Identifier> &names) {
	options_probe = input.named_parameters;
	types.push_back(LogicalType::BOOLEAN);
	names.emplace_back("ok");
	return make_uniq<TableFunctionData>();
}

} // namespace

TEST_CASE("A table function receives the options its **kwargs declares", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	TypedKwargs options;
	options.Add("header", LogicalType::BOOLEAN)
	    .Add("delim", LogicalType::VARCHAR)
	    .Alias("sep")
	    .Add("sample_size", LogicalType::BIGINT)
	    .Add("columns", LogicalType::ANY);
	FunctionSignature sig;
	sig.AddParameter("path", LogicalType::VARCHAR);
	sig.AddTypedKwargs("options", std::move(options));
	sig.Verify();
	TableFunction function("options_probe", std::move(sig), DefaultsProbeScan, OptionsProbeBind);
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	// an option the call leaves out is absent
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM options_probe('x')"));
	REQUIRE(options_probe.empty());

	// passed options are cast to their declared types - a literal as leniently as for a parameter - and an alias is
	// received under the name of its option
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM options_probe('x', header := 'true', sep := '|', sample_size := 10)"));
	REQUIRE(options_probe.size() == 3);
	REQUIRE(options_probe.at("header") == Value::BOOLEAN(true));
	REQUIRE(options_probe.at("delim") == Value("|"));
	REQUIRE(options_probe.at("sample_size") == Value::BIGINT(10));
	// in the order they were passed, each under its canonical name
	REQUIRE(options_probe.Keys() == vector<Identifier> {"header", "delim", "sample_size"});

	// an ANY option is passed through as it is
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM options_probe('x', columns := {'a': 'INTEGER'})"));
	REQUIRE(options_probe.at("columns").type().id() == LogicalTypeId::STRUCT);

	// a NULL is passed as a value
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM options_probe('x', header := NULL)"));
	REQUIRE(options_probe.at("header").IsNull());

	// an undeclared name is rejected, naming the declared options
	auto result = con.Query("SELECT * FROM options_probe('x', heder := true)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Invalid named parameter \"heder\""));
	REQUIRE(StringUtil::Contains(result->GetError(), "Did you mean: \"header\""));

	// a value no implicit cast reaches the declared type selects no overload
	result = con.Query("SELECT * FROM options_probe('x', sample_size := 1.5::DOUBLE)");
	REQUIRE(result->HasError());
	result = con.Query("SELECT * FROM options_probe('x', sample_size := '1' || '0')");
	REQUIRE(result->HasError());

	// an option passed by its name and an alias is passed twice
	result = con.Query("SELECT * FROM options_probe('x', delim := ',', sep := '|')");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "passed more than once"));
	con.Rollback();
}

TEST_CASE("Named arguments arrive in binding order", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	FunctionSignature sig;
	sig.AddParameter("path", LogicalType::VARCHAR);
	sig.AddKeywordOnly("k1", LogicalType::INTEGER, Value::INTEGER(1));
	sig.AddKeywordOnly("k2", LogicalType::INTEGER, Value::INTEGER(2));
	sig.AddTypedKwargs("options", TypedKwargs().Add("o1", LogicalType::INTEGER).Add("o2", LogicalType::INTEGER));
	TableFunction function("order_probe", std::move(sig), DefaultsProbeScan, OptionsProbeBind);
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	// keyword-only parameters in declaration order, passed or defaulted, then the options in the order they were passed
	REQUIRE_NO_FAIL(con.Query("SELECT * FROM order_probe('x', o2 := 3, k2 := 5, o1 := 4)"));
	REQUIRE(options_probe.Keys() == vector<Identifier> {"k1", "k2", "o2", "o1"});
	REQUIRE(options_probe.at("k1") == Value::INTEGER(1));
	REQUIRE(options_probe.at("k2") == Value::INTEGER(5));

	REQUIRE_NO_FAIL(con.Query("SELECT * FROM order_probe('x', o1 := 4, K2 := 5)"));
	REQUIRE(options_probe.Keys() == vector<Identifier> {"k1", "k2", "o1"});
	con.Rollback();
}

TEST_CASE("A signature with options has a **kwargs parameter that no option shadows", "[api][table_function]") {
	FunctionSignature typed;
	typed.AddParameter("path", LogicalType::VARCHAR);
	typed.AddTypedKwargs("options", TypedKwargs().Add("header", LogicalType::BOOLEAN));
	REQUIRE(typed.GetKwargs()->GetName() == "options");
	typed.Verify();
	// a second "**kwargs" follows the first, which must be the last parameter
	typed.AddTypedKwargs("more", TypedKwargs());
	REQUIRE_THROWS_WITH(typed.Verify(), Catch::Matchers::Contains("must be the last parameter"));

	FunctionSignature shadowed;
	shadowed.AddParameter("path", LogicalType::VARCHAR);
	shadowed.AddTypedKwargs("options", TypedKwargs().Add("x", LogicalType::BOOLEAN).Alias("path"));
	REQUIRE_THROWS_WITH(shadowed.Verify(), Catch::Matchers::Contains("same name as a parameter"));

	// names are checked once the schema is complete, case-insensitively and across merged schemas
	REQUIRE_THROWS_WITH(TypedKwargs().Add("a", LogicalType::BOOLEAN).Alias("A").Verify(),
	                    Catch::Matchers::Contains("Duplicate option name"));
	auto left = TypedKwargs().Add("a", LogicalType::BOOLEAN);
	auto right = TypedKwargs().Add("b", LogicalType::BOOLEAN).Alias("a");
	REQUIRE_THROWS_WITH(left.Merge(right).Verify(), Catch::Matchers::Contains("Duplicate option name"));
}

TEST_CASE("A typed **kwargs receives its arguments cast to its type", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto &context = *con.context;

	FunctionSignature sig;
	sig.AddKwargs("rest", LogicalType::BIGINT);
	TableFunction function("kwargs_probe", std::move(sig), DefaultsProbeScan, OptionsProbeBind);
	CreateTableFunctionInfo info(function);
	Catalog::GetSystemCatalog(context).CreateFunction(context, info);

	REQUIRE_NO_FAIL(con.Query("SELECT * FROM kwargs_probe(x := 1, y := 2::TINYINT)"));
	REQUIRE(options_probe.at("x") == Value::BIGINT(1));
	REQUIRE(options_probe.at("x").type() == LogicalType::BIGINT);
	REQUIRE(options_probe.at("y").type() == LogicalType::BIGINT);
	// a value no implicit cast reaches the type selects no overload
	REQUIRE(con.Query("SELECT * FROM kwargs_probe(x := 1.5::DOUBLE)")->HasError());
	con.Rollback();
}

TEST_CASE("Two overloads are the same when no call tells them apart", "[api][table_function]") {
	auto path = [](const LogicalType &type) {
		return FunctionSignature().AddParameter("path", type);
	};
	auto keywords = [](vector<const char *> names) {
		FunctionSignature result;
		for (auto name : names) {
			result.AddKeywordOnly(name, LogicalType::INTEGER);
		}
		return result;
	};
	auto args = [](const LogicalType &type) {
		return FunctionSignature().AddArgs("args", type);
	};

	// parameters that take a position match in order, by type
	REQUIRE(path(LogicalType::VARCHAR).IsSameOverload(path(LogicalType::VARCHAR)));
	REQUIRE(!path(LogicalType::VARCHAR).IsSameOverload(path(LogicalType::BIGINT)));
	// "*args" of different types accept different calls
	REQUIRE(!args(LogicalType::INTEGER).IsSameOverload(args(LogicalType::VARCHAR)));
	REQUIRE(!path(LogicalType::VARCHAR).IsSameOverload(path(LogicalType::VARCHAR).AddArgs("args", LogicalType::ANY)));

	// whatever their names, and whether they are positional-only
	REQUIRE(path(LogicalType::VARCHAR).IsSameOverload(FunctionSignature().AddParameter("file", LogicalType::VARCHAR)));
	REQUIRE(
	    path(LogicalType::VARCHAR).IsSameOverload(FunctionSignature().AddPositionalOnly("file", LogicalType::VARCHAR)));

	// keyword-only parameters match by name, in any order
	REQUIRE(!keywords({"a"}).IsSameOverload(keywords({"b"})));
	REQUIRE(keywords({"a"}).IsSameOverload(keywords({"A"})));
	REQUIRE(keywords({"a", "b"}).IsSameOverload(keywords({"b", "a"})));

	// defaults and options take no part
	auto with_default = FunctionSignature().AddParameter("path", LogicalType::VARCHAR, Value("x"));
	auto with_option = path(LogicalType::VARCHAR).WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("opt", LogicalType::INTEGER);
	});
	auto with_other_option = path(LogicalType::VARCHAR).WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("other", LogicalType::VARCHAR);
	});
	REQUIRE(path(LogicalType::VARCHAR).IsSameOverload(with_default));
	REQUIRE(with_option.IsSameOverload(with_other_option));
	// an optional parameter is a parameter all the same
	REQUIRE(!path(LogicalType::VARCHAR)
	             .IsSameOverload(
	                 path(LogicalType::VARCHAR).AddKeywordOnly("opt", LogicalType::INTEGER, Value::INTEGER(1))));

	// merging keeps overloads that some call tells apart, and rejects one that is already there
	TableFunctionSet set("overloaded");
	set.AddFunction(TableFunction("overloaded", args(LogicalType::INTEGER), DefaultsProbeScan, OptionsProbeBind));
	TableFunctionSet other_args("overloaded");
	other_args.AddFunction(
	    TableFunction("overloaded", args(LogicalType::VARCHAR), DefaultsProbeScan, OptionsProbeBind));
	REQUIRE(set.MergeFunctionSet(other_args));
	REQUIRE(set.Size() == 2);
	REQUIRE(!set.MergeFunctionSet(other_args));
	REQUIRE(set.Size() == 2);
}

TEST_CASE("An option is checked once the overload is chosen", "[api][table_function]") {
	DuckDB db(nullptr);
	Connection con(db);

	// a name that is no option suggests the options it resembles, not the other overloads
	auto result = con.Query("FROM read_csv('x.csv', heder := true)");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(), "Invalid named parameter \"heder\" for function read_csv"));
	REQUIRE(StringUtil::Contains(result->GetError(), "header"));
	REQUIRE(!StringUtil::Contains(result->GetError(), "Candidate functions"));

	// a value of the wrong type is cast to the option's type like an explicit cast, and a failure names the option
	result = con.Query("FROM read_csv('x.csv', header := ['a'])");
	REQUIRE(result->HasError());
	REQUIRE(StringUtil::Contains(result->GetError(),
	                             "Could not cast value ['a'] to named parameter \"header\" of type BOOLEAN: "
	                             "Unimplemented type for cast (VARCHAR[] -> BOOLEAN)"));
	REQUIRE(!StringUtil::Contains(result->GetError(), "No function matches"));
}
