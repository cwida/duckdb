#include "duckdb/common/types/value.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/aggregate_operators.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"

#include "utf8proc_wrapper.hpp"
#include "duckdb/common/operator/numeric_binary_operators.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

Value::Value(LogicalType type) : type_(move(type)), is_null(true) {
}

Value::Value(int32_t val) : type_(LogicalType::INTEGER), is_null(false) {
	value_.integer = val;
}

Value::Value(int64_t val) : type_(LogicalType::BIGINT), is_null(false) {
	value_.bigint = val;
}

Value::Value(float val) : type_(LogicalType::FLOAT), is_null(false) {
	if (!Value::FloatIsValid(val)) {
		throw OutOfRangeException("Invalid float value %f", val);
	}
	value_.float_ = val;
}

Value::Value(double val) : type_(LogicalType::DOUBLE), is_null(false) {
	if (!Value::DoubleIsValid(val)) {
		throw OutOfRangeException("Invalid double value %f", val);
	}
	value_.double_ = val;
}

Value::Value(const char *val) : Value(val ? string(val) : string()) {
}

Value::Value(std::nullptr_t val) : Value(LogicalType::VARCHAR) {
}

Value::Value(string_t val) : Value(string(val.GetDataUnsafe(), val.GetSize())) {
}

Value::Value(string val) : type_(LogicalType::VARCHAR), is_null(false), str_value(move(val)) {
	if (!Value::StringIsValid(str_value.c_str(), str_value.size())) {
		throw Exception("String value is not valid UTF8");
	}
}

Value Value::MinimumValue(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Value::BOOLEAN(false);
	case LogicalTypeId::TINYINT:
		return Value::TINYINT(NumericLimits<int8_t>::Minimum());
	case LogicalTypeId::SMALLINT:
		return Value::SMALLINT(NumericLimits<int16_t>::Minimum());
	case LogicalTypeId::INTEGER:
		return Value::INTEGER(NumericLimits<int32_t>::Minimum());
	case LogicalTypeId::DATE:
		return Value::DATE(date_t(NumericLimits<int32_t>::Minimum()));
	case LogicalTypeId::TIME:
		return Value::TIME(dtime_t(NumericLimits<int64_t>::Minimum()));
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(NumericLimits<int64_t>::Minimum());
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(NumericLimits<uint8_t>::Minimum());
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(NumericLimits<uint16_t>::Minimum());
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(NumericLimits<uint32_t>::Minimum());
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(NumericLimits<uint64_t>::Minimum());
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(NumericLimits<int64_t>::Minimum()));
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TimestampSec(timestamp_t(NumericLimits<int64_t>::Minimum()));
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TimestampMs(timestamp_t(NumericLimits<int64_t>::Minimum()));
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TimestampNs(timestamp_t(NumericLimits<int64_t>::Minimum()));
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(NumericLimits<hugeint_t>::Minimum());
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(NumericLimits<float>::Minimum());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(NumericLimits<double>::Minimum());
	case LogicalTypeId::DECIMAL: {
		Value result;
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			result = Value::MinimumValue(LogicalType::SMALLINT);
			break;
		case PhysicalType::INT32:
			result = Value::MinimumValue(LogicalType::INTEGER);
			break;
		case PhysicalType::INT64:
			result = Value::MinimumValue(LogicalType::BIGINT);
			break;
		case PhysicalType::INT128:
			result = Value::MinimumValue(LogicalType::HUGEINT);
			break;
		default:
			throw InternalException("Unknown decimal type");
		}
		result.type_ = type;
		return result;
	}
	default:
		throw InvalidTypeException(type, "MinimumValue requires numeric type");
	}
}

