#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/main/capi/capi_internal.hpp"

using duckdb::LogicalTypeId;

static duckdb_value WrapValue(duckdb::Value *value) {
	return reinterpret_cast<duckdb_value>(value);
}

static duckdb::LogicalType &UnwrapType(duckdb_logical_type type) {
	return *(reinterpret_cast<duckdb::LogicalType *>(type));
}

static duckdb::Value &UnwrapValue(duckdb_value value) {
	return *(reinterpret_cast<duckdb::Value *>(value));
}
void duckdb_destroy_value(duckdb_value *value) {
	if (value && *value) {
		auto &unwrap_value = UnwrapValue(*value);
		delete &unwrap_value;
		*value = nullptr;
	}
}

duckdb_value duckdb_create_varchar_length(const char *text, idx_t length) {
	try {
		return WrapValue(new duckdb::Value(std::string(text, length)));
	} catch (...) {
		return nullptr;
	}
}

duckdb_value duckdb_create_varchar(const char *text) {
	return duckdb_create_varchar_length(text, strlen(text));
}

template <class T>
static duckdb_value CAPICreateValue(T input) {
	return WrapValue(new duckdb::Value(duckdb::Value::CreateValue<T>(input)));
}

template <class T, LogicalTypeId TYPE_ID>
static T CAPIGetValue(duckdb_value val) {
	auto &v = UnwrapValue(val);
	auto cast = v.DefaultTryCastAs(TYPE_ID);
	if (!cast) {
		return duckdb::NullValue<T>();
	}
	return cast->template GetValue<T>();
}

