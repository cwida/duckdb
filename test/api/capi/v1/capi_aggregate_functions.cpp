#include "capi_tester.hpp"

using namespace duckdb;

struct WeightedSumState {
	int64_t sum;
	uint64_t count;
};

idx_t WeightedSumSize(duckdb_function_info info) {
	return sizeof(WeightedSumState);
}

void WeightedSumInit(duckdb_function_info info, duckdb_aggregate_state state_p) {
	auto state = reinterpret_cast<WeightedSumState *>(state_p);
	state->sum = 0;
	state->count = 0;
}

void WeightedSumUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
	auto state = reinterpret_cast<WeightedSumState **>(states);
	auto row_count = duckdb_data_chunk_get_size(input);
	auto input_vector = duckdb_data_chunk_get_vector(input, 0);
	auto input_data = static_cast<int64_t *>(duckdb_vector_get_data(input_vector));
	auto input_validity = duckdb_vector_get_validity(input_vector);

	if (duckdb_data_chunk_get_column_count(input) == 1) {
		// single argument
		for (idx_t i = 0; i < row_count; i++) {
			if (duckdb_validity_row_is_valid(input_validity, i)) {
				state[i]->sum += input_data[i];
				state[i]->count++;
			}
		}
	} else {
		// two arguments
		auto weight_vector = duckdb_data_chunk_get_vector(input, 1);
		auto weight_data = static_cast<int64_t *>(duckdb_vector_get_data(weight_vector));
		auto weight_validity = duckdb_vector_get_validity(weight_vector);
		for (idx_t i = 0; i < row_count; i++) {
			if (duckdb_validity_row_is_valid(input_validity, i) && duckdb_validity_row_is_valid(weight_validity, i)) {
				state[i]->sum += input_data[i] * weight_data[i];
				state[i]->count++;
			}
		}
	}
}

void WeightedSumCombine(duckdb_function_info info, duckdb_aggregate_state *source_p, duckdb_aggregate_state *target_p,
                        idx_t count) {
	auto source = reinterpret_cast<WeightedSumState **>(source_p);
	auto target = reinterpret_cast<WeightedSumState **>(target_p);

	for (idx_t i = 0; i < count; i++) {
		target[i]->sum += source[i]->sum;
		target[i]->count += source[i]->count;
	}
}

void WeightedSumFinalize(duckdb_function_info info, duckdb_aggregate_state *source_p, duckdb_vector result, idx_t count,
                         idx_t offset) {
	auto source = reinterpret_cast<WeightedSumState **>(source_p);
	auto result_data = static_cast<int64_t *>(duckdb_vector_get_data(result));
	duckdb_vector_ensure_validity_writable(result);
	auto result_validity = duckdb_vector_get_validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (source[i]->count == 0) {
			duckdb_validity_set_row_invalid(result_validity, offset + i);
		} else {
			result_data[offset + i] = source[i]->sum;
		}
	}
}

static duckdb_aggregate_function CAPIGetAggregateFunction(duckdb_connection connection, const char *name,
                                                          idx_t parameter_count = 2) {
	// create an aggregate function
	auto function = duckdb_create_aggregate_function();
	duckdb_aggregate_function_set_name(nullptr, name);
	duckdb_aggregate_function_set_name(function, nullptr);
	duckdb_aggregate_function_set_name(function, name);
	duckdb_aggregate_function_set_name(function, name);

	// add a two bigint parameters
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_aggregate_function_add_parameter(nullptr, type);
	duckdb_aggregate_function_add_parameter(function, nullptr);
	for (idx_t idx = 0; idx < parameter_count; idx++) {
		duckdb_aggregate_function_add_parameter(function, type);
	}

	// set the return type to bigint
	duckdb_aggregate_function_set_return_type(nullptr, type);
	duckdb_aggregate_function_set_return_type(function, nullptr);
	duckdb_aggregate_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	// set up the function
	duckdb_aggregate_function_set_functions(nullptr, nullptr, nullptr, nullptr, nullptr, nullptr);
	duckdb_aggregate_function_set_functions(function, nullptr, nullptr, nullptr, nullptr, nullptr);
	duckdb_aggregate_function_set_functions(function, WeightedSumSize, WeightedSumInit, WeightedSumUpdate,
	                                        WeightedSumCombine, WeightedSumFinalize);
	return function;
}

