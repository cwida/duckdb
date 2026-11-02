#include "capi_tester.hpp"

using namespace duckdb;

void AddVariadicNumbersTogether(duckdb_function_info, duckdb_data_chunk input, duckdb_vector output) {
	// get the total number of rows in this chunk
	auto input_size = duckdb_data_chunk_get_size(input);

	// extract the input vectors
	auto column_count = duckdb_data_chunk_get_column_count(input);
	std::vector<duckdb_vector> inputs;
	std::vector<int64_t *> data_ptrs;
	std::vector<uint64_t *> validity_masks;

	auto result_data = (int64_t *)duckdb_vector_get_data(output);
	duckdb_vector_ensure_validity_writable(output);
	auto result_validity = duckdb_vector_get_validity(output);

	// early-out by setting each row to NULL
	if (column_count == 0) {
		for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
			duckdb_validity_set_row_invalid(result_validity, row_idx);
		}
		return;
	}

	// setup
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		inputs.push_back(duckdb_data_chunk_get_vector(input, col_idx));
		auto data_ptr = (int64_t *)duckdb_vector_get_data(inputs.back());
		data_ptrs.push_back(data_ptr);
		auto validity_mask = duckdb_vector_get_validity(inputs.back());
		validity_masks.push_back(validity_mask);
	}

	// execution
	for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
		// validity check
		auto invalid = false;
		for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
			if (!duckdb_validity_row_is_valid(validity_masks[col_idx], row_idx)) {
				// not valid, set to NULL
				duckdb_validity_set_row_invalid(result_validity, row_idx);
				invalid = true;
				break;
			}
		}
		if (invalid) {
			continue;
		}

		result_data[row_idx] = 0;
		for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
			auto data = data_ptrs[col_idx][row_idx];
			result_data[row_idx] += data;
		}
	}
}

static duckdb_scalar_function CAPIGetScalarFunction(duckdb_connection connection, const char *name,
                                                    idx_t parameter_count = 2) {
	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(nullptr, name);
	duckdb_scalar_function_set_name(function, nullptr);
	duckdb_scalar_function_set_name(function, name);
	duckdb_scalar_function_set_name(function, name);

	// add a two bigint parameters
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_scalar_function_add_parameter(nullptr, type);
	duckdb_scalar_function_add_parameter(function, nullptr);
	for (idx_t idx = 0; idx < parameter_count; idx++) {
		duckdb_scalar_function_add_parameter(function, type);
	}

	// set the return type to bigint
	duckdb_scalar_function_set_return_type(nullptr, type);
	duckdb_scalar_function_set_return_type(function, nullptr);
	duckdb_scalar_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	// set up the function
	duckdb_scalar_function_set_function(nullptr, AddVariadicNumbersTogether);
	duckdb_scalar_function_set_function(function, nullptr);
	duckdb_scalar_function_set_function(function, AddVariadicNumbersTogether);
	return function;
}

static void CAPIRegisterAddition(duckdb_connection connection, const char *name, duckdb_state expected_outcome) {
	duckdb_state status;

	// create a scalar function
	auto function = CAPIGetScalarFunction(connection, name);

	// register and cleanup
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == expected_outcome);

	duckdb_destroy_scalar_function(&function);
	duckdb_destroy_scalar_function(&function);
	duckdb_destroy_scalar_function(nullptr);
}