duckdb_value duckdb_create_bool(bool input) {
	return CAPICreateValue(input);
}
bool duckdb_get_bool(duckdb_value val) {
	return CAPIGetValue<bool, LogicalTypeId::BOOLEAN>(val);
}
duckdb_value duckdb_create_int8(int8_t input) {
	return CAPICreateValue(input);
}
int8_t duckdb_get_int8(duckdb_value val) {
	return CAPIGetValue<int8_t, LogicalTypeId::TINYINT>(val);
}
duckdb_value duckdb_create_uint8(uint8_t input) {
	return CAPICreateValue(input);
}
uint8_t duckdb_get_uint8(duckdb_value val) {
	return CAPIGetValue<uint8_t, LogicalTypeId::UTINYINT>(val);
}
duckdb_value duckdb_create_int16(int16_t input) {
	return CAPICreateValue(input);
}
int16_t duckdb_get_int16(duckdb_value val) {
	return CAPIGetValue<int16_t, LogicalTypeId::SMALLINT>(val);
}
duckdb_value duckdb_create_uint16(uint16_t input) {
	return CAPICreateValue(input);
}
uint16_t duckdb_get_uint16(duckdb_value val) {
	return CAPIGetValue<uint16_t, LogicalTypeId::USMALLINT>(val);
}
duckdb_value duckdb_create_int32(int32_t input) {
	return CAPICreateValue(input);
}
int32_t duckdb_get_int32(duckdb_value val) {
	return CAPIGetValue<int32_t, LogicalTypeId::INTEGER>(val);
}
duckdb_value duckdb_create_uint32(uint32_t input) {
	return CAPICreateValue(input);
}
uint32_t duckdb_get_uint32(duckdb_value val) {
	return CAPIGetValue<uint32_t, LogicalTypeId::UINTEGER>(val);
}
duckdb_value duckdb_create_uint64(uint64_t input) {
	return CAPICreateValue(input);
}
uint64_t duckdb_get_uint64(duckdb_value val) {
	return CAPIGetValue<uint64_t, LogicalTypeId::UBIGINT>(val);
}
duckdb_value duckdb_create_int64(int64_t input) {
	return CAPICreateValue(input);
}
int64_t duckdb_get_int64(duckdb_value val) {
	return CAPIGetValue<int64_t, LogicalTypeId::BIGINT>(val);
}
duckdb_value duckdb_create_hugeint(duckdb_hugeint input) {
	return WrapValue(new duckdb::Value(duckdb::Value::HUGEINT(duckdb::hugeint_t(input.upper, input.lower))));
}
duckdb_hugeint duckdb_get_hugeint(duckdb_value val) {
	auto res = CAPIGetValue<duckdb::hugeint_t, LogicalTypeId::HUGEINT>(val);
	return {res.lower, res.upper};
}
duckdb_value duckdb_create_uhugeint(duckdb_uhugeint input) {
	return WrapValue(new duckdb::Value(duckdb::Value::UHUGEINT(duckdb::uhugeint_t(input.upper, input.lower))));
}
duckdb_uhugeint duckdb_get_uhugeint(duckdb_value val) {
	auto res = CAPIGetValue<duckdb::uhugeint_t, LogicalTypeId::UHUGEINT>(val);
	return {res.lower, res.upper};
}
duckdb_value duckdb_create_bignum(duckdb_bignum input) {
	return WrapValue(new duckdb::Value(
	    duckdb::Value::BIGNUM(duckdb::Bignum::FromByteArray(input.data, input.size, input.is_negative))));
}
duckdb_bignum duckdb_get_bignum(duckdb_value val) {
	auto v = UnwrapValue(val).DefaultCastAs(duckdb::LogicalType::BIGNUM);
	auto &str = duckdb::StringValue::Get(v);
	duckdb::vector<uint8_t> byte_array;
	bool is_negative;
	duckdb::Bignum::GetByteArray(byte_array, is_negative, duckdb::string_t(str));
	auto size = byte_array.size();
	auto data = reinterpret_cast<uint8_t *>(malloc(size));
	if (!data) {
		return {nullptr, 0, is_negative};
	}
	memcpy(data, byte_array.data(), size);
	return {data, size, is_negative};
}
duckdb_value duckdb_create_decimal(duckdb_decimal input) {
	if (!duckdb::Decimal::IsValidWidthScale(input.width, input.scale)) {
		return nullptr;
	}
	duckdb::hugeint_t hugeint(input.value.upper, input.value.lower);
	int64_t int64;
	if (duckdb::Hugeint::TryCast<int64_t>(hugeint, int64)) {
		// The int64 DECIMAL value constructor will select the appropriate physical type based on width.
		return WrapValue(new duckdb::Value(duckdb::Value::DECIMAL(int64, input.width, input.scale)));
	} else {
		// The hugeint DECIMAL value constructor always uses a physical hugeint, and requires width >= MAX_WIDTH_INT64.
		return WrapValue(new duckdb::Value(duckdb::Value::DECIMAL(hugeint, input.width, input.scale)));
	}
}
duckdb_decimal duckdb_get_decimal(duckdb_value val) {
	auto &v = UnwrapValue(val);
	auto &type = v.type();
	if (type.id() != LogicalTypeId::DECIMAL) {
		return {0, 0, {0, 0}};
	}
	auto width = duckdb::DecimalType::GetWidth(type);
	auto scale = duckdb::DecimalType::GetScale(type);
	duckdb::hugeint_t hugeint = duckdb::IntegralValue::Get(v);
	return {duckdb::NumericCast<uint8_t>(width), duckdb::NumericCast<uint8_t>(scale), {hugeint.lower, hugeint.upper}};
}
duckdb_value duckdb_create_float(float input) {
	return CAPICreateValue(input);
}
float duckdb_get_float(duckdb_value val) {
	return CAPIGetValue<float, LogicalTypeId::FLOAT>(val);
}
duckdb_value duckdb_create_double(double input) {
	return CAPICreateValue(input);
}
double duckdb_get_double(duckdb_value val) {
	return CAPIGetValue<double, LogicalTypeId::DOUBLE>(val);
}
duckdb_value duckdb_create_date(duckdb_date input) {
	return CAPICreateValue(duckdb::date_t(input.days));
}
duckdb_date duckdb_get_date(duckdb_value val) {
	return {CAPIGetValue<duckdb::date_t, LogicalTypeId::DATE>(val).days};
}
duckdb_value duckdb_create_time(duckdb_time input) {
	return CAPICreateValue(duckdb::dtime_t(input.micros));
}
duckdb_time duckdb_get_time(duckdb_value val) {
	return {CAPIGetValue<duckdb::dtime_t, LogicalTypeId::TIME>(val).value};
}
duckdb_value duckdb_create_time_tz_value(duckdb_time_tz input) {
	return CAPICreateValue(duckdb::dtime_tz_t(input.bits));
}
duckdb_time_tz duckdb_get_time_tz(duckdb_value val) {
	return {CAPIGetValue<duckdb::dtime_tz_t, LogicalTypeId::TIME_TZ>(val).bits};
}
duckdb_value duckdb_create_time_ns(duckdb_time_ns input) {
	return CAPICreateValue(duckdb::dtime_ns_t(input.nanos));
}
duckdb_time_ns duckdb_get_time_ns(duckdb_value val) {
	return {CAPIGetValue<duckdb::dtime_ns_t, LogicalTypeId::TIME_NS>(val).value};
}