static void CAPIRegisterWeightedSum(duckdb_connection connection, const char *name, duckdb_state expected_outcome) {
	duckdb_state status;

	// create an aggregate function
	auto function = CAPIGetAggregateFunction(connection, name);
	// register and cleanup
	status = duckdb_register_aggregate_function(connection, function);
	REQUIRE(status == expected_outcome);

	duckdb_destroy_aggregate_function(&function);
	duckdb_destroy_aggregate_function(&function);
	duckdb_destroy_aggregate_function(nullptr);
}

struct CAPICallbacks {
	duckdb_aggregate_state_size state_size;
	duckdb_aggregate_init_t init;
	duckdb_aggregate_update_t update;
	duckdb_aggregate_combine_t combine;
	duckdb_aggregate_finalize_t finalize;
};

idx_t CallbackSize(duckdb_function_info info) {
	auto callbacks = (CAPICallbacks *)duckdb_aggregate_function_get_extra_info(info);
	return callbacks->state_size(info);
}

void CallbackInit(duckdb_function_info info, duckdb_aggregate_state state_p) {
	auto callbacks = (CAPICallbacks *)duckdb_aggregate_function_get_extra_info(info);
	callbacks->init(info, state_p);
}

void CallbackUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
	auto callbacks = (CAPICallbacks *)duckdb_aggregate_function_get_extra_info(info);
	callbacks->update(info, input, states);
}

void CallbackCombine(duckdb_function_info info, duckdb_aggregate_state *source_p, duckdb_aggregate_state *target_p,
                     idx_t count) {
	auto callbacks = (CAPICallbacks *)duckdb_aggregate_function_get_extra_info(info);
	callbacks->combine(info, source_p, target_p, count);
}

void CallbackFinalize(duckdb_function_info info, duckdb_aggregate_state *source_p, duckdb_vector result, idx_t count,
                      idx_t offset) {
	auto callbacks = (CAPICallbacks *)duckdb_aggregate_function_get_extra_info(info);
	callbacks->finalize(info, source_p, result, count, offset);
}

static void CAPIRegisterWeightedSumExtraInfo(duckdb_connection connection, const char *name,
                                             duckdb_state expected_outcome) {
	duckdb_state status;

	// create an aggregate function
	auto function = duckdb_create_aggregate_function();
	duckdb_aggregate_function_set_name(function, name);

	// add a two bigint parameters
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_aggregate_function_add_parameter(function, type);
	duckdb_aggregate_function_add_parameter(function, type);

	// set the return type to bigint
	duckdb_aggregate_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	auto callback_ptr = malloc(sizeof(CAPICallbacks));
	auto callback_struct = (CAPICallbacks *)callback_ptr;
	callback_struct->state_size = WeightedSumSize;
	callback_struct->init = WeightedSumInit;
	callback_struct->update = WeightedSumUpdate;
	callback_struct->combine = WeightedSumCombine;
	callback_struct->finalize = WeightedSumFinalize;

	duckdb_aggregate_function_set_extra_info(function, callback_ptr, free);

	// set up the function
	duckdb_aggregate_function_set_functions(function, CallbackSize, CallbackInit, CallbackUpdate, CallbackCombine,
	                                        CallbackFinalize);

	// register and cleanup
	status = duckdb_register_aggregate_function(connection, function);
	REQUIRE(status == expected_outcome);

	duckdb_destroy_aggregate_function(&function);
}