Value Value::MaximumValue(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Value::BOOLEAN(false);
	case LogicalTypeId::TINYINT:
		return Value::TINYINT(NumericLimits<int8_t>::Maximum());
	case LogicalTypeId::SMALLINT:
		return Value::SMALLINT(NumericLimits<int16_t>::Maximum());
	case LogicalTypeId::INTEGER:
		return Value::INTEGER(NumericLimits<int32_t>::Maximum());
	case LogicalTypeId::DATE:
		return Value::DATE(date_t(NumericLimits<int32_t>::Maximum()));
	case LogicalTypeId::TIME:
		return Value::TIME(dtime_t(NumericLimits<int64_t>::Maximum()));
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(NumericLimits<int64_t>::Maximum());
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(NumericLimits<uint8_t>::Maximum());
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(NumericLimits<uint16_t>::Maximum());
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(NumericLimits<uint32_t>::Maximum());
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(NumericLimits<uint64_t>::Maximum());
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(NumericLimits<int64_t>::Maximum()));
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TimestampMs(timestamp_t(NumericLimits<int64_t>::Maximum()));
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TimestampNs(timestamp_t(NumericLimits<int64_t>::Maximum()));
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TimestampSec(timestamp_t(NumericLimits<int64_t>::Maximum()));
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(NumericLimits<hugeint_t>::Maximum());
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(NumericLimits<float>::Maximum());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(NumericLimits<double>::Maximum());
	case LogicalTypeId::DECIMAL: {
		Value result;
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			result = Value::MaximumValue(LogicalType::SMALLINT);
			break;
		case PhysicalType::INT32:
			result = Value::MaximumValue(LogicalType::INTEGER);
			break;
		case PhysicalType::INT64:
			result = Value::MaximumValue(LogicalType::BIGINT);
			break;
		case PhysicalType::INT128:
			result = Value::MaximumValue(LogicalType::HUGEINT);
			break;
		default:
			throw InternalException("Unknown decimal type");
		}
		result.type_ = type;
		return result;
	}
	default:
		throw InvalidTypeException(type, "MaximumValue requires numeric type");
	}
}

Value Value::BOOLEAN(int8_t value) {
	Value result(LogicalType::BOOLEAN);
	result.value_.boolean = value ? true : false;
	result.is_null = false;
	return result;
}

Value Value::TINYINT(int8_t value) {
	Value result(LogicalType::TINYINT);
	result.value_.tinyint = value;
	result.is_null = false;
	return result;
}

Value Value::SMALLINT(int16_t value) {
	Value result(LogicalType::SMALLINT);
	result.value_.smallint = value;
	result.is_null = false;
	return result;
}

Value Value::INTEGER(int32_t value) {
	Value result(LogicalType::INTEGER);
	result.value_.integer = value;
	result.is_null = false;
	return result;
}

Value Value::BIGINT(int64_t value) {
	Value result(LogicalType::BIGINT);
	result.value_.bigint = value;
	result.is_null = false;
	return result;
}

Value Value::HUGEINT(hugeint_t value) {
	Value result(LogicalType::HUGEINT);
	result.value_.hugeint = value;
	result.is_null = false;
	return result;
}

Value Value::UTINYINT(uint8_t value) {
	Value result(LogicalType::UTINYINT);
	result.value_.utinyint = value;
	result.is_null = false;
	return result;
}

Value Value::USMALLINT(uint16_t value) {
	Value result(LogicalType::USMALLINT);
	result.value_.usmallint = value;
	result.is_null = false;
	return result;
}

Value Value::UINTEGER(uint32_t value) {
	Value result(LogicalType::UINTEGER);
	result.value_.uinteger = value;
	result.is_null = false;
	return result;
}

Value Value::UBIGINT(uint64_t value) {
	Value result(LogicalType::UBIGINT);
	result.value_.ubigint = value;
	result.is_null = false;
	return result;
}

bool Value::FloatIsValid(float value) {
	return !(std::isnan(value) || std::isinf(value));
}

bool Value::DoubleIsValid(double value) {
	return !(std::isnan(value) || std::isinf(value));
}

bool Value::StringIsValid(const char *str, idx_t length) {
	auto utf_type = Utf8Proc::Analyze(str, length);
	return utf_type != UnicodeType::INVALID;
}

Value Value::DECIMAL(int16_t value, uint8_t width, uint8_t scale) {
	D_ASSERT(width <= Decimal::MAX_WIDTH_INT16);
	Value result(LogicalType::DECIMAL(width, scale));
	result.value_.smallint = value;
	result.is_null = false;
	return result;
}

Value Value::DECIMAL(int32_t value, uint8_t width, uint8_t scale) {
	D_ASSERT(width >= Decimal::MAX_WIDTH_INT16 && width <= Decimal::MAX_WIDTH_INT32);
	Value result(LogicalType::DECIMAL(width, scale));
	result.value_.integer = value;
	result.is_null = false;
	return result;
}

Value Value::DECIMAL(int64_t value, uint8_t width, uint8_t scale) {
	auto decimal_type = LogicalType::DECIMAL(width, scale);
	Value result(decimal_type);
	switch (decimal_type.InternalType()) {
	case PhysicalType::INT16:
		result.value_.smallint = value;
		break;
	case PhysicalType::INT32:
		result.value_.integer = value;
		break;
	case PhysicalType::INT64:
		result.value_.bigint = value;
		break;
	default:
		result.value_.hugeint = value;
		break;
	}
	result.type_.Verify();
	result.is_null = false;
	return result;
}

