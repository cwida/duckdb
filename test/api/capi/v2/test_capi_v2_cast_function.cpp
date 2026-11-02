#include "test_capi_v2.hpp"

#include <cstring>
#include <string>

// ---------------------------------------------------------------------------
// V2 cast function tests: register a custom type TEMPERATURE (an alias of
// INTEGER) plus two casts between it and VARCHAR, then reach them from SQL
// through CAST / TRY_CAST and through implicit argument conversion.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error the test
// asserts on. Cross-callback observations are latched into file-scope statics.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t CastIdent(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// The mode the last cast callback ran in, latched for the test to assert on.
DUCKDB_V2_CAST_MODE last_cast_mode = DUCKDB_V2_CAST_MODE_MAX_ENUM;

void FailCast(duckdb_v2_error_info_handle *err, DUCKDB_V2_ERROR code, const std::string &message) {
	duckdb_v2_error_info_set_code(*err, code);
	auto text_str = Convert(message);
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// Clears the output vector's validity bit for row `index`.
DUCKDB_V2_ERROR SetOutputNull(duckdb_v2_vector_handle output, idx_t index, duckdb_v2_error_info_handle *err) {
	uint64_t *validity = nullptr;
	auto rc = duckdb_v2_vector_flat_get_validity_mutable(output, &validity, err);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return rc;
	}
	validity[index / 64] &= ~(UINT64_C(1) << (index % 64));
	return DUCKDB_V2_ERROR_NONE;
}

// Pulls the three things every cast callback needs out of the info handle.
struct CastArgs {
	duckdb_v2_vector_handle input = nullptr;
	duckdb_v2_vector_handle output = nullptr;
	idx_t count = 0;
	duckdb_v2_vector_view view {};
};

