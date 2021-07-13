#include "catch.hpp"
#include "test_helpers.hpp"
#include "udf_functions_to_test.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("UDF functions with template", "[udf_function]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	string func_name, table_name, col_type;
	// The types supported by the templated CreateScalarFunction
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
			con.CreateScalarFunction<bool, bool>(func_name + "_1", &udf_bool);
			con.CreateScalarFunction<bool, bool, bool>(func_name + "_2", &udf_bool);
			con.CreateScalarFunction<bool, bool, bool, bool>(func_name + "_3", &udf_bool);
			break;
		}
		case LogicalTypeId::TINYINT: {
			con.CreateScalarFunction<int8_t, int8_t>(func_name + "_1", &udf_int8);
			con.CreateScalarFunction<int8_t, int8_t, int8_t>(func_name + "_2", &udf_int8);
			con.CreateScalarFunction<int8_t, int8_t, int8_t, int8_t>(func_name + "_3", &udf_int8);
			break;
		}
		case LogicalTypeId::SMALLINT: {
			con.CreateScalarFunction<int16_t, int16_t>(func_name + "_1", &udf_int16);
			con.CreateScalarFunction<int16_t, int16_t, int16_t>(func_name + "_2", &udf_int16);
			con.CreateScalarFunction<int16_t, int16_t, int16_t, int16_t>(func_name + "_3", &udf_int16);
			break;
		}
		case LogicalTypeId::INTEGER: {
			con.CreateScalarFunction<int32_t, int32_t>(func_name + "_1", &udf_int);
			con.CreateScalarFunction<int32_t, int32_t, int32_t>(func_name + "_2", &udf_int);
			con.CreateScalarFunction<int32_t, int32_t, int32_t, int32_t>(func_name + "_3", &udf_int);
			break;
		}
		case LogicalTypeId::BIGINT: {
			con.CreateScalarFunction<int64_t, int64_t>(func_name + "_1", &udf_int64);
			con.CreateScalarFunction<int64_t, int64_t, int64_t>(func_name + "_2", &udf_int64);
			con.CreateScalarFunction<int64_t, int64_t, int64_t, int64_t>(func_name + "_3", &udf_int64);
			break;
		}
		case LogicalTypeId::FLOAT:
			// FIXME: there is an implicit cast to DOUBLE before calling the function: float_1(CAST[DOUBLE](a)),
			// because of that we cannot invoke such a function: float udf_float(float a);
			//		{
			//			con.CreateScalarFunction<float, float>(func_name + "_1", &FLOAT);
			//			con.CreateScalarFunction<float, float, float>(func_name + "_2", &FLOAT);
			//			con.CreateScalarFunction<float, float, float, float>(func_name + "_3", &FLOAT);
			//			break;
			//		}
		case LogicalTypeId::DOUBLE: {
			con.CreateScalarFunction<double, double>(func_name + "_1", &udf_double);
			con.CreateScalarFunction<double, double, double>(func_name + "_2", &udf_double);
			con.CreateScalarFunction<double, double, double, double>(func_name + "_3", &udf_double);
			break;
		}
		case LogicalTypeId::VARCHAR: {
			con.CreateScalarFunction<string_t, string_t>(func_name + "_1", &udf_varchar);
			con.CreateScalarFunction<string_t, string_t, string_t>(func_name + "_2", &udf_varchar);
			con.CreateScalarFunction<string_t, string_t, string_t, string_t>(func_name + "_3", &udf_varchar);
			break;
		}
		default:
			break;
		}
	}

	SECTION("Testing UDF functions") {
		// Inserting values
		for (LogicalType sql_type : sql_templated_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

			string query = "INSERT INTO " + table_name + " VALUES";
			if (sql_type == LogicalType::BOOLEAN) {
				con.Query(query + "(true, true, true), (true, true, false), (false, false, false);");
			} else if (sql_type.IsNumeric()) {
				con.Query(query + "(1, 10, 100),(2, 10, 100),(3, 10, 100);");
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
				REQUIRE(CHECK_COLUMN(result, 0, {111, 112, 113}));

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

	SECTION("Checking NULLs with UDF functions") {
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
}