duckdb_value duckdb_create_timestamp(duckdb_timestamp input) {
	duckdb::timestamp_t ts(input.micros);
	return CAPICreateValue(ts);
}

duckdb_timestamp duckdb_get_timestamp(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_t, LogicalTypeId::TIMESTAMP>(val).value};
}

duckdb_value duckdb_create_timestamp_tz(duckdb_timestamp input) {
	duckdb::timestamp_tz_t ts(input.micros);
	return CAPICreateValue(ts);
}

duckdb_timestamp duckdb_get_timestamp_tz(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_tz_t, LogicalTypeId::TIMESTAMP_TZ>(val).value};
}

duckdb_value duckdb_create_timestamp_tz_ns(duckdb_timestamp_ns input) {
	duckdb::timestamp_tz_ns_t ts(input.nanos);
	return CAPICreateValue(ts);
}

duckdb_timestamp_ns duckdb_get_timestamp_tz_ns(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_tz_ns_t, LogicalTypeId::TIMESTAMP_TZ_NS>(val).value};
}

duckdb_value duckdb_create_timestamp_s(duckdb_timestamp_s input) {
	duckdb::timestamp_sec_t ts(input.seconds);
	return CAPICreateValue(ts);
}

duckdb_timestamp_s duckdb_get_timestamp_s(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_sec_t, LogicalTypeId::TIMESTAMP_SEC>(val).value};
}

duckdb_value duckdb_create_timestamp_ms(duckdb_timestamp_ms input) {
	duckdb::timestamp_ms_t ts(input.millis);
	return CAPICreateValue(ts);
}

duckdb_timestamp_ms duckdb_get_timestamp_ms(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_ms_t, LogicalTypeId::TIMESTAMP_MS>(val).value};
}

duckdb_value duckdb_create_timestamp_ns(duckdb_timestamp_ns input) {
	duckdb::timestamp_ns_t ts(input.nanos);
	return CAPICreateValue(ts);
}

duckdb_timestamp_ns duckdb_get_timestamp_ns(duckdb_value val) {
	if (!val) {
		return {0};
	}
	return {CAPIGetValue<duckdb::timestamp_ns_t, LogicalTypeId::TIMESTAMP_NS>(val).value};
}

