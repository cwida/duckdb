#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/main/client_context.hpp"

using namespace duckdb;

static void NoopExec(DataChunk &args, ExpressionState &state, Vector &result) {
}

//! f(list T[], value T, *args VARCHAR, opt BIGINT := 0, **kwargs ANY) -> T
static ScalarFunctionSet GetTemplatedSet() {
	FunctionSignature sig;
	sig.AddParameter("list", LogicalType::LIST(LogicalType::TEMPLATE("T")));
	sig.AddParameter("value", LogicalType::TEMPLATE("T"));
	sig.AddArgs("args", LogicalType::VARCHAR);
	sig.AddKeywordOnly("opt", LogicalType::BIGINT, Value::BIGINT(0));
	sig.AddKwargs("kwargs", LogicalType::ANY);
	sig.SetReturnType(LogicalType::TEMPLATE("T"));
	ScalarFunctionSet set("f");
	set.AddFunction(ScalarFunction("f", std::move(sig), NoopExec));
	return set;
}

//! g(a INTEGER, b BIGINT), g(a BIGINT, b INTEGER), g(a VARCHAR)
static ScalarFunctionSet GetOverloadedSet() {
	ScalarFunctionSet set("g");
	for (auto &parameters : vector<vector<LogicalType>> {{LogicalType::INTEGER, LogicalType::BIGINT},
	                                                     {LogicalType::BIGINT, LogicalType::INTEGER},
	                                                     {LogicalType::VARCHAR}}) {
		FunctionSignature sig;
		sig.AddParameter("a", parameters[0]);
		if (parameters.size() > 1) {
			sig.AddParameter("b", parameters[1]);
		}
		sig.SetReturnType(LogicalType::INTEGER);
		set.AddFunction(ScalarFunction("g", std::move(sig), NoopExec));
	}
	return set;
}

//! Why no overload matches - the selection must have failed to match
static const vector<FunctionMismatch> &GetMismatches(const FunctionBindResult &selection) {
	REQUIRE(!selection.IsSuccess());
	REQUIRE(selection.GetError().GetType() == FunctionBindError::Type::NO_MATCH);
	return selection.GetError().Cast<FunctionBindError::NoMatch>().mismatches;
}

static string GetErrorMessage(const FunctionBindResult &selection) {
	return selection.GetError().RawMessage();
}

TEST_CASE("Selecting a function maps every argument to the parameter that receives it", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto set = GetTemplatedSet();

	vector<LogicalType> positional {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER, LogicalType::VARCHAR,
	                                LogicalType::VARCHAR};
	vector<pair<Identifier, LogicalType>> named {{"opt", LogicalType::BIGINT}, {"extra", LogicalType::BOOLEAN}};
	auto selection = FunctionBinder::SelectFunction(*con.context, set, {positional, named});
	REQUIRE(selection.IsSuccess());
	REQUIRE(selection.GetBinding().GetOverloadIndex() == 0);

	// list, value, the two arguments "*args" receives, the keyword-only parameter, and "**kwargs"
	auto &binding = selection.GetBinding();
	const vector<idx_t> expected_parameters {0, 1, 2, 2, 3, 4};
	REQUIRE(binding.argument_to_parameters == expected_parameters);
	const vector<LogicalType> expected_types {LogicalType::LIST(LogicalType::INTEGER), LogicalType::INTEGER,
	                                          LogicalType::VARCHAR, LogicalType::BIGINT, LogicalType::ANY};
	REQUIRE(binding.parameter_types == expected_types);
	REQUIRE(binding.return_type == LogicalType::INTEGER);
}