TEST_CASE("Test Scalar Functions C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterAddition(tester.connection, "my_addition", DuckDBSuccess);
	// try to register it again - this should not be an error
	CAPIRegisterAddition(tester.connection, "my_addition", DuckDBSuccess);

	// now call it
	result = tester.Query("SELECT my_addition(40, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 42);

	result = tester.Query("SELECT my_addition(40, NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	result = tester.Query("SELECT my_addition(NULL, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	// call it over a vector of values
	result = tester.Query("SELECT my_addition(1000000, i) FROM range(10000) t(i)");
	REQUIRE_NO_FAIL(*result);
	for (idx_t row = 0; row < 10000; row++) {
		REQUIRE(result->Fetch<int64_t>(0, row) == static_cast<int64_t>(1000000 + row));
	}

	// v1 declares no parameter names, so the synthetic ones it is given cannot be used by a caller
	result = tester.Query("SELECT my_addition(col0 := 40, col1 := 2)");
	REQUIRE(result->HasError());
	REQUIRE(duckdb::StringUtil::Contains(result->ErrorMessage(), "No function matches"));
	result = tester.Query("SELECT my_addition(40, col1 := 2)");
	REQUIRE(result->HasError());
	REQUIRE(duckdb::StringUtil::Contains(result->ErrorMessage(), "No function matches"));
}

void ReturnStringInfo(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto extra_info = string((const char *)duckdb_scalar_function_get_extra_info(info));
	// get the total number of rows in this chunk
	auto input_size = duckdb_data_chunk_get_size(input);
	// extract the two input vectors
	auto input_vector = duckdb_data_chunk_get_vector(input, 0);
	// get the data pointers for the input vectors (both int64 as specified by the parameter types)
	auto input_data = (duckdb_string_t *)duckdb_vector_get_data(input_vector);
	// get the validity vectors
	auto input_validity = duckdb_vector_get_validity(input_vector);
	duckdb_vector_ensure_validity_writable(output);
	auto result_validity = duckdb_vector_get_validity(output);
	for (idx_t row = 0; row < input_size; row++) {
		if (duckdb_validity_row_is_valid(input_validity, row)) {
			// not null - do the operation
			auto input_string = input_data[row];
			string result = extra_info + "_";
			if (duckdb_string_is_inlined(input_string)) {
				result += string(input_string.value.inlined.inlined, input_string.value.inlined.length);
			} else {
				result += string(input_string.value.pointer.ptr, input_string.value.pointer.length);
			}
			duckdb_vector_assign_string_element_len(output, row, result.c_str(), result.size());
		} else {
			// either a or b is NULL - set the result row to NULL
			duckdb_validity_set_row_invalid(result_validity, row);
		}
	}
}

static void CAPIRegisterStringInfo(duckdb_connection connection, const char *name, duckdb_function_info info,
                                   duckdb_delete_callback_t destroy_func) {
	duckdb_state status;

	// create a scalar function
	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);

	// add a single varchar parameter
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_scalar_function_add_parameter(function, type);

	// set the return type to varchar
	duckdb_scalar_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	// set up the function
	duckdb_scalar_function_set_function(function, ReturnStringInfo);

	// set the extra info
	duckdb_scalar_function_set_extra_info(function, info, destroy_func);

	// register and cleanup
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - strings & extra_info", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	auto string_data = reinterpret_cast<char *>(malloc(100));
	strcpy(string_data, "my_prefix");
	auto extra_info = reinterpret_cast<duckdb_function_info>(string_data);

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterStringInfo(tester.connection, "my_prefix", extra_info, free);

	// now call it
	result = tester.Query("SELECT my_prefix('hello_world')");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<string>(0, 0) == "my_prefix_hello_world");

	result = tester.Query("SELECT my_prefix(NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));
}

static void CAPIRegisterVarargsFun(duckdb_connection connection, const char *name, duckdb_state expected_outcome) {
	duckdb_state status;

	// create a scalar function
	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);

	// set the variable arguments
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_scalar_function_set_varargs(function, type);

	// set the return type to bigint
	duckdb_scalar_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	// set up the function
	duckdb_scalar_function_set_function(function, AddVariadicNumbersTogether);

	// register and cleanup
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == expected_outcome);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - variadic number of input parameters", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterVarargsFun(tester.connection, "my_addition", DuckDBSuccess);

	result = tester.Query("SELECT my_addition(40, 2, 100, 3)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 145);

	result = tester.Query("SELECT my_addition(40, 42, NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	result = tester.Query("SELECT my_addition(NULL, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	result = tester.Query("SELECT my_addition()");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	result = tester.Query("SELECT my_addition('hello', [1])");
	REQUIRE_FAIL(result);
}

void CountNULLValues(duckdb_function_info, duckdb_data_chunk input, duckdb_vector output) {
	// Get the total number of rows and columns in this chunk.
	auto input_size = duckdb_data_chunk_get_size(input);
	auto column_count = duckdb_data_chunk_get_column_count(input);

	// Extract the validity masks.
	std::vector<uint64_t *> validity_masks;
	for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
		auto col = duckdb_data_chunk_get_vector(input, col_idx);
		auto validity_mask = duckdb_vector_get_validity(col);
		validity_masks.push_back(validity_mask);
	}

	// Execute the function.
	auto result_data = (uint64_t *)duckdb_vector_get_data(output);
	for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
		idx_t null_count = 0;
		idx_t other_null_count = 0;
		for (idx_t col_idx = 0; col_idx < column_count; col_idx++) {
			if (!duckdb_validity_row_is_valid(validity_masks[col_idx], row_idx)) {
				null_count++;
			}

			// Alternative code path using SQLNULL.
			auto duckdb_vector = duckdb_data_chunk_get_vector(input, col_idx);
			auto logical_type = duckdb_vector_get_column_type(duckdb_vector);
			auto type_id = duckdb_get_type_id(logical_type);
			if (type_id == DUCKDB_TYPE_SQLNULL) {
				other_null_count++;
			}
			duckdb_destroy_logical_type(&logical_type);
		}
		REQUIRE(null_count == other_null_count);
		result_data[row_idx] = null_count;
	}
}

static void CAPIRegisterANYFun(duckdb_connection connection, const char *name, duckdb_state expected_outcome) {
	duckdb_state status;

	// create a scalar function
	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);

	// set the variable arguments
	auto any_type = duckdb_create_logical_type(DUCKDB_TYPE_ANY);
	duckdb_scalar_function_set_varargs(function, any_type);
	duckdb_destroy_logical_type(&any_type);

	// Set special null handling.
	duckdb_scalar_function_set_special_handling(function);
	duckdb_scalar_function_set_volatile(function);
	duckdb_scalar_function_set_special_handling(nullptr);
	duckdb_scalar_function_set_volatile(nullptr);

	// set the return type uto bigint
	auto return_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
	duckdb_scalar_function_set_return_type(function, return_type);
	duckdb_destroy_logical_type(&return_type);

	// set up the function
	duckdb_scalar_function_set_function(function, CountNULLValues);

	// register and cleanup
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == expected_outcome);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - variadic number of ANY parameters", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterANYFun(tester.connection, "my_null_count", DuckDBSuccess);

	result = tester.Query("SELECT my_null_count(40, [1], 'hello', 3)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 0);

	result = tester.Query("SELECT my_null_count([1], 42, NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 1);

	result = tester.Query("SELECT my_null_count(NULL, NULL, NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 3);

	result = tester.Query("SELECT my_null_count()");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 0);
}

static void CAPIRegisterAdditionOverloads(duckdb_connection connection, const char *name,
                                          duckdb_state expected_outcome) {
	duckdb_state status;

	auto function_set = duckdb_create_scalar_function_set(name);
	// create a scalar function with 2 parameters
	auto function = CAPIGetScalarFunction(connection, name, 2);
	duckdb_add_scalar_function_to_set(function_set, function);
	duckdb_destroy_scalar_function(&function);

	// create a scalar function with 3 parameters
	function = CAPIGetScalarFunction(connection, name, 3);
	duckdb_add_scalar_function_to_set(function_set, function);
	duckdb_destroy_scalar_function(&function);

	// register and cleanup
	status = duckdb_register_scalar_function_set(connection, function_set);
	REQUIRE(status == expected_outcome);

	duckdb_destroy_scalar_function_set(&function_set);
	duckdb_destroy_scalar_function_set(&function_set);
	duckdb_destroy_scalar_function_set(nullptr);
}

TEST_CASE("Test Scalar Function Overloads C API", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterAdditionOverloads(tester.connection, "my_addition", DuckDBSuccess);
	// try to register it again - this should not be an error
	CAPIRegisterAdditionOverloads(tester.connection, "my_addition", DuckDBSuccess);

	// now call it
	result = tester.Query("SELECT my_addition(40, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 42);

	result = tester.Query("SELECT my_addition(40, 2, 2)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<int64_t>(0, 0) == 44);

	// call it over a vector of values
	result = tester.Query("SELECT my_addition(1000000, i, i) FROM range(10000) t(i)");
	REQUIRE_NO_FAIL(*result);
	for (idx_t row = 0; row < 10000; row++) {
		REQUIRE(result->Fetch<int64_t>(0, row) == static_cast<int64_t>(1000000 + row + row));
	}
}

struct ConnectionIdStruct {
	idx_t connection_id;
	idx_t folded_value;
};

void *CopyConnectionIdStruct(void *in_data_ptr) {
	auto in_data = reinterpret_cast<ConnectionIdStruct *>(in_data_ptr);
	auto out_data = reinterpret_cast<ConnectionIdStruct *>(malloc(sizeof(ConnectionIdStruct)));
	out_data->connection_id = in_data->connection_id;
	out_data->folded_value = in_data->folded_value;
	return out_data;
}

void GetConnectionIdBind(duckdb_bind_info info) {
	// Get the extra info.
	auto extra_info_ptr = duckdb_scalar_function_bind_get_extra_info(info);
	auto extra_info = string(reinterpret_cast<const char *>(extra_info_ptr));
	if (extra_info.empty()) {
		return;
	}

	// Get the connection ID.
	duckdb_client_context context;
	duckdb_scalar_function_get_client_context(info, &context);
	auto connection_id = duckdb_client_context_get_connection_id(context);

	// Get the expression.
	auto argument_count = duckdb_scalar_function_bind_get_argument_count(info);
	REQUIRE(argument_count == 1);
	auto expr = duckdb_scalar_function_bind_get_argument(info, 0);

	auto foldable = duckdb_expression_is_foldable(expr);
	if (!foldable) {
		duckdb_scalar_function_bind_set_error(info, "input argument must be foldable");
		duckdb_destroy_expression(&expr);
		duckdb_destroy_client_context(&context);
		return;
	}

	// Fold the expression.
	duckdb_value value;
	auto error_data = duckdb_expression_fold(context, expr, &value);
	auto has_error = duckdb_error_data_has_error(error_data);
	if (has_error) {
		auto error_msg = duckdb_error_data_message(error_data);
		duckdb_scalar_function_bind_set_error(info, error_msg);
		duckdb_destroy_expression(&expr);
		duckdb_destroy_client_context(&context);
		duckdb_destroy_error_data(&error_data);
		return;
	}

	auto value_type = duckdb_get_value_type(value);
	auto value_type_id = duckdb_get_type_id(value_type);
	REQUIRE(value_type_id == DUCKDB_TYPE_UBIGINT);
	auto uint64_value = duckdb_get_uint64(value);

	duckdb_destroy_value(&value);
	duckdb_destroy_expression(&expr);
	duckdb_destroy_client_context(&context);

	// Set the connection id.
	auto bind_data = reinterpret_cast<ConnectionIdStruct *>(malloc(sizeof(ConnectionIdStruct)));
	bind_data->connection_id = connection_id;
	bind_data->folded_value = uint64_value;
	duckdb_scalar_function_set_bind_data(info, bind_data, free);
	duckdb_scalar_function_set_bind_data_copy(info, CopyConnectionIdStruct);
}

void GetConnectionId(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto bind_data_ptr = duckdb_scalar_function_get_bind_data(info);
	if (bind_data_ptr == nullptr) {
		duckdb_scalar_function_set_error(info, "empty bind data");
		return;
	}

	auto bind_data = reinterpret_cast<ConnectionIdStruct *>(bind_data_ptr);
	auto input_size = duckdb_data_chunk_get_size(input);

	auto result_data = reinterpret_cast<uint64_t *>(duckdb_vector_get_data(output));
	for (idx_t row_idx = 0; row_idx < input_size; row_idx++) {
		result_data[row_idx] = bind_data->connection_id + bind_data->folded_value;
	}
}

static void CAPIRegisterGetConnectionId(duckdb_connection connection, bool is_volatile, string name) {
	duckdb_state status;

	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name.c_str());

	// Set the return type to UBIGINT.
	auto type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
	duckdb_scalar_function_add_parameter(function, type);
	duckdb_scalar_function_set_return_type(function, type);
	duckdb_destroy_logical_type(&type);

	if (is_volatile) {
		duckdb_scalar_function_set_volatile(function);
	}

	// Set up the bind and function callbacks.
	duckdb_scalar_function_set_bind(function, GetConnectionIdBind);
	duckdb_scalar_function_set_function(function, GetConnectionId);

	// Set some extra info to retrieve during binding.
	auto string_data = reinterpret_cast<char *>(malloc(100));
	strcpy(string_data, "my_prefix");
	auto extra_info = reinterpret_cast<duckdb_function_info>(string_data);
	duckdb_scalar_function_set_extra_info(function, extra_info, free);

	// Register and cleanup.
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Function with Bind Info", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterGetConnectionId(tester.connection, false, "get_connection_id");

	duckdb_client_context context;
	duckdb_connection_get_client_context(tester.connection, &context);
	auto first_conn_id = duckdb_client_context_get_connection_id(context);
	duckdb_destroy_client_context(&context);

	result = tester.Query("SELECT get_connection_id((40 + 2)::UBIGINT)");
	REQUIRE_NO_FAIL(*result);
	auto first_result = result->Fetch<uint64_t>(0, 0);
	REQUIRE(first_result == first_conn_id + 42);

	tester.ChangeConnection();

	duckdb_connection_get_client_context(tester.connection, &context);
	auto second_conn_id = duckdb_client_context_get_connection_id(context);
	duckdb_destroy_client_context(&context);

	result = tester.Query("SELECT get_connection_id((44 - 2)::UBIGINT)");
	REQUIRE_NO_FAIL(*result);
	auto second_result = result->Fetch<uint64_t>(0, 0);
	REQUIRE(second_conn_id + 42 == second_result);
	REQUIRE(first_result != second_result);

	result = tester.Query("SELECT get_connection_id(random()::UBIGINT)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->ErrorMessage(), "input argument must be foldable"));

	result = tester.Query("SELECT get_connection_id(200::UTINYINT + 200::UTINYINT)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->ErrorMessage(), "Overflow in addition of"));

	// Correlated subquery as argument: BoundSubqueryExpression::Copy() throws SerializationException.
	// Before the fix, this exception escaped the C API boundary and called std::terminate() because
	// Go's CGo frames (and plain C callers) have no C++ unwind tables. The fix catches at the C
	// boundary and surfaces it as a normal query error.
	result = tester.Query("SELECT get_connection_id((SELECT 42::UBIGINT)) FROM (VALUES(1)) t(x)");
	REQUIRE_FAIL(result);
}

TEST_CASE("Test volatile scalar function with bind in WHERE clause", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterGetConnectionId(tester.connection, true, "my_volatile_fun");

	result = tester.Query("SELECT true WHERE my_volatile_fun((40 + 2)::UBIGINT) != 0");
	REQUIRE(!result->HasError());
	REQUIRE(result->Fetch<bool>(0, 0));
}

void ListSum(duckdb_function_info, duckdb_data_chunk input, duckdb_vector output) {
	auto input_vector = duckdb_data_chunk_get_vector(input, 0);
	auto input_size = duckdb_data_chunk_get_size(input);
	auto input_validity = duckdb_vector_get_validity(input_vector);

	auto list_entry = reinterpret_cast<duckdb_list_entry *>(duckdb_vector_get_data(input_vector));
	auto list_child = duckdb_list_vector_get_child(input_vector);
	auto child_validity = duckdb_vector_get_validity(list_child);
	auto child_data = reinterpret_cast<uint64_t *>(duckdb_vector_get_data(list_child));

	auto result_data = reinterpret_cast<uint64_t *>(duckdb_vector_get_data(output));
	duckdb_vector_ensure_validity_writable(output);
	auto result_validity = duckdb_vector_get_validity(output);

	for (idx_t row = 0; row < input_size; row++) {
		if (!duckdb_validity_row_is_valid(input_validity, row)) {
			duckdb_validity_set_row_invalid(result_validity, row);
			continue;
		}
		auto entry = list_entry[row];
		auto offset = entry.offset;
		auto length = entry.length;
		uint64_t sum = 0;
		for (idx_t idx = offset; idx < offset + length; idx++) {
			if (duckdb_validity_row_is_valid(child_validity, idx)) {
				sum += child_data[idx];
			}
		}
		result_data[row] = sum;
	}
}

static void CAPIRegisterListSum(duckdb_connection connection, const char *name) {
	duckdb_state status;

	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);

	auto ubigint_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
	auto list_type = duckdb_create_list_type(ubigint_type);
	duckdb_scalar_function_add_parameter(function, list_type);
	duckdb_scalar_function_set_return_type(function, ubigint_type);
	duckdb_destroy_logical_type(&list_type);
	duckdb_destroy_logical_type(&ubigint_type);

	duckdb_scalar_function_set_function(function, ListSum);
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - LIST", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterListSum(tester.connection, "my_list_sum");

	result = tester.Query("SELECT my_list_sum([1::uint64])");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 1);

	result = tester.Query("SELECT my_list_sum(NULL)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->IsNull(0, 0));

	result = tester.Query("SELECT my_list_sum([])");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<uint64_t>(0, 0) == 0);
}

static void CounterFunctionBind(duckdb_bind_info info) {
	// Get the start counter from the extra info.
	auto extra_info_ptr = duckdb_scalar_function_bind_get_extra_info(info);
	auto &extra_info = *static_cast<int64_t *>(extra_info_ptr);

	auto bind_data_ptr = malloc(sizeof(int64_t));
	auto &bind_data = *reinterpret_cast<int64_t *>(bind_data_ptr);

	bind_data = extra_info + 10;

	duckdb_scalar_function_set_bind_data(info, bind_data_ptr, free);
	duckdb_scalar_function_set_bind_data_copy(info, [](void *ptr) -> void * {
		auto copy = static_cast<int64_t *>(malloc(sizeof(int64_t)));
		*copy = *static_cast<int64_t *>(ptr);
		return copy;
	});
}

static void CounterFunctionInit(duckdb_init_info info) {
	auto extra_info_ptr = duckdb_scalar_function_init_get_extra_info(info);
	auto &extra_info = *static_cast<int64_t *>(extra_info_ptr);

	auto bind_data_ptr = duckdb_scalar_function_init_get_bind_data(info);
	auto &bind_data = *reinterpret_cast<int64_t *>(bind_data_ptr);

	// Ensure we can get the client context
	duckdb_client_context context;
	duckdb_scalar_function_init_get_client_context(info, &context);
	REQUIRE(context != nullptr);
	duckdb_destroy_client_context(&context);

	if (extra_info < 0) {
		duckdb_scalar_function_init_set_error(info, "lower limit cannot be negative");
		return;
	}
	if (bind_data > 100) {
		duckdb_scalar_function_init_set_error(info, "upper limit cannot be greater than 100");
		return;
	}

	auto state_ptr = malloc(sizeof(int64_t));
	auto &state = *reinterpret_cast<int64_t *>(state_ptr);

	state = extra_info;

	duckdb_scalar_function_init_set_state(info, state_ptr, free);
}

static void CounterFunctionExec(duckdb_function_info info, duckdb_data_chunk input, duckdb_vector output) {
	auto state_ptr = duckdb_scalar_function_get_state(info);
	auto &state = *reinterpret_cast<int64_t *>(state_ptr);

	auto input_size = duckdb_data_chunk_get_size(input);
	auto result_data = reinterpret_cast<int64_t *>(duckdb_vector_get_data(output));
	for (idx_t row = 0; row < input_size; row++) {
		result_data[row] = state;
		state++;
	}
}

static void CAPIRegisterCounterFunction(duckdb_connection connection, const char *name, int64_t start) {
	duckdb_state status;

	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);
	// The counter advances on every row, even when its argument is constant.
	duckdb_scalar_function_set_volatile(function);

	auto bigint_type = duckdb_create_logical_type(DUCKDB_TYPE_BIGINT);
	duckdb_scalar_function_add_parameter(function, bigint_type);
	duckdb_scalar_function_set_return_type(function, bigint_type);
	duckdb_destroy_logical_type(&bigint_type);

	duckdb_scalar_function_set_bind(function, CounterFunctionBind);
	duckdb_scalar_function_set_init(function, CounterFunctionInit);
	duckdb_scalar_function_set_function(function, CounterFunctionExec);
	duckdb_scalar_function_set_extra_info(function, new int64_t(start),
	                                      [](void *ptr) { delete reinterpret_cast<int64_t *>(ptr); });

	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - Local State", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterCounterFunction(tester.connection, "my_counter", 5);

	result = tester.Query("SELECT my_counter(i) FROM range(3) r(i)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<idx_t>(0, 0) == 5);
	REQUIRE(result->Fetch<idx_t>(0, 1) == 6);
	REQUIRE(result->Fetch<idx_t>(0, 2) == 7);

	result = tester.Query("SELECT my_counter(0) FROM range(3)");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Fetch<idx_t>(0, 0) == 5);
	REQUIRE(result->Fetch<idx_t>(0, 1) == 6);
	REQUIRE(result->Fetch<idx_t>(0, 2) == 7);

	// Now test error conditions.
	CAPIRegisterCounterFunction(tester.connection, "my_counter_error_low", -5);
	result = tester.Query("SELECT my_counter_error_low(0)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->ErrorMessage(), "lower limit cannot be negative"));

	CAPIRegisterCounterFunction(tester.connection, "my_counter_error_high", 95);
	result = tester.Query("SELECT my_counter_error_high(0) FROM range(10)");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->ErrorMessage(), "upper limit cannot be greater than 100"));
}

