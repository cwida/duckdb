#include "test_capi_v2.hpp"

#include <cstring>
#include <string>
#include <vector>

// ---------------------------------------------------------------------------
// V2 scalar function tests: build a function on a connection, configure its
// signature, register it, and call it through SQL.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error that the
// test asserts on. Cross-callback observations are latched into file-scope
// statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t Ident(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// Create a scalar function on the connection with the given name.
duckdb_v2_scalar_function_handle MakeScalar(duckdb_v2_connection_handle conn, const char *name) {
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto str = Convert(name);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &str, nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

// The function's borrowed signature.
duckdb_v2_function_signature_handle SigOf(duckdb_v2_scalar_function_handle function) {
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(sig != nullptr);
	return sig;
}

void SigParam(duckdb_v2_function_signature_handle sig, const char *name, duckdb_v2_logical_type_handle type,
              duckdb_v2_value_handle default_value = nullptr) {
	auto name_str = Ident(name);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, type, default_value,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
}

// Run a query producing a single INTEGER cell.
int32_t QueryI32(duckdb_v2_connection_handle conn, const char *sql) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int32_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// ---------------------------------------------------------------------------
// Exec callbacks
// ---------------------------------------------------------------------------

// out[i] = a[i] + b[i]
void AddExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
             duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle lhs = nullptr;
	duckdb_v2_vector_handle rhs = nullptr;
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_arg(info, 0, &lhs, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_arg(info, 1, &rhs, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view lhs_view {};
	duckdb_v2_vector_view rhs_view {};
	if (duckdb_v2_vector_get_view(lhs, &lhs_view, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_view(rhs, &rhs_view, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *lhs_data = static_cast<const int32_t *>(lhs_view.data);
	const auto *rhs_data = static_cast<const int32_t *>(rhs_view.data);
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[i] = lhs_data[SelAt(lhs_view.sel, i)] + rhs_data[SelAt(rhs_view.sel, i)];
	}
}

// out[i] = sum over every argument vector: exercises the variadic tail via
// exec_get_arg_count, which splits the args list into its fixed and variadic part.
void VarargSumExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle out = nullptr;
	if (duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	idx_t fixed_count = 0;
	idx_t variadic_count = 0;
	if (duckdb_v2_scalar_function_exec_get_arg_count(info, &fixed_count, &variadic_count, nullptr, nullptr, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto arg_count = fixed_count + variadic_count;
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t col = 0; col < arg_count; col++) {
		duckdb_v2_vector_handle vec = nullptr;
		if (duckdb_v2_scalar_function_exec_get_arg(info, col, &vec, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		duckdb_v2_vector_view view {};
		if (duckdb_v2_vector_get_view(vec, &view, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		const auto *data = static_cast<const int32_t *>(view.data);
		for (idx_t i = 0; i < count; i++) {
			const auto value = data[SelAt(view.sel, i)];
			out_data[i] = col == 0 ? value : out_data[i] + value;
		}
	}
	// The count is the exclusive bound: index `count` is refused.
	duckdb_v2_vector_handle past_the_end = nullptr;
	if (duckdb_v2_scalar_function_exec_get_arg(info, arg_count, &past_the_end, nullptr) !=
	    DUCKDB_V2_ERROR_INPUT_INVALID) {
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_API);
		auto text_str = Convert("past-the-end argument vector index was not refused");
		duckdb_v2_error_info_set_text(*err, &text_str);
	}
}

void NoopExec(duckdb_v2_scalar_function_exec_info_handle, duckdb_v2_context_handle, duckdb_v2_error_info_handle *) {
}

// Fails the query through the callback's error slot.
void FailingExec(duckdb_v2_scalar_function_exec_info_handle, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("scalar exec failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// ---------------------------------------------------------------------------
// Bind / init / exec data-flow probes. The bind callback resolves an ANY
// return type to INTEGER and plants bind data; init plants init data; exec
// doubles its input and latches every pointer it saw.
// ---------------------------------------------------------------------------

int bind_marker = 0;
int init_marker = 0;
struct {
	void *user_data_in_bind = &bind_marker; // expected to arrive as nullptr
	void *bind_data_in_init = nullptr;
	void *bind_data_in_exec = nullptr;
	void *init_data_in_exec = nullptr;
	int bind_data_destroys = 0;
	int init_data_destroys = 0;
} flow;

void FlowDestroyBindData(void *) {
	flow.bind_data_destroys++;
}
void FlowDestroyInitData(void *) {
	flow.init_data_destroys++;
}

// Resolve the declared ANY return type to a concrete INTEGER.
void IntegerResolveTypes(duckdb_v2_function_bind_info_handle,
                         duckdb_v2_scalar_function_resolve_types_info_handle result, duckdb_v2_context_handle context,
                         duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_scalar_function_resolve_types_set_return_type(result, integer, err);
	duckdb_v2_logical_type_destroy(&integer);
}

void FlowBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_scalar_function_bind_info_handle result,
              duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_function_bind_get_user_data(info, &flow.user_data_in_bind, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {&bind_marker, FlowDestroyBindData, nullptr};
	duckdb_v2_scalar_function_bind_set_bind_data(result, &bind_data, err);
}

void FlowInit(duckdb_v2_scalar_function_init_info_handle info, duckdb_v2_context_handle,
              duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_scalar_function_init_get_bind_data(info, &flow.bind_data_in_init, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque init_data = {&init_marker, FlowDestroyInitData, nullptr};
	duckdb_v2_scalar_function_init_set_init_data(info, &init_data, err);
}

void FlowExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
              duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_scalar_function_exec_get_bind_data(info, &flow.bind_data_in_exec, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_init_data(info, &flow.init_data_in_exec, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_handle src = nullptr;
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_arg(info, 0, &src, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view view {};
	void *raw = nullptr;
	if (duckdb_v2_vector_get_view(src, &view, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *data = static_cast<const int32_t *>(view.data);
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[i] = data[SelAt(view.sel, i)] * 2;
	}
}

// ---------------------------------------------------------------------------
// Bind-time argument introspection. The bind callback reads the argument
// count, the resolved type of every argument and the constant folded out of
// the second one, then resolves the ANY return type to INTEGER. Exec fills
// the result with the constant bind saw.
// ---------------------------------------------------------------------------

struct {
	idx_t arg_count = 0;
	DUCKDB_V2_LOGICAL_TYPE_ID arg_types[2] = {DUCKDB_V2_LOGICAL_TYPE_ID_INVALID, DUCKDB_V2_LOGICAL_TYPE_ID_INVALID};
	int32_t constant = 0;
	// Out-of-range probes, latched with their own (null) error slot.
	DUCKDB_V2_ERROR oob_type_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_value_rc = DUCKDB_V2_ERROR_NONE;
	bool oob_type_cleared = false;
	bool oob_value_cleared = false;
} arg_probe;

void ArgProbeBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_scalar_function_bind_info_handle,
                  duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	// Without "*args" the fixed part is the whole list, so the variadic part is not asked for.
	if (duckdb_v2_function_bind_get_arg_count(info, &arg_probe.arg_count, nullptr, nullptr, nullptr, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < arg_probe.arg_count && i < 2; i++) {
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_function_bind_get_arg_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &arg_probe.arg_types[i], err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
	// An index past the last argument is an input error, and leaves the out-parameter cleared.
	duckdb_v2_logical_type_handle oob_type = nullptr;
	duckdb_v2_value_handle oob_value = nullptr;
	arg_probe.oob_type_rc = duckdb_v2_function_bind_get_arg_type(info, 5, &oob_type, nullptr);
	arg_probe.oob_value_rc = duckdb_v2_function_bind_get_arg_value(info, 5, &oob_value, nullptr);
	arg_probe.oob_type_cleared = oob_type == nullptr;
	arg_probe.oob_value_cleared = oob_value == nullptr;

	// Fold the second argument to a constant. A non-constant argument fails here.
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 1, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_value_get_int(value, &arg_probe.constant, err);
	duckdb_v2_value_destroy(&value);
}

// ---------------------------------------------------------------------------
// Resolve types sees the argument types as the caller passed them, bind sees
// the parameter types they were cast to. Only bind can set bind data.
// ---------------------------------------------------------------------------

struct {
	DUCKDB_V2_LOGICAL_TYPE_ID resolve_arg_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	DUCKDB_V2_LOGICAL_TYPE_ID bind_arg_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	DUCKDB_V2_ERROR resolve_set_bind_data_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR bind_shared_set_bind_data_rc = DUCKDB_V2_ERROR_NONE;
} phase_probe;

DUCKDB_V2_LOGICAL_TYPE_ID GetArgTypeId(duckdb_v2_function_bind_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_logical_type_handle type = nullptr;
	auto id = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	if (duckdb_v2_function_bind_get_arg_type(info, 0, &type, err) == DUCKDB_V2_ERROR_NONE) {
		duckdb_v2_logical_type_get_id(type, &id, err);
		duckdb_v2_logical_type_destroy(&type);
	}
	return id;
}

void PhaseProbeResolveTypes(duckdb_v2_function_bind_info_handle info,
                            duckdb_v2_scalar_function_resolve_types_info_handle, duckdb_v2_context_handle,
                            duckdb_v2_error_info_handle *err) {
	const auto arg_type = GetArgTypeId(info, err);
	// Plan verification can rebind the function with already-cast arguments.
	if (phase_probe.resolve_arg_type == DUCKDB_V2_LOGICAL_TYPE_ID_INVALID) {
		phase_probe.resolve_arg_type = arg_type;
	}
	duckdb_v2_opaque bind_data = {&bind_marker, nullptr, nullptr};
	phase_probe.resolve_set_bind_data_rc = duckdb_v2_function_bind_set_bind_data(info, &bind_data, nullptr);
}

void PhaseProbeBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_scalar_function_bind_info_handle,
                    duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	phase_probe.bind_arg_type = GetArgTypeId(info, err);
	duckdb_v2_opaque bind_data = {&bind_marker, nullptr, nullptr};
	phase_probe.bind_shared_set_bind_data_rc = duckdb_v2_function_bind_set_bind_data(info, &bind_data, nullptr);
}

// out[i] = the constant the bind callback folded out of the second argument.
void ArgProbeExec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                  duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[i] = arg_probe.constant;
	}
}

} // namespace

// ===========================================================================
// Register on a connection and call through SQL.
// ===========================================================================

TEST_CASE("V2 scalar: register on connection and execute", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	auto function = MakeScalar(fx.conn, "my_add");
	auto sig = SigOf(function);
	SigParam(sig, "a", integer);
	SigParam(sig, "b", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, AddExec, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(function == nullptr);
	duckdb_v2_logical_type_destroy(&integer);

	REQUIRE(QueryI32(fx.conn, "SELECT my_add(4, 5)") == 9);
	// More rows than one chunk, so the callback sees full vectors.
	REQUIRE(QueryI32(fx.conn, "SELECT sum(my_add(r::INTEGER, 1))::INTEGER FROM range(5000) t(r)") ==
	        static_cast<int32_t>(5000LL * 4999 / 2 + 5000));
}

TEST_CASE("V2 scalar: parameter defaults and named-argument calls", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	// defsum(a INTEGER, b INTEGER DEFAULT 5) -> a + b
	auto function = MakeScalar(fx.conn, "defsum");
	auto sig = SigOf(function);
	auto five = MakeInt32Value(fx.factory, 5);
	SigParam(sig, "a", integer);
	SigParam(sig, "b", integer, five);
	duckdb_v2_value_destroy(&five);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, AddExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	// Omitted default -> b is 5; named arguments resolve to the signature's
	// parameter names.
	REQUIRE(QueryI32(fx.conn, "SELECT defsum(10)") == 15);
	REQUIRE(QueryI32(fx.conn, "SELECT defsum(10, 20)") == 30);
	REQUIRE(QueryI32(fx.conn, "SELECT defsum(a := 7)") == 12);
	REQUIRE(QueryI32(fx.conn, "SELECT defsum(a := 7, b := 8)") == 15);
}

TEST_CASE("V2 scalar: variadic tail", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	// vsum(INTEGER...) -> INTEGER
	auto function = MakeScalar(fx.conn, "vsum");
	auto sig = SigOf(function);
	auto args_ident = Ident("args");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &args_ident, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, VarargSumExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	REQUIRE(QueryI32(fx.conn, "SELECT vsum(1, 2, 3)") == 6);
	REQUIRE(QueryI32(fx.conn, "SELECT vsum(42)") == 42);
	// without "**kwargs", a named argument that matches no parameter is rejected
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT vsum(1, x := 2)", &result) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&result);
}

TEST_CASE("V2 scalar: invalid parameter name preserves variadic tail", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto function = MakeScalar(fx.conn, "vsum_retry");
	auto sig = SigOf(function);

	// "*args" is added before the standard parameter: the signature orders parameters by kind
	auto args_str = Ident("args");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &args_str, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	auto name_str = Ident("\x80");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, integer, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	SigParam(sig, "first", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, VarargSumExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	REQUIRE(QueryI32(fx.conn, "SELECT vsum_retry(1, 2, 3)") == 6);
}

// ===========================================================================
// Resolve types resolves an ANY return type; bind/init/exec data flows through.
// ===========================================================================

TEST_CASE("V2 scalar: resolve types callback resolves ANY return and data flows", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	auto function = MakeScalar(fx.conn, "any_double");
	auto sig = SigOf(function);
	SigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_resolve_types_callback(function, IntegerResolveTypes, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_bind_callback(function, FlowBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_init_callback(function, FlowInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, FlowExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);

	flow = {};
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT any_double(21) AS d", &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	// The resolve types callback resolved the ANY return type to INTEGER.
	RequireColumn(result, 0, "d", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(static_cast<const int32_t *>(view.data)[SelAt(view.sel, 0)] == 42);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);

	// No user data was set, so bind saw nullptr; the planted pointers flowed
	// from bind to init to exec.
	REQUIRE(flow.user_data_in_bind == nullptr);
	REQUIRE(flow.bind_data_in_init == &bind_marker);
	REQUIRE(flow.bind_data_in_exec == &bind_marker);
	REQUIRE(flow.init_data_in_exec == &init_marker);
	// The init data (per-execution) was destroyed with the finished query.
	REQUIRE(flow.init_data_destroys >= 1);
}

// ===========================================================================
// Bind-time argument introspection: count, types, constant folding.
// ===========================================================================

TEST_CASE("V2 scalar: bind reads argument count, types and constants", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// arg_probe(x ANY, y INTEGER) -> ANY
	auto function = MakeScalar(fx.conn, "arg_probe");
	auto sig = SigOf(function);
	SigParam(sig, "x", any);
	SigParam(sig, "y", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_resolve_types_callback(function, IntegerResolveTypes, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_bind_callback(function, ArgProbeBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, ArgProbeExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);

	arg_probe = {};
	REQUIRE(QueryI32(fx.conn, "SELECT arg_probe('hello', 21)") == 21);
	REQUIRE(arg_probe.arg_count == 2);
	// The ANY parameter reports the type the call resolved it to.
	REQUIRE(arg_probe.arg_types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(arg_probe.arg_types[1] == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(arg_probe.constant == 21);
	REQUIRE(arg_probe.oob_type_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(arg_probe.oob_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(arg_probe.oob_type_cleared);
	REQUIRE(arg_probe.oob_value_cleared);

	// A foldable expression is still a constant.
	arg_probe = {};
	REQUIRE(QueryI32(fx.conn, "SELECT arg_probe(42, 20 + 1)") == 21);
	REQUIRE(arg_probe.arg_types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(arg_probe.constant == 21);

	// A column reference is not: the binder error surfaces from the bind callback.
	arg_probe = {};
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT arg_probe('hello', i) FROM (VALUES (21)) t(i)", &result) ==
	        DUCKDB_V2_ERROR_QUERY_BINDER);
	duckdb_v2_result_destroy(&result);
}

// ===========================================================================
// Resolve types runs before the arguments are cast, bind after.
// ===========================================================================

TEST_CASE("V2 scalar: resolve types sees the uncast argument types, bind the cast ones", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	// phase_probe(x INTEGER) -> INTEGER
	auto function = MakeScalar(fx.conn, "phase_probe");
	auto sig = SigOf(function);
	SigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_resolve_types_callback(function, PhaseProbeResolveTypes, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_bind_callback(function, PhaseProbeBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, ArgProbeExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	phase_probe = {};
	arg_probe = {};
	REQUIRE(QueryI32(fx.conn, "SELECT phase_probe(1::SMALLINT)") == 0);
	REQUIRE(phase_probe.resolve_arg_type == DUCKDB_V2_LOGICAL_TYPE_ID_SMALLINT);
	REQUIRE(phase_probe.bind_arg_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	// Scalar functions set the bind data through their own bind info, in the bind callback only.
	REQUIRE(phase_probe.resolve_set_bind_data_rc != DUCKDB_V2_ERROR_NONE);
	REQUIRE(phase_probe.bind_shared_set_bind_data_rc != DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Errors
// ===========================================================================

// An error set in the exec callback's slot fails the query with its code.
TEST_CASE("V2 scalar: exec error propagates to the result", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	auto function = MakeScalar(fx.conn, "always_fails");
	auto sig = SigOf(function);
	SigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, FailingExec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT always_fails(1)", &result) == DUCKDB_V2_ERROR_NONE);
	// Execution is lazy: the failure surfaces while stepping.
	auto rc = DUCKDB_V2_ERROR_NONE;
	auto status = DUCKDB_V2_RESULT_STEP_STATUS_WAITING;
	for (int i = 0; i < 100000 && rc == DUCKDB_V2_ERROR_NONE && status != DUCKDB_V2_RESULT_STEP_STATUS_FINISHED; i++) {
		duckdb_v2_data_chunk_handle chunk = nullptr;
		rc = duckdb_v2_result_step(result, &chunk, &status, nullptr);
		if (chunk) {
			duckdb_v2_data_chunk_destroy(&chunk);
		}
		if (rc == DUCKDB_V2_ERROR_NONE && status == DUCKDB_V2_RESULT_STEP_STATUS_WAITING) {
			rc = duckdb_v2_result_wait(result, nullptr);
		}
	}
	duckdb_v2_result_destroy(&result);
	// The callback's code round-trips through the engine's exception machinery.
	REQUIRE(rc == DUCKDB_V2_ERROR_IO_GENERAL);
}

TEST_CASE("V2 scalar: registration refusals", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// No name.
	{
		duckdb_v2_scalar_function_handle function = nullptr;
		REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto sig = SigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// No exec callback.
	{
		auto function = MakeScalar(fx.conn, "no_exec");
		auto sig = SigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// No return type set at all.
	{
		auto function = MakeScalar(fx.conn, "no_return");
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// An ANY return type without a resolve types callback to resolve it.
	{
		auto function = MakeScalar(fx.conn, "any_no_resolve");
		auto sig = SigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// A bind callback cannot resolve an ANY return type.
	{
		auto function = MakeScalar(fx.conn, "any_bind_only");
		auto sig = SigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_set_bind_callback(function, FlowBind, nullptr);
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// Duplicate parameter names are rejected by signature verification.
	{
		auto function = MakeScalar(fx.conn, "dup_names");
		auto sig = SigOf(function);
		SigParam(sig, "x", integer);
		SigParam(sig, "x", integer);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	// A required parameter after a defaulted one.
	{
		auto function = MakeScalar(fx.conn, "bad_default_order");
		auto sig = SigOf(function);
		auto five = MakeInt32Value(fx.factory, 5);
		SigParam(sig, "a", integer, five);
		SigParam(sig, "b", integer);
		duckdb_v2_value_destroy(&five);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_set_exec_callback(function, NoopExec, nullptr);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}

	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);
}

TEST_CASE("V2 scalar: null arguments and destroy null-safety", "[capi_v2][scalar_function]") {
	EnvFixture fx;

	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(nullptr, &function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(function == nullptr);
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_name(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto invalid_name = Convert("\x80");
	REQUIRE(duckdb_v2_scalar_function_set_name(function, &invalid_name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_signature(nullptr, &sig, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_scalar_function_destroy(&function);

	REQUIRE(duckdb_v2_scalar_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_handle null_function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_destroy(&null_function) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Function properties.
// ===========================================================================

namespace {

// out[i] = 42, regardless of the input.
void Const42Exec(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                 duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	if (duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[i] = 42;
	}
}

} // namespace

TEST_CASE("V2 scalar: function properties", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	// SPECIAL null handling: the exec callback runs even for a NULL argument and produces a value.
	auto function = MakeScalar(fx.conn, "always42");
	auto sig = SigOf(function);
	SigParam(sig, "a", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, Const42Exec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING_SPECIAL,
	                                               nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_VOLATILE,
	                                               nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);

	// With default null handling this would be NULL without invoking the callback.
	REQUIRE(QueryI32(fx.conn, "SELECT COALESCE(always42(NULL::INTEGER), -1)") == 42);

	// The catalog reports CONSISTENT_WITHIN_QUERY, and index keys cannot contain the function.
	function = MakeScalar(fx.conn, "prop_query");
	sig = SigOf(function);
	SigParam(sig, "a", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, Const42Exec, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_CONSISTENT_WITHIN_QUERY,
	                                               nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);

	REQUIRE(QueryI32(fx.conn, "SELECT prop_query(1)") == 42);
	REQUIRE(QueryI32(fx.conn, "SELECT count(*)::INTEGER FROM duckdb_functions() WHERE function_name = 'prop_query' "
	                          "AND stability = 'CONSISTENT_WITHIN_QUERY'") == 1);
	REQUIRE(RunSql(fx.conn, "CREATE TABLE prop_keys (a INTEGER)") == DUCKDB_V2_ERROR_NONE);
	REQUIRE(RunSql(fx.conn, "CREATE INDEX prop_index ON prop_keys ((prop_query(a)))") == DUCKDB_V2_ERROR_QUERY_BINDER);

	// Invalid combinations are rejected.
	function = MakeScalar(fx.conn, "prop_errors");
	// A value that does not belong to the key.
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_NULL_HANDLING_SPECIAL,
	                                               nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// An aggregate-only key on a scalar function.
	REQUIRE(duckdb_v2_scalar_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_AGG_ORDER_DEPENDENT,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_AGG_ORDER_DEPENDENT_NO,
	                                               nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// An unknown key in the COMMON group.
	REQUIRE(duckdb_v2_scalar_function_set_property(function, static_cast<DUCKDB_V2_FUNCTION_PROPERTY_KEY>(0x01FF00),
	                                               static_cast<DUCKDB_V2_FUNCTION_PROPERTY_VALUE>(0x01FF00),
	                                               nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// A null function handle.
	REQUIRE(duckdb_v2_scalar_function_set_property(nullptr, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                               DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_VOLATILE,
	                                               nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
}

namespace {

// ---------------------------------------------------------------------------
// kw_sum(x, /, *args, scale := 1, **kwargs) = (x + sum(args) + sum(kwargs)) * scale.
// Bind latches the four parts of the argument list and every argument's name, and resolves "scale" to its index;
// exec reads the arguments at the indices bind saw.
// ---------------------------------------------------------------------------

struct KindProbe {
	idx_t positional_fixed = 0;
	idx_t positional_variadic = 0;
	idx_t named_fixed = 0;
	idx_t named_variadic = 0;
	// Every out-parameter of the count is optional
	DUCKDB_V2_ERROR null_counts_rc = DUCKDB_V2_ERROR_API;
	std::vector<std::string> names;
	idx_t scale_index = 0;
	bool missing_found = true;
	bool x_found = true;
	idx_t x_index = 0;
	DUCKDB_V2_LOGICAL_TYPE_ID scale_type = DUCKDB_V2_LOGICAL_TYPE_ID_INVALID;
	bool scale_is_constant = false;
	int32_t scale_constant = 0;
	DUCKDB_V2_ERROR oob_name_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_type_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_value_rc = DUCKDB_V2_ERROR_NONE;
	// Exec-side: the four parts, and whether an index past the last argument was refused
	idx_t exec_counts[4] = {};
	bool exec_oob_refused = false;
};
KindProbe kind_probe;

struct KindBind {
	idx_t scale_index = 0;
};

void DeleteKindBind(void *ptr) {
	delete static_cast<KindBind *>(ptr);
}

void KindBindCb(duckdb_v2_function_bind_info_handle info, duckdb_v2_scalar_function_bind_info_handle result,
                duckdb_v2_context_handle, duckdb_v2_error_info_handle *err) {
	kind_probe = KindProbe {};
	if (duckdb_v2_function_bind_get_arg_count(info, &kind_probe.positional_fixed, &kind_probe.positional_variadic,
	                                          &kind_probe.named_fixed, &kind_probe.named_variadic,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto arg_count = kind_probe.positional_fixed + kind_probe.positional_variadic + kind_probe.named_fixed +
	                       kind_probe.named_variadic;
	kind_probe.null_counts_rc =
	    duckdb_v2_function_bind_get_arg_count(info, nullptr, nullptr, nullptr, nullptr, nullptr);

	for (idx_t i = 0; i < arg_count; i++) {
		duckdb_v2_identifier_t name {};
		if (duckdb_v2_function_bind_get_arg_name(info, i, &name, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		kind_probe.names.push_back(Convert(name));
	}

	bool found = false;
	auto scale_ident = Ident("SCALE");
	if (duckdb_v2_function_bind_get_arg_index(info, &scale_ident, &kind_probe.scale_index, &found, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	if (!found) {
		SetErrorInfo(err, DUCKDB_V2_ERROR_API, "the named-only parameter was not found");
		return;
	}
	idx_t missing_index = 0;
	auto missing_ident = Ident("missing");
	auto x_ident = Ident("x");
	if (duckdb_v2_function_bind_get_arg_index(info, &missing_ident, &missing_index, &kind_probe.missing_found, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_function_bind_get_arg_index(info, &x_ident, &kind_probe.x_index, &kind_probe.x_found, err) !=
	        DUCKDB_V2_ERROR_NONE) {
		return;
	}

	duckdb_v2_logical_type_handle type = nullptr;
	if (duckdb_v2_function_bind_get_arg_type(info, kind_probe.scale_index, &type, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto rc = duckdb_v2_logical_type_get_id(type, &kind_probe.scale_type, err);
	duckdb_v2_logical_type_destroy(&type);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// A column passed as "scale" has no constant value: that is reported, not a failure of the call.
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, kind_probe.scale_index, &value, nullptr) == DUCKDB_V2_ERROR_NONE) {
		kind_probe.scale_is_constant = true;
		rc = duckdb_v2_value_get_int(value, &kind_probe.scale_constant, err);
		duckdb_v2_value_destroy(&value);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}

	// An index past the last argument is an input error.
	duckdb_v2_identifier_t oob_name {};
	duckdb_v2_logical_type_handle oob_type = nullptr;
	duckdb_v2_value_handle oob_value = nullptr;
	kind_probe.oob_name_rc = duckdb_v2_function_bind_get_arg_name(info, arg_count, &oob_name, nullptr);
	kind_probe.oob_type_rc = duckdb_v2_function_bind_get_arg_type(info, arg_count, &oob_type, nullptr);
	kind_probe.oob_value_rc = duckdb_v2_function_bind_get_arg_value(info, arg_count, &oob_value, nullptr);

	duckdb_v2_opaque bind_data = {new KindBind {kind_probe.scale_index}, DeleteKindBind, nullptr};
	duckdb_v2_scalar_function_bind_set_bind_data(result, &bind_data, err);
}

// Adds the rows of an INTEGER vector into out, or multiplies out by them.
bool AccumulateI32(duckdb_v2_vector_handle vec, int32_t *out, idx_t count, bool multiply,
                   duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_view view {};
	if (duckdb_v2_vector_get_view(vec, &view, err) != DUCKDB_V2_ERROR_NONE) {
		return false;
	}
	const auto *data = static_cast<const int32_t *>(view.data);
	for (idx_t i = 0; i < count; i++) {
		const auto value = data[SelAt(view.sel, i)];
		out[i] = multiply ? out[i] * value : out[i] + value;
	}
	return true;
}

void KindExecCb(duckdb_v2_scalar_function_exec_info_handle info, duckdb_v2_context_handle,
                duckdb_v2_error_info_handle *err) {
	void *bind_ptr = nullptr;
	duckdb_v2_vector_handle out = nullptr;
	idx_t count = 0;
	auto &counts = kind_probe.exec_counts;
	if (duckdb_v2_scalar_function_exec_get_bind_data(info, &bind_ptr, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_result(info, &out, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_scalar_function_exec_get_arg_count(info, &counts[0], &counts[1], &counts[2], &counts[3], err) !=
	        DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto arg_count = counts[0] + counts[1] + counts[2] + counts[3];
	const auto scale_index = static_cast<KindBind *>(bind_ptr)->scale_index;
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(out, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[i] = 0;
	}
	for (idx_t col = 0; col < arg_count; col++) {
		duckdb_v2_vector_handle vec = nullptr;
		if (col != scale_index &&
		    (duckdb_v2_scalar_function_exec_get_arg(info, col, &vec, err) != DUCKDB_V2_ERROR_NONE ||
		     !AccumulateI32(vec, out_data, count, false, err))) {
			return;
		}
	}
	duckdb_v2_vector_handle scale = nullptr;
	if (duckdb_v2_scalar_function_exec_get_arg(info, scale_index, &scale, err) != DUCKDB_V2_ERROR_NONE ||
	    !AccumulateI32(scale, out_data, count, true, err)) {
		return;
	}
	duckdb_v2_vector_handle past_the_end = nullptr;
	kind_probe.exec_oob_refused = duckdb_v2_scalar_function_exec_get_arg(info, arg_count, &past_the_end, nullptr) ==
	                              DUCKDB_V2_ERROR_INPUT_INVALID;
}

void AddKindParam(duckdb_v2_function_signature_handle sig, const char *name, duckdb_v2_logical_type_handle type,
                  DUCKDB_V2_FUNCTION_PARAMETER_KIND kind, duckdb_v2_value_handle default_value = nullptr) {
	auto name_ident = Ident(name);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_ident, type, default_value, kind, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
}

using Counts = std::vector<idx_t>;
using Names = std::vector<std::string>;

Counts BindCounts() {
	return {kind_probe.positional_fixed, kind_probe.positional_variadic, kind_probe.named_fixed,
	        kind_probe.named_variadic};
}

Counts ExecCounts() {
	return {kind_probe.exec_counts[0], kind_probe.exec_counts[1], kind_probe.exec_counts[2], kind_probe.exec_counts[3]};
}

} // namespace

// ===========================================================================
// Parameter kinds: the arguments reach bind and exec as one list in four parts.
// ===========================================================================

TEST_CASE("V2 scalar: parameter kinds lay out the argument list", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto one = MakeInt32Value(fx.factory, 1);

	auto function = MakeScalar(fx.conn, "kw_sum");
	auto sig = SigOf(function);
	// Added out of order: the signature orders the parameters by kind.
	AddKindParam(sig, "kwargs", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_VARIADIC);
	AddKindParam(sig, "scale", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY, one);
	AddKindParam(sig, "args", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC);
	AddKindParam(sig, "x", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_ONLY);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_bind_callback(function, KindBindCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, KindExecCb, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_scalar_function_destroy(&function);
	duckdb_v2_value_destroy(&one);
	duckdb_v2_logical_type_destroy(&integer);

	SECTION("an omitted named-only parameter carries its default") {
		REQUIRE(QueryI32(fx.conn, "SELECT kw_sum(4)") == 4);
		REQUIRE(BindCounts() == Counts {1, 0, 1, 0});
		REQUIRE(kind_probe.null_counts_rc == DUCKDB_V2_ERROR_NONE);
		REQUIRE(kind_probe.names == Names {"x", "scale"});
		REQUIRE(kind_probe.scale_index == 1);
		REQUIRE_FALSE(kind_probe.missing_found);
		// A caller cannot pass a positional-only parameter by name, so the lookup skips it.
		REQUIRE_FALSE(kind_probe.x_found);
		REQUIRE(kind_probe.scale_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
		REQUIRE(kind_probe.scale_is_constant);
		REQUIRE(kind_probe.scale_constant == 1);
		REQUIRE(kind_probe.oob_name_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(kind_probe.oob_type_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(kind_probe.oob_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
		REQUIRE(ExecCounts() == Counts {1, 0, 1, 0});
		REQUIRE(kind_probe.exec_oob_refused);
	}
	SECTION("extra positional arguments go to *args, after the positional parameters") {
		REQUIRE(QueryI32(fx.conn, "SELECT kw_sum(1, 2, 3, scale := 10)") == 60);
		REQUIRE(BindCounts() == Counts {1, 2, 1, 0});
		// An argument *args received has no name.
		REQUIRE(kind_probe.names == Names {"x", "", "", "scale"});
		REQUIRE(kind_probe.scale_index == 3);
		REQUIRE(kind_probe.scale_constant == 10);
		REQUIRE(ExecCounts() == Counts {1, 2, 1, 0});
	}
	SECTION("unmatched names go to **kwargs, after the named-only parameters, in call order") {
		REQUIRE(QueryI32(fx.conn, "SELECT kw_sum(1, extra := 5, scale := 2, more := 6)") == 24);
		REQUIRE(BindCounts() == Counts {1, 0, 1, 2});
		REQUIRE(kind_probe.names == Names {"x", "scale", "extra", "more"});
		REQUIRE(ExecCounts() == Counts {1, 0, 1, 2});
	}
	SECTION("the name of a positional-only parameter goes to **kwargs, where the lookup finds it") {
		REQUIRE(QueryI32(fx.conn, "SELECT kw_sum(1, x := 5)") == 6);
		REQUIRE(BindCounts() == Counts {1, 0, 1, 1});
		REQUIRE(kind_probe.names == Names {"x", "scale", "x"});
		REQUIRE(kind_probe.x_found);
		REQUIRE(kind_probe.x_index == 2);
	}
	SECTION("a named argument can be a column") {
		REQUIRE(QueryI32(fx.conn, "SELECT sum(kw_sum(r, scale := r))::INTEGER FROM (SELECT range::INTEGER r "
		                          "FROM range(4))") == 14);
		REQUIRE_FALSE(kind_probe.scale_is_constant);
		REQUIRE(kind_probe.scale_type == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	}
	SECTION("a positional-only parameter cannot be passed by name") {
		duckdb_v2_result_handle result = nullptr;
		REQUIRE(Query(fx.conn, "SELECT kw_sum(x := 1)", &result) != DUCKDB_V2_ERROR_NONE);
		duckdb_v2_result_destroy(&result);
	}
}

TEST_CASE("V2 scalar: parameter kind refusals", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto one = MakeInt32Value(fx.factory, 1);

	SECTION("a kind outside the enum") {
		auto function = MakeScalar(fx.conn, "bad_kind");
		auto a_ident = Ident("a");
		REQUIRE(duckdb_v2_function_signature_add_parameter(SigOf(function), &a_ident, integer, nullptr,
		                                                   static_cast<DUCKDB_V2_FUNCTION_PARAMETER_KIND>(42),
		                                                   nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_scalar_function_destroy(&function);
	}
	SECTION("a default on *args") {
		auto function = MakeScalar(fx.conn, "bad_default");
		auto sig = SigOf(function);
		AddKindParam(sig, "args", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_POSITIONAL_VARIADIC, one);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, VarargSumExec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) != DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_destroy(&function);
	}
	SECTION("two **kwargs parameters") {
		auto function = MakeScalar(fx.conn, "two_kwargs");
		auto sig = SigOf(function);
		AddKindParam(sig, "a", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_VARIADIC);
		AddKindParam(sig, "b", integer, DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_VARIADIC);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_scalar_function_set_exec_callback(function, VarargSumExec, nullptr) == DUCKDB_V2_ERROR_NONE);
		REQUIRE(duckdb_v2_connection_register_scalar_function(fx.conn, function, nullptr) != DUCKDB_V2_ERROR_NONE);
		duckdb_v2_scalar_function_destroy(&function);
	}
	duckdb_v2_value_destroy(&one);
	duckdb_v2_logical_type_destroy(&integer);
}

TEST_CASE("V2 function bind: null arguments", "[capi_v2][scalar_function]") {
	idx_t count = 0;
	duckdb_v2_logical_type_handle type = nullptr;
	duckdb_v2_value_handle value = nullptr;
	duckdb_v2_identifier_t name {};
	bool found = false;
	void *data = nullptr;
	REQUIRE(duckdb_v2_function_bind_get_user_data(nullptr, &data, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_bind_set_bind_data(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_scalar_function_bind_set_bind_data(nullptr, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_bind_get_arg_count(nullptr, &count, &count, &count, &count, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_bind_get_arg_type(nullptr, 0, &type, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_bind_get_arg_value(nullptr, 0, &value, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_bind_get_arg_name(nullptr, 0, &name, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	auto a_ident = Ident("a");
	REQUIRE(duckdb_v2_function_bind_get_arg_index(nullptr, &a_ident, &count, &found, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);
}

} // namespace test_capi_v2

namespace test_capi_v2 {

TEST_CASE("V2 function docs: borrowed from the function, and argument validation", "[capi_v2][scalar_function]") {
	EnvFixture fx;
	duckdb_v2_scalar_function_handle function = nullptr;
	REQUIRE(duckdb_v2_scalar_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);

	duckdb_v2_function_docs_handle docs = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_docs(function, &docs, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(docs != nullptr);
	// Borrowed: asking again gives the same documentation.
	duckdb_v2_function_docs_handle again = nullptr;
	REQUIRE(duckdb_v2_scalar_function_get_docs(function, &again, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(again == docs);

	auto text = Convert("Adds one.");
	auto title = Convert("Add one");
	auto example = Convert("f(1)");
	REQUIRE(duckdb_v2_function_docs_set_description(docs, &text, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_docs_add_example(docs, &title, &example, nullptr) == DUCKDB_V2_ERROR_NONE);
	// The title is optional.
	REQUIRE(duckdb_v2_function_docs_add_example(docs, nullptr, &example, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto category = Convert("numeric");
	REQUIRE(duckdb_v2_function_docs_add_category(docs, &category, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_docs_add_category(docs, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_add_category(nullptr, &category, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	auto malformed = duckdb_v2_str {nullptr, 3};
	REQUIRE(duckdb_v2_scalar_function_get_docs(nullptr, &again, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_scalar_function_get_docs(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_set_description(nullptr, &text, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_set_description(docs, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_add_example(docs, &title, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_add_example(docs, &malformed, &example, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_function_docs_add_example(nullptr, &title, &example, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_scalar_function_destroy(&function);
}

} // namespace test_capi_v2