Value Value::DECIMAL(hugeint_t value, uint8_t width, uint8_t scale) {
	D_ASSERT(width >= Decimal::MAX_WIDTH_INT64 && width <= Decimal::MAX_WIDTH_INT128);
	Value result(LogicalType::DECIMAL(width, scale));
	result.value_.hugeint = value;
	result.is_null = false;
	return result;
}

Value Value::FLOAT(float value) {
	if (!Value::FloatIsValid(value)) {
		throw OutOfRangeException("Invalid float value %f", value);
	}
	Value result(LogicalType::FLOAT);
	result.value_.float_ = value;
	result.is_null = false;
	return result;
}

Value Value::DOUBLE(double value) {
	if (!Value::DoubleIsValid(value)) {
		throw OutOfRangeException("Invalid double value %f", value);
	}
	Value result(LogicalType::DOUBLE);
	result.value_.double_ = value;
	result.is_null = false;
	return result;
}

Value Value::HASH(hash_t value) {
	Value result(LogicalType::HASH);
	result.value_.hash = value;
	result.is_null = false;
	return result;
}

Value Value::POINTER(uintptr_t value) {
	Value result(LogicalType::POINTER);
	result.value_.pointer = value;
	result.is_null = false;
	return result;
}

Value Value::DATE(date_t value) {
	Value result(LogicalType::DATE);
	result.value_.date = value;
	result.is_null = false;
	return result;
}

Value Value::DATE(int32_t year, int32_t month, int32_t day) {
	return Value::DATE(Date::FromDate(year, month, day));
}

Value Value::TIME(dtime_t value) {
	Value result(LogicalType::TIME);
	result.value_.time = value;
	result.is_null = false;
	return result;
}

Value Value::TIME(int32_t hour, int32_t min, int32_t sec, int32_t micros) {
	return Value::TIME(Time::FromTime(hour, min, sec, micros));
}

Value Value::TIMESTAMP(timestamp_t value) {
	Value result(LogicalType::TIMESTAMP);
	result.value_.timestamp = value;
	result.is_null = false;
	return result;
}