TEST_CASE("Selecting a function deduces the templates of the overload", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto set = GetTemplatedSet();

	// the template unifies the element type of the list with the value
	auto selection = FunctionBinder::SelectFunction(*con.context, set,
	                                                {LogicalType::LIST(LogicalType::INTEGER), LogicalType::BIGINT});
	REQUIRE(selection.IsSuccess());
	auto &parameter_types = selection.GetBinding().parameter_types;
	REQUIRE(parameter_types[0] == LogicalType::LIST(LogicalType::BIGINT));
	REQUIRE(parameter_types[1] == LogicalType::BIGINT);
	REQUIRE(selection.GetBinding().return_type == LogicalType::BIGINT);

	// arguments that determine conflicting types for the template do not match the overload
	vector<LogicalType> conflicting {LogicalType::LIST(LogicalType::DATE), LogicalType::BOOLEAN};
	selection = FunctionBinder::SelectFunction(*con.context, set, conflicting);
	auto &deduction = GetMismatches(selection)[0];
	REQUIRE(deduction.GetType() == FunctionMismatch::Type::TEMPLATE_DEDUCTION);
	REQUIRE(deduction.Cast<FunctionMismatch::TemplateDeduction>().template_name == "T");
	REQUIRE(deduction.Cast<FunctionMismatch::TemplateDeduction>().argument.GetIndex() == 1);
	REQUIRE(StringUtil::Contains(GetErrorMessage(selection), "Cannot deduce template type 'T'"));

	// without a client context, the template is unified with the built-in implicit casts
	selection =
	    FunctionBinder::SelectFunction(nullptr, set, {LogicalType::LIST(LogicalType::INTEGER), LogicalType::BIGINT});
	REQUIRE(selection.IsSuccess());
	REQUIRE(selection.GetBinding().return_type == LogicalType::BIGINT);

	// a template that neither the arguments nor the defaults determine does not match either
	FunctionSignature sig;
	sig.AddParameter("value", LogicalType::INTEGER);
	sig.SetReturnType(LogicalType::TEMPLATE("T"));
	ScalarFunctionSet undetermined("u");
	undetermined.AddFunction(ScalarFunction("u", std::move(sig), NoopExec));
	selection = FunctionBinder::SelectFunction(*con.context, undetermined, {LogicalType::INTEGER});
	REQUIRE(GetMismatches(selection)[0].GetType() == FunctionMismatch::Type::TEMPLATE_UNRESOLVED);
	REQUIRE(StringUtil::Contains(GetErrorMessage(selection), "is not determined by the arguments"));
}