void ReportErrorFunction(duckdb_function_info info, duckdb_data_chunk, duckdb_vector) {
	duckdb_scalar_function_set_error(info, "Error report test");
}

static void CAPIRegisterReportError(duckdb_connection connection, const char *name) {
	duckdb_state status;
	auto function = duckdb_create_scalar_function();
	duckdb_scalar_function_set_name(function, name);
	auto varchar_type = duckdb_create_logical_type(DUCKDB_TYPE_VARCHAR);
	duckdb_scalar_function_set_return_type(function, varchar_type);
	duckdb_destroy_logical_type(&varchar_type);
	duckdb_scalar_function_set_function(function, ReportErrorFunction);
	status = duckdb_register_scalar_function(connection, function);
	REQUIRE(status == DuckDBSuccess);
	duckdb_destroy_scalar_function(&function);
}

TEST_CASE("Test Scalar Functions - Error Reporting", "[capi]") {
	CAPITester tester;
	duckdb::unique_ptr<CAPIResult> result;

	REQUIRE(tester.OpenDatabase(nullptr));
	CAPIRegisterReportError(tester.connection, "my_report_error");

	result = tester.Query("SELECT my_report_error()");
	REQUIRE_FAIL(result);
	REQUIRE(StringUtil::Contains(result->ErrorMessage(), "Error report test"));
	REQUIRE(!StringUtil::Contains(result->ErrorMessage(), "INTERNAL Error:"));
}