Value Value::TimestampNs(timestamp_t timestamp) {
	Value result(LogicalType::TIMESTAMP_NS);
	result.value_.timestamp = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TimestampMs(timestamp_t timestamp) {
	Value result(LogicalType::TIMESTAMP_MS);
	result.value_.timestamp = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TimestampSec(timestamp_t timestamp) {
	Value result(LogicalType::TIMESTAMP_S);
	result.value_.timestamp = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMP(date_t date, dtime_t time) {
	return Value::TIMESTAMP(Timestamp::FromDatetime(date, time));
}

Value Value::TIMESTAMP(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t min, int32_t sec,
                       int32_t micros) {
	auto val = Value::TIMESTAMP(Date::FromDate(year, month, day), Time::FromTime(hour, min, sec, micros));
	val.type_ = LogicalType::TIMESTAMP;
	return val;
}

Value Value::STRUCT(child_list_t<Value> values) {
	Value result;
	child_list_t<LogicalType> child_types;
	for (auto &child : values) {
		child_types.push_back(make_pair(move(child.first), child.second.type()));
		result.struct_value.push_back(move(child.second));
	}
	result.type_ = LogicalType::STRUCT(move(child_types));

	result.is_null = false;
	return result;
}

Value Value::MAP(Value key, Value value) {
	Value result;
	child_list_t<LogicalType> child_types;
	child_types.push_back({"key", key.type()});
	child_types.push_back({"value", value.type()});

	result.type_ = LogicalType::MAP(move(child_types));

	result.struct_value.push_back(move(key));
	result.struct_value.push_back(move(value));
	result.is_null = false;
	return result;
}

Value Value::LIST(vector<Value> values) {
	D_ASSERT(!values.empty());
#ifdef DEBUG
	for (idx_t i = 1; i < values.size(); i++) {
		D_ASSERT(values[i].type() == values[0].type());
	}
#endif
	Value result;
	result.type_ = LogicalType::LIST(values[0].type());
	result.list_value = move(values);
	result.is_null = false;
	return result;
}

Value Value::BLOB(const_data_ptr_t data, idx_t len) {
	Value result(LogicalType::BLOB);
	result.is_null = false;
	result.str_value = string((const char *)data, len);
	return result;
}

Value Value::BLOB(const string &data) {
	Value result(LogicalType::BLOB);
	result.is_null = false;
	result.str_value = Blob::ToBlob(string_t(data));
	return result;
}

Value Value::INTERVAL(int32_t months, int32_t days, int64_t micros) {
	Value result(LogicalType::INTERVAL);
	result.is_null = false;
	result.value_.interval.months = months;
	result.value_.interval.days = days;
	result.value_.interval.micros = micros;
	return result;
}

Value Value::INTERVAL(interval_t interval) {
	return Value::INTERVAL(interval.months, interval.days, interval.micros);
}

//===--------------------------------------------------------------------===//
// CreateValue
//===--------------------------------------------------------------------===//
template <>
Value Value::CreateValue(bool value) {
	return Value::BOOLEAN(value);
}

template <>
Value Value::CreateValue(int8_t value) {
	return Value::TINYINT(value);
}

template <>
Value Value::CreateValue(int16_t value) {
	return Value::SMALLINT(value);
}

template <>
Value Value::CreateValue(int32_t value) {
	return Value::INTEGER(value);
}

template <>
Value Value::CreateValue(int64_t value) {
	return Value::BIGINT(value);
}

template <>
Value Value::CreateValue(uint8_t value) {
	return Value::UTINYINT(value);
}

template <>
Value Value::CreateValue(uint16_t value) {
	return Value::USMALLINT(value);
}

template <>
Value Value::CreateValue(uint32_t value) {
	return Value::UINTEGER(value);
}

template <>
Value Value::CreateValue(uint64_t value) {
	return Value::UBIGINT(value);
}

template <>
Value Value::CreateValue(hugeint_t value) {
	return Value::HUGEINT(value);
}

template <>
Value Value::CreateValue(date_t value) {
	return Value::DATE(value);
}

template <>
Value Value::CreateValue(dtime_t value) {
	return Value::TIME(value);
}

template <>
Value Value::CreateValue(timestamp_t value) {
	return Value::TIMESTAMP(value);
}

template <>
Value Value::CreateValue(const char *value) {
	return Value(string(value));
}

template <>
Value Value::CreateValue(string value) { // NOLINT: required for templating
	return Value::BLOB(value);
}

template <>
Value Value::CreateValue(string_t value) {
	return Value(value);
}

template <>
Value Value::CreateValue(float value) {
	return Value::FLOAT(value);
}

template <>
Value Value::CreateValue(double value) {
	return Value::DOUBLE(value);
}

template <>
Value Value::CreateValue(interval_t value) {
	return Value::INTERVAL(value);
}

template <>
Value Value::CreateValue(Value value) {
	return value;
}

//===--------------------------------------------------------------------===//
// GetValue
//===--------------------------------------------------------------------===//
template <class T>
T Value::GetValueInternal() const {
	if (is_null) {
		return NullValue<T>();
	}
	switch (type_.id()) {
	case LogicalTypeId::BOOLEAN:
		return Cast::Operation<bool, T>(value_.boolean);
	case LogicalTypeId::TINYINT:
		return Cast::Operation<int8_t, T>(value_.tinyint);
	case LogicalTypeId::SMALLINT:
		return Cast::Operation<int16_t, T>(value_.smallint);
	case LogicalTypeId::INTEGER:
		return Cast::Operation<int32_t, T>(value_.integer);
	case LogicalTypeId::BIGINT:
		return Cast::Operation<int64_t, T>(value_.bigint);
	case LogicalTypeId::HUGEINT:
		return Cast::Operation<hugeint_t, T>(value_.hugeint);
	case LogicalTypeId::DATE:
		return Cast::Operation<int32_t, T>(value_.integer);
	case LogicalTypeId::TIME:
		return Cast::Operation<int64_t, T>(value_.bigint);
	case LogicalTypeId::TIMESTAMP:
		return Cast::Operation<int64_t, T>(value_.bigint);
	case LogicalTypeId::UTINYINT:
		return Cast::Operation<uint8_t, T>(value_.utinyint);
	case LogicalTypeId::USMALLINT:
		return Cast::Operation<uint16_t, T>(value_.usmallint);
	case LogicalTypeId::UINTEGER:
		return Cast::Operation<uint32_t, T>(value_.uinteger);
	case LogicalTypeId::UBIGINT:
		return Cast::Operation<uint64_t, T>(value_.ubigint);
	case LogicalTypeId::FLOAT:
		return Cast::Operation<float, T>(value_.float_);
	case LogicalTypeId::DOUBLE:
		return Cast::Operation<double, T>(value_.double_);
	case LogicalTypeId::VARCHAR:
		return Cast::Operation<string_t, T>(str_value.c_str());
	case LogicalTypeId::DECIMAL:
		return CastAs(LogicalType::DOUBLE).GetValueInternal<T>();
	default:
		throw NotImplementedException("Unimplemented type \"%s\" for GetValue()", type_.ToString());
	}
}

template <>
bool Value::GetValue() const {
	return GetValueInternal<int8_t>();
}
template <>
int8_t Value::GetValue() const {
	return GetValueInternal<int8_t>();
}
template <>
int16_t Value::GetValue() const {
	return GetValueInternal<int16_t>();
}
template <>
int32_t Value::GetValue() const {
	if (type_.id() == LogicalTypeId::DATE) {
		return value_.integer;
	}
	return GetValueInternal<int32_t>();
}
template <>
int64_t Value::GetValue() const {
	if (type_.id() == LogicalTypeId::TIMESTAMP || type_.id() == LogicalTypeId::TIME ||
	    type_.id() == LogicalTypeId::TIMESTAMP_SEC || type_.id() == LogicalTypeId::TIMESTAMP_NS ||
	    type_.id() == LogicalTypeId::TIMESTAMP_MS) {
		return value_.bigint;
	}
	return GetValueInternal<int64_t>();
}
template <>
hugeint_t Value::GetValue() const {
	return GetValueInternal<hugeint_t>();
}
template <>
uint8_t Value::GetValue() const {
	return GetValueInternal<uint8_t>();
}
template <>
uint16_t Value::GetValue() const {
	return GetValueInternal<uint16_t>();
}
template <>
uint32_t Value::GetValue() const {
	return GetValueInternal<uint32_t>();
}
template <>
uint64_t Value::GetValue() const {
	return GetValueInternal<uint64_t>();
}
template <>
string Value::GetValue() const {
	return ToString();
}
template <>
float Value::GetValue() const {
	return GetValueInternal<float>();
}
template <>
double Value::GetValue() const {
	return GetValueInternal<double>();
}
template <>
date_t Value::GetValue() const {
	return GetValueInternal<date_t>();
}
template <>
dtime_t Value::GetValue() const {
	return GetValueInternal<dtime_t>();
}
template <>
timestamp_t Value::GetValue() const {
	return GetValueInternal<timestamp_t>();
}

uintptr_t Value::GetPointer() const {
	D_ASSERT(type() == LogicalType::POINTER);
	return value_.pointer;
}

Value Value::Numeric(const LogicalType &type, int64_t value) {
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		D_ASSERT(value <= NumericLimits<int8_t>::Maximum());
		return Value::TINYINT((int8_t)value);
	case LogicalTypeId::SMALLINT:
		D_ASSERT(value <= NumericLimits<int16_t>::Maximum());
		return Value::SMALLINT((int16_t)value);
	case LogicalTypeId::INTEGER:
		D_ASSERT(value <= NumericLimits<int32_t>::Maximum());
		return Value::INTEGER((int32_t)value);
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(value);
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(value);
	case LogicalTypeId::DECIMAL:
		return Value::DECIMAL(value, DecimalType::GetWidth(type), DecimalType::GetScale(type));
	case LogicalTypeId::FLOAT:
		return Value((float)value);
	case LogicalTypeId::DOUBLE:
		return Value((double)value);
	case LogicalTypeId::HASH:
		return Value::HASH(value);
	case LogicalTypeId::POINTER:
		return Value::POINTER(value);
	case LogicalTypeId::DATE:
		D_ASSERT(value <= NumericLimits<int32_t>::Maximum());
		return Value::DATE(date_t(value));
	case LogicalTypeId::TIME:
		return Value::TIME(dtime_t(value));
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(value));
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TimestampNs(timestamp_t(value));
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TimestampMs(timestamp_t(value));
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TimestampSec(timestamp_t(value));
	default:
		throw InvalidTypeException(type, "Numeric requires numeric type");
	}
}

//===--------------------------------------------------------------------===//
// GetValueUnsafe
//===--------------------------------------------------------------------===//
template <>
int8_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT8 || type_.InternalType() == PhysicalType::BOOL);
	return value_.tinyint;
}

