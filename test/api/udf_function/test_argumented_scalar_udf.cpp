#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "udf_functions_to_test.hpp"

using namespace duckdb;
using namespace std;

TEST_CASE("UDF functions with arguments", "[udf_function]") {
	unique_ptr<QueryResult> result;
	DuckDB db(nullptr);
	Connection con(db);
	con.EnableQueryVerification();

	string func_name, table_name, col_type;
	// The types supported by the argumented CreateScalarFunction
	const vector<LogicalTypeId> all_sql_types = {
	    LogicalTypeId::BOOLEAN, LogicalTypeId::TINYINT, LogicalTypeId::SMALLINT, LogicalTypeId::DATE,
	    LogicalTypeId::TIME,    LogicalTypeId::INTEGER, LogicalTypeId::BIGINT,   LogicalTypeId::TIMESTAMP,
	    LogicalTypeId::FLOAT,   LogicalTypeId::DOUBLE,  LogicalTypeId::DECIMAL,  LogicalTypeId::VARCHAR};

	// Creating the tables
	for (LogicalType sql_type : all_sql_types) {
		col_type = LogicalTypeIdToString(sql_type.id());
		table_name = StringUtil::Lower(col_type);

		con.Query("CREATE TABLE " + table_name + " (a " + col_type + ", b " + col_type + ", c " + col_type + ")");
	}

	// Creating the UDF functions into the catalog
	for (LogicalType sql_type : all_sql_types) {
		func_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

		switch (sql_type.id()) {
		case LogicalTypeId::BOOLEAN: {
			con.CreateScalarFunction<bool, bool>(func_name + "_1", {LogicalType::BOOLEAN}, LogicalType::BOOLEAN,
			                                     &udf_bool);

			con.CreateScalarFunction<bool, bool, bool>(func_name + "_2", {LogicalType::BOOLEAN, LogicalType::BOOLEAN},
			                                           LogicalType::BOOLEAN, &udf_bool);

			con.CreateScalarFunction<bool, bool, bool, bool>(
			    func_name + "_3", {LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::BOOLEAN},
			    LogicalType::BOOLEAN, &udf_bool);
			break;
		}
		case LogicalTypeId::TINYINT: {
			con.CreateScalarFunction<int8_t, int8_t>(func_name + "_1", {LogicalType::TINYINT}, LogicalType::TINYINT,
			                                         &udf_int8);

			con.CreateScalarFunction<int8_t, int8_t, int8_t>(
			    func_name + "_2", {LogicalType::TINYINT, LogicalType::TINYINT}, LogicalType::TINYINT, &udf_int8);

			con.CreateScalarFunction<int8_t, int8_t, int8_t, int8_t>(
			    func_name + "_3", {LogicalType::TINYINT, LogicalType::TINYINT, LogicalType::TINYINT},
			    LogicalType::TINYINT, &udf_int8);
			break;
		}
		case LogicalTypeId::SMALLINT: {
			con.CreateScalarFunction<int16_t, int16_t>(func_name + "_1", {LogicalType::SMALLINT}, LogicalType::SMALLINT,
			                                           &udf_int16);

			con.CreateScalarFunction<int16_t, int16_t, int16_t>(
			    func_name + "_2", {LogicalType::SMALLINT, LogicalType::SMALLINT}, LogicalType::SMALLINT, &udf_int16);

			con.CreateScalarFunction<int16_t, int16_t, int16_t, int16_t>(
			    func_name + "_3", {LogicalType::SMALLINT, LogicalType::SMALLINT, LogicalType::SMALLINT},
			    LogicalType::SMALLINT, &udf_int16);
			break;
		}
		case LogicalTypeId::DATE: {
			con.CreateScalarFunction<date_t, date_t>(func_name + "_1", {LogicalType::DATE}, LogicalType::DATE,
			                                         &udf_date);

			con.CreateScalarFunction<date_t, date_t, date_t>(func_name + "_2", {LogicalType::DATE, LogicalType::DATE},
			                                                 LogicalType::DATE, &udf_date);

			con.CreateScalarFunction<date_t, date_t, date_t, date_t>(
			    func_name + "_3", {LogicalType::DATE, LogicalType::DATE, LogicalType::DATE}, LogicalType::DATE,
			    &udf_date);
			break;
		}
		case LogicalTypeId::TIME: {
			con.CreateScalarFunction<dtime_t, dtime_t>(func_name + "_1", {LogicalType::TIME}, LogicalType::TIME,
			                                           &udf_time);

			con.CreateScalarFunction<dtime_t, dtime_t, dtime_t>(
			    func_name + "_2", {LogicalType::TIME, LogicalType::TIME}, LogicalType::TIME, &udf_time);

			con.CreateScalarFunction<dtime_t, dtime_t, dtime_t, dtime_t>(
			    func_name + "_3", {LogicalType::TIME, LogicalType::TIME, LogicalType::TIME}, LogicalType::TIME,
			    &udf_time);
			break;
		}
		case LogicalTypeId::INTEGER: {
			con.CreateScalarFunction<int32_t, int32_t>(func_name + "_1", {LogicalType::INTEGER}, LogicalType::INTEGER,
			                                           &udf_int);

			con.CreateScalarFunction<int32_t, int32_t, int32_t>(
			    func_name + "_2", {LogicalType::INTEGER, LogicalType::INTEGER}, LogicalType::INTEGER, &udf_int);

			con.CreateScalarFunction<int32_t, int32_t, int32_t, int32_t>(
			    func_name + "_3", {LogicalType::INTEGER, LogicalType::INTEGER, LogicalType::INTEGER},
			    LogicalType::INTEGER, &udf_int);
			break;
		}
		case LogicalTypeId::BIGINT: {
			con.CreateScalarFunction<int64_t, int64_t>(func_name + "_1", {LogicalType::BIGINT}, LogicalType::BIGINT,
			                                           &udf_int64);

			con.CreateScalarFunction<int64_t, int64_t, int64_t>(
			    func_name + "_2", {LogicalType::BIGINT, LogicalType::BIGINT}, LogicalType::BIGINT, &udf_int64);

			con.CreateScalarFunction<int64_t, int64_t, int64_t, int64_t>(
			    func_name + "_3", {LogicalType::BIGINT, LogicalType::BIGINT, LogicalType::BIGINT}, LogicalType::BIGINT,
			    &udf_int64);
			break;
		}
		case LogicalTypeId::TIMESTAMP: {
			con.CreateScalarFunction<timestamp_t, timestamp_t>(func_name + "_1", {LogicalType::TIMESTAMP},
			                                                   LogicalType::TIMESTAMP, &udf_timestamp);

			con.CreateScalarFunction<timestamp_t, timestamp_t, timestamp_t>(
			    func_name + "_2", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP}, LogicalType::TIMESTAMP,
			    &udf_timestamp);

			con.CreateScalarFunction<timestamp_t, timestamp_t, timestamp_t, timestamp_t>(
			    func_name + "_3", {LogicalType::TIMESTAMP, LogicalType::TIMESTAMP, LogicalType::TIMESTAMP},
			    LogicalType::TIMESTAMP, &udf_timestamp);
			break;
		}
		case LogicalTypeId::FLOAT:
			// FIXME: there is an implicit cast to double before calling the function
			// float_1(CAST[DOUBLE](a))
			//		{
			//			con.CreateScalarFunction<float, float>(func_name + "_1", &FLOAT);
			//			con.CreateScalarFunction<float, float, float>(func_name + "_2", &FLOAT);
			//			con.CreateScalarFunction<float, float, float, float>(func_name + "_3", &FLOAT);
			//			break;
			//		}
		case LogicalTypeId::DOUBLE: {
			con.CreateScalarFunction<double, double>(func_name + "_1", {LogicalType::DOUBLE}, LogicalType::DOUBLE,
			                                         &udf_double);

			con.CreateScalarFunction<double, double, double>(
			    func_name + "_2", {LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE, &udf_double);

			con.CreateScalarFunction<double, double, double, double>(
			    func_name + "_3", {LogicalType::DOUBLE, LogicalType::DOUBLE, LogicalType::DOUBLE}, LogicalType::DOUBLE,
			    &udf_double);
			break;
		}
		case LogicalTypeId::VARCHAR: {
			con.CreateScalarFunction<string_t, string_t>(func_name + "_1", {LogicalType::VARCHAR}, LogicalType::VARCHAR,
			                                             &udf_varchar);

			con.CreateScalarFunction<string_t, string_t, string_t>(
			    func_name + "_2", {LogicalType::VARCHAR, LogicalType::VARCHAR}, LogicalType::VARCHAR, &udf_varchar);

			con.CreateScalarFunction<string_t, string_t, string_t, string_t>(
			    func_name + "_3", {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR},
			    LogicalType::VARCHAR, &udf_varchar);
			break;
		}
		default:
			break;
		}
	}

	SECTION("Testing UDF functions") {
		// Inserting values
		for (LogicalType sql_type : all_sql_types) {
			table_name = StringUtil::Lower(LogicalTypeIdToString(sql_type.id()));

			string query = "INSERT INTO " + table_name + " VALUES";
			if (sql_type == LogicalType::BOOLEAN) {
				con.Query(query + "(true, true, true), (true, true, false), (false, false, false);");
			} else if (sql_type.IsNumeric()) {
				con.Query(query + "(1, 10, 100),(2, 10, 100),(3, 10, 100);");
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
			if (sql_type.id() == LogicalTypeId::DECIMAL) {
				continue;
			}
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

	SECTION("Checking NULLs with UDF functions") {
		for (LogicalType sql_type : all_sql_types) {
			if (sql_type.id() == LogicalTypeId::DECIMAL) {
				continue;
			}
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
