#include "catch.hpp"
#include "test_helpers.hpp"
#include "udf_functions_to_test.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Vectorized UDF functions using templates", "[udf_function]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	string func_name, table_name, col_type;
	// The types supported by the templated CreateVectorizedFunction
	const vector<LogicalType> sql_templated_types = {LogicalType::BOOLEAN, LogicalType::TINYINT, LogicalType::SMALLINT,
	                                                 LogicalType::INTEGER, LogicalType::BIGINT,  LogicalType::FLOAT,
	                                                 LogicalType::DOUBLE,  LogicalType::VARCHAR};

	// Creating the tables
	for (LogicalType sql_type : sql_templated_types) {
		col_type = LogicalTypeIdToString(sql_type.id());
		table_name = StringUtil::Lower(col_type);

		con.Query("CREATE TABLE " + table_name + " (a " + col_type + ", b " + col_type + ", c " + col_type + ")");
	}

	// Create the UDF functions into the catalog
	for (LogicalType sql_type : sql_templated_types) {
		func_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

		switch (sql_type.id()) {
		case LogicalTypeId::BOOLEAN: {
			con.CreateVectorizedFunction<bool, bool>(func_name + "_1", &udf_unary_function<bool>);
			con.CreateVectorizedFunction<bool, bool, bool>(func_name + "_2", &udf_binary_function<bool>);
			con.CreateVectorizedFunction<bool, bool, bool, bool>(func_name + "_3", &udf_ternary_function<bool>);
			break;
		}
		case LogicalTypeId::TINYINT: {
			con.CreateVectorizedFunction<int8_t, int8_t>(func_name + "_1", &udf_unary_function<int8_t>);
			con.CreateVectorizedFunction<int8_t, int8_t, int8_t>(func_name + "_2", &udf_binary_function<int8_t>);
			con.CreateVectorizedFunction<int8_t, int8_t, int8_t, int8_t>(func_name + "_3",
			                                                             &udf_ternary_function<int8_t>);
			break;
		}
		case LogicalTypeId::SMALLINT: {
			con.CreateVectorizedFunction<int16_t, int16_t>(func_name + "_1", &udf_unary_function<int16_t>);
			con.CreateVectorizedFunction<int16_t, int16_t, int16_t>(func_name + "_2", &udf_binary_function<int16_t>);
			con.CreateVectorizedFunction<int16_t, int16_t, int16_t, int16_t>(func_name + "_3",
			                                                                 &udf_ternary_function<int16_t>);
			break;
		}
		case LogicalTypeId::INTEGER: {
			con.CreateVectorizedFunction<int, int>(func_name + "_1", &udf_unary_function<int>);
			con.CreateVectorizedFunction<int, int, int>(func_name + "_2", &udf_binary_function<int>);
			con.CreateVectorizedFunction<int, int, int, int>(func_name + "_3", &udf_ternary_function<int>);
			break;
		}
		case LogicalTypeId::BIGINT: {
			con.CreateVectorizedFunction<int64_t, int64_t>(func_name + "_1", &udf_unary_function<int64_t>);
			con.CreateVectorizedFunction<int64_t, int64_t, int64_t>(func_name + "_2", &udf_binary_function<int64_t>);
			con.CreateVectorizedFunction<int64_t, int64_t, int64_t, int64_t>(func_name + "_3",
			                                                                 &udf_ternary_function<int64_t>);
			break;
		}
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE: {
			con.CreateVectorizedFunction<double, double>(func_name + "_1", &udf_unary_function<double>);
			con.CreateVectorizedFunction<double, double, double>(func_name + "_2", &udf_binary_function<double>);
			con.CreateVectorizedFunction<double, double, double, double>(func_name + "_3",
			                                                             &udf_ternary_function<double>);
			break;
		}
		case LogicalTypeId::VARCHAR: {
			con.CreateVectorizedFunction<string_t, string_t>(func_name + "_1", &udf_unary_function<char *>);
			con.CreateVectorizedFunction<string_t, string_t, string_t>(func_name + "_2", &udf_binary_function<char *>);
			con.CreateVectorizedFunction<string_t, string_t, string_t, string_t>(func_name + "_3",
			                                                                     &udf_ternary_function<char *>);
			break;
		}
		default:
			break;
		}
	}

	SECTION("Testing Vectorized UDF functions") {
		// Inserting values
		for (LogicalType sql_type : sql_templated_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

			string query = "INSERT INTO " + table_name + " VALUES";
			if (sql_type == LogicalType::BOOLEAN) {
				con.Query(query + "(true, true, true), (true, true, false), (false, false, false);");
			} else if (sql_type.IsNumeric()) {
				con.Query(query + "(1, 10, 101),(2, 20, 102),(3, 30, 103);");
			} else if (sql_type == LogicalType::VARCHAR) {
				con.Query(query + "('a', 'b', 'c'),('a', 'b', 'c'),('a', 'b', 'c');");
			}
		}

		// Running the UDF functions and checking the results
		for (LogicalType sql_type : sql_templated_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));
			func_name = table_name;
			if (sql_type.IsNumeric()) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {10, 20, 30}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {101, 102, 103}));

			} else if (sql_type == LogicalType::BOOLEAN) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {true, true, false}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {true, true, false}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {true, false, false}));

			} else if (sql_type == LogicalType::VARCHAR) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"a", "a", "a"}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"b", "b", "b"}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"c", "c", "c"}));
			}
		}
	}

	SECTION("Cheking NULLs with Vectorized UDF functions") {
		for (LogicalType sql_type : sql_templated_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));
			func_name = table_name;

			// Deleting old values
			REQUIRE_NO_FAIL(con.Query("DELETE FROM " + table_name));

			// Inserting NULLs
			string query = "INSERT INTO " + table_name + " VALUES";
			con.Query(query + "(NULL, NULL, NULL), (NULL, NULL, NULL), (NULL, NULL, NULL);");

			// Testing NULLs
			result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
			REQUIRE(CHECK_COLUMN(result, 0, {Value(nullptr), Value(nullptr), Value(nullptr)}));

			result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
			REQUIRE(CHECK_COLUMN(result, 0, {Value(nullptr), Value(nullptr), Value(nullptr)}));

			result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
			REQUIRE(CHECK_COLUMN(result, 0, {Value(nullptr), Value(nullptr), Value(nullptr)}));
		}
	}

	SECTION("Cheking Vectorized UDF functions with several input columns") {
		// UDF with 4 input ints, return the last one
		con.CreateVectorizedFunction<int, int, int, int, int>("udf_four_ints", &udf_several_constant_input<int, 4>);
		result = con.Query("SELECT udf_four_ints(1, 2, 3, 4)");
		REQUIRE(CHECK_COLUMN(result, 0, {4}));

		// UDF with 5 input ints, return the last one
		con.CreateVectorizedFunction<int, int, int, int, int, int>("udf_five_ints",
		                                                           &udf_several_constant_input<int, 5>);
		result = con.Query("SELECT udf_five_ints(1, 2, 3, 4, 5)");
		REQUIRE(CHECK_COLUMN(result, 0, {5}));

		// UDF with 10 input ints, return the last one
		con.CreateVectorizedFunction<int, int, int, int, int, int, int, int, int, int, int>(
		    "udf_ten_ints", &udf_several_constant_input<int, 10>);
		result = con.Query("SELECT udf_ten_ints(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)");
		REQUIRE(CHECK_COLUMN(result, 0, {10}));
	}

	SECTION("Cheking Vectorized UDF functions with varargs and constant values") {
		// Test udf_max with integer
		con.CreateVectorizedFunction<int, int>("udf_const_max_int", &udf_max_constant<int>, LogicalType::INTEGER);
		result = con.Query("SELECT udf_const_max_int(1, 2, 3, 4, 999, 5, 6, 7)");
		REQUIRE(CHECK_COLUMN(result, 0, {999}));

		result = con.Query("SELECT udf_const_max_int(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)");
		REQUIRE(CHECK_COLUMN(result, 0, {10}));

		// Test udf_max with double
		con.CreateVectorizedFunction<double, double>("udf_const_max_double", &udf_max_constant<double>,
		                                             LogicalType::DOUBLE);
		result = con.Query("SELECT udf_const_max_double(1.0, 2.0, 3.0, 4.0, 999.0, 5.0, 6.0, 7.0)");
		REQUIRE(CHECK_COLUMN(result, 0, {999.0}));

		result = con.Query("SELECT udf_const_max_double(1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 10.0)");
		REQUIRE(CHECK_COLUMN(result, 0, {10.0}));
	}

	SECTION("Cheking Vectorized UDF functions with varargs and input columns") {
		// Test udf_max with integer
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE integers (a INTEGER, b INTEGER, c INTEGER, d INTEGER)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO integers VALUES(1, 2, 3, 4), (10, 20, 30, 40), (100, 200, 300, 400), "
		                          "(1000, 2000, 3000, 4000)"));

		con.CreateVectorizedFunction<int, int>("udf_flat_max_int", &udf_max_flat<int>, LogicalType::INTEGER);
		result = con.Query("SELECT udf_flat_max_int(a, b, c, d) FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_int(d, c, b, a) FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_int(c, b) FROM integers");
		REQUIRE(CHECK_COLUMN(result, 0, {3, 30, 300, 3000}));

		// Test udf_max with double
		REQUIRE_NO_FAIL(con.Query("CREATE TABLE doubles (a DOUBLE, b DOUBLE, c DOUBLE, d DOUBLE)"));
		REQUIRE_NO_FAIL(con.Query("INSERT INTO doubles VALUES(1, 2, 3, 4), (10, 20, 30, 40), (100, 200, 300, 400), "
		                          "(1000, 2000, 3000, 4000)"));

		con.CreateVectorizedFunction<double, double>("udf_flat_max_double", &udf_max_flat<double>, LogicalType::DOUBLE);
		result = con.Query("SELECT udf_flat_max_double(a, b, c, d) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_double(d, c, b, a) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_double(c, b) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {3, 30, 300, 3000}));
	}
}