template <>
int16_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT16);
	return value_.smallint;
}

template <>
int32_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT32);
	return value_.integer;
}

template <>
int64_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.bigint;
}

template <>
hugeint_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT128);
	return value_.hugeint;
}

template <>
uint8_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT8);
	return value_.utinyint;
}

template <>
uint16_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT16);
	return value_.usmallint;
}

template <>
uint32_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT32);
	return value_.uinteger;
}

template <>
uint64_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT64);
	return value_.ubigint;
}

template <>
string &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::VARCHAR);
	return str_value;
}

template <>
float &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::FLOAT);
	return value_.float_;
}

template <>
double &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::DOUBLE);
	return value_.double_;
}

template <>
date_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT32);
	return value_.date;
}

template <>
dtime_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.time;
}

template <>
timestamp_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp;
}

template <>
interval_t &Value::GetValueUnsafe() {
	D_ASSERT(type_.InternalType() == PhysicalType::INTERVAL);
	return value_.interval;
}

Value Value::Numeric(const LogicalType &type, hugeint_t value) {
	switch (type.id()) {
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(value);
	default:
		return Value::Numeric(type, Hugeint::Cast<int64_t>(value));
	}
}

string Value::ToString() const {
	if (is_null) {
		return "NULL";
	}
	switch (type_.id()) {
	case LogicalTypeId::BOOLEAN:
		return value_.boolean ? "True" : "False";
	case LogicalTypeId::TINYINT:
		return to_string(value_.tinyint);
	case LogicalTypeId::SMALLINT:
		return to_string(value_.smallint);
	case LogicalTypeId::INTEGER:
		return to_string(value_.integer);
	case LogicalTypeId::BIGINT:
		return to_string(value_.bigint);
	case LogicalTypeId::UTINYINT:
		return to_string(value_.utinyint);
	case LogicalTypeId::USMALLINT:
		return to_string(value_.usmallint);
	case LogicalTypeId::UINTEGER:
		return to_string(value_.uinteger);
	case LogicalTypeId::UBIGINT:
		return to_string(value_.ubigint);
	case LogicalTypeId::HUGEINT:
		return Hugeint::ToString(value_.hugeint);
	case LogicalTypeId::FLOAT:
		return to_string(value_.float_);
	case LogicalTypeId::DOUBLE:
		return to_string(value_.double_);
	case LogicalTypeId::DECIMAL: {
		auto internal_type = type_.InternalType();
		auto scale = DecimalType::GetScale(type_);
		if (internal_type == PhysicalType::INT16) {
			return Decimal::ToString(value_.smallint, scale);
		} else if (internal_type == PhysicalType::INT32) {
			return Decimal::ToString(value_.integer, scale);
		} else if (internal_type == PhysicalType::INT64) {
			return Decimal::ToString(value_.bigint, scale);
		} else {
			D_ASSERT(internal_type == PhysicalType::INT128);
			return Decimal::ToString(value_.hugeint, scale);
		}
	}
	case LogicalTypeId::DATE:
		return Date::ToString(value_.date);
	case LogicalTypeId::TIME:
		return Time::ToString(value_.time);
	case LogicalTypeId::TIMESTAMP:
		return Timestamp::ToString(value_.timestamp);
	case LogicalTypeId::TIMESTAMP_SEC:
		return Timestamp::ToString(Timestamp::FromEpochSeconds(value_.timestamp.value));
	case LogicalTypeId::TIMESTAMP_MS:
		return Timestamp::ToString(Timestamp::FromEpochMs(value_.timestamp.value));
	case LogicalTypeId::TIMESTAMP_NS:
		return Timestamp::ToString(Timestamp::FromEpochNanoSeconds(value_.timestamp.value));
	case LogicalTypeId::INTERVAL:
		return Interval::ToString(value_.interval);
	case LogicalTypeId::VARCHAR:
		return str_value;
	case LogicalTypeId::BLOB:
		return Blob::ToString(string_t(str_value));
	case LogicalTypeId::POINTER:
		return to_string(value_.pointer);
	case LogicalTypeId::HASH:
		return to_string(value_.hash);
	case LogicalTypeId::STRUCT: {
		string ret = "{";
		auto &child_types = StructType::GetChildTypes(type_);
		for (size_t i = 0; i < struct_value.size(); i++) {
			auto &name = child_types[i].first;
			auto &child = struct_value[i];
			ret += "'" + name + "': " + child.ToString();
			if (i < struct_value.size() - 1) {
				ret += ", ";
			}
		}
		ret += "}";
		return ret;
	}
	case LogicalTypeId::LIST: {
		string ret = "[";
		for (size_t i = 0; i < list_value.size(); i++) {
			auto &child = list_value[i];
			ret += child.ToString();
			if (i < list_value.size() - 1) {
				ret += ", ";
			}
		}
		ret += "]";
		return ret;
	}
	case LogicalTypeId::MAP: {
		string ret = "{";
		auto &key_list = struct_value[0].list_value;
		auto &value_list = struct_value[1].list_value;
		for (size_t i = 0; i < key_list.size(); i++) {
			ret += key_list[i].ToString() + "=" + value_list[i].ToString();
			if (i < key_list.size() - 1) {
				ret += ", ";
			}
		}
		ret += "}";
		return ret;
	}
	default:
		throw NotImplementedException("Unimplemented type for printing: %s", type_.ToString());
	}
}

