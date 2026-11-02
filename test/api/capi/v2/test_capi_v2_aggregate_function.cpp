#include "test_capi_v2.hpp"

#include <cstring>

// ---------------------------------------------------------------------------
// V2 aggregate function tests: build a function on a connection, configure
// its signature, register it, and call it through SQL.
//
// Callbacks avoid Catch assertions: a REQUIRE would throw through the C
// callback boundary into the engine. On failure they populate the provided
// error slot and return; the failure then surfaces as a query error that the
// test asserts on. Cross-callback observations are latched into file-scope
// statics and asserted after the query.
// ---------------------------------------------------------------------------

namespace test_capi_v2 {

namespace {

duckdb_v2_identifier_t AggName(const char *s) {
	return duckdb_v2_identifier_t {s, std::strlen(s)};
}

// Create an aggregate function on the connection with the given name.
duckdb_v2_aggregate_function_handle MakeAggregate(duckdb_v2_connection_handle conn, const char *name) {
	duckdb_v2_aggregate_function_handle function = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_create(FactoryOf(conn), &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	auto str = Convert(name);
	REQUIRE(duckdb_v2_aggregate_function_set_name(function, &str, nullptr) == DUCKDB_V2_ERROR_NONE);
	return function;
}

// The function's borrowed signature.
duckdb_v2_function_signature_handle AggSigOf(duckdb_v2_aggregate_function_handle function) {
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_get_signature(function, &sig, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(sig != nullptr);
	return sig;
}

void AggSigParam(duckdb_v2_function_signature_handle sig, const char *name, duckdb_v2_logical_type_handle type) {
	auto name_str = AggName(name);
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &name_str, type, nullptr,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_STANDARD,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
}

// Run a query and read the BIGINT cell of the first row in column `col`.
int64_t AggQueryI64(duckdb_v2_connection_handle conn, const char *sql, idx_t col = 0) {
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(conn, sql, &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, col, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	auto out = static_cast<const int64_t *>(view.data)[SelAt(view.sel, 0)];
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);
	return out;
}

// ---------------------------------------------------------------------------
// my_sum(INTEGER) -> BIGINT: an int64 running sum kept in each state.
// ---------------------------------------------------------------------------

void SumSize(duckdb_v2_aggregate_function_size_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_aggregate_function_size_set_state_size(info, sizeof(int64_t), err);
}

void SumInit(duckdb_v2_aggregate_function_init_info_handle info, duckdb_v2_error_info_handle *err) {
	void **states = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_init_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_init_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int64_t *>(states[i]) = 0;
	}
}

void SumUpdate(duckdb_v2_aggregate_function_update_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle arg = nullptr;
	void **states = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_update_get_arg(info, 0, &arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view view {};
	if (duckdb_v2_vector_get_view(arg, &view, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *data = static_cast<const int32_t *>(view.data);
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int64_t *>(states[i]) += data[SelAt(view.sel, i)];
	}
	// The count is the exclusive bound: index `count` of the argument vectors
	// is refused.
	idx_t arg_count = 0;
	if (duckdb_v2_aggregate_function_update_get_arg_count(info, &arg_count, nullptr, nullptr, nullptr, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_handle past_the_end = nullptr;
	if (duckdb_v2_aggregate_function_update_get_arg(info, arg_count, &past_the_end, nullptr) !=
	    DUCKDB_V2_ERROR_INPUT_INVALID) {
		duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_API);
		auto text_str = Convert("past-the-end argument vector index was not refused");
		duckdb_v2_error_info_set_text(*err, &text_str);
	}
}

void SumCombine(duckdb_v2_aggregate_function_combine_info_handle info, duckdb_v2_error_info_handle *err) {
	void **sources = nullptr;
	void **targets = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_combine_get_sources(info, &sources, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_combine_get_targets(info, &targets, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_combine_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int64_t *>(targets[i]) += *static_cast<const int64_t *>(sources[i]);
	}
}

void SumFinalize(duckdb_v2_aggregate_function_finalize_info_handle info, duckdb_v2_error_info_handle *err) {
	void **states = nullptr;
	idx_t count = 0;
	idx_t offset = 0;
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_aggregate_function_finalize_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result_offset(info, &offset, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int64_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[offset + i] = *static_cast<const int64_t *>(states[i]);
	}
}

// Like SumFinalize, plus one: an aggregate that differs from my_sum only in its finalize callback.
void SumPlusOneFinalize(duckdb_v2_aggregate_function_finalize_info_handle info, duckdb_v2_error_info_handle *err) {
	SumFinalize(info, err);
	duckdb_v2_vector_handle result = nullptr;
	idx_t count = 0;
	idx_t offset = 0;
	void *raw = nullptr;
	if (duckdb_v2_aggregate_function_finalize_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result_offset(info, &offset, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_data_mutable(result, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		static_cast<int64_t *>(raw)[offset + i] += 1;
	}
}

// Registers a sum aggregate `name`(INTEGER) -> BIGINT with the given finalize callback, and destroys the builder
// handle.
void RegisterSumAggregate(EnvFixture &fx, const char *name,
                          duckdb_v2_aggregate_function_finalize_callback_fn finalize = SumFinalize) {
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	auto function = MakeAggregate(fx.conn, name);
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, SumSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, SumInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, SumUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, SumCombine, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, finalize, nullptr) == DUCKDB_V2_ERROR_NONE);

	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_destroy(&function) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(function == nullptr);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&bigint);
}

// Registers my_sum on the connection.
void RegisterMySum(EnvFixture &fx) {
	RegisterSumAggregate(fx, "my_sum");
}

// Noop callbacks for registration-refusal tests.
void AggNoopSize(duckdb_v2_aggregate_function_size_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_aggregate_function_size_set_state_size(info, sizeof(int64_t), err);
}
void AggNoopInit(duckdb_v2_aggregate_function_init_info_handle, duckdb_v2_error_info_handle *) {
}
void AggNoopUpdate(duckdb_v2_aggregate_function_update_info_handle, duckdb_v2_error_info_handle *) {
}
void AggNoopCombine(duckdb_v2_aggregate_function_combine_info_handle, duckdb_v2_error_info_handle *) {
}
void AggNoopFinalize(duckdb_v2_aggregate_function_finalize_info_handle, duckdb_v2_error_info_handle *) {
}

// Fails the query through the update callback's error slot.
void FailingUpdate(duckdb_v2_aggregate_function_update_info_handle, duckdb_v2_error_info_handle *err) {
	duckdb_v2_error_info_set_code(*err, DUCKDB_V2_ERROR_IO_GENERAL);
	auto text_str = Convert("aggregate update failed on purpose");
	duckdb_v2_error_info_set_text(*err, &text_str);
}

// ---------------------------------------------------------------------------
// Bind / execution data-flow probes. The bind callback resolves an ANY
// return type to INTEGER and plants bind data; every later callback latches
// the bind data pointer it saw. The aggregate keeps the last int32 it saw and
// finalizes to double that value.
// ---------------------------------------------------------------------------

int agg_bind_marker = 0;
struct {
	void *user_data_in_bind = &agg_bind_marker; // expected to arrive as nullptr
	void *bind_data_in_size = nullptr;
	void *bind_data_in_init = nullptr;
	void *bind_data_in_update = nullptr;
	void *bind_data_in_combine = nullptr;
	void *bind_data_in_finalize = nullptr;
	int combine_runs = 0;
	int bind_data_destroys = 0;
} agg_flow;

void AggFlowDestroyBindData(void *) {
	agg_flow.bind_data_destroys++;
}

void AggFlowBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_aggregate_function_bind_info_handle result,
                 duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	if (duckdb_v2_function_bind_get_user_data(info, &agg_flow.user_data_in_bind, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_opaque bind_data = {&agg_bind_marker, AggFlowDestroyBindData, nullptr};
	if (duckdb_v2_function_bind_set_bind_data(info, &bind_data, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	// Resolve the declared ANY return type to a concrete INTEGER.
	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_aggregate_function_bind_set_return_type(result, integer, err);
	duckdb_v2_logical_type_destroy(&integer);
}

void AggFlowSize(duckdb_v2_aggregate_function_size_info_handle info, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_aggregate_function_size_get_bind_data(info, &agg_flow.bind_data_in_size, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_aggregate_function_size_set_state_size(info, sizeof(int32_t), err);
}

void AggFlowInit(duckdb_v2_aggregate_function_init_info_handle info, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_aggregate_function_init_get_bind_data(info, &agg_flow.bind_data_in_init, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void **states = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_init_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_init_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int32_t *>(states[i]) = 0;
	}
}

void AggFlowUpdate(duckdb_v2_aggregate_function_update_info_handle info, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_aggregate_function_update_get_bind_data(info, &agg_flow.bind_data_in_update, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_handle arg = nullptr;
	void **states = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_update_get_arg(info, 0, &arg, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view view {};
	if (duckdb_v2_vector_get_view(arg, &view, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *data = static_cast<const int32_t *>(view.data);
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int32_t *>(states[i]) = data[SelAt(view.sel, i)];
	}
}

void AggFlowCombine(duckdb_v2_aggregate_function_combine_info_handle info, duckdb_v2_error_info_handle *err) {
	agg_flow.combine_runs++;
	if (duckdb_v2_aggregate_function_combine_get_bind_data(info, &agg_flow.bind_data_in_combine, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void **sources = nullptr;
	void **targets = nullptr;
	idx_t count = 0;
	if (duckdb_v2_aggregate_function_combine_get_sources(info, &sources, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_combine_get_targets(info, &targets, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_combine_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		const auto source = *static_cast<const int32_t *>(sources[i]);
		if (source != 0) {
			*static_cast<int32_t *>(targets[i]) = source;
		}
	}
}

void AggFlowFinalize(duckdb_v2_aggregate_function_finalize_info_handle info, duckdb_v2_error_info_handle *err) {
	if (duckdb_v2_aggregate_function_finalize_get_bind_data(info, &agg_flow.bind_data_in_finalize, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void **states = nullptr;
	idx_t count = 0;
	idx_t offset = 0;
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_aggregate_function_finalize_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result_offset(info, &offset, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[offset + i] = *static_cast<const int32_t *>(states[i]) * 2;
	}
}

// ---------------------------------------------------------------------------
// Bind-time argument introspection. The bind callback reads the argument
// count, the resolved argument types and the constant folded out of the second
// argument, then resolves the ANY return type to INTEGER. The aggregate itself
// finalizes to that constant.
// ---------------------------------------------------------------------------

struct {
	idx_t arg_count = 0;
	DUCKDB_V2_LOGICAL_TYPE_ID arg_types[2] = {DUCKDB_V2_LOGICAL_TYPE_ID_INVALID, DUCKDB_V2_LOGICAL_TYPE_ID_INVALID};
	int32_t constant = 0;
	// Out-of-range probes, latched with their own (null) error slot.
	DUCKDB_V2_ERROR oob_type_rc = DUCKDB_V2_ERROR_NONE;
	DUCKDB_V2_ERROR oob_value_rc = DUCKDB_V2_ERROR_NONE;
} agg_arg_probe;

void AggArgProbeBind(duckdb_v2_function_bind_info_handle info, duckdb_v2_aggregate_function_bind_info_handle result,
                     duckdb_v2_context_handle context, duckdb_v2_error_info_handle *err) {
	duckdb_v2_factory_handle factory = nullptr;
	duckdb_v2_context_get_factory(context, &factory, nullptr);
	if (duckdb_v2_function_bind_get_arg_count(info, &agg_arg_probe.arg_count, nullptr, nullptr, nullptr, err) !=
	    DUCKDB_V2_ERROR_NONE) {
		return;
	}
	for (idx_t i = 0; i < agg_arg_probe.arg_count && i < 2; i++) {
		duckdb_v2_logical_type_handle type = nullptr;
		if (duckdb_v2_function_bind_get_arg_type(info, i, &type, err) != DUCKDB_V2_ERROR_NONE) {
			return;
		}
		auto rc = duckdb_v2_logical_type_get_id(type, &agg_arg_probe.arg_types[i], err);
		duckdb_v2_logical_type_destroy(&type);
		if (rc != DUCKDB_V2_ERROR_NONE) {
			return;
		}
	}
	// An index past the last argument is an input error.
	duckdb_v2_logical_type_handle oob_type = nullptr;
	duckdb_v2_value_handle oob_value = nullptr;
	agg_arg_probe.oob_type_rc = duckdb_v2_function_bind_get_arg_type(info, 5, &oob_type, nullptr);
	agg_arg_probe.oob_value_rc = duckdb_v2_function_bind_get_arg_value(info, 5, &oob_value, nullptr);

	// Fold the second argument to a constant. A non-constant argument fails here.
	duckdb_v2_value_handle value = nullptr;
	if (duckdb_v2_function_bind_get_arg_value(info, 1, &value, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto rc = duckdb_v2_value_get_int(value, &agg_arg_probe.constant, err);
	duckdb_v2_value_destroy(&value);
	if (rc != DUCKDB_V2_ERROR_NONE) {
		return;
	}

	duckdb_v2_logical_type_handle integer = nullptr;
	if (duckdb_v2_factory_create_type_from_id(factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER, nullptr, nullptr, 0, &integer,
	                                          err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_aggregate_function_bind_set_return_type(result, integer, err);
	duckdb_v2_logical_type_destroy(&integer);
}

// The state is unused: the aggregate finalizes to the constant bind folded.
void AggArgProbeSize(duckdb_v2_aggregate_function_size_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_aggregate_function_size_set_state_size(info, sizeof(int32_t), err);
}

void AggArgProbeFinalize(duckdb_v2_aggregate_function_finalize_info_handle info, duckdb_v2_error_info_handle *err) {
	void **states = nullptr;
	idx_t count = 0;
	idx_t offset = 0;
	duckdb_v2_vector_handle result = nullptr;
	if (duckdb_v2_aggregate_function_finalize_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_state_count(info, &count, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result(info, &result, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_finalize_get_result_offset(info, &offset, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	void *raw = nullptr;
	if (duckdb_v2_vector_get_data_mutable(result, &raw, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	auto *out_data = static_cast<int32_t *>(raw);
	for (idx_t i = 0; i < count; i++) {
		out_data[offset + i] = agg_arg_probe.constant;
	}
}

} // namespace

// ===========================================================================
// Register on a connection and call through SQL.
// ===========================================================================

TEST_CASE("V2 aggregate: register on connection and execute", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	RegisterMySum(fx);

	// Ungrouped, more rows than one chunk, so the callbacks see full vectors.
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(r::INTEGER) FROM range(5000) t(r)") == 5000LL * 4999 / 2);
	// A constant argument vector.
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(5) FROM range(10)") == 50);
	// Grouped: every row lands in its group's state; result offsets are
	// exercised by finalizing many groups.
	REQUIRE(AggQueryI64(fx.conn, "SELECT sum(s)::BIGINT FROM (SELECT my_sum(r::INTEGER) AS s "
	                             "FROM range(5000) t(r) GROUP BY r % 3000)") == 5000LL * 4999 / 2);
	// Window and ordered aggregates update a single constant state with many rows.
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(r::INTEGER) OVER () FROM range(5000) t(r) LIMIT 1") ==
	        5000LL * 4999 / 2);
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(r::INTEGER ORDER BY r DESC) FROM range(5000) t(r)") ==
	        5000LL * 4999 / 2);
}

// ===========================================================================
// Different aggregates with the same signature stay separate in one query.
// ===========================================================================

TEST_CASE("V2 aggregate: different aggregates over the same arguments are not merged",
          "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	RegisterMySum(fx);
	RegisterSumAggregate(fx, "my_sum_plus_one", SumPlusOneFinalize);

	// Each on its own.
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(r::INTEGER) FROM range(10) t(r)") == 45);
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum_plus_one(r::INTEGER) FROM range(10) t(r)") == 46);
	// Together over the same column: both are computed, in either order.
	const char *both = "SELECT my_sum(r::INTEGER), my_sum_plus_one(r::INTEGER) FROM range(10) t(r)";
	REQUIRE(AggQueryI64(fx.conn, both, 0) == 45);
	REQUIRE(AggQueryI64(fx.conn, both, 1) == 46);
	const char *swapped = "SELECT my_sum_plus_one(r::INTEGER), my_sum(r::INTEGER) FROM range(10) t(r)";
	REQUIRE(AggQueryI64(fx.conn, swapped, 0) == 46);
	REQUIRE(AggQueryI64(fx.conn, swapped, 1) == 45);
	// The same aggregate twice is still the same.
	REQUIRE(AggQueryI64(fx.conn, "SELECT my_sum(r::INTEGER) - my_sum(r::INTEGER) FROM range(10) t(r)") == 0);
}

// ===========================================================================
// Bind resolves an ANY return type; bind data reaches every later callback.
// ===========================================================================

TEST_CASE("V2 aggregate: bind callback resolves ANY return and bind data flows", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	auto function = MakeAggregate(fx.conn, "any_double");
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_bind_callback(function, AggFlowBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, AggFlowSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, AggFlowInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, AggFlowUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, AggFlowCombine, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, AggFlowFinalize, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);

	agg_flow = {};
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT any_double(21) AS d", &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	// The bind callback resolved the ANY return type to INTEGER.
	RequireColumn(result, 0, "d", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(static_cast<const int32_t *>(view.data)[SelAt(view.sel, 0)] == 42);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);

	// No user data was set, so bind saw nullptr; the planted bind data reached
	// every later callback.
	REQUIRE(agg_flow.user_data_in_bind == nullptr);
	REQUIRE(agg_flow.bind_data_in_size == &agg_bind_marker);
	REQUIRE(agg_flow.bind_data_in_init == &agg_bind_marker);
	REQUIRE(agg_flow.bind_data_in_update == &agg_bind_marker);
	REQUIRE(agg_flow.bind_data_in_finalize == &agg_bind_marker);
	if (agg_flow.combine_runs > 0) {
		REQUIRE(agg_flow.bind_data_in_combine == &agg_bind_marker);
	}
}

// ===========================================================================
// Bind-time argument introspection: count, types, constant folding.
// ===========================================================================

TEST_CASE("V2 aggregate: bind reads argument count, types and constants", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// agg_arg_probe(x ANY, y INTEGER) -> ANY
	auto function = MakeAggregate(fx.conn, "agg_arg_probe");
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", any);
	AggSigParam(sig, "y", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_bind_callback(function, AggArgProbeBind, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, AggArgProbeSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, AggNoopInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, AggNoopUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, AggNoopCombine, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, AggArgProbeFinalize, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);

	agg_arg_probe = {};
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT agg_arg_probe('hello', 21) AS d", &result) == DUCKDB_V2_ERROR_NONE);
	auto chunk = StepChunk(result);
	REQUIRE(chunk != nullptr);
	RequireColumn(result, 0, "d", DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	duckdb_v2_vector_handle vec = nullptr;
	duckdb_v2_data_chunk_get_vector(chunk, 0, &vec, nullptr);
	duckdb_v2_vector_view view {};
	duckdb_v2_vector_get_view(vec, &view, nullptr);
	REQUIRE(static_cast<const int32_t *>(view.data)[SelAt(view.sel, 0)] == 21);
	duckdb_v2_data_chunk_destroy(&chunk);
	duckdb_v2_result_destroy(&result);

	REQUIRE(agg_arg_probe.arg_count == 2);
	// The ANY parameter reports the type the call resolved it to.
	REQUIRE(agg_arg_probe.arg_types[0] == DUCKDB_V2_LOGICAL_TYPE_ID_VARCHAR);
	REQUIRE(agg_arg_probe.arg_types[1] == DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	REQUIRE(agg_arg_probe.constant == 21);
	REQUIRE(agg_arg_probe.oob_type_rc == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(agg_arg_probe.oob_value_rc == DUCKDB_V2_ERROR_INPUT_INVALID);

	// A column reference is not a constant: the binder error surfaces from the bind callback.
	agg_arg_probe = {};
	duckdb_v2_result_handle failed = nullptr;
	REQUIRE(Query(fx.conn, "SELECT agg_arg_probe('hello', i) FROM (VALUES (21)) t(i)", &failed) ==
	        DUCKDB_V2_ERROR_QUERY_BINDER);
	duckdb_v2_result_destroy(&failed);
}

// ===========================================================================
// Errors
// ===========================================================================

// An error set in the update callback's slot fails the query with its code.
TEST_CASE("V2 aggregate: update error propagates to the result", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);

	auto function = MakeAggregate(fx.conn, "agg_always_fails");
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, AggNoopSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, AggNoopInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, FailingUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, AggNoopCombine, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, AggNoopFinalize, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);

	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT agg_always_fails(1)", &result) == DUCKDB_V2_ERROR_NONE);
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

TEST_CASE("V2 aggregate: registration refusals", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto any = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_ANY);

	// Sets every required callback except one, controlled by the arguments.
	auto configure = [&](duckdb_v2_aggregate_function_handle function, bool with_size, bool with_init, bool with_update,
	                     bool with_combine, bool with_finalize) {
		if (with_size) {
			duckdb_v2_aggregate_function_set_size_callback(function, AggNoopSize, nullptr);
		}
		if (with_init) {
			duckdb_v2_aggregate_function_set_init_callback(function, AggNoopInit, nullptr);
		}
		if (with_update) {
			duckdb_v2_aggregate_function_set_update_callback(function, AggNoopUpdate, nullptr);
		}
		if (with_combine) {
			duckdb_v2_aggregate_function_set_combine_callback(function, AggNoopCombine, nullptr);
		}
		if (with_finalize) {
			duckdb_v2_aggregate_function_set_finalize_callback(function, AggNoopFinalize, nullptr);
		}
	};

	// No name.
	{
		duckdb_v2_aggregate_function_handle function = nullptr;
		REQUIRE(duckdb_v2_aggregate_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
		auto sig = AggSigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		configure(function, true, true, true, true, true);
		REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_aggregate_function_destroy(&function);
	}

	// Each required callback missing in turn.
	for (int missing = 0; missing < 5; missing++) {
		auto function = MakeAggregate(fx.conn, "missing_callback");
		auto sig = AggSigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, integer, nullptr) == DUCKDB_V2_ERROR_NONE);
		configure(function, missing != 0, missing != 1, missing != 2, missing != 3, missing != 4);
		REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_aggregate_function_destroy(&function);
	}

	// No return type set at all.
	{
		auto function = MakeAggregate(fx.conn, "no_return");
		configure(function, true, true, true, true, true);
		REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_aggregate_function_destroy(&function);
	}

	// An ANY return type without a bind callback to resolve it.
	{
		auto function = MakeAggregate(fx.conn, "any_no_bind");
		auto sig = AggSigOf(function);
		REQUIRE(duckdb_v2_function_signature_set_return_type(sig, any, nullptr) == DUCKDB_V2_ERROR_NONE);
		configure(function, true, true, true, true, true);
		REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) ==
		        DUCKDB_V2_ERROR_INPUT_INVALID);
		duckdb_v2_aggregate_function_destroy(&function);
	}

	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&any);
}

TEST_CASE("V2 aggregate: null arguments and destroy null-safety", "[capi_v2][aggregate_function]") {
	EnvFixture fx;

	duckdb_v2_aggregate_function_handle function = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_create(nullptr, &function, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(function == nullptr);
	REQUIRE(duckdb_v2_aggregate_function_create(fx.factory, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_aggregate_function_create(fx.factory, &function, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_name(function, nullptr, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	duckdb_v2_function_signature_handle sig = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_get_signature(nullptr, &sig, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, nullptr, nullptr) ==
	        DUCKDB_V2_ERROR_INPUT_INVALID);

	duckdb_v2_aggregate_function_destroy(&function);

	REQUIRE(duckdb_v2_aggregate_function_destroy(nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_handle null_function = nullptr;
	REQUIRE(duckdb_v2_aggregate_function_destroy(&null_function) == DUCKDB_V2_ERROR_NONE);
}

// ===========================================================================
// Function properties.
// ===========================================================================

TEST_CASE("V2 aggregate: function properties", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);

	// Both COMMON and AGGREGATE group keys are accepted.
	auto function = MakeAggregate(fx.conn, "prop_sum");
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", integer);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, SumSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, SumInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, SumUpdate, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, SumCombine, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, SumFinalize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_STABILITY,
	                                                  DUCKDB_V2_FUNCTION_PROPERTY_STABILITY_CONSISTENT,
	                                                  nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_AGG_ORDER_DEPENDENT,
	                                                  DUCKDB_V2_FUNCTION_PROPERTY_AGG_ORDER_DEPENDENT_NO,
	                                                  nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_AGG_DISTINCT_DEPENDENT,
	                                                  DUCKDB_V2_FUNCTION_PROPERTY_AGG_DISTINCT_DEPENDENT_NO,
	                                                  nullptr) == DUCKDB_V2_ERROR_NONE);

	// A value that does not belong to the key is rejected.
	REQUIRE(duckdb_v2_aggregate_function_set_property(function, DUCKDB_V2_FUNCTION_PROPERTY_AGG_ORDER_DEPENDENT,
	                                                  DUCKDB_V2_FUNCTION_PROPERTY_AGG_DISTINCT_DEPENDENT_YES,
	                                                  nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);
	// A key from an unknown group is rejected.
	REQUIRE(duckdb_v2_aggregate_function_set_property(function, static_cast<DUCKDB_V2_FUNCTION_PROPERTY_KEY>(0x7F0000),
	                                                  static_cast<DUCKDB_V2_FUNCTION_PROPERTY_VALUE>(0x7F0000),
	                                                  nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID);

	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_logical_type_destroy(&integer);
	duckdb_v2_logical_type_destroy(&bigint);

	REQUIRE(AggQueryI64(fx.conn, "SELECT prop_sum(r::INTEGER) FROM range(100) t(r)") == 100LL * 99 / 2);
}

namespace {

// ---------------------------------------------------------------------------
// weighted_sum(x, *, weight := 1) -> BIGINT: the sum of x * weight. The named-only weight reaches update after
// the positional arguments.
// ---------------------------------------------------------------------------

struct WeightedProbe {
	idx_t counts[4] = {};
	bool oob_arg_refused = false;
};
WeightedProbe weighted_probe;

void WeightedUpdate(duckdb_v2_aggregate_function_update_info_handle info, duckdb_v2_error_info_handle *err) {
	duckdb_v2_vector_handle x = nullptr;
	duckdb_v2_vector_handle weight = nullptr;
	void **states = nullptr;
	idx_t count = 0;
	auto &counts = weighted_probe.counts;
	if (duckdb_v2_aggregate_function_update_get_arg_count(info, &counts[0], &counts[1], &counts[2], &counts[3], err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_arg(info, 0, &x, err) != DUCKDB_V2_ERROR_NONE ||
	    // the first named argument follows the positional ones
	    duckdb_v2_aggregate_function_update_get_arg(info, counts[0] + counts[1], &weight, err) !=
	        DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_states(info, &states, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_aggregate_function_update_get_row_count(info, &count, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	duckdb_v2_vector_view x_view {};
	duckdb_v2_vector_view weight_view {};
	if (duckdb_v2_vector_get_view(x, &x_view, err) != DUCKDB_V2_ERROR_NONE ||
	    duckdb_v2_vector_get_view(weight, &weight_view, err) != DUCKDB_V2_ERROR_NONE) {
		return;
	}
	const auto *x_data = static_cast<const int32_t *>(x_view.data);
	const auto *weight_data = static_cast<const int32_t *>(weight_view.data);
	for (idx_t i = 0; i < count; i++) {
		*static_cast<int64_t *>(states[i]) +=
		    int64_t(x_data[SelAt(x_view.sel, i)]) * weight_data[SelAt(weight_view.sel, i)];
	}
	duckdb_v2_vector_handle past_the_end = nullptr;
	const auto arg_count = counts[0] + counts[1] + counts[2] + counts[3];
	weighted_probe.oob_arg_refused = duckdb_v2_aggregate_function_update_get_arg(
	                                     info, arg_count, &past_the_end, nullptr) == DUCKDB_V2_ERROR_INPUT_INVALID;
}

} // namespace

// ===========================================================================
// A named-only parameter reaches update after the positional arguments.
// ===========================================================================

TEST_CASE("V2 aggregate: named-only arguments reach update", "[capi_v2][aggregate_function]") {
	EnvFixture fx;
	auto integer = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_INTEGER);
	auto bigint = MakeType(fx.factory, DUCKDB_V2_LOGICAL_TYPE_ID_BIGINT);
	auto one = MakeInt32Value(fx.factory, 1);

	auto function = MakeAggregate(fx.conn, "weighted_sum");
	auto sig = AggSigOf(function);
	AggSigParam(sig, "x", integer);
	auto weight_ident = AggName("weight");
	REQUIRE(duckdb_v2_function_signature_add_parameter(sig, &weight_ident, integer, one,
	                                                   DUCKDB_V2_FUNCTION_PARAMETER_KIND_NAMED_ONLY,
	                                                   nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_function_signature_set_return_type(sig, bigint, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_size_callback(function, SumSize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_init_callback(function, SumInit, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_update_callback(function, WeightedUpdate, nullptr) ==
	        DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_combine_callback(function, SumCombine, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_aggregate_function_set_finalize_callback(function, SumFinalize, nullptr) == DUCKDB_V2_ERROR_NONE);
	REQUIRE(duckdb_v2_connection_register_aggregate_function(fx.conn, function, nullptr) == DUCKDB_V2_ERROR_NONE);
	duckdb_v2_aggregate_function_destroy(&function);
	duckdb_v2_value_destroy(&one);
	duckdb_v2_logical_type_destroy(&bigint);
	duckdb_v2_logical_type_destroy(&integer);

	// The omitted weight carries its default.
	REQUIRE(AggQueryI64(fx.conn, "SELECT weighted_sum(r::INTEGER) FROM range(10) t(r)") == 45);
	REQUIRE(weighted_probe.counts[0] == 1);
	REQUIRE(weighted_probe.counts[1] == 0);
	REQUIRE(weighted_probe.counts[2] == 1);
	REQUIRE(weighted_probe.counts[3] == 0);
	REQUIRE(weighted_probe.oob_arg_refused);
	// A constant weight, and a column.
	REQUIRE(AggQueryI64(fx.conn, "SELECT weighted_sum(r::INTEGER, weight := 3) FROM range(10) t(r)") == 135);
	REQUIRE(AggQueryI64(fx.conn, "SELECT weighted_sum(r::INTEGER, weight := r::INTEGER) FROM range(10) t(r)") == 285);
	// The weight can only be passed by name.
	duckdb_v2_result_handle result = nullptr;
	REQUIRE(Query(fx.conn, "SELECT weighted_sum(1, 2)", &result) != DUCKDB_V2_ERROR_NONE);
	duckdb_v2_result_destroy(&result);
}

} // namespace test_capi_v2