duckdb_value duckdb_create_interval(duckdb_interval input) {
	return WrapValue(new duckdb::Value(duckdb::Value::INTERVAL(input.months, input.days, input.micros)));
}
duckdb_interval duckdb_get_interval(duckdb_value val) {
	auto interval = CAPIGetValue<duckdb::interval_t, LogicalTypeId::INTERVAL>(val);
	return {interval.months, interval.days, interval.micros};
}
duckdb_value duckdb_create_blob(const uint8_t *data, idx_t length) {
	return WrapValue(new duckdb::Value(duckdb::Value::BLOB((const uint8_t *)data, length)));
}
duckdb_blob duckdb_get_blob(duckdb_value val) {
	auto res = UnwrapValue(val).DefaultCastAs(duckdb::LogicalType::BLOB);
	auto &str = duckdb::StringValue::Get(res);

	auto result = reinterpret_cast<void *>(malloc(sizeof(char) * str.size()));
	if (!result) {
		return {nullptr, 0};
	}
	memcpy(result, str.c_str(), str.size());
	return {result, str.size()};
}
duckdb_value duckdb_create_bit(duckdb_bit input) {
	return WrapValue(new duckdb::Value(duckdb::Value::BIT(input.data, input.size)));
}
duckdb_bit duckdb_get_bit(duckdb_value val) {
	auto v = UnwrapValue(val).DefaultCastAs(duckdb::LogicalType::BIT);
	auto &str = duckdb::StringValue::Get(v);
	auto size = str.size();
	auto data = reinterpret_cast<uint8_t *>(malloc(size));
	if (!data) {
		return {nullptr, 0};
	}
	memcpy(data, str.c_str(), size);
	return {data, size};
}
duckdb_value duckdb_create_uuid(duckdb_uhugeint input) {
	// uhugeint_t has a constexpr ctor with upper first
	return WrapValue(new duckdb::Value(duckdb::Value::UUID(duckdb::UUID::FromUHugeint({input.upper, input.lower}))));
}
duckdb_uhugeint duckdb_get_uuid(duckdb_value val) {
	auto hugeint = CAPIGetValue<duckdb::hugeint_t, LogicalTypeId::UUID>(val);
	auto uhugeint = duckdb::UUID::ToUHugeint(hugeint);
	// duckdb_uhugeint has no constexpr ctor; struct is lower first
	return {uhugeint.lower, uhugeint.upper};
}

duckdb_logical_type duckdb_get_value_type(duckdb_value val) {
	auto &type = UnwrapValue(val).type();
	return (duckdb_logical_type)(&type);
}

char *duckdb_get_varchar(duckdb_value value) {
	auto val = reinterpret_cast<duckdb::Value *>(value);
	auto str_val = val->DefaultCastAs(duckdb::LogicalType::VARCHAR);
	auto &str = duckdb::StringValue::Get(str_val);

	auto result = reinterpret_cast<char *>(malloc(sizeof(char) * (str.size() + 1)));
	if (!result) {
		return nullptr;
	}
	memcpy(result, str.c_str(), str.size());
	result[str.size()] = '\0';
	return result;
}
duckdb_value duckdb_create_struct_value(duckdb_logical_type type, duckdb_value *values) {
	if (!type || !values) {
		return nullptr;
	}
	const auto &logical_type = UnwrapType(type);
	if (!duckdb::StructType::IsStruct(logical_type)) {
		return nullptr;
	}
	if (duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::ANY)) {
		return nullptr;
	}

	auto count = duckdb::StructType::GetChildCount(logical_type);
	duckdb::vector<duckdb::Value> unwrapped_values;
	for (idx_t i = 0; i < count; i++) {
		auto value = values[i];
		if (!value) {
			return nullptr;
		}
		unwrapped_values.emplace_back(UnwrapValue(value));
	}
	duckdb::Value *struct_value = new duckdb::Value;
	try {
		*struct_value = duckdb::Value::STRUCT(logical_type, std::move(unwrapped_values));
	} catch (...) {
		delete struct_value;
		return nullptr;
	}
	return WrapValue(struct_value);
}

duckdb_value duckdb_create_list_value(duckdb_logical_type type, duckdb_value *values, idx_t value_count) {
	if (!type || !values) {
		return nullptr;
	}
	auto &logical_type = UnwrapType(type);
	duckdb::vector<duckdb::Value> unwrapped_values;
	if (duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::ANY)) {
		return nullptr;
	}

	for (idx_t i = 0; i < value_count; i++) {
		auto value = values[i];
		if (!value) {
			return nullptr;
		}
		unwrapped_values.push_back(UnwrapValue(value));
	}
	auto list_value = new duckdb::Value;
	try {
		*list_value = duckdb::Value::LIST(logical_type, std::move(unwrapped_values));
	} catch (...) {
		delete list_value;
		return nullptr;
	}
	return WrapValue(list_value);
}