//===--------------------------------------------------------------------===//
// Numeric Operators
//===--------------------------------------------------------------------===//
Value Value::operator+(const Value &rhs) const {
	return ValueOperations::Add(*this, rhs);
}

Value Value::operator-(const Value &rhs) const {
	return ValueOperations::Subtract(*this, rhs);
}

Value Value::operator*(const Value &rhs) const {
	return ValueOperations::Multiply(*this, rhs);
}

Value Value::operator/(const Value &rhs) const {
	return ValueOperations::Divide(*this, rhs);
}

Value Value::operator%(const Value &rhs) const {
	throw NotImplementedException("value modulo");
	// return ValueOperations::Modulo(*this, rhs);
}

//===--------------------------------------------------------------------===//
// Comparison Operators
//===--------------------------------------------------------------------===//
bool Value::operator==(const Value &rhs) const {
	return ValueOperations::Equals(*this, rhs);
}

bool Value::operator!=(const Value &rhs) const {
	return ValueOperations::NotEquals(*this, rhs);
}

bool Value::operator<(const Value &rhs) const {
	return ValueOperations::LessThan(*this, rhs);
}

bool Value::operator>(const Value &rhs) const {
	return ValueOperations::GreaterThan(*this, rhs);
}

bool Value::operator<=(const Value &rhs) const {
	return ValueOperations::LessThanEquals(*this, rhs);
}

