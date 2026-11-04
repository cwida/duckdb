#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/function/table/system_functions.hpp"

#include <cmath>
#include <limits>

#include "duckdb/common/types/bignum.hpp"

namespace duckdb {

struct TestAllTypesData : public GlobalTableFunctionState {
	TestAllTypesData() : offset(0) {
	}

	vector<vector<Value>> entries;
	idx_t offset;
};

vector<TestType> TestAllTypesFun::GetTestTypes(const bool use_large_enum, const bool use_large_bignum) {
	vector<TestType> result;

	// Numeric types.
	result.emplace_back(LogicalType::BOOLEAN, "bool");
	result.emplace_back(LogicalType::TINYINT, "tinyint");
	result.emplace_back(LogicalType::SMALLINT, "smallint");
	result.emplace_back(LogicalType::INTEGER, "int");
	result.emplace_back(LogicalType::BIGINT, "bigint");
	result.emplace_back(LogicalType::HUGEINT, "hugeint");
	result.emplace_back(LogicalType::UHUGEINT, "uhugeint");
	result.emplace_back(LogicalType::UTINYINT, "utinyint");
	result.emplace_back(LogicalType::USMALLINT, "usmallint");
	result.emplace_back(LogicalType::UINTEGER, "uint");
	result.emplace_back(LogicalType::UBIGINT, "ubigint");

	// BIGNUM.
	if (use_large_bignum) {
		string data;
		constexpr idx_t total_data_size = Bignum::BIGNUM_HEADER_SIZE + Bignum::MAX_DATA_SIZE;
		data.resize(total_data_size);

		// Let's set the max header.
		Bignum::SetHeader(&data[0], Bignum::MAX_DATA_SIZE, false);
		// Set all other max bits.
		memset(&data[Bignum::BIGNUM_HEADER_SIZE], 0xFF, Bignum::MAX_DATA_SIZE);
		auto max = Value::BIGNUM(data);

		// Let's set the min header.
		Bignum::SetHeader(&data[0], Bignum::MAX_DATA_SIZE, true);
		// Set all other min bits.
		memset(&data[Bignum::BIGNUM_HEADER_SIZE], 0x00, Bignum::MAX_DATA_SIZE);
		auto min = Value::BIGNUM(data);
		result.emplace_back(LogicalType::BIGNUM, "bignum", min, max);

	} else {
		result.emplace_back(LogicalType::BIGNUM, "bignum");
	}

	// Time-types.
	result.emplace_back(LogicalType::DATE, "date");
	result.emplace_back(LogicalType::TIME, "time");
	result.emplace_back(LogicalType::TIMESTAMP, "timestamp");
	result.emplace_back(LogicalType::TIMESTAMP_S, "timestamp_s");
	result.emplace_back(LogicalType::TIMESTAMP_MS, "timestamp_ms");
	result.emplace_back(LogicalType::TIMESTAMP_NS, "timestamp_ns");
	result.emplace_back(LogicalType::TIME_TZ, "time_tz");
	result.emplace_back(LogicalType::TIMESTAMP_TZ, "timestamp_tz");
	result.emplace_back(LogicalType::TIMESTAMP_TZ_NS, "timestamp_tz_ns");

	// More complex numeric types.
	result.emplace_back(LogicalType::FLOAT, "float", Value::FLOAT(std::numeric_limits<float>::lowest()),
	                    Value::FLOAT(std::numeric_limits<float>::max()));
	result.emplace_back(LogicalType::DOUBLE, "double", Value::DOUBLE(std::numeric_limits<double>::lowest()),
	                    Value::DOUBLE(std::numeric_limits<double>::max()));
	result.emplace_back(LogicalType::DECIMAL(4, 1), "dec_4_1");
	result.emplace_back(LogicalType::DECIMAL(9, 4), "dec_9_4");
	result.emplace_back(LogicalType::DECIMAL(18, 6), "dec_18_6");
	result.emplace_back(LogicalType::DECIMAL(38, 10), "dec38_10");
	result.emplace_back(LogicalType::UUID, "uuid");

	// Interval.
	interval_t min_interval;
	min_interval.months = 0;
	min_interval.days = 0;
	min_interval.micros = 0;

	interval_t max_interval;
	max_interval.months = 999;
	max_interval.days = 999;
	max_interval.micros = 999999999;
	result.emplace_back(LogicalType::INTERVAL, "interval", Value::INTERVAL(min_interval),
	                    Value::INTERVAL(max_interval));

	// VARCHAR / BLOB / Bitstrings.
	result.emplace_back(LogicalType::VARCHAR, "varchar", Value("🦆🦆🦆🦆🦆🦆"),
	                    Value(string("goo\x00se", 6)));
	result.emplace_back(LogicalType::BLOB, "blob", Value::BLOB("thisisalongblob\\x00withnullbytes"),
	                    Value::BLOB("\\x00\\x00\\x00a"));
	result.emplace_back(LogicalType::BIT, "bit", Value::BIT("0010001001011100010101011010111"), Value::BIT("10101"));

	// ENUMs.
	Vector small_enum(LogicalType::VARCHAR, 2);
	auto small_enum_ptr = FlatVector::Writer<string_t>(small_enum, 2);
	small_enum_ptr.WriteValue(string_t("DUCK_DUCK_ENUM"));
	small_enum_ptr.WriteValue(string_t("GOOSE"));
	result.emplace_back(LogicalType::ENUM(small_enum, 2), "small_enum");

	Vector medium_enum(LogicalType::VARCHAR, 300);
	auto medium_enum_ptr = FlatVector::Writer<string_t>(medium_enum, 300);
	for (idx_t i = 0; i < 300; i++) {
		medium_enum_ptr.WriteValue(string_t(string("enum_") + to_string(i)));
	}
	result.emplace_back(LogicalType::ENUM(medium_enum, 300), "medium_enum");

	if (use_large_enum) {
		// this is a big one... not sure if we should push this one here, but it's required for completeness
		Vector large_enum(LogicalType::VARCHAR, 70000);
		auto large_enum_ptr = FlatVector::Writer<string_t>(large_enum, 70000);
		for (idx_t i = 0; i < 70000; i++) {
			large_enum_ptr.WriteValue(string_t(string("enum_") + to_string(i)));
		}
		result.emplace_back(LogicalType::ENUM(large_enum, 70000), "large_enum");
	} else {
		Vector large_enum(LogicalType::VARCHAR, 2);
		auto large_enum_ptr = FlatVector::Writer<string_t>(large_enum, 2);
		large_enum_ptr.WriteValue(string_t(string("enum_") + to_string(0)));
		large_enum_ptr.WriteValue(string_t(string("enum_") + to_string(69999)));
		result.emplace_back(LogicalType::ENUM(large_enum, 2), "large_enum");
	}

	// ARRAYs.
	auto int_list_type = LogicalType::LIST(LogicalType::INTEGER);
	auto empty_int_list = Value::LIST(LogicalType::INTEGER, vector<Value>());
	auto int_list =
	    Value::LIST(LogicalType::INTEGER, {Value::INTEGER(42), Value::INTEGER(999), Value(LogicalType::INTEGER),
	                                       Value(LogicalType::INTEGER), Value::INTEGER(-42)});
	result.emplace_back(int_list_type, "int_array", empty_int_list, int_list);

	auto double_list_type = LogicalType::LIST(LogicalType::DOUBLE);
	auto empty_double_list = Value::LIST(LogicalType::DOUBLE, vector<Value>());
	auto double_list = Value::LIST(LogicalType::DOUBLE, {Value::DOUBLE(42), Value::DOUBLE(NAN),
	                                                     Value::DOUBLE(std::numeric_limits<double>::infinity()),
	                                                     Value::DOUBLE(-std::numeric_limits<double>::infinity()),
	                                                     Value(LogicalType::DOUBLE), Value::DOUBLE(-42)});
	result.emplace_back(double_list_type, "double_array", empty_double_list, double_list);

	auto date_list_type = LogicalType::LIST(LogicalType::DATE);
	auto empty_date_list = Value::LIST(LogicalType::DATE, vector<Value>());
	auto date_list = Value::LIST(LogicalType::DATE, {Value::DATE(date_t()), Value::DATE(date_t::infinity()),
	                                                 Value::DATE(date_t::ninfinity()), Value(LogicalType::DATE),
	                                                 Value::DATE(Date::FromString("2022-05-12"))});
	result.emplace_back(date_list_type, "date_array", empty_date_list, date_list);

	auto timestamp_list_type = LogicalType::LIST(LogicalType::TIMESTAMP);
	auto empty_timestamp_list = Value::LIST(LogicalType::TIMESTAMP, vector<Value>());
	auto timestamp_list =
	    Value::LIST(LogicalType::TIMESTAMP, {Value::TIMESTAMP(timestamp_t()), Value::TIMESTAMP(timestamp_t::infinity()),
	                                         Value::TIMESTAMP(timestamp_t::ninfinity()), Value(LogicalType::TIMESTAMP),
	                                         Value::TIMESTAMP(Timestamp::FromString("2022-05-12 16:23:45", false))});
	result.emplace_back(timestamp_list_type, "timestamp_array", empty_timestamp_list, timestamp_list);

	auto timestamptz_list_type = LogicalType::LIST(LogicalType::TIMESTAMP_TZ);
	auto empty_timestamptz_list = Value::LIST(LogicalType::TIMESTAMP_TZ, vector<Value>());
	auto timestamptz_list =
	    Value::LIST(LogicalType::TIMESTAMP_TZ,
	                {Value::TIMESTAMPTZ(timestamp_tz_t()), Value::TIMESTAMPTZ(timestamp_tz_t::infinity()),
	                 Value::TIMESTAMPTZ(timestamp_tz_t::ninfinity()), Value(LogicalType::TIMESTAMP_TZ),
	                 Value::TIMESTAMPTZ(timestamp_tz_t(Timestamp::FromString("2022-05-12 16:23:45-07", true)))});
	result.emplace_back(timestamptz_list_type, "timestamptz_array", empty_timestamptz_list, timestamptz_list);

	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);
	auto empty_varchar_list = Value::LIST(LogicalType::VARCHAR, vector<Value>());
	auto varchar_list = Value::LIST(LogicalType::VARCHAR, {Value("🦆🦆🦆🦆🦆🦆"), Value("goose"),
	                                                       Value(LogicalType::VARCHAR), Value("")});
	result.emplace_back(varchar_list_type, "varchar_array", empty_varchar_list, varchar_list);

