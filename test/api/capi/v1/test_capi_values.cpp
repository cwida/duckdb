#include "capi_tester.hpp"

using namespace duckdb;

TEST_CASE("Test MAP getters", "[capi]") {
	auto uint_val = duckdb_create_uint64(42);
	REQUIRE(uint_val);

	auto size = duckdb_get_map_size(nullptr);
	REQUIRE(size == 0);
	size = duckdb_get_map_size(uint_val);
	REQUIRE(size == 0);

	auto key = duckdb_get_map_key(nullptr, 0);
	REQUIRE(!key);
	key = duckdb_get_map_key(uint_val, 0);
	REQUIRE(!key);

	auto value = duckdb_get_map_value(nullptr, 0);
	REQUIRE(!value);
	value = duckdb_get_map_value(uint_val, 0);
	REQUIRE(!value);

	duckdb_destroy_value(&uint_val);
}

TEST_CASE("Test LIST getters", "[capi]") {
	duckdb_value list_vals[2];
	list_vals[0] = duckdb_create_uint64(42);
	list_vals[1] = duckdb_create_uint64(43);
	duckdb_logical_type uint64_type = duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT);
	duckdb_value list_value = duckdb_create_list_value(uint64_type, list_vals, 2);
	duckdb_destroy_value(&list_vals[0]);
	duckdb_destroy_value(&list_vals[1]);
	duckdb_destroy_logical_type(&uint64_type);

	auto size = duckdb_get_list_size(nullptr);
	REQUIRE(size == 0);

	size = duckdb_get_list_size(list_value);
	REQUIRE(size == 2);

	auto val = duckdb_get_list_child(nullptr, 0);
	REQUIRE(!val);
	duckdb_destroy_value(&val);

	val = duckdb_get_list_child(list_value, 0);
	REQUIRE(val);
	REQUIRE(duckdb_get_uint64(val) == 42);
	duckdb_destroy_value(&val);

	val = duckdb_get_list_child(list_value, 1);
	REQUIRE(val);
	REQUIRE(duckdb_get_uint64(val) == 43);
	duckdb_destroy_value(&val);

	val = duckdb_get_list_child(list_value, 2);
	REQUIRE(!val);
	duckdb_destroy_value(&val);

	duckdb_destroy_value(&list_value);
}

TEST_CASE("Test ENUM getters", "[capi]") {
	const char *mnames[5] = {"apple", "banana", "cherry", "orange", "elderberry"};
	duckdb_logical_type enum_type = duckdb_create_enum_type(mnames, 5);

	duckdb_value enum_val = duckdb_create_enum_value(enum_type, 2);
	REQUIRE(enum_val);

	auto val = duckdb_get_enum_value(nullptr);
	REQUIRE(val == 0);

	val = duckdb_get_enum_value(enum_val);
	REQUIRE(val == 2);

	duckdb_destroy_value(&enum_val);

	enum_val = duckdb_create_enum_value(enum_type, 4);
	REQUIRE(enum_val);

	val = duckdb_get_enum_value(enum_val);
	REQUIRE(val == 4);

	duckdb_destroy_value(&enum_val);

	enum_val = duckdb_create_enum_value(enum_type, 5);
	REQUIRE(!enum_val);

	enum_val = duckdb_create_enum_value(enum_type, 6);
	REQUIRE(!enum_val);

	duckdb_destroy_value(&enum_val);

	duckdb_destroy_logical_type(&enum_type);
}

TEST_CASE("Test STRUCT getters", "[capi]") {
	duckdb_logical_type mtypes[2] = {duckdb_create_logical_type(DUCKDB_TYPE_UBIGINT),
	                                 duckdb_create_logical_type(DUCKDB_TYPE_BIGINT)};
	const char *mnames[2] = {"a", "b"};
	duckdb_logical_type struct_type = duckdb_create_struct_type(mtypes, mnames, 2);
	duckdb_destroy_logical_type(&mtypes[0]);
	duckdb_destroy_logical_type(&mtypes[1]);

	duckdb_value svals[2] = {duckdb_create_uint64(42), duckdb_create_int64(-42)};
	duckdb_value struct_val = duckdb_create_struct_value(struct_type, svals);
	duckdb_destroy_logical_type(&struct_type);
	duckdb_destroy_value(&svals[0]);
	duckdb_destroy_value(&svals[1]);

	auto val = duckdb_get_struct_child(nullptr, 0);
	REQUIRE(!val);

	val = duckdb_get_struct_child(struct_val, 0);
	REQUIRE(val);
	REQUIRE(duckdb_get_uint64(val) == 42);
	duckdb_destroy_value(&val);

	val = duckdb_get_struct_child(struct_val, 1);
	REQUIRE(val);
	REQUIRE(duckdb_get_int64(val) == -42);
	duckdb_destroy_value(&val);

	val = duckdb_get_struct_child(struct_val, 2);
	REQUIRE(!val);

	duckdb_destroy_value(&struct_val);
}