bool Value::operator>=(const Value &rhs) const {
	return ValueOperations::GreaterThanEquals(*this, rhs);
}

bool Value::operator==(const int64_t &rhs) const {
	return *this == Value::Numeric(type_, rhs);
}

bool Value::operator!=(const int64_t &rhs) const {
	return *this != Value::Numeric(type_, rhs);
}

bool Value::operator<(const int64_t &rhs) const {
	return *this < Value::Numeric(type_, rhs);
}

bool Value::operator>(const int64_t &rhs) const {
	return *this > Value::Numeric(type_, rhs);
}

bool Value::operator<=(const int64_t &rhs) const {
	return *this <= Value::Numeric(type_, rhs);
}

bool Value::operator>=(const int64_t &rhs) const {
	return *this >= Value::Numeric(type_, rhs);
}

Value Value::CastAs(const LogicalType &target_type, bool strict) const {
	if (type_ == target_type) {
		return Copy();
	}
	Vector input(*this);
	Vector result(target_type);
	VectorOperations::Cast(input, result, 1, strict);
	return result.GetValue(0);
}

bool Value::TryCastAs(const LogicalType &target_type, bool strict) {
	try {
		Value new_value = CastAs(target_type, strict);
		type_ = target_type;
		is_null = new_value.is_null;
		value_ = new_value.value_;
		str_value = new_value.str_value;
		struct_value = new_value.struct_value;
		list_value = new_value.list_value;
		return true;
	} catch (Exception &) {
		return false;
	}
}

void Value::Serialize(Serializer &serializer) {
	type_.Serialize(serializer);
	serializer.Write<bool>(is_null);
	if (!is_null) {
		switch (type_.InternalType()) {
		case PhysicalType::BOOL:
			serializer.Write<int8_t>(value_.boolean);
			break;
		case PhysicalType::INT8:
			serializer.Write<int8_t>(value_.tinyint);
			break;
		case PhysicalType::INT16:
			serializer.Write<int16_t>(value_.smallint);
			break;
		case PhysicalType::INT32:
			serializer.Write<int32_t>(value_.integer);
			break;
		case PhysicalType::INT64:
			serializer.Write<int64_t>(value_.bigint);
			break;
		case PhysicalType::UINT8:
			serializer.Write<uint8_t>(value_.utinyint);
			break;
		case PhysicalType::UINT16:
			serializer.Write<uint16_t>(value_.usmallint);
			break;
		case PhysicalType::UINT32:
			serializer.Write<uint32_t>(value_.uinteger);
			break;
		case PhysicalType::UINT64:
			serializer.Write<uint64_t>(value_.ubigint);
			break;
		case PhysicalType::INT128:
			serializer.Write<hugeint_t>(value_.hugeint);
			break;
		case PhysicalType::FLOAT:
			serializer.Write<float>(value_.float_);
			break;
		case PhysicalType::DOUBLE:
			serializer.Write<double>(value_.double_);
			break;
		case PhysicalType::POINTER:
			serializer.Write<uintptr_t>(value_.pointer);
			break;
		case PhysicalType::INTERVAL:
			serializer.Write<interval_t>(value_.interval);
			break;
		case PhysicalType::VARCHAR:
			serializer.WriteString(str_value);
			break;
		default: {
			Vector v(*this);
			v.Serialize(1, serializer);
			break;
		}
		}
	}
}