	// nested arrays
	auto nested_list_type = LogicalType::LIST(int_list_type);
	auto empty_nested_list = Value::LIST(int_list_type, vector<Value>());
	auto nested_int_list =
	    Value::LIST(int_list_type, {empty_int_list, int_list, Value(int_list_type), empty_int_list, int_list});
	result.emplace_back(nested_list_type, "nested_int_array", empty_nested_list, nested_int_list);

	// structs
	child_list_t<LogicalType> struct_type_list;
	struct_type_list.emplace_back(make_pair("a", LogicalType::INTEGER));
	struct_type_list.emplace_back(make_pair("b", LogicalType::VARCHAR));
	auto struct_type = LogicalType::STRUCT(struct_type_list);

	child_list_t<Value> min_struct_list;
	min_struct_list.emplace_back(make_pair("a", Value(LogicalType::INTEGER)));
	min_struct_list.emplace_back(make_pair("b", Value(LogicalType::VARCHAR)));
	auto min_struct_val = Value::STRUCT(std::move(min_struct_list));

	child_list_t<Value> max_struct_list;
	max_struct_list.emplace_back(make_pair("a", Value::INTEGER(42)));
	max_struct_list.emplace_back(make_pair("b", Value("🦆🦆🦆🦆🦆🦆")));
	auto max_struct_val = Value::STRUCT(std::move(max_struct_list));