duckdb_value duckdb_create_array_value(duckdb_logical_type type, duckdb_value *values, idx_t value_count) {
	if (!type || !values) {
		return nullptr;
	}
	if (value_count >= duckdb::ArrayType::MAX_ARRAY_SIZE) {
		return nullptr;
	}
	auto &logical_type = UnwrapType(type);
	if (duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(logical_type, duckdb::LogicalTypeId::ANY)) {
		return nullptr;
	}
	duckdb::vector<duckdb::Value> unwrapped_values;

	for (idx_t i = 0; i < value_count; i++) {
		auto value = values[i];
		if (!value) {
			return nullptr;
		}
		unwrapped_values.push_back(UnwrapValue(value));
	}
	duckdb::Value *array_value = new duckdb::Value;
	try {
		*array_value = duckdb::Value::ARRAY(logical_type, std::move(unwrapped_values));
	} catch (...) {
		delete array_value;
		return nullptr;
	}
	return WrapValue(array_value);
}

duckdb_value duckdb_create_map_value(duckdb_logical_type map_type, duckdb_value *keys, duckdb_value *values,
                                     idx_t entry_count) {
	if (!map_type || !keys || !values) {
		return nullptr;
	}
	const auto &map_logical_type = UnwrapType(map_type);
	if (map_logical_type.id() != duckdb::LogicalTypeId::MAP) {
		return nullptr;
	}
	if (duckdb::TypeVisitor::Contains(map_logical_type, duckdb::LogicalTypeId::INVALID) ||
	    duckdb::TypeVisitor::Contains(map_logical_type, duckdb::LogicalTypeId::ANY)) {
		return nullptr;
	}

	const auto &key_logical_type = duckdb::MapType::KeyType(map_logical_type);
	const auto &value_logical_type = duckdb::MapType::ValueType(map_logical_type);
	duckdb::vector<duckdb::Value> unwrapped_keys;
	duckdb::vector<duckdb::Value> unwrapped_values;
	for (idx_t i = 0; i < entry_count; i++) {
		const auto key = keys[i];
		const auto value = values[i];
		if (!key || !value) {
			return nullptr;
		}
		unwrapped_keys.emplace_back(UnwrapValue(key));
		unwrapped_values.emplace_back(UnwrapValue(value));
	}
	duckdb::Value *map_value = new duckdb::Value;
	try {
		*map_value = duckdb::Value::MAP(key_logical_type, value_logical_type, std::move(unwrapped_keys),
		                                std::move(unwrapped_values));
	} catch (...) {
		delete map_value;
		return nullptr;
	}
	return WrapValue(map_value);
}

duckdb_value duckdb_create_union_value(duckdb_logical_type union_type, idx_t tag_index, duckdb_value value) {
	if (!union_type || !value) {
		return nullptr;
	}
	const auto &union_logical_type = UnwrapType(union_type);
	if (union_logical_type.id() != duckdb::LogicalTypeId::UNION) {
		return nullptr;
	}
	idx_t member_count = duckdb::UnionType::GetMemberCount(union_logical_type);
	if (tag_index >= member_count) {
		return nullptr;
	}
	const auto &member_type = duckdb::UnionType::GetMemberType(union_logical_type, tag_index);
	const auto &unwrapped_value = UnwrapValue(value);
	if (unwrapped_value.type() != member_type) {
		return nullptr;
	}
	const auto member_types = duckdb::UnionType::CopyMemberTypes(union_logical_type);
	duckdb::Value *union_value = new duckdb::Value;
	try {
		*union_value = duckdb::Value::UNION(member_types, duckdb::NumericCast<uint8_t>(tag_index), unwrapped_value);
	} catch (...) {
		delete union_value;
		return nullptr;
	}
	return WrapValue(union_value);
}