TEST_CASE("A call no overload matches records why each overload fails", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto set = GetOverloadedSet();

	auto selection = FunctionBinder::SelectFunction(*con.context, set, {LogicalType::DATE});
	REQUIRE(!selection);
	auto message = GetErrorMessage(selection);
	REQUIRE(StringUtil::Contains(message, "No function matches"));

	auto &failed = GetMismatches(selection);
	REQUIRE(failed.size() == 3);
	// the last cannot take a DATE as its VARCHAR, which is closer than the first two leaving "b" unfilled
	REQUIRE(failed[0].GetOverloadIndex() == 2);
	REQUIRE(failed[0].GetType() == FunctionMismatch::Type::INCOMPATIBLE_ARGUMENT);
	REQUIRE(failed[0].Cast<FunctionMismatch::IncompatibleArgument>().argument == 0);
	REQUIRE(failed[1].GetOverloadIndex() == 0);
	REQUIRE(failed[1].GetType() == FunctionMismatch::Type::MISSING_PARAMETER);
	REQUIRE(failed[1].Cast<FunctionMismatch::MissingParameter>().parameter == 1);
	REQUIRE(failed[2].GetOverloadIndex() == 1);
	REQUIRE(failed[0].GetDistance() < failed[1].GetDistance());
	REQUIRE(failed[1].GetDistance() == failed[2].GetDistance());
	REQUIRE(failed[0].Cast<FunctionMismatch::IncompatibleArgument>().parameter_type == LogicalType::VARCHAR);
	REQUIRE(selection.GetError().Cast<FunctionBindError::NoMatch>().GetClosest()->GetOverloadIndex() == 2);
	REQUIRE(StringUtil::Contains(message, "Candidates:\n    g(a VARCHAR) -> INTEGER\n        Argument 1 of type DATE "
	                                      "cannot be implicitly cast to VARCHAR"));

	// too many arguments, and a name no overload has
	vector<LogicalType> three_varchars {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	selection = FunctionBinder::SelectFunction(*con.context, set, three_varchars);
	REQUIRE(GetMismatches(selection)[2].GetOverloadIndex() == 2);
	REQUIRE(GetMismatches(selection)[2].GetType() == FunctionMismatch::Type::TOO_MANY_ARGUMENTS);
	// the two overloads taking two arguments are equally close, so neither is the closest
	REQUIRE(!selection.GetError().Cast<FunctionBindError::NoMatch>().GetClosest());

	// h(a VARCHAR, b INTEGER := 0) takes two arguments, but none named "c"
	FunctionSignature sig;
	sig.AddParameter("a", LogicalType::VARCHAR);
	sig.AddParameter("b", LogicalType::INTEGER, Value::INTEGER(0));
	sig.SetReturnType(LogicalType::INTEGER);
	ScalarFunctionSet defaulted("h");
	defaulted.AddFunction(ScalarFunction("h", std::move(sig), NoopExec));
	vector<pair<Identifier, LogicalType>> named_c {{"c", LogicalType::INTEGER}};
	selection = FunctionBinder::SelectFunction(*con.context, defaulted, {{LogicalType::VARCHAR}, named_c});
	REQUIRE(GetMismatches(selection)[0].GetType() == FunctionMismatch::Type::UNKNOWN_NAMED_ARGUMENT);
	REQUIRE(GetMismatches(selection)[0].Cast<FunctionMismatch::UnknownNamedArgument>().argument == 1);
	message = GetErrorMessage(selection);
	REQUIRE(StringUtil::Contains(message, "    h(a VARCHAR, b INTEGER := 0) -> INTEGER\n        Has no parameter "
	                                      "named \"c\""));
}

TEST_CASE("Fewer arguments that cannot be cast are a closer miss", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	// k(a DATE, b DATE), k(a INTEGER, b DATE)
	ScalarFunctionSet set("k");
	for (auto &first : {LogicalType::DATE, LogicalType::INTEGER}) {
		FunctionSignature sig;
		sig.AddParameter("a", first);
		sig.AddParameter("b", LogicalType::DATE);
		sig.SetReturnType(LogicalType::INTEGER);
		set.AddFunction(ScalarFunction("k", std::move(sig), NoopExec));
	}

	auto selection = FunctionBinder::SelectFunction(*con.context, set, {LogicalType::INTEGER, LogicalType::BOOLEAN});
	auto &failed = GetMismatches(selection);
	REQUIRE(failed[0].GetOverloadIndex() == 1);
	REQUIRE(failed[0].Cast<FunctionMismatch::IncompatibleArgument>().argument == 1);
	REQUIRE(failed[0].GetDistance() == 1);
	REQUIRE(failed[1].Cast<FunctionMismatch::IncompatibleArgument>().argument == 0);
	REQUIRE(failed[1].GetDistance() == 2);
}

TEST_CASE("A call several overloads match at the same cost is ambiguous", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto set = GetOverloadedSet();

	vector<LogicalType> two_integers {LogicalType::INTEGER, LogicalType::INTEGER};
	auto selection = FunctionBinder::SelectFunction(*con.context, set, two_integers);
	REQUIRE(!selection);
	REQUIRE(selection.GetError().GetType() == FunctionBindError::Type::AMBIGUOUS_MATCH);
	REQUIRE(StringUtil::Contains(GetErrorMessage(selection), "Could not choose a best candidate"));
	// only the overloads tied at the lowest cost
	auto &candidates = selection.GetError().Cast<FunctionBindError::AmbiguousMatch>().candidates;
	REQUIRE(candidates.size() == 2);
	REQUIRE(candidates[0].cost == candidates[1].cost);
}

TEST_CASE("A named argument passed twice matches no overload", "[api][function_binding]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto set = GetOverloadedSet();

	vector<pair<Identifier, LogicalType>> named {{"a", LogicalType::VARCHAR}, {"a", LogicalType::VARCHAR}};
	auto selection = FunctionBinder::SelectFunction(*con.context, set, FunctionCallArguments({}, named));
	REQUIRE(!selection);
	REQUIRE(selection.GetError().GetType() == FunctionBindError::Type::DUPLICATE_ARGUMENT);
	REQUIRE(selection.GetError().Cast<FunctionBindError::DuplicateArgument>().argument == 1);
	REQUIRE(StringUtil::Contains(GetErrorMessage(selection), "Duplicate named argument"));
}