bool ReadCastArgs(duckdb_v2_cast_function_exec_info_handle info, CastArgs &out, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_cast_function_exec_get_input(info, &out.input, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_cast_function_exec_get_output(info, &out.output, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_cast_function_exec_get_row_count(info, &out.count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_cast_function_exec_get_mode(info, &last_cast_mode, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_view(out.input, &out.view, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	return true;
}

// Parses "<digits>C" into an int32. Returns false on any malformed input.
bool ParseTemperature(const char *data, idx_t len, int32_t &out) {
	if (len < 2 || data[len - 1] != 'C') {
		return false;
	}
	idx_t i = data[0] == '-' ? 1 : 0;
	if (i == len - 1) {
		return false; // no digits before the 'C'
	}
	int64_t value = 0;
	for (; i < len - 1; i++) {
		if (data[i] < '0' || data[i] > '9') {
			return false;
		}
		value = value * 10 + (data[i] - '0');
	}
	out = static_cast<int32_t>(data[0] == '-' ? -value : value);
	return true;
}

// TEMPERATURE -> VARCHAR. Infallible: every input value formats.
void TempToVarchar(duckdb_v2_cast_function_exec_info_handle info, duckdb_v2_context_handle context,
                   duckdb_v2_error_info_handle *err) {
	if (!context) {
		FailCast(err, DUCKDB_V2_ERROR_INPUT_INVALID, "cast callback ran without a context");
		return;
	}
	CastArgs args;
	if (!ReadCastArgs(info, args, err)) {
		return;
	}
	const auto *in = static_cast<const int32_t *>(args.view.data);
	for (idx_t i = 0; i < args.count; i++) {
		auto idx = SelAt(args.view.sel, i);
		if (!RowValid(args.view, idx)) {
			if (SetOutputNull(args.output, i, err) != DUCKDB_V2_ERROR_NONE) {
				return;
			}
			continue;
		}
		auto formatted = std::to_string(in[idx]) + "C";
		if (V2VectorAssignString(args.output, i, formatted.data(), formatted.size(), err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
}

// VARCHAR -> TEMPERATURE. Fails on malformed input: the failing row is left NULL, which is what a TRY cast keeps,
// and the reported error is what a normal cast aborts on. Also checks that the user data was threaded through.
void VarcharToTemp(duckdb_v2_cast_function_exec_info_handle info, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	void *user_data = nullptr;
	if (duckdb_v2_cast_function_exec_get_user_data(info, &user_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *secret = static_cast<std::string *>(user_data);
	if (!secret || *secret != "secret") {
		FailCast(err, DUCKDB_V2_ERROR_INPUT_INVALID, "user data did not reach the cast callback");
		return;
	}

	CastArgs args;
	if (!ReadCastArgs(info, args, err)) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(args.output, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *in = static_cast<const duckdb_v2_bytes *>(args.view.data);
	auto *out = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < args.count; i++) {
		auto idx = SelAt(args.view.sel, i);
		if (!RowValid(args.view, idx)) {
			if (SetOutputNull(args.output, i, err) != DUCKDB_V2_ERROR_NONE) {
				return;
			}
			continue;
		}
		auto text = Convert(in[idx]);
		int32_t parsed = 0;
		if (!ParseTemperature(text.ptr, text.len, parsed)) {
			if (SetOutputNull(args.output, i, err) != DUCKDB_V2_ERROR_NONE) {
				return;
			}
			FailCast(err, DUCKDB_V2_ERROR_TYPE_CONVERSION, "Could not convert '" + Convert(text) + "' to TEMPERATURE");
			continue;
		}
		out[i] = parsed;
	}
}

void NoopCast(duckdb_v2_cast_function_exec_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

// Registers the TEMPERATURE custom type and hands back a logical type handle for it.
duckdb_v2_logical_type_handle RegisterTemperatureType(EnvFixture &fx, duckdb_v2_logical_type_handle integer) {
	duckdb_v2_custom_type_handle custom = nullptr;
	REQUIRE(duckdb_v2_custom_type_create(fx.factory, &custom, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = CastIdent("TEMPERATURE");
	REQUIRE(duckdb_v2_custom_type_set_name(custom, &name_str, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_custom_type_set_base_type(custom, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_custom_type(fx.conn, custom, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_custom_type_destroy(&custom);

	duckdb_v2_logical_type_handle temperature = nullptr;
	REQUIRE(duckdb_v2_factory_create_type_with_alias(fx.factory, integer, &name_str, &temperature, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	return temperature;
}

// Creates a cast function on the connection, configured but not yet registered.
duckdb_v2_cast_function_handle MakeCast(duckdb_v2_connection_handle conn, duckdb_v2_logical_type_handle source,
                                        duckdb_v2_logical_type_handle target,
                                        duckdb_v2_cast_function_exec_callback_fn callback) {
	duckdb_v2_cast_function_handle function = nullptr;
	REQUIRE(duckdb_v2_cast_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_source_type(function, source, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_target_type(function, target, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_exec_callback(function, callback, nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

void DestroySecret(void *data) {
	delete static_cast<std::string *>(data);
}

// Registers both TEMPERATURE casts, with `cost` as the VARCHAR -> TEMPERATURE implicit cast cost.
void RegisterTemperatureCasts(duckdb_v2_connection_handle conn, duckdb_v2_logical_type_handle temperature,
                              duckdb_v2_logical_type_handle varchar, int64_t cost) {
	auto to_varchar = MakeCast(conn, temperature, varchar, TempToVarchar);
	REQUIRE(duckdb_v2_connection_register_cast_function(conn, to_varchar, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_destroy(&to_varchar) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(to_varchar == nullptr);

	auto from_varchar = MakeCast(conn, varchar, temperature, VarcharToTemp);
	REQUIRE(duckdb_v2_cast_function_set_implicit_cast_cost(from_varchar, cost, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_opaque user_data = {new std::string("secret"), DestroySecret, nullptr};
	REQUIRE(duckdb_v2_cast_function_set_user_data(from_varchar, &user_data, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_cast_function(conn, from_varchar, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_cast_function_destroy(&from_varchar);
}

// Run a query producing a single VARCHAR cell.
std::string CastQueryText(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = Convert(Convert(static_cast<const duckdb_v2_bytes *>(view.data)[SelAt(view.sel, 0)]));
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// Runs a query to exhaustion, returning the code the failure surfaced with (execution is lazy, so a runtime
// failure only shows up while stepping).
DUCKDB_V2_ERROR CastQueryError(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	auto rc = Query(conn, sql, &result);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_result_destroy(&result);
		return rc;
	}
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	while (rc == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc != DUCKDB_V2_ERROR_NONE || status == DUCKDB_V2_RESULT_STEP_STATUS_FINISHED ||
		    status == DUCKDB_V2_RESULT_STEP_STATUS_CANCELLED) {
			break;
		}
		if (status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	return rc;
}

// out[i] = in[i]: an identity scalar function used to observe implicit argument conversion.
void IdentityExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle in = nullptr;
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_arg(info, 0, &in, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view view {};
	void *raw = nullptr;
	if (duckdb_v2_vector_get_view(in, &view, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *src = static_cast<const int32_t *>(view.data);
	auto *dst = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		dst[i] = src[SelAt(view.sel, i)];
	}
}

// Registers reading(t TEMPERATURE) -> INTEGER, whose single argument is what an implicit cast has to reach.
void RegisterReading(duckdb_v2_connection_handle conn, duckdb_v2_logical_type_handle temperature,
                     duckdb_v2_logical_type_handle integer) {
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name = Convert("reading");
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &name, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = CastIdent("t");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, temperature, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, IdentityExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
}

} // namespace

TEST_CASE("V2 cast: round-trip between a custom type and VARCHAR", "[capi_v2][cast_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	auto temperature = RegisterTemperatureType(fx, integer);
	RegisterTemperatureCasts(fx.conn, temperature, varchar, -1);
	duckdb_v2_logical_type_destroy(&temperature);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&integer);

	last_cast_mode = DUCKDB_V2_CAST_MODE_MAX_ENUM;
	REQUIRE(CastQueryText(fx.conn, "SELECT CAST(CAST(42 AS TEMPERATURE) AS VARCHAR)") == "42C");
	// An explicit CAST runs in normal mode.
	REQUIRE(last_cast_mode == DUCKDB_V2_CAST_MODE_NORMAL);
	REQUIRE(CastQueryText(fx.conn, "SELECT CAST(CAST(-7 AS TEMPERATURE) AS VARCHAR)") == "-7C");

	// And back the other way, through the base type so the result is readable as text.
	REQUIRE(CastQueryText(fx.conn, "SELECT CAST(CAST('100C' AS TEMPERATURE) AS INTEGER)::VARCHAR") == "100");
	REQUIRE(CastQueryText(fx.conn, "SELECT CAST(CAST('-5C' AS TEMPERATURE) AS INTEGER)::VARCHAR") == "-5");

	// NULLs reach the callback, which propagates them.
	REQUIRE(CastQueryText(fx.conn, "SELECT (CAST(CAST(NULL AS TEMPERATURE) AS VARCHAR) IS NULL)::VARCHAR") == "true");

	// Several rows in one batch, so the callback sees a full vector rather than a constant.
	REQUIRE(CastQueryText(fx.conn, "SELECT string_agg(CAST(CAST(t AS TEMPERATURE) AS VARCHAR), ',' ORDER BY t) "
	                               "FROM (VALUES (1), (2)) v(t)") == "1C,2C");
}

TEST_CASE("V2 cast: normal casts abort, try casts yield NULL", "[capi_v2][cast_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	auto temperature = RegisterTemperatureType(fx, integer);
	RegisterTemperatureCasts(fx.conn, temperature, varchar, -1);
	duckdb_v2_logical_type_destroy(&temperature);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&integer);

	// A normal cast: the callback's error aborts the query, and its code round-trips.
	last_cast_mode = DUCKDB_V2_CAST_MODE_MAX_ENUM;
	REQUIRE(CastQueryError(fx.conn, "SELECT CAST('not-a-temp' AS TEMPERATURE)") == DUCKDB_V2_ERROR_TYPE_CONVERSION);
	REQUIRE(last_cast_mode == DUCKDB_V2_CAST_MODE_NORMAL);

	// A try cast: the error is discarded and the row the callback left NULL is kept.
	last_cast_mode = DUCKDB_V2_CAST_MODE_MAX_ENUM;
	REQUIRE(CastQueryText(fx.conn, "SELECT (TRY_CAST('not-a-temp' AS TEMPERATURE) IS NULL)::VARCHAR") == "true");
	REQUIRE(last_cast_mode == DUCKDB_V2_CAST_MODE_TRY);

	// A try cast over valid input still converts.
	REQUIRE(CastQueryText(fx.conn, "SELECT CAST(TRY_CAST('12C' AS TEMPERATURE) AS INTEGER)::VARCHAR") == "12");
}

TEST_CASE("V2 cast: implicit cast cost governs argument conversion", "[capi_v2][cast_function]") {
	// A negative cost -- the default -- keeps the cast out of implicit conversion, so binding a VARCHAR
	// argument to a TEMPERATURE parameter fails. The argument has to come from a column: a string literal is
	// bound loosely and reaches any parameter type regardless of the cast's cost.
	{
		EnvFixture fx;
		auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
		auto temperature = RegisterTemperatureType(fx, integer);
		RegisterTemperatureCasts(fx.conn, temperature, varchar, -1);
		RegisterReading(fx.conn, temperature, integer);
		duckdb_v2_logical_type_destroy(&temperature);
		duckdb_v2_logical_type_destroy(&varchar);
		duckdb_v2_logical_type_destroy(&integer);

		REQUIRE(CastQueryText(fx.conn, "SELECT reading(CAST('30C' AS TEMPERATURE))::VARCHAR") == "30");
		REQUIRE(CastQueryError(fx.conn, "SELECT reading(v) FROM (VALUES ('30C')) t(v)") != DUCKDB_V2_ERROR_NONE);
	}

	// A non-negative cost makes the same cast available to the binder.
	{
		EnvFixture fx;
		auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
		auto temperature = RegisterTemperatureType(fx, integer);
		RegisterTemperatureCasts(fx.conn, temperature, varchar, 0);
		RegisterReading(fx.conn, temperature, integer);
		duckdb_v2_logical_type_destroy(&temperature);
		duckdb_v2_logical_type_destroy(&varchar);
		duckdb_v2_logical_type_destroy(&integer);

		REQUIRE(CastQueryText(fx.conn, "SELECT reading(v)::VARCHAR FROM (VALUES ('30C')) t(v)") == "30");
	}
}

TEST_CASE("V2 cast: registration refusals", "[capi_v2][cast_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto varchar = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// Nothing configured at all, then each missing piece in turn.
	{
		duckdb_v2_cast_function_handle function = nullptr;
		REQUIRE(duckdb_v2_cast_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);

		REQUIRE(duckdb_v2_cast_function_set_source_type(function, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);

		REQUIRE(duckdb_v2_cast_function_set_target_type(function, varchar, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);

		REQUIRE(duckdb_v2_cast_function_set_exec_callback(function, NoopCast, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_cast_function_destroy(&function);
	}

	// ANY is a signature wildcard, not a cast endpoint -- on either side.
	{
		auto function = MakeCast(fx.conn, any, varchar, NoopCast);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_cast_function_destroy(&function);
	}
	{
		auto function = MakeCast(fx.conn, varchar, any, NoopCast);
		REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_cast_function_destroy(&function);
	}

	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&varchar);
	duckdb_v2_logical_type_destroy(&any);
}

TEST_CASE("V2 cast: null arguments and destroy null-safety", "[capi_v2][cast_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	duckdb_v2_cast_function_handle function = nullptr;
	REQUIRE(duckdb_v2_cast_function_create(nullptr, &function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(function == nullptr);
	REQUIRE(duckdb_v2_cast_function_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_cast_function(nullptr, function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_cast_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_set_source_type(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_source_type(nullptr, integer, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_target_type(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_target_type(nullptr, integer, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_implicit_cast_cost(nullptr, 0, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_user_data(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_set_exec_callback(nullptr, NoopCast, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_cast_function(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	// The exec accessors reject a null info handle and a null out-parameter alike.
	void *data = nullptr;
	idx_t count = 0;
	duckdb_v2_vector_handle vector = nullptr;
	auto mode = DUCKDB_V2_CAST_MODE_NORMAL;
	REQUIRE(duckdb_v2_cast_function_exec_get_user_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_exec_get_row_count(nullptr, &count, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_exec_get_input(nullptr, &vector, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_exec_get_output(nullptr, &vector, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_cast_function_exec_get_mode(nullptr, &mode, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_cast_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(function == nullptr);
	REQUIRE(duckdb_v2_cast_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_cast_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_logical_type_destroy(&integer);
}

} // namespace test_capi_v2