	result.emplace_back(struct_type, "struct", min_struct_val, max_struct_val);

	// Empty struct
	child_list_t<Value> empty_struct_values;
	child_list_t<LogicalType> empty_struct_types;
	auto empty_struct_type = LogicalType::STRUCT(empty_struct_types);
	auto empty_struct_val = Value::STRUCT(empty_struct_values);
	result.emplace_back(empty_struct_type, "empty_struct", empty_struct_val, empty_struct_val);

	// structs with lists
	child_list_t<LogicalType> struct_list_type_list;
	struct_list_type_list.emplace_back(make_pair("a", int_list_type));
	struct_list_type_list.emplace_back(make_pair("b", varchar_list_type));
	auto struct_list_type = LogicalType::STRUCT(struct_list_type_list);

	child_list_t<Value> min_struct_vl_list;
	min_struct_vl_list.emplace_back(make_pair("a", Value(int_list_type)));
	min_struct_vl_list.emplace_back(make_pair("b", Value(varchar_list_type)));
	auto min_struct_val_list = Value::STRUCT(std::move(min_struct_vl_list));

	child_list_t<Value> max_struct_vl_list;
	max_struct_vl_list.emplace_back(make_pair("a", int_list));
	max_struct_vl_list.emplace_back(make_pair("b", varchar_list));
	auto max_struct_val_list = Value::STRUCT(std::move(max_struct_vl_list));