idx_t duckdb_get_map_size(duckdb_value value) {
	if (!value) {
		return 0;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::MAP || val.IsNull()) {
		return 0;
	}

	auto &children = duckdb::MapValue::GetChildren(val);
	return children.size();
}

duckdb_value duckdb_get_map_key(duckdb_value value, idx_t index) {
	if (!value) {
		return nullptr;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::MAP || val.IsNull()) {
		return nullptr;
	}

	auto &children = duckdb::MapValue::GetChildren(val);
	if (index >= children.size()) {
		return nullptr;
	}

	auto &child = children[index];
	auto &child_struct = duckdb::StructValue::GetChildren(child);
	return WrapValue(new duckdb::Value(child_struct[0]));
}

duckdb_value duckdb_get_map_value(duckdb_value value, idx_t index) {
	if (!value) {
		return nullptr;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::MAP || val.IsNull()) {
		return nullptr;
	}

	auto &children = duckdb::MapValue::GetChildren(val);
	if (index >= children.size()) {
		return nullptr;
	}

	auto &child = children[index];
	auto &child_struct = duckdb::StructValue::GetChildren(child);
	return WrapValue(new duckdb::Value(child_struct[1]));
}

bool duckdb_is_null_value(duckdb_value value) {
	if (!value) {
		return false;
	}
	return UnwrapValue(value).IsNull();
}

duckdb_value duckdb_create_null_value() {
	return WrapValue(new duckdb::Value());
}

idx_t duckdb_get_list_size(duckdb_value value) {
	if (!value) {
		return 0;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::LIST || val.IsNull()) {
		return 0;
	}

	auto &children = duckdb::ListValue::GetChildren(val);
	return children.size();
}

duckdb_value duckdb_get_list_child(duckdb_value value, idx_t index) {
	if (!value) {
		return nullptr;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::LIST || val.IsNull()) {
		return nullptr;
	}

	auto &children = duckdb::ListValue::GetChildren(val);
	if (index >= children.size()) {
		return nullptr;
	}

	return WrapValue(new duckdb::Value(children[index]));
}

duckdb_value duckdb_create_enum_value(duckdb_logical_type type, uint64_t value) {
	if (!type) {
		return nullptr;
	}

	auto &logical_type = UnwrapType(type);
	if (logical_type.id() != LogicalTypeId::ENUM) {
		return nullptr;
	}

	if (value >= duckdb::EnumType::GetSize(logical_type)) {
		return nullptr;
	}

	return WrapValue(new duckdb::Value(duckdb::Value::ENUM(value, logical_type)));
}

uint64_t duckdb_get_enum_value(duckdb_value value) {
	if (!value) {
		return 0;
	}

	auto val = UnwrapValue(value);
	if (val.type().id() != LogicalTypeId::ENUM || val.IsNull()) {
		return 0;
	}

	return val.GetValue<uint64_t>();
}

duckdb_value duckdb_get_struct_child(duckdb_value value, idx_t index) {
	if (!value) {
		return nullptr;
	}

	auto val = UnwrapValue(value);
	if (!duckdb::StructType::IsStruct(val.type()) || val.IsNull()) {
		return nullptr;
	}

	auto &children = duckdb::StructValue::GetChildren(val);
	if (index >= children.size()) {
		return nullptr;
	}

	return WrapValue(new duckdb::Value(children[index]));
}

char *duckdb_value_to_string(duckdb_value val) {
	if (!val) {
		return nullptr;
	}

	auto v = UnwrapValue(val);
	auto str = v.ToSQLString();

	auto result = reinterpret_cast<char *>(malloc(sizeof(char) * (str.size() + 1)));
	if (!result) {
		return nullptr;
	}
	memcpy(result, str.c_str(), str.size());
	result[str.size()] = '\0';
	return result;
}
