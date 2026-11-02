#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 pragma function tests: register query pragmas (which expand to SQL) and
// exec pragmas (which run for their side effects), then call them through the
// PRAGMA statement.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error the test
// asserts on.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t PragmaIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

void FailPragma(duckdb_v2_error_info_handle *err, DUCKDB_V2_ERROR code, const std::string &message) {
	duckdb_v2_error_info_set_code(*err, code);
	auto text_str = Convert(message);
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// Reads the argument at `index` as a string, through its constant value.
bool ArgText(duckdb_v2_function_bind_info_handle info, idx_t index, std::string &out,
             duckdb_v2_error_info_handle *err) {
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, index, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	// Not Render: it asserts, which must not happen inside a callback.
	idx_t len = 0;
	auto rc = duckdb_v2_value_to_string(value, nullptr, 0, &len, err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		std::string text(len + 1, '\0');
		rc = duckdb_v2_value_to_string(value, &text[0], text.size(), &len, err);
		out = text.substr(0, len);
	}
	duckdb_v2_value_destroy(&value);
	return rc == DUCKDB_V2_ERROR_NONE;
}

void SetQuery(duckdb_v2_pragma_function_query_info_handle result, const std::string &sql,
              duckdb_v2_error_info_handle *err) {
	auto sql_str = Convert(sql);
	duckdb_v2_pragma_function_query_set_query(result, &sql_str, err);
}

// PRAGMA answer -> SELECT '42'
void AnswerQuery(duckdb_v2_function_bind_info_handle info, duckdb_v2_pragma_function_query_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	if (!context) {
		FailPragma(err, DUCKDB_V2_ERROR_INPUT_INVALID, "query callback ran without a context");
		return;
	}
	idx_t positional = 1;
	if (duckdb_v2_function_bind_get_arg_count(info, &positional, nullptr, nullptr, nullptr, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (positional != 0) {
		FailPragma(err, DUCKDB_V2_ERROR_INPUT_INVALID, "a pragma without parameters received arguments");
		return;
	}
	SetQuery(result, "SELECT '42'", err);
}

// PRAGMA label(n INTEGER, *, prefix VARCHAR := 'x') -> SELECT '<prefix><n>'
void LabelQuery(duckdb_v2_function_bind_info_handle info, duckdb_v2_pragma_function_query_info_handle result,
                duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	idx_t counts[4] = {};
	if (duckdb_v2_function_bind_get_arg_count(info, &counts[0], &counts[1], &counts[2], &counts[3], err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (counts[0] != 1 || counts[1] != 0 || counts[2] != 1 || counts[3] != 0) {
		FailPragma(err, DUCKDB_V2_ERROR_INPUT_INVALID, "unexpected argument counts");
		return;
	}
	auto prefix_name = PragmaIdent("prefix");
	idx_t prefix_index = 0;
	bool found = false;
	if (duckdb_v2_function_bind_get_arg_index(info, &prefix_name, &prefix_index, &found, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (!found || prefix_index != 1) {
		FailPragma(err, DUCKDB_V2_ERROR_INPUT_INVALID, "the prefix argument is not at index 1");
		return;
	}
	std::string n;
	std::string prefix;
	if (!ArgText(info, 0, n, err) || !ArgText(info, prefix_index, prefix, err)) {
		return;
	}
	SetQuery(result, "SELECT '" + prefix + n + "'", err);
}

// Reports a callback error, checking that the code round-trips.
void FailingQuery(duckdb_v2_function_bind_info_handle, duckdb_v2_pragma_function_query_info_handle,
                  duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	FailPragma(err, DUCKDB_V2_ERROR_TYPE_CONVERSION, "the query pragma failed");
}

// Returns without setting a query.
void SilentQuery(duckdb_v2_function_bind_info_handle, duckdb_v2_pragma_function_query_info_handle,
                 duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

// Pragmas have no bind data: setting it must fail, which the callback turns into its own error.
void BindDataQuery(duckdb_v2_function_bind_info_handle info, duckdb_v2_pragma_function_query_info_handle result,
                   duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_opaque data = {new int(1), [](void *ptr) { delete static_cast<int *>(ptr); }, nullptr};
	duckdb_v2_error_info_handle bind_err = nullptr;
	auto rc = duckdb_v2_function_bind_set_bind_data(info, &data, &bind_err);
	duckdb_v2_error_info_destroy(&bind_err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		FailPragma(err, DUCKDB_V2_ERROR_INPUT_INVALID, "a pragma accepted bind data");
		return;
	}
	data.destroy(data.ptr);
	SetQuery(result, "SELECT 'no bind data'", err);
}

// The side effects of the exec pragma, carried in its user data.
struct Counter {
	int64_t total = 0;
	idx_t calls = 0;
};

// PRAGMA bump(*amounts BIGINT): adds every amount to the counter.
void BumpExec(duckdb_v2_function_bind_info_handle info, duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	if (duckdb_v2_function_bind_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto &counter = *static_cast<Counter *>(user_data);
	idx_t fixed = 0;
	idx_t variadic = 0;
	if (duckdb_v2_function_bind_get_arg_count(info, &fixed, &variadic, nullptr, nullptr, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = fixed; i < fixed + variadic; i++) {
		std::string amount;
		if (!ArgText(info, i, amount, err)) {
			return;
		}
		counter.total += std::stoll(amount);
	}
	counter.calls++;
}

// Builds a pragma with the given name and callbacks; the caller adds parameters and registers it.
duckdb_v2_pragma_function_handle MakePragma(duckdb_v2_factory_handle factory, const char *name,
                                            duckdb_v2_pragma_function_query_callback_fn query,
                                            duckdb_v2_pragma_function_exec_callback_fn exec) {
	duckdb_v2_pragma_function_handle function = nullptr;
	REQUIRE(duckdb_v2_pragma_function_create(factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = PragmaIdent(name);
	REQUIRE(duckdb_v2_pragma_function_set_name(function, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	if (query) {
		REQUIRE(duckdb_v2_pragma_function_set_query_callback(function, query, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	if (exec) {
		REQUIRE(duckdb_v2_pragma_function_set_exec_callback(function, exec, nullptr) == DUCKDB_V2_ERROR_NONE);
	}
	return function;
}

void RegisterPragma(duckdb_v2_connection_handle conn, duckdb_v2_pragma_function_handle function) {
	REQUIRE(duckdb_v2_connection_register_pragma_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_pragma_function_destroy(&function);
}

// The code registering `function` fails with, destroying the function.
DUCKDB_V2_ERROR RegisterPragmaError(duckdb_v2_connection_handle conn, duckdb_v2_pragma_function_handle function) {
	auto rc = duckdb_v2_connection_register_pragma_function(conn, function, nullptr);
	duckdb_v2_pragma_function_destroy(&function);
	return rc;
}

// Run a query producing a single VARCHAR cell.
std::string PragmaQueryText(duckdb_v2_connection_handle conn, const char *sql) {
	QueryResult result;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = Convert(Convert(static_cast<const duckdb_v2_bytes *>(view.data)[SelAt(view.sel, 0)]));
	duckdb_v2_data_chunk_destroy(&chunk);
	return out;
}

// Runs a query to exhaustion, returning its row count.
idx_t PragmaQueryRows(duckdb_v2_connection_handle conn, const char *sql) {
	QueryResult result;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	return DrainRowCount(result);
}

// The code a query fails with, along with its message.
DUCKDB_V2_ERROR PragmaQueryError(duckdb_v2_connection_handle conn, const char *sql, std::string &message) {
	duckdb_v2_error_info_handle err = nullptr;
	QueryResult result;
	auto rc = Query(conn, sql, &result, &err);
	if (rc == DUCKDB_V2_ERROR_NONE) {
		DrainRowCount(result);
	} else {
		duckdb_v2_str text = {nullptr, 0};
		duckdb_v2_error_info_get_text(err, &text);
		message = Convert(text);
	}
	duckdb_v2_error_info_destroy(&err);
	return rc;
}

} // namespace

TEST_CASE("V2 pragma: a query pragma without parameters", "[capi_v2][pragma_function]") {
	EnvFixture fx;
	RegisterPragma(fx.conn, MakePragma(fx.factory, "answer", AnswerQuery, nullptr));

	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA answer") == "42");

	// Arguments the signature does not declare are rejected.
	std::string message;
	REQUIRE(PragmaQueryError(fx.conn, "PRAGMA answer(1)", message) != DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 pragma: a query pragma receives its arguments", "[capi_v2][pragma_function]") {
	EnvFixture fx;
	auto function = MakePragma(fx.factory, "label", LabelQuery, nullptr);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_pragma_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	auto n_name = PragmaIdent("n");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &n_name, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_handle default_prefix = nullptr;
	auto x = Convert("x");
	REQUIRE(duckdb_v2_value_create_varchar(fx.factory, &x, &default_prefix, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto prefix_name = PragmaIdent("prefix");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &prefix_name, varchar, default_prefix,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_value_destroy(&default_prefix);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&integer);
	RegisterPragma(fx.conn, function);

	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA label(7)") == "x7");
	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA label(7, prefix = 'y')") == "y7");
	// The argument is cast to the parameter type.
	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA label('8')") == "x8");
	// A standard parameter can be passed by name.
	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA label(n = 9)") == "x9");

	std::string message;
	REQUIRE(PragmaQueryError(fx.conn, "PRAGMA label", message) != DUCKDB_V2_ERROR_NONE);
	REQUIRE(PragmaQueryError(fx.conn, "PRAGMA label(1, suffix = 'z')", message) != DUCKDB_V2_ERROR_NONE);
}

TEST_CASE("V2 pragma: an exec pragma runs for its side effects", "[capi_v2][pragma_function]") {
	EnvFixture fx;
	auto counter = new Counter();
	auto function = MakePragma(fx.factory, "bump", nullptr, BumpExec);
	duckdb_v2_opaque user_data = {counter, [](void *ptr) { delete static_cast<Counter *>(ptr); }, nullptr};
	REQUIRE(duckdb_v2_pragma_function_set_user_data(function, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_pragma_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto amounts_name = PragmaIdent("amounts");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &amounts_name, bigint, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_logical_type_destroy(&bigint);
	RegisterPragma(fx.conn, function);

	// The statement produces no rows.
	REQUIRE(PragmaQueryRows(fx.conn, "PRAGMA bump(5)") == 0);
	REQUIRE(counter->calls == 1);
	REQUIRE(counter->total == 5);

	REQUIRE(PragmaQueryRows(fx.conn, "PRAGMA bump(1, 2, 3)") == 0);
	REQUIRE(counter->calls == 2);
	REQUIRE(counter->total == 11);

	REQUIRE(PragmaQueryRows(fx.conn, "PRAGMA bump") == 0);
	REQUIRE(counter->calls == 3);
	REQUIRE(counter->total == 11);
}

TEST_CASE("V2 pragma: callback failures surface as statement errors", "[capi_v2][pragma_function]") {
	EnvFixture fx;
	RegisterPragma(fx.conn, MakePragma(fx.factory, "failing", FailingQuery, nullptr));
	RegisterPragma(fx.conn, MakePragma(fx.factory, "silent", SilentQuery, nullptr));
	RegisterPragma(fx.conn, MakePragma(fx.factory, "bind_data", BindDataQuery, nullptr));

	std::string message;
	REQUIRE(PragmaQueryError(fx.conn, "PRAGMA failing", message) == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(message.find("the query pragma failed") != std::string::npos);

	REQUIRE(PragmaQueryError(fx.conn, "PRAGMA silent", message) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(message.find("did not set a query") != std::string::npos);

	REQUIRE(PragmaQueryText(fx.conn, "PRAGMA bind_data") == "no bind data");
}

TEST_CASE("V2 pragma: registration validates the function", "[capi_v2][pragma_function]") {
	EnvFixture fx;

	SECTION("a name is required") {
		duckdb_v2_pragma_function_handle function = nullptr;
		REQUIRE(duckdb_v2_pragma_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_pragma_function_set_query_callback(function, AnswerQuery, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(RegisterPragmaError(fx.conn, function) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a callback is required") {
		REQUIRE(RegisterPragmaError(fx.conn, MakePragma(fx.factory, "none", nullptr, nullptr)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("only one callback may be set") {
		REQUIRE(RegisterPragmaError(fx.conn, MakePragma(fx.factory, "both", AnswerQuery, BumpExec)) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("the signature cannot set a return type") {
		auto function = MakePragma(fx.factory, "typed", AnswerQuery, nullptr);
		duckdb_v2_function_signature_handle sig = nullptr;
		REQUIRE(duckdb_v2_pragma_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_logical_type_destroy(&integer);
		REQUIRE(RegisterPragmaError(fx.conn, function) == DUCKDB_V2_ERROR_INPUT_INVALID);
	}
	SECTION("a name can only be registered once") {
		RegisterPragma(fx.conn, MakePragma(fx.factory, "answer", AnswerQuery, nullptr));
		REQUIRE(RegisterPragmaError(fx.conn, MakePragma(fx.factory, "answer", AnswerQuery, nullptr)) !=
		        DUCKDB_V2_ERROR_NONE);
		// Nor can it take over a built-in pragma.
		REQUIRE(RegisterPragmaError(fx.conn, MakePragma(fx.factory, "show_tables", AnswerQuery, nullptr)) !=
		        DUCKDB_V2_ERROR_NONE);
		REQUIRE(PragmaQueryText(fx.conn, "PRAGMA answer") == "42");
	}
	SECTION("null handles are rejected, and destroy is null-safe") {
		REQUIRE(duckdb_v2_connection_register_pragma_function(fx.conn, nullptr, nullptr) != DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_pragma_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_pragma_function_handle function = nullptr;
		REQUIRE(duckdb_v2_pragma_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	}
}

TEST_CASE("V2 pragma: documentation is reported by duckdb_functions()", "[capi_v2][pragma_function]") {
	EnvFixture fx;
	auto function = MakePragma(fx.factory, "answer", AnswerQuery, nullptr);
	duckdb_v2_function_docs_handle docs = nullptr;
	REQUIRE(duckdb_v2_pragma_function_get_docs(function, &docs, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto description = Convert("Returns the answer");
	REQUIRE(duckdb_v2_function_docs_set_description(docs, &description, nullptr) == DUCKDB_V2_ERROR_NONE);
	RegisterPragma(fx.conn, function);

	REQUIRE(PragmaQueryText(fx.conn, "SELECT function_type || ': ' || description FROM duckdb_functions() "
	                                 "WHERE function_name = 'answer'") == "pragma: Returns the answer");
}

} // namespace test_capi_v2