	result.emplace_back(struct_list_type, "struct_of_arrays", std::move(min_struct_val_list),
	                    std::move(max_struct_val_list));

	// array of structs
	auto array_of_structs_type = LogicalType::LIST(struct_type);
	auto min_array_of_struct_val = Value::LIST(struct_type, vector<Value>());
	auto max_array_of_struct_val = Value::LIST(struct_type, {min_struct_val, max_struct_val, Value(struct_type)});
	result.emplace_back(array_of_structs_type, "array_of_structs", std::move(min_array_of_struct_val),
	                    std::move(max_array_of_struct_val));

	// map
	auto map_type = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	auto min_map_value = Value::MAP(ListType::GetChildType(map_type), vector<Value>());

	child_list_t<Value> map_struct1;
	map_struct1.emplace_back(make_pair("key", Value("key1")));
	map_struct1.emplace_back(make_pair("value", Value("🦆🦆🦆🦆🦆🦆")));
	child_list_t<Value> map_struct2;
	map_struct2.emplace_back(make_pair("key", Value("key2")));
	map_struct2.emplace_back(make_pair("value", Value("goose")));

	vector<Value> map_values;
	map_values.push_back(Value::STRUCT(map_struct1));
	map_values.push_back(Value::STRUCT(map_struct2));

	auto max_map_value = Value::MAP(ListType::GetChildType(map_type), map_values);
	result.emplace_back(map_type, "map", std::move(min_map_value), std::move(max_map_value));

	// union
	child_list_t<LogicalType> members = {{"name", LogicalType::VARCHAR}, {"age", LogicalType::SMALLINT}};
	auto union_type = LogicalType::UNION(members);
	const Value &min = Value::UNION(members, 0, Value("Frank"));
	const Value &max = Value::UNION(members, 1, Value::SMALLINT(5));
	result.emplace_back(union_type, "union", min, max);

	// fixed int array
	auto fixed_int_array_type = LogicalType::ARRAY(LogicalType::INTEGER, 3);
	auto fixed_int_min_array_value = Value::ARRAY(LogicalType::INTEGER, {Value(LogicalType::INTEGER), 2, 3});
	auto fixed_int_max_array_value = Value::ARRAY(LogicalType::INTEGER, {4, 5, 6});
	result.emplace_back(fixed_int_array_type, "fixed_int_array", fixed_int_min_array_value, fixed_int_max_array_value);

	// fixed varchar array
	auto fixed_varchar_array_type = LogicalType::ARRAY(LogicalType::VARCHAR, 3);
	auto fixed_varchar_min_array_value =
	    Value::ARRAY(LogicalType::VARCHAR, {Value("a"), Value(LogicalType::VARCHAR), Value("c")});
	auto fixed_varchar_max_array_value = Value::ARRAY(LogicalType::VARCHAR, {Value("d"), Value("e"), Value("f")});
	result.emplace_back(fixed_varchar_array_type, "fixed_varchar_array", fixed_varchar_min_array_value,
	                    fixed_varchar_max_array_value);

	// fixed nested int array
	auto fixed_nested_int_array_type = LogicalType::ARRAY(fixed_int_array_type, 3);
	auto fixed_nested_int_min_array_value = Value::ARRAY(
	    fixed_int_array_type, {fixed_int_min_array_value, Value(fixed_int_array_type), fixed_int_min_array_value});
	auto fixed_nested_int_max_array_value = Value::ARRAY(
	    fixed_int_array_type, {fixed_int_max_array_value, fixed_int_min_array_value, fixed_int_max_array_value});
	result.emplace_back(fixed_nested_int_array_type, "fixed_nested_int_array", fixed_nested_int_min_array_value,
	                    fixed_nested_int_max_array_value);