TEST_CASE("Test Aggregate Functions C API", "[capi]") {
	typedef void (*register_function_t)(duckdb_connection, const char *, duckdb_state);

	duckdb::vector<register_function_t> register_functions {CAPIRegisterWeightedSum, CAPIRegisterWeightedSumExtraInfo};
	for (auto &register_function : register_functions) {
		CAPITester tester;
		duckdb::unique_ptr<CAPIResult> result;

		REQUIRE(tester.OpenDatabase(nullptr));
		register_function(tester.connection, "my_weighted_sum", DuckDBSuccess);
		// try to register it again - this should be an error
		register_function(tester.connection, "my_weighted_sum", DuckDBError);

		// now call it
		result = tester.Query("SELECT my_weighted_sum(40, 2)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 80);

		result = tester.Query("SELECT my_weighted_sum(40, NULL)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->IsNull(0, 0));

		result = tester.Query("SELECT my_weighted_sum(NULL, 2)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->IsNull(0, 0));

		result = tester.Query("SELECT my_weighted_sum(i, 2) FROM range(100) t(i)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 9900);

		result = tester.Query("SELECT i % 2 AS gr, my_weighted_sum(i, 2) FROM range(100) t(i) GROUP BY gr ORDER BY gr");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 0);
		REQUIRE(result->Fetch<int64_t>(1, 0) == 4900);
		REQUIRE(result->Fetch<int64_t>(0, 1) == 1);
		REQUIRE(result->Fetch<int64_t>(1, 1) == 5000);

		// window and ordered aggregates update a single constant state with many rows
		result = tester.Query("SELECT my_weighted_sum(i, 2) OVER () FROM range(3000) t(i) LIMIT 1");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 8997000);

		result = tester.Query("SELECT my_weighted_sum(i, 2 ORDER BY i DESC) FROM range(3000) t(i)");
		REQUIRE_NO_FAIL(*result);
		REQUIRE(result->Fetch<int64_t>(0, 0) == 8997000);
	}
}

struct RepeatedStringAggState {
	char *data;
	idx_t size;
};

idx_t RepeatedStringAggSize(duckdb_function_info info) {
	return sizeof(RepeatedStringAggState);
}

void RepeatedStringAggInit(duckdb_function_info info, duckdb_aggregate_state state_p) {
	auto state = reinterpret_cast<RepeatedStringAggState *>(state_p);
	state->data = nullptr;
	state->size = 0;
}

void RepeatedStringAggUpdate(duckdb_function_info info, duckdb_data_chunk input, duckdb_aggregate_state *states) {
	auto state = reinterpret_cast<RepeatedStringAggState **>(states);
	auto row_count = duckdb_data_chunk_get_size(input);
	auto input_vector = duckdb_data_chunk_get_vector(input, 0);
	auto input_data = static_cast<duckdb_string_t *>(duckdb_vector_get_data(input_vector));
	auto input_validity = duckdb_vector_get_validity(input_vector);

	auto weight_vector = duckdb_data_chunk_get_vector(input, 1);
	auto weight_data = static_cast<int64_t *>(duckdb_vector_get_data(weight_vector));
	auto weight_validity = duckdb_vector_get_validity(weight_vector);
	for (idx_t i = 0; i < row_count; i++) {
		if (!duckdb_validity_row_is_valid(input_validity, i) || !duckdb_validity_row_is_valid(weight_validity, i)) {
			continue;
		}
		auto length = duckdb_string_t_length(input_data[i]);
		auto data = duckdb_string_t_data(input_data + i);
		auto weight = weight_data[i];
		if (weight < 0) {
			duckdb_aggregate_function_set_error(info, "Weight must be >= 0");
			return;
		}
		auto new_data = (char *)malloc(state[i]->size + length * weight + 1);
		if (state[i]->size > 0) {
			memcpy((void *)(new_data), state[i]->data, state[i]->size);
		}
		if (state[i]->data) {
			free((void *)(state[i]->data));
		}
		idx_t offset = state[i]->size;
		for (idx_t rep_idx = 0; rep_idx < static_cast<idx_t>(weight); rep_idx++) {
			memcpy((void *)(new_data + offset), data, length);
			offset += length;
		}
		state[i]->data = new_data;
		state[i]->size = offset;
		state[i]->data[state[i]->size] = '\0';
	}
}

void RepeatedStringAggCombine(duckdb_function_info info, duckdb_aggregate_state *source_p,
                              duckdb_aggregate_state *target_p, idx_t count) {
	auto source = reinterpret_cast<RepeatedStringAggState **>(source_p);
	auto target = reinterpret_cast<RepeatedStringAggState **>(target_p);

	for (idx_t i = 0; i < count; i++) {
		if (source[i]->size == 0) {
			continue;
		}
		auto new_data = (char *)malloc(target[i]->size + source[i]->size + 1);
		if (target[i]->size > 0) {
			memcpy((void *)new_data, target[i]->data, target[i]->size);
		}
		if (target[i]->data) {
			free((void *)target[i]->data);
		}
		memcpy((void *)(new_data + target[i]->size), source[i]->data, source[i]->size);
		target[i]->data = new_data;
		target[i]->size += source[i]->size;
		target[i]->data[target[i]->size] = '\0';
	}
}

void RepeatedStringAggFinalize(duckdb_function_info info, duckdb_aggregate_state *source_p, duckdb_vector result,
                               idx_t count, idx_t offset) {
	auto source = reinterpret_cast<RepeatedStringAggState **>(source_p);
	duckdb_vector_ensure_validity_writable(result);
	auto result_validity = duckdb_vector_get_validity(result);

	for (idx_t i = 0; i < count; i++) {
		if (!source[i]->data) {
			duckdb_validity_set_row_invalid(result_validity, offset + i);
		} else {
			duckdb_vector_assign_string_element_len(result, offset + i, reinterpret_cast<const char *>(source[i]->data),
			                                        source[i]->size);
		}
	}
}

void RepeatedStringAggDestructor(duckdb_aggregate_state *states, idx_t count) {
	auto source = reinterpret_cast<RepeatedStringAggState **>(states);
	for (idx_t i = 0; i < count; i++) {
		if (source[i]->data) {
			free(source[i]->data);
		}
	}
}

static void CAPIRegisterRepeatedStringAgg(duckdb_connection connection) {
	duckdb_state status;

	// create an aggregate function
	auto function = duckdb_create_aggregate_function();
	duckdb_aggregate_function_set_name(function, "repeated_string_agg");

	// add a varchar/bigint parameter
	auto varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	auto bigint_type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_aggregate_function_add_parameter(function, varchar_type);
	duckdb_aggregate_function_add_parameter(function, bigint_type);

	// set the return type to varchar
	duckdb_aggregate_function_set_return_type(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_destroy_logical_type(&bigint_type);

	// set up the function
	duckdb_aggregate_function_set_functions(function, RepeatedStringAggSize, RepeatedStringAggInit,
	                                        RepeatedStringAggUpdate, RepeatedStringAggCombine,
	                                        RepeatedStringAggFinalize);
	duckdb_aggregate_function_set_destructor(function, RepeatedStringAggDestructor);

	// register and cleanup
	status = duckdb_register_aggregate_function(connection, function);
	REQUIRE(status == DuckDBSuccess);

	duckdb_destroy_aggregate_function(&function);
}

TEST_CASE("Test String Aggregate Function", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterRepeatedStringAgg(tester.connection);

	// now call it
	result = tester.Query("SELECT repeated_string_agg('x', 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "xx");

	result = tester.Query("SELECT repeated_string_agg('', 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "");

	result = tester.Query("SELECT repeated_string_agg('abcdefgh', 3)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "abcdefghabcdefghabcdefgh");

	result = tester.Query("SELECT repeated_string_agg(NULL, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	REQUIRE_FAIL(tester.Query("SELECT repeated_string_agg('x', -1)"));

	result = tester.Query(
	    "SELECT repeated_string_agg(CASE WHEN i%10=0 THEN i::VARCHAR ELSE '' END, 2) FROM range(100) t(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "00101020203030404050506060707080809090");
}

static void CAPIRegisterWeightedSumOverloads(duckdb_connection connection, const char *name,
                                             duckdb_state expected_outcome) {
	duckdb_state status;

	auto function_set = duckdb_create_aggregate_function_set(name);
	// create an aggregate function with 2 parameters
	auto function = CAPIGetAggregateFunction(connection, name, 1);
	duckdb_add_aggregate_function_to_set(function_set, function);
	duckdb_destroy_aggregate_function(&function);

	// create an aggregate function with 3 parameters
	function = CAPIGetAggregateFunction(connection, name, 2);
	duckdb_add_aggregate_function_to_set(function_set, function);
	duckdb_destroy_aggregate_function(&function);

	// register and cleanup
	status = duckdb_register_aggregate_function_set(connection, function_set);
	REQUIRE(status == expected_outcome);

	duckdb_destroy_aggregate_function_set(&function_set);
	duckdb_destroy_aggregate_function_set(&function_set);
	duckdb_destroy_aggregate_function_set(nullptr);
}

TEST_CASE("Test Aggregate Function Overloads C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterWeightedSumOverloads(tester.connection, "my_weighted_sum", DuckDBSuccess);
	// try to register it again - this should be an error
	CAPIRegisterWeightedSumOverloads(tester.connection, "my_weighted_sum", DuckDBError);

	// now call it
	result = tester.Query("SELECT my_weighted_sum(40)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 40);

	result = tester.Query("SELECT my_weighted_sum(40, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 80);
}