TEST_CASE("Test NULL value", "[capi]") {
	auto null_value = duckdb_create_null_value();
	REQUIRE(null_value);

	REQUIRE(!duckdb_is_null_value(nullptr));
	auto uint_val = duckdb_create_uint64(42);
	REQUIRE(!duckdb_is_null_value(uint_val));
	REQUIRE(duckdb_is_null_value(null_value));

	duckdb_destroy_value(&uint_val);
	duckdb_destroy_value(&null_value);
}

TEST_CASE("Test BIGNUM value", "[capi]") {
	{
		uint8_t data[] {0};
		duckdb_bignum input {data, 1, false};
		auto value = duckdb_create_bignum(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIGNUM);
		auto output = duckdb_get_bignum(value);
		REQUIRE(output.is_negative == input.is_negative);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
	{
		uint8_t data[] {1};
		duckdb_bignum input {data, 1, true};
		auto value = duckdb_create_bignum(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIGNUM);
		auto output = duckdb_get_bignum(value);
		REQUIRE(output.is_negative == input.is_negative);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
	{ // 256 == big endian {0x01, 0x00}: the most-significant byte comes first
		uint8_t data[] {0x01, 0x00};
		duckdb_bignum input {data, 2, false};
		auto value = duckdb_create_bignum(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIGNUM);
		auto str = duckdb_get_varchar(value);
		REQUIRE(string(str) == "256");
		duckdb_free(str);
		auto output = duckdb_get_bignum(value);
		REQUIRE(output.is_negative == input.is_negative);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
	{ // max bignum == max double == 2^1023 * (1 + (1 − 2^−52)) == 2^1024 - 2^971 ==
	  // 179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
		uint8_t data[] {
		    // big endian
		    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		};
		duckdb_bignum input {data, 128, false};
		auto value = duckdb_create_bignum(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIGNUM);
		auto str = duckdb_get_varchar(value);
		REQUIRE(
		    string(str) ==
		    "1797693134862315708145274237317043567980705675258449965989174768031572607800285387605895586327668781715"
		    "4045895351438246423432132688946418276846754670353751698604991057655128207624549009038932894407586850845"
		    "5133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368");
		duckdb_free(str);
		auto output = duckdb_get_bignum(value);
		REQUIRE(output.is_negative == input.is_negative);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
	{ // min bignum == min double == -(2^1023 * (1 + (1 − 2^−52))) == -(2^1024 - 2^971) ==
		// -179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540458953514382464234321326889464182768467546703537516986049910576551282076245490090389328944075868508455133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368
		uint8_t data[] {
		    // big endian (absolute value)
		    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xf8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		};
		duckdb_bignum input {data, 128, true};
		auto value = duckdb_create_bignum(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIGNUM);
		auto str = duckdb_get_varchar(value);
		REQUIRE(
		    string(str) ==
		    "-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171"
		    "5404589535143824642343213268894641827684675467035375169860499105765512820762454900903893289440758685084"
		    "55133942304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368");
		duckdb_free(str);
		auto output = duckdb_get_bignum(value);
		REQUIRE(output.is_negative == input.is_negative);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
}

TEST_CASE("Test DECIMAL value", "[capi]") {
	{
		auto hugeint = Hugeint::POWERS_OF_TEN[4] - hugeint_t(1);
		duckdb_decimal input {4, 1, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_SMALLINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = -(Hugeint::POWERS_OF_TEN[4] - hugeint_t(1));
		duckdb_decimal input {4, 1, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_SMALLINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = Hugeint::POWERS_OF_TEN[9] - hugeint_t(1);
		duckdb_decimal input {9, 4, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_INTEGER);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = -(Hugeint::POWERS_OF_TEN[9] - hugeint_t(1));
		duckdb_decimal input {9, 4, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_INTEGER);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = Hugeint::POWERS_OF_TEN[18] - hugeint_t(1);
		duckdb_decimal input {18, 6, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_BIGINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = -(Hugeint::POWERS_OF_TEN[18] - hugeint_t(1));
		duckdb_decimal input {18, 8, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_BIGINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = Hugeint::POWERS_OF_TEN[38] - hugeint_t(1);
		duckdb_decimal input {38, 10, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_HUGEINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		auto hugeint = -(Hugeint::POWERS_OF_TEN[38] - hugeint_t(1));
		duckdb_decimal input {38, 10, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_HUGEINT);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		duckdb_destroy_value(&value);
	}
	{
		// decimals wider than 38 digits store a BIGNUM blob; duckdb_decimal still carries values that fit in 128 bits
		auto hugeint = -(Hugeint::POWERS_OF_TEN[38] - hugeint_t(1));
		duckdb_decimal input {60, 10, {hugeint.lower, hugeint.upper}};
		auto value = duckdb_create_decimal(input);
		auto type = duckdb_get_value_type(value);
		REQUIRE(duckdb_get_type_id(type) == DUCKDB_TYPE_DECIMAL);
		REQUIRE(duckdb_decimal_width(type) == input.width);
		REQUIRE(duckdb_decimal_scale(type) == input.scale);
		REQUIRE(duckdb_decimal_internal_type(type) == DUCKDB_TYPE_BIGNUM);
		auto output = duckdb_get_decimal(value);
		REQUIRE(output.width == input.width);
		REQUIRE(output.scale == input.scale);
		REQUIRE(output.value.lower == input.value.lower);
		REQUIRE(output.value.upper == input.value.upper);
		auto text = duckdb_get_varchar(value);
		REQUIRE(string(text) == "-9999999999999999999999999999.9999999999");
		duckdb_free(text);
		duckdb_destroy_value(&value);
	}
	{
		// a wide decimal beyond 128 bits cannot be reported as a duckdb_decimal and yields the zero default, the
		// exact value is available through the string fetch
		CAPITester tester;
		REQUIRE(tester.OpenDatabase(nullptr));
		auto result = tester.Query("SELECT '12345678901234567890123456789012345678901234567890.5'::DECIMAL(60,1) AS "
		                           "huge, '-12345.5'::DECIMAL(60,1) AS small");
		REQUIRE_NO_FAIL(*result);
		auto huge = result->Fetch<duckdb_decimal>(0, 0);
		REQUIRE(huge.value.lower == 0);
		REQUIRE(huge.value.upper == 0);
		REQUIRE(result->Fetch<string>(0, 0) == "12345678901234567890123456789012345678901234567890.5");
		auto small = result->Fetch<duckdb_decimal>(1, 0);
		REQUIRE(small.width == 60);
		REQUIRE(small.scale == 1);
		REQUIRE(small.value.lower == static_cast<uint64_t>(-123455));
		REQUIRE(small.value.upper == -1);
		REQUIRE(result->Fetch<string>(1, 0) == "-12345.5");
		REQUIRE(result->Fetch<int64_t>(1, 0) == -12346);
		REQUIRE(result->Fetch<double>(1, 0) == -12345.5);
	}
	{
		// invalid width/scale combinations return nullptr instead of throwing or returning a malformed value
		duckdb::vector<std::pair<uint8_t, uint8_t>> invalid = {
		    {0, 0},   // width below the minimum
		    {4, 5},   // scale greater than width
		    {60, 61}, // scale greater than width of a wide decimal
		};
		for (auto &entry : invalid) {
			duckdb_decimal input {entry.first, entry.second, {0, 0}};
			REQUIRE(duckdb_create_decimal(input) == nullptr);
		}
	}
}

TEST_CASE("Test BIT value", "[capi]") {
	{
		uint8_t data[] {5, 0xf9, 0x56}; // 0b11111001 0b01010110
		duckdb_bit input {data, 3};
		auto value = duckdb_create_bit(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIT);
		auto output = duckdb_get_bit(value);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
	{
		uint8_t data[] {0, 0x00};
		duckdb_bit input {data, 2};
		auto value = duckdb_create_bit(input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(value)) == DUCKDB_TYPE_BIT);
		auto output = duckdb_get_bit(value);
		REQUIRE(output.size == input.size);
		REQUIRE_FALSE(memcmp(output.data, input.data, input.size));
		duckdb_free(output.data);
		duckdb_destroy_value(&value);
	}
}

TEST_CASE("Test UUID value", "[capi]") {
	{
		duckdb_uhugeint uhugeint_input {0x0000000000000000, 0x0000000000000000};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
	{
		duckdb_uhugeint uhugeint_input {0x0000000000000001, 0x0000000000000000};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
	{
		duckdb_uhugeint uhugeint_input {0xffffffffffffffff, 0xffffffffffffffff};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
	{
		duckdb_uhugeint uhugeint_input {0xfffffffffffffffe, 0xffffffffffffffff};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
	{
		duckdb_uhugeint uhugeint_input {0xffffffffffffffff, 0x8fffffffffffffff};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
	{
		duckdb_uhugeint uhugeint_input {0x0000000000000000, 0x7000000000000000};
		auto uuid_value = duckdb_create_uuid(uhugeint_input);
		REQUIRE(duckdb_get_type_id(duckdb_get_value_type(uuid_value)) == DUCKDB_TYPE_UUID);
		auto uhugeint_output = duckdb_get_uuid(uuid_value);
		REQUIRE(uhugeint_output.lower == uhugeint_input.lower);
		REQUIRE(uhugeint_output.upper == uhugeint_input.upper);
		duckdb_destroy_value(&uuid_value);
	}
}

TEST_CASE("Test UTF-8 string creation with and without embedded nulls", "[capi]") {
	// Create a valid and an invalid null-terminated string without embedded null-bytes.
	string valid_utf8 = "é";
	idx_t len = strlen(valid_utf8.c_str());
	auto invalid_utf8 = static_cast<char *>(malloc(len));
	memcpy(invalid_utf8, valid_utf8.c_str() + 1, len);

	duckdb_error_data error_data = nullptr;
	duckdb_value value = nullptr;

	// Valid null-terminated string.
	value = duckdb_create_varchar(valid_utf8.c_str());
	REQUIRE(value);
	auto res = duckdb_get_varchar(value);
	REQUIRE(StringUtil::Equals(res, valid_utf8));
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Invalid null-terminated string via duckdb_create_varchar returns nullptr.
	invalid_utf8[len - 1] = '\0';
	value = duckdb_create_varchar(invalid_utf8);
	REQUIRE(!value);

	// Invalid string via duckdb_create_varchar_length also returns nullptr.
	value = duckdb_create_varchar_length(invalid_utf8, len - 1);
	free(invalid_utf8);
	REQUIRE(!value);

	// Validate with duckdb_valid_utf8_check for error details.
	auto invalid_utf8_2 = valid_utf8.c_str() + 1;
	error_data = duckdb_valid_utf8_check(invalid_utf8_2, len - 1);
	REQUIRE(error_data != nullptr);
	REQUIRE(duckdb_error_data_has_error(error_data));
	auto err_type = duckdb_error_data_error_type(error_data);
	REQUIRE(err_type == DUCKDB_ERROR_INVALID_INPUT);
	auto err_msg = duckdb_error_data_message(error_data);
	REQUIRE(StringUtil::Contains(err_msg, "invalid Unicode detected, str must be valid UTF-8"));
	duckdb_destroy_error_data(&error_data);

	// Create a valid and an invalid string with embedded null-bytes.
	constexpr idx_t VALID_NULL_UTF8_LEN = 6;
	string valid_null_utf8("é\0b\0c\0", VALID_NULL_UTF8_LEN);
	auto invalid_null_utf8 = static_cast<char *>(malloc(VALID_NULL_UTF8_LEN));
	memcpy(invalid_null_utf8, valid_null_utf8.c_str() + 1, VALID_NULL_UTF8_LEN);

	// Valid string with embedded nulls.
	value = duckdb_create_varchar_length(valid_null_utf8.c_str(), VALID_NULL_UTF8_LEN);
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(StringUtil::Equals(res, valid_null_utf8));
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Invalid string with embedded nulls returns nullptr.
	value = duckdb_create_varchar_length(invalid_null_utf8, VALID_NULL_UTF8_LEN - 1);
	free(invalid_null_utf8);
	REQUIRE(!value);
}

TEST_CASE("Test duckdb_create_varchar edge cases", "[capi]") {
	duckdb_value value = nullptr;

	// Empty string is valid.
	value = duckdb_create_varchar("");
	REQUIRE(value);
	auto res = duckdb_get_varchar(value);
	REQUIRE(strlen(res) == 0);
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Empty string via _length is valid.
	value = duckdb_create_varchar_length("", 0);
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(strlen(res) == 0);
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Valid ASCII.
	value = duckdb_create_varchar("hello");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "hello");
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Valid multibyte (3-byte: Euro sign).
	value = duckdb_create_varchar("\xe2\x82\xac");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "\xe2\x82\xac");
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Valid 4-byte (emoji).
	value = duckdb_create_varchar("\xf0\x9f\x98\x80");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "\xf0\x9f\x98\x80");
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Valid 2-byte 'é' (0xC3 0xA9), then lone continuation byte (0xA9 without lead byte) is invalid.
	value = duckdb_create_varchar("\xc3\xa9");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "\xc3\xa9");
	duckdb_free(res);
	duckdb_destroy_value(&value);

	char lone_cont[] = {(char)0xA9, '\0'};
	value = duckdb_create_varchar(lone_cont);
	REQUIRE(!value);

	// Valid '/' (0x2F), then overlong encoding of '/' (0xC0 0xAF) is invalid.
	value = duckdb_create_varchar("/");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "/");
	duckdb_free(res);
	duckdb_destroy_value(&value);

	char overlong[] = {(char)0xC0, (char)0xAF, '\0'};
	value = duckdb_create_varchar(overlong);
	REQUIRE(!value);

	// Valid U+D7FF (0xED 0x9F 0xBF, last before surrogates), then surrogate half U+D800 is invalid.
	value = duckdb_create_varchar("\xed\x9f\xbf");
	REQUIRE(value);
	duckdb_destroy_value(&value);

	char surrogate[] = {(char)0xED, (char)0xA0, (char)0x80, '\0'};
	value = duckdb_create_varchar(surrogate);
	REQUIRE(!value);

	// Valid 'A' (0x41), then 0xFF byte is invalid (never valid in UTF-8).
	value = duckdb_create_varchar("A");
	REQUIRE(value);
	duckdb_destroy_value(&value);

	char ff_byte[] = {(char)0xFF, '\0'};
	value = duckdb_create_varchar(ff_byte);
	REQUIRE(!value);
}

TEST_CASE("Test duckdb_create_varchar null truncation vs length version", "[capi]") {
	// duckdb_create_varchar uses strlen, so it truncates at the first embedded null byte.
	// duckdb_create_varchar_length preserves the full content including embedded nulls.
	duckdb_value value = nullptr;

	// "hello\0world" — non-len version sees only "hello".
	value = duckdb_create_varchar("hello\0world");
	REQUIRE(value);
	auto res = duckdb_get_varchar(value);
	REQUIRE(string(res) == "hello");
	REQUIRE(strlen(res) == 5);
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Same bytes via _length version preserves the full 11 bytes.
	string full("hello\0world", 11);
	value = duckdb_create_varchar_length(full.c_str(), full.length());
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res, 11) == full);
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Leading null byte — non-len version sees empty string.
	value = duckdb_create_varchar("\0trailing");
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(strlen(res) == 0);
	duckdb_free(res);
	duckdb_destroy_value(&value);

	// Same bytes via _length version preserves all 9 bytes.
	string leading_null("\0trailing", 9);
	value = duckdb_create_varchar_length(leading_null.c_str(), leading_null.length());
	REQUIRE(value);
	res = duckdb_get_varchar(value);
	REQUIRE(string(res, 9) == leading_null);
	duckdb_free(res);
	duckdb_destroy_value(&value);
}

TEST_CASE("Test SQL string conversion", "[capi]") {
	auto uint_val = duckdb_create_uint64(42);
	auto uint_val_str = duckdb_value_to_string(uint_val);
	REQUIRE(string(uint_val_str).compare("42") == 0);

	duckdb_destroy_value(&uint_val);
	duckdb_free(uint_val_str);
}