Value Value::Deserialize(Deserializer &source) {
	auto type = LogicalType::Deserialize(source);
	auto is_null = source.Read<bool>();
	Value new_value = Value(type);
	if (is_null) {
		return new_value;
	}
	new_value.is_null = false;
	switch (type.InternalType()) {
	case PhysicalType::BOOL:
		new_value.value_.boolean = source.Read<int8_t>();
		break;
	case PhysicalType::INT8:
		new_value.value_.tinyint = source.Read<int8_t>();
		break;
	case PhysicalType::INT16:
		new_value.value_.smallint = source.Read<int16_t>();
		break;
	case PhysicalType::INT32:
		new_value.value_.integer = source.Read<int32_t>();
		break;
	case PhysicalType::INT64:
		new_value.value_.bigint = source.Read<int64_t>();
		break;
	case PhysicalType::UINT8:
		new_value.value_.utinyint = source.Read<uint8_t>();
		break;
	case PhysicalType::UINT16:
		new_value.value_.usmallint = source.Read<uint16_t>();
		break;
	case PhysicalType::UINT32:
		new_value.value_.uinteger = source.Read<uint32_t>();
		break;
	case PhysicalType::UINT64:
		new_value.value_.ubigint = source.Read<uint64_t>();
		break;
	case PhysicalType::INT128:
		new_value.value_.hugeint = source.Read<hugeint_t>();
		break;
	case PhysicalType::FLOAT:
		new_value.value_.float_ = source.Read<float>();
		break;
	case PhysicalType::DOUBLE:
		new_value.value_.double_ = source.Read<double>();
		break;
	case PhysicalType::POINTER:
		new_value.value_.pointer = source.Read<uint64_t>();
		break;
	case PhysicalType::INTERVAL:
		new_value.value_.interval = source.Read<interval_t>();
		break;
	case PhysicalType::VARCHAR:
		new_value.str_value = source.Read<string>();
		break;
	default: {
		Vector v(type);
		v.Deserialize(1, source);
		return v.GetValue(0);
	}
	}
	return new_value;
}

void Value::Print() {
	Printer::Print(ToString());
}

bool Value::ValuesAreEqual(const Value &result_value, const Value &value) {
	if (result_value.is_null != value.is_null) {
		return false;
	}
	if (result_value.is_null && value.is_null) {
		// NULL = NULL in checking code
		return true;
	}
	switch (value.type_.id()) {
	case LogicalTypeId::FLOAT: {
		auto other = result_value.CastAs(LogicalType::FLOAT);
		float ldecimal = value.value_.float_;
		float rdecimal = other.value_.float_;
		return ApproxEqual(ldecimal, rdecimal);
	}
	case LogicalTypeId::DOUBLE: {
		auto other = result_value.CastAs(LogicalType::DOUBLE);
		double ldecimal = value.value_.double_;
		double rdecimal = other.value_.double_;
		return ApproxEqual(ldecimal, rdecimal);
	}
	case LogicalTypeId::VARCHAR: {
		auto other = result_value.CastAs(LogicalType::VARCHAR);
		// some results might contain padding spaces, e.g. when rendering
		// VARCHAR(10) and the string only has 6 characters, they will be padded
		// with spaces to 10 in the rendering. We don't do that here yet as we
		// are looking at internal structures. So just ignore any extra spaces
		// on the right
		string left = other.str_value;
		string right = value.str_value;
		StringUtil::RTrim(left);
		StringUtil::RTrim(right);
		return left == right;
	}
	default:
		return value == result_value;
	}
}

template <>
bool Value::IsValid(float value) {
	return Value::FloatIsValid(value);
}

template <>
bool Value::IsValid(double value) {
	return Value::DoubleIsValid(value);
}

} // namespace duckdb