	// fixed nested varchar array
	auto fixed_nested_varchar_array_type = LogicalType::ARRAY(fixed_varchar_array_type, 3);
	auto fixed_nested_varchar_min_array_value =
	    Value::ARRAY(fixed_varchar_array_type,
	                 {fixed_varchar_min_array_value, Value(fixed_varchar_array_type), fixed_varchar_min_array_value});
	auto fixed_nested_varchar_max_array_value =
	    Value::ARRAY(fixed_varchar_array_type,
	                 {fixed_varchar_max_array_value, fixed_varchar_min_array_value, fixed_varchar_max_array_value});
	result.emplace_back(fixed_nested_varchar_array_type, "fixed_nested_varchar_array",
	                    fixed_nested_varchar_min_array_value, fixed_nested_varchar_max_array_value);

	// fixed array of structs
	auto fixed_struct_array_type = LogicalType::ARRAY(struct_type, 3);
	auto fixed_struct_min_array_value = Value::ARRAY(struct_type, {min_struct_val, max_struct_val, min_struct_val});
	auto fixed_struct_max_array_value = Value::ARRAY(struct_type, {max_struct_val, min_struct_val, max_struct_val});
	result.emplace_back(fixed_struct_array_type, "fixed_struct_array", fixed_struct_min_array_value,
	                    fixed_struct_max_array_value);

	// struct of fixed array
	auto struct_of_fixed_array_type =
	    LogicalType::STRUCT({{"a", fixed_int_array_type}, {"b", fixed_varchar_array_type}});
	auto struct_of_fixed_array_min_value =
	    Value::STRUCT({{"a", fixed_int_min_array_value}, {"b", fixed_varchar_min_array_value}});
	auto struct_of_fixed_array_max_value =
	    Value::STRUCT({{"a", fixed_int_max_array_value}, {"b", fixed_varchar_max_array_value}});
	result.emplace_back(struct_of_fixed_array_type, "struct_of_fixed_array", struct_of_fixed_array_min_value,
	                    struct_of_fixed_array_max_value);

	// fixed array of list of int
	auto fixed_array_of_list_of_int_type = LogicalType::ARRAY(int_list_type, 3);
	auto fixed_array_of_list_of_int_min_value = Value::ARRAY(int_list_type, {empty_int_list, int_list, empty_int_list});
	auto fixed_array_of_list_of_int_max_value = Value::ARRAY(int_list_type, {int_list, empty_int_list, int_list});
	result.emplace_back(fixed_array_of_list_of_int_type, "fixed_array_of_int_list",
	                    fixed_array_of_list_of_int_min_value, fixed_array_of_list_of_int_max_value);

	// list of fixed array of int
	auto list_of_fixed_array_of_int_type = LogicalType::LIST(fixed_int_array_type);
	auto list_of_fixed_array_of_int_min_value = Value::LIST(
	    fixed_int_array_type, {fixed_int_min_array_value, fixed_int_max_array_value, fixed_int_min_array_value});
	auto list_of_fixed_array_of_int_max_value = Value::LIST(
	    fixed_int_array_type, {fixed_int_max_array_value, fixed_int_min_array_value, fixed_int_max_array_value});
	result.emplace_back(list_of_fixed_array_of_int_type, "list_of_fixed_int_array",
	                    list_of_fixed_array_of_int_min_value, list_of_fixed_array_of_int_max_value);

	result.emplace_back(LogicalType::TIME_NS, "time_ns");

	// GEOMETRY
	// - For min, use a regular empty point
	// - For max, use some complicated nested geometry collection with a variety of empty and non-empty geometries,
	// to cover as many code paths as possible

