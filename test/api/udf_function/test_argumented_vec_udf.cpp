#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "udf_functions_to_test.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("Vectorized UDF functions using arguments", "[udf_function]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	string func_name, table_name, col_type;
	// The types supported by the templated CreateVectorizedFunction
	const vector<LogicalTypeId> all_sql_types = {
	    LogicalTypeId::BOOLEAN, LogicalTypeId::TINYINT, LogicalTypeId::SMALLINT, LogicalTypeId::DATE,
	    LogicalTypeId::TIME,    LogicalTypeId::INTEGER, LogicalTypeId::BIGINT,   LogicalTypeId::TIMESTAMP,
	    LogicalTypeId::FLOAT,   LogicalTypeId::DOUBLE,  LogicalTypeId::VARCHAR};

	// Creating the tables
	for (LogicalType sql_type : all_sql_types) {
		col_type = LogicalTypeIdToString(sql_type.id());
		table_name = StringUtil::Lower(col_type);

		con.Query("CREATE TABLE " + table_name + " (a " + col_type + ", b " + col_type + ", c " + col_type + ")");
	}

	// Create the UDF functions into the catalog
	for (LogicalType sql_type : all_sql_types) {
		func_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

		switch (sql_type.id()) {
		case LogicalTypeId::BOOLEAN: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::BOOLEAN}, LogicalType::BOOLEAN,
			                             &udf_unary_function<bool>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::BOOLEAN, LogicalType::BOOLEAN},
			                             LogicalType::BOOLEAN, &udf_binary_function<bool>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN},
			                             LogicalType::BOOLEAN, &udf_ternary_function<bool>);
			break;
		}
		case LogicalTypeId::TINYINT: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::TINYINT}, LogicalType::TINYINT,
			                             &udf_unary_function<int8_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::TINYINT, LogicalType::TINYINT},
			                             LogicalType::TINYINT, &udf_binary_function<int8_t>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::TINYINT, LogicalType::TINYINT, LogicalType::TINYINT},
			                             LogicalType::TINYINT, &udf_ternary_function<int8_t>);
			break;
		}
		case LogicalTypeId::SMALLINT: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::SMALLINT}, LogicalType::SMALLINT,
			                             &udf_unary_function<int16_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::SMALLINT, LogicalType::SMALLINT},
			                             LogicalType::SMALLINT, &udf_binary_function<int16_t>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::SMALLINT, LogicalType::SMALLINT, LogicalType::SMALLINT},
			                             LogicalType::SMALLINT, &udf_ternary_function<int16_t>);
			break;
		}
		case LogicalTypeId::DATE: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::DATE}, LogicalType::DATE,
			                             &udf_unary_function<date_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::DATE, LogicalType::DATE}, LogicalType::DATE,
			                             &udf_binary_function<date_t>);

			con.CreateVectorizedFunction(func_name + "_3", {LogicalType::DATE, LogicalType::DATE, LogicalType::DATE},
			                             LogicalType::DATE, &udf_ternary_function<date_t>);
			break;
		}
		case LogicalTypeId::TIME: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::TIME}, LogicalType::TIME,
			                             &udf_unary_function<dtime_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::TIME, LogicalType::TIME}, LogicalType::TIME,
			                             &udf_binary_function<dtime_t>);

			con.CreateVectorizedFunction(func_name + "_3", {LogicalType::TIME, LogicalType::TIME, LogicalType::TIME},
			                             LogicalType::TIME, &udf_ternary_function<dtime_t>);
			break;
		}
		case LogicalTypeId::INTEGER: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::INTEGER}, LogicalType::INTEGER,
			                             &udf_unary_function<int32_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::INTEGER, LogicalType::INTEGER},
			                             LogicalType::INTEGER, &udf_binary_function<int32_t>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER},
			                             LogicalType::INTEGER, &udf_ternary_function<int32_t>);
			break;
		}
		case LogicalTypeId::BIGINT: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::BIGINT}, LogicalType::BIGINT,
			                             &udf_unary_function<int64_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::BIGINT, LogicalType::BIGINT},
			                             LogicalType::BIGINT, &udf_binary_function<int64_t>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT},
			                             LogicalType::BIGINT, &udf_ternary_function<int64_t>);
			break;
		}
		case LogicalTypeId::TIMESTAMP: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
			                             &udf_unary_function<timestamp_t>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP},
			                             LogicalType::TIMESTAMP, &udf_binary_function<timestamp_t>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP},
			                             LogicalType::TIMESTAMP, &udf_ternary_function<timestamp_t>);
			break;
		}
		case LogicalTypeId::FLOAT:
		case LogicalTypeId::DOUBLE: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
			                             &udf_unary_function<double>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::DOUBLE, LogicalType::DOUBLE},
			                             LogicalType::DOUBLE, &udf_binary_function<double>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE},
			                             LogicalType::DOUBLE, &udf_ternary_function<double>);
			break;
		}
		case LogicalTypeId::VARCHAR: {
			con.CreateVectorizedFunction(func_name + "_1", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
			                             &udf_unary_function<char *>);

			con.CreateVectorizedFunction(func_name + "_2", {LogicalType::VARCHAR, LogicalType::VARCHAR},
			                             LogicalType::VARCHAR, &udf_binary_function<char *>);

			con.CreateVectorizedFunction(func_name + "_3",
			                             {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
			                             LogicalType::VARCHAR, &udf_ternary_function<char *>);
			break;
		}
		default:
			break;
		}
	}

	SECTION("Testing Vectorized UDF functions") {
		// Inserting values
		for (LogicalType sql_type : all_sql_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

			string query = "INSERT INTO " + table_name + " VALUES";
			if (sql_type == LogicalType::BOOLEAN) {
				con.Query(query + "(true, true, true), (true, true, false), (false, false, false);");
			} else if (sql_type.IsNumeric()) {
				con.Query(query + "(1, 10, 100),(2, 20, 100),(3, 30, 100);");
			} else if (sql_type == LogicalType::VARCHAR) {
				con.Query(query + "('a', 'b', 'c'),('a', 'b', 'c'),('a', 'b', 'c');");
			} else if (sql_type == LogicalType::DATE) {
				con.Query(query + "('2008-01-01', '2009-01-01', '2010-01-01')," +
				          "('2008-01-01', '2009-01-01', '2010-01-01')," + "('2008-01-01', '2009-01-01', '2010-01-01')");
			} else if (sql_type == LogicalType::TIME) {
				con.Query(query + "('01:00:00', '02:00:00', '03:00:00')," + "('04:00:00', '05:00:00', '06:00:00')," +
				          "('07:00:00', '08:00:00', '09:00:00')");
			} else if (sql_type == LogicalType::TIMESTAMP) {
				con.Query(query + "('2008-01-01 00:00:00', '2009-01-01 00:00:00', '2010-01-01 00:00:00')," +
				          "('2008-01-01 00:00:00', '2009-01-01 00:00:00', '2010-01-01 00:00:00')," +
				          "('2008-01-01 00:00:00', '2009-01-01 00:00:00', '2010-01-01 00:00:00')");
			}
		}

		// Running the UDF functions and checking the results
		for (LogicalType sql_type : all_sql_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));
			func_name = table_name;
			if (sql_type.IsNumeric()) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {1, 2, 3}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {10, 20, 30}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {100, 100, 100}));

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
			} else if (sql_type == LogicalType::DATE) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2008-01-01", "2008-01-01", "2008-01-01"}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2009-01-01", "2009-01-01", "2009-01-01"}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2010-01-01", "2010-01-01", "2010-01-01"}));
			} else if (sql_type == LogicalType::TIME) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"01:00:00", "04:00:00", "07:00:00"}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"02:00:00", "05:00:00", "08:00:00"}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"03:00:00", "06:00:00", "09:00:00"}));
			} else if (sql_type == LogicalType::TIMESTAMP) {
				result = con.Query("SELECT " + func_name + "_1(a) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2008-01-01 00:00:00", "2008-01-01 00:00:00", "2008-01-01 00:00:00"}));

				result = con.Query("SELECT " + func_name + "_2(a, b) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2009-01-01 00:00:00", "2009-01-01 00:00:00", "2009-01-01 00:00:00"}));

				result = con.Query("SELECT " + func_name + "_3(a, b, c) FROM " + table_name);
				REQUIRE(CHECK_COLUMN(result, 0, {"2010-01-01 00:00:00", "2010-01-01 00:00:00", "2010-01-01 00:00:00"}));
			}
		}
	}

	SECTION("Cheking NULLs with Vectorized UDF functions") {
		for (LogicalType sql_type : all_sql_types) {
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
		vector<LogicalType> sql_args = {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER,
		                                LogicalType::INTEGER};
		// UDF with 4 input ints, return the last one
		con.CreateVectorizedFunction("udf_four_ints", sql_args, LogicalType::INTEGER,
		                             &udf_several_constant_input<int, 4>);
		result = con.Query("SELECT udf_four_ints(1, 2, 3, 4)");
		REQUIRE(CHECK_COLUMN(result, 0, {4}));

		// UDF with 5 input ints, return the last one
		sql_args.push_back(LogicalType::INTEGER);
		con.CreateVectorizedFunction("udf_five_ints", sql_args, LogicalType::INTEGER,
		                             &udf_several_constant_input<int, 5>);
		result = con.Query("SELECT udf_five_ints(1, 2, 3, 4, 5)");
		REQUIRE(CHECK_COLUMN(result, 0, {5}));

		// UDF with 10 input ints, return the last one
		for (idx_t i = 0; i < 5; ++i) {
			// adding more 5 items
			sql_args.push_back(LogicalType::INTEGER);
		}
		con.CreateVectorizedFunction("udf_ten_ints", sql_args, LogicalType::INTEGER,
		                             &udf_several_constant_input<int, 10>);
		result = con.Query("SELECT udf_ten_ints(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)");
		REQUIRE(CHECK_COLUMN(result, 0, {10}));
	}

	SECTION("Cheking Vectorized UDF functions with varargs and constant values") {
		// Test udf_max with integer
		con.CreateVectorizedFunction("udf_const_max_int", {LogicalType::INTEGER}, LogicalType::INTEGER,
		                             &udf_max_constant<int>, LogicalType::INTEGER);
		result = con.Query("SELECT udf_const_max_int(1, 2, 3, 4, 999, 5, 6, 7)");
		REQUIRE(CHECK_COLUMN(result, 0, {999}));

		result = con.Query("SELECT udf_const_max_int(1, 2, 3, 4, 5, 6, 7, 8, 9, 10)");
		REQUIRE(CHECK_COLUMN(result, 0, {10}));

		// Test udf_max with double
		con.CreateVectorizedFunction("udf_const_max_double", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
		                             &udf_max_constant<double>, LogicalType::DOUBLE);
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

		con.CreateVectorizedFunction("udf_flat_max_int", {LogicalType::INTEGER}, LogicalType::INTEGER,
		                             &udf_max_flat<int>, LogicalType::INTEGER);
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

		con.CreateVectorizedFunction("udf_flat_max_double", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
		                             &udf_max_flat<double>, LogicalType::DOUBLE);
		result = con.Query("SELECT udf_flat_max_double(a, b, c, d) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_double(d, c, b, a) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {4, 40, 400, 4000}));

		result = con.Query("SELECT udf_flat_max_double(c, b) FROM doubles");
		REQUIRE(CHECK_COLUMN(result, 0, {3, 30, 300, 3000}));
	}
}