	constexpr auto big_geom_wkt = R"WKT_LITERAL(
		GEOMETRYCOLLECTION (
			POINT (1 2),
			POINT EMPTY,
			LINESTRING (0 0, 1 1),
			LINESTRING EMPTY,
			POLYGON ((0 0, 0 1, 1 1, 1 0, 0 0)),
			POLYGON EMPTY,
	        MULTIPOINT (
				5 6,
				EMPTY
			),
			MULTILINESTRING (
				(0 0, 1 1),
				EMPTY,
				(2 2, 3 3),
				EMPTY
			),
			MULTILINESTRING EMPTY,
			MULTIPOLYGON (
				((0 0, 0 1, 1 1, 1 0, 0 0)),
				EMPTY,
				((0 0, 0 2, 2 2, 2 0, 0 0)),
				EMPTY
			),
			MULTIPOLYGON EMPTY,
			GEOMETRYCOLLECTION (
				POINT (5 6)
			),
			GEOMETRYCOLLECTION EMPTY
		)
	)WKT_LITERAL";

	auto min_geometry = Value("POINT EMPTY").DefaultCastAs(LogicalType::GEOMETRY());
	auto max_geometry = Value(big_geom_wkt).DefaultCastAs(LogicalType::GEOMETRY());

	result.emplace_back(LogicalType::GEOMETRY(), "geometry", min_geometry, max_geometry);

	// unnamed tuple - added last so existing column positions are unchanged
	auto tuple_type = LogicalType::TUPLE({LogicalType::INTEGER, LogicalType::VARCHAR});
	auto min_tuple_val = Value::TUPLE({Value(LogicalType::INTEGER), Value(LogicalType::VARCHAR)});
	auto max_tuple_val = Value::TUPLE({Value::INTEGER(42), Value("🦆🦆🦆🦆🦆🦆")});
	result.emplace_back(tuple_type, "tuple", min_tuple_val, max_tuple_val);

	return result;
}

struct TestAllTypesBindData : public TableFunctionData {
	vector<TestType> test_types;
};

static unique_ptr<FunctionData> TestAllTypesBind(ClientContext &context, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<TestAllTypesBindData>();
	bool use_large_enum = false;
	bool use_large_bignum = false;
	auto entry = input.named_parameters.find("use_large_enum");
	if (entry != input.named_parameters.end()) {
		if (entry->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for use_large_enum");
		}
		use_large_enum = BooleanValue::Get(entry->second);
	}
	entry = input.named_parameters.find("use_large_bignum");
	if (entry != input.named_parameters.end()) {
		if (entry->second.IsNull()) {
			throw InvalidInputException("Cannot use NULL as argument for use_large_bignum");
		}
		use_large_bignum = BooleanValue::Get(entry->second);
	}
	result->test_types = TestAllTypesFun::GetTestTypes(use_large_enum, use_large_bignum);
	for (auto &test_type : result->test_types) {
		return_types.push_back(test_type.type);
		names.emplace_back(test_type.name);
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> TestAllTypesInit(ClientContext &context, TableFunctionInitInput &input) {
	auto &bind_data = input.bind_data->Cast<TestAllTypesBindData>();
	auto result = make_uniq<TestAllTypesData>();
	// 3 rows: min, max and NULL
	result->entries.resize(3);
	// initialize the values
	for (auto &test_type : bind_data.test_types) {
		result->entries[0].push_back(test_type.min_value);
		result->entries[1].push_back(test_type.max_value);
		result->entries[2].emplace_back(test_type.type);
	}
	return std::move(result);
}

static void TestAllTypesFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &data = data_p.global_state->Cast<TestAllTypesData>();
	if (data.offset >= data.entries.size()) {
		// finished returning values
		return;
	}
	// start returning values
	// either fill up the chunk or return all the remaining columns
	idx_t count = 0;
	while (data.offset < data.entries.size() && count < STANDARD_VECTOR_SIZE) {
		auto &vals = data.entries[data.offset++];
		for (idx_t col_idx = 0; col_idx < vals.size(); col_idx++) {
			output.data[col_idx].Append(vals[col_idx]);
		}
		count++;
	}
}

void TestAllTypesFun::RegisterFunction(BuiltinFunctions &set) {
	TableFunction test_all_types("test_all_types", {}, TestAllTypesFunction, TestAllTypesBind, TestAllTypesInit);
	test_all_types.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("use_large_enum", LogicalType::BOOLEAN).Add("use_large_bignum", LogicalType::BOOLEAN);
	});
	set.AddFunction(test_all_types);
}

} // namespace duckdb
