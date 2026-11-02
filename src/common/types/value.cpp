#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/common/types/value.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#include "duckdb/common/uhugeint.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hugeint.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/error_manager.hpp"
#include "duckdb/common/types/bignum.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/types/string.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

#include <utility>
#include <cmath>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Extra Value Info
//===--------------------------------------------------------------------===//
enum class ExtraValueInfoType : uint8_t { INVALID_TYPE_INFO = 0, STRING_VALUE_INFO = 1, NESTED_VALUE_INFO = 2 };

struct ExtraValueInfo {
	explicit ExtraValueInfo(ExtraValueInfoType type) : type(type) {
	}
	virtual ~ExtraValueInfo() {
	}

	ExtraValueInfoType type;

public:
	bool Equals(ExtraValueInfo *other_p) const {
		if (!other_p) {
			return false;
		}
		if (type != other_p->type) {
			return false;
		}
		return EqualsInternal(other_p);
	}

	template <class T>
	T &Get() {
		if (type != T::TYPE) {
			throw InternalException("ExtraValueInfo type mismatch");
		}
		return (T &)*this;
	}

protected:
	virtual bool EqualsInternal(ExtraValueInfo *other_p) const {
		return true;
	}
};

//===--------------------------------------------------------------------===//
// String Value Info
//===--------------------------------------------------------------------===//
struct StringValueInfo : public ExtraValueInfo {
	static constexpr const ExtraValueInfoType TYPE = ExtraValueInfoType::STRING_VALUE_INFO;

public:
	explicit StringValueInfo(string str_p)
	    : ExtraValueInfo(ExtraValueInfoType::STRING_VALUE_INFO), str(std::move(str_p)) {
	}

	const string &GetString() {
		return str;
	}

protected:
	bool EqualsInternal(ExtraValueInfo *other_p) const override {
		return other_p->Get<StringValueInfo>().str == str;
	}

	string str;
};

//===--------------------------------------------------------------------===//
// Nested Value Info
//===--------------------------------------------------------------------===//
struct NestedValueInfo : public ExtraValueInfo {
	static constexpr const ExtraValueInfoType TYPE = ExtraValueInfoType::NESTED_VALUE_INFO;

public:
	NestedValueInfo() : ExtraValueInfo(ExtraValueInfoType::NESTED_VALUE_INFO) {
	}
	explicit NestedValueInfo(vector<Value> values_p)
	    : ExtraValueInfo(ExtraValueInfoType::NESTED_VALUE_INFO), values(std::move(values_p)) {
	}

	const vector<Value> &GetValues() {
		return values;
	}

protected:
	bool EqualsInternal(ExtraValueInfo *other_p) const override {
		return other_p->Get<NestedValueInfo>().values == values;
	}

	vector<Value> values;
};
//===--------------------------------------------------------------------===//
// Value
//===--------------------------------------------------------------------===//
Value::Value(LogicalType type) : type_(std::move(type)), is_null(true) {
}

Value::Value(int32_t val) : type_(LogicalType::INTEGER), is_null(false) {
	value_.integer = val;
}

Value::Value(bool val) : type_(LogicalType::BOOLEAN), is_null(false) {
	value_.boolean = val;
}

Value::Value(int64_t val) : type_(LogicalType::BIGINT), is_null(false) {
	value_.bigint = val;
}

Value::Value(float val) : type_(LogicalType::FLOAT), is_null(false) {
	value_.float_ = val;
}

Value::Value(double val) : type_(LogicalType::DOUBLE), is_null(false) {
	value_.double_ = val;
}

Value::Value(const Identifier &val) : Value(val.GetIdentifierName()) {
}

Value::Value(const char *val) : Value(val ? string(val) : string()) {
}

static void VerifyStringValue(const Value &value) {
	if (!value.IsValid()) {
		throw ErrorManager::InvalidUnicodeError(StringValue::Get(value), "value construction");
	}
}

Value::Value(std::nullptr_t val) : Value(LogicalType::VARCHAR) {
}

Value::Value(string_t val) : Value(val.GetString()) {
}

Value::Value(string val) : type_(LogicalType::VARCHAR), is_null(false) {
	value_info_ = make_shared_ptr<StringValueInfo>(std::move(val));
	VerifyStringValue(*this);
}

Value::Value(String val) : type_(LogicalType::VARCHAR), is_null(false) {
	value_info_ = make_shared_ptr<StringValueInfo>(val.ToStdString());
	VerifyStringValue(*this);
}

Value::Value(std::string_view val) : type_(LogicalType::VARCHAR), is_null(false) {
	value_info_ = make_shared_ptr<StringValueInfo>(string(val));
	VerifyStringValue(*this);
}

Value::~Value() {
}

Value::Value(const Value &other)
    : type_(other.type_), is_null(other.is_null), value_(other.value_), value_info_(other.value_info_) {
}

Value::Value(Value &&other) noexcept
    : type_(std::move(other.type_)), is_null(other.is_null), value_(other.value_),
      value_info_(std::move(other.value_info_)) {
}

Value &Value::operator=(const Value &other) {
	if (this == &other) {
		return *this;
	}
	type_ = other.type_;
	is_null = other.is_null;
	value_ = other.value_;
	value_info_ = other.value_info_;
	return *this;
}

Value &Value::operator=(Value &&other) noexcept {
	type_ = std::move(other.type_);
	is_null = other.is_null;
	value_ = other.value_;
	value_info_ = std::move(other.value_info_);
	return *this;
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
	case LogicalTypeId::SQLNULL:
		return Value::INTEGER(NumericLimits<int32_t>::Minimum());
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(NumericLimits<int64_t>::Minimum());
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(NumericLimits<hugeint_t>::Minimum());
	case LogicalTypeId::UHUGEINT:
		return Value::UHUGEINT(NumericLimits<uhugeint_t>::Minimum());
	case LogicalTypeId::UUID:
		return Value::UUID(NumericLimits<hugeint_t>::Minimum());
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(NumericLimits<uint8_t>::Minimum());
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(NumericLimits<uint16_t>::Minimum());
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(NumericLimits<uint32_t>::Minimum());
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(NumericLimits<uint64_t>::Minimum());
	case LogicalTypeId::DATE:
		return Value::DATE(Date::FromDate(Date::DATE_MIN_YEAR, Date::DATE_MIN_MONTH, Date::DATE_MIN_DAY));
	case LogicalTypeId::TIME:
		return Value::TIME(dtime_t(0));
	case LogicalTypeId::TIME_NS:
		return Value::TIME_NS(dtime_ns_t(0));
	case LogicalTypeId::TIMESTAMP: {
		const auto date = Date::FromDate(Timestamp::MIN_YEAR, Timestamp::MIN_MONTH, Timestamp::MIN_DAY);
		return Value::TIMESTAMP(date, dtime_t(0));
	}
	case LogicalTypeId::TIMESTAMP_SEC: {
		// Get the minimum timestamp and convert it to timestamp_sec_t.
		const auto min_ts = MinimumValue(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		const timestamp_sec_t ts(min_ts.value / Interval::MICROS_PER_SEC);
		return Value::TIMESTAMPSEC(ts);
	}
	case LogicalTypeId::TIMESTAMP_MS: {
		// Get the minimum timestamp and convert it to timestamp_ms_t.
		const auto min_ts = MinimumValue(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		const timestamp_ms_t ts(min_ts.value / Interval::MICROS_PER_MSEC);
		return Value::TIMESTAMPMS(ts);
	}
	case LogicalTypeId::TIMESTAMP_NS: {
		// Clear the fractional day.
		auto min_ns = NumericLimits<int64_t>::Minimum();
		min_ns /= Interval::NANOS_PER_DAY;
		min_ns *= Interval::NANOS_PER_DAY;
		return Value::TIMESTAMPNS(timestamp_ns_t(min_ns));
	}
	case LogicalTypeId::TIME_TZ:
		//	"00:00:00+1559" from the PG docs, but actually 00:00:00+15:59:59
		return Value::TIMETZ(dtime_tz_t(dtime_t(0), dtime_tz_t::MAX_OFFSET));
	case LogicalTypeId::TIMESTAMP_TZ: {
		const auto date = Date::FromDate(Timestamp::MIN_YEAR, Timestamp::MIN_MONTH, Timestamp::MIN_DAY);
		const auto ts = Timestamp::FromDatetime(date, dtime_t(0));
		return Value::TIMESTAMPTZ(timestamp_tz_t(ts));
	}
	case LogicalTypeId::TIMESTAMP_TZ_NS: {
		// Clear the fractional day.
		auto min_ns = NumericLimits<int64_t>::Minimum();
		min_ns /= Interval::NANOS_PER_DAY;
		min_ns *= Interval::NANOS_PER_DAY;
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t(min_ns));
	}
	case LogicalTypeId::INTERVAL: {
		const auto min_months = NumericLimits<int32_t>::Minimum();
		const auto min_days = NumericLimits<int32_t>::Minimum();
		const auto min_micros = NumericLimits<int64_t>::Minimum();
		return Value::INTERVAL(min_months, min_days, min_micros);
	}
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(NumericLimits<float>::Minimum());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(NumericLimits<double>::Minimum());
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return Value::DECIMAL(int16_t(-NumericHelper::POWERS_OF_TEN[width] + 1), width, scale);
		case PhysicalType::INT32:
			return Value::DECIMAL(int32_t(-NumericHelper::POWERS_OF_TEN[width] + 1), width, scale);
		case PhysicalType::INT64:
			return Value::DECIMAL(int64_t(-NumericHelper::POWERS_OF_TEN[width] + 1), width, scale);
		case PhysicalType::INT128:
			return Value::DECIMAL(-Hugeint::POWERS_OF_TEN[width] + 1, width, scale);
		default:
			throw InternalException("Unknown decimal type");
		}
	}
	case LogicalTypeId::ENUM:
		return Value::ENUM(0, type);
	case LogicalTypeId::BIGNUM:
		return Value::BIGNUM(Bignum::VarcharToBignum(
		    "-179769313486231570814527423731704356798070567525844996598917476803157260780028538760589558632766878171540"
		    "4589535143824642343213268894641827684675467035375169860499105765512820762454900903893289440758685084551339"
		    "42304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368"));
	default:
		throw InvalidTypeException(type, "MinimumValue requires numeric type");
	}
}

Value Value::MaximumValue(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		return Value::BOOLEAN(true);
	case LogicalTypeId::TINYINT:
		return Value::TINYINT(NumericLimits<int8_t>::Maximum());
	case LogicalTypeId::SMALLINT:
		return Value::SMALLINT(NumericLimits<int16_t>::Maximum());
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::SQLNULL:
		return Value::INTEGER(NumericLimits<int32_t>::Maximum());
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(NumericLimits<int64_t>::Maximum());
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(NumericLimits<hugeint_t>::Maximum());
	case LogicalTypeId::UHUGEINT:
		return Value::UHUGEINT(NumericLimits<uhugeint_t>::Maximum());
	case LogicalTypeId::UUID:
		return Value::UUID(NumericLimits<hugeint_t>::Maximum());
	case LogicalTypeId::UTINYINT:
		return Value::UTINYINT(NumericLimits<uint8_t>::Maximum());
	case LogicalTypeId::USMALLINT:
		return Value::USMALLINT(NumericLimits<uint16_t>::Maximum());
	case LogicalTypeId::UINTEGER:
		return Value::UINTEGER(NumericLimits<uint32_t>::Maximum());
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(NumericLimits<uint64_t>::Maximum());
	case LogicalTypeId::DATE:
		return Value::DATE(Date::FromDate(Date::DATE_MAX_YEAR, Date::DATE_MAX_MONTH, Date::DATE_MAX_DAY));
	case LogicalTypeId::TIME:
		//	24:00:00 according to PG
		return Value::TIME(dtime_t(Interval::MICROS_PER_DAY));
	case LogicalTypeId::TIME_NS:
		//	24:00:00 according to PG
		return Value::TIME_NS(dtime_ns_t(Interval::NANOS_PER_DAY));
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(NumericLimits<int64_t>::Maximum() - 1));
	case LogicalTypeId::TIMESTAMP_SEC: {
		// Get the maximum timestamp and convert it to timestamp_s_t.
		const auto max_ts = MaximumValue(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		const timestamp_sec_t ts(max_ts.value / Interval::MICROS_PER_SEC);
		return Value::TIMESTAMPSEC(ts);
	}
	case LogicalTypeId::TIMESTAMP_MS: {
		// Get the maximum timestamp and convert it to timestamp_ms_t.
		const auto max_ts = MaximumValue(LogicalType::TIMESTAMP).GetValue<timestamp_t>();
		const timestamp_ms_t ts(max_ts.value / Interval::MICROS_PER_MSEC);
		return Value::TIMESTAMPMS(ts);
	}
	case LogicalTypeId::TIMESTAMP_NS: {
		const auto ts = timestamp_ns_t(NumericLimits<int64_t>::Maximum() - 1);
		return Value::TIMESTAMPNS(ts);
	}
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t(NumericLimits<int64_t>::Maximum() - 1));
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t(NumericLimits<int64_t>::Maximum() - 1));
	case LogicalTypeId::TIME_TZ:
		// "24:00:00-1559" from the PG docs but actually "24:00:00-15:59:59".
		return Value::TIMETZ(dtime_tz_t(dtime_t(Interval::MICROS_PER_DAY), dtime_tz_t::MIN_OFFSET));
	case LogicalTypeId::INTERVAL: {
		const auto max_months = NumericLimits<int32_t>::Maximum();
		const auto max_days = NumericLimits<int32_t>::Maximum();
		const auto max_micros = NumericLimits<int64_t>::Maximum();
		return Value::INTERVAL(max_months, max_days, max_micros);
	}
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(NumericLimits<float>::Maximum());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(NumericLimits<double>::Maximum());
	case LogicalTypeId::DECIMAL: {
		auto width = DecimalType::GetWidth(type);
		auto scale = DecimalType::GetScale(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
			return Value::DECIMAL(int16_t(NumericHelper::POWERS_OF_TEN[width] - 1), width, scale);
		case PhysicalType::INT32:
			return Value::DECIMAL(int32_t(NumericHelper::POWERS_OF_TEN[width] - 1), width, scale);
		case PhysicalType::INT64:
			return Value::DECIMAL(int64_t(NumericHelper::POWERS_OF_TEN[width] - 1), width, scale);
		case PhysicalType::INT128:
			return Value::DECIMAL(Hugeint::POWERS_OF_TEN[width] - 1, width, scale);
		default:
			throw InternalException("Unknown decimal type");
		}
	}
	case LogicalTypeId::ENUM: {
		auto enum_size = EnumType::GetSize(type);
		return Value::ENUM(enum_size - (enum_size ? 1 : 0), type);
	}
	case LogicalTypeId::BIGNUM:
		return Value::BIGNUM(Bignum::VarcharToBignum(
		    "1797693134862315708145274237317043567980705675258449965989174768031572607800285387605895586327668781715404"
		    "5895351438246423432132688946418276846754670353751698604991057655128207624549009038932894407586850845513394"
		    "2304583236903222948165808559332123348274797826204144723168738177180919299881250404026184124858368"));
	default:
		throw InvalidTypeException(type, "MaximumValue requires numeric type");
	}
}

Value Value::Infinity(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return Value::DATE(date_t::infinity());
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t::infinity());
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TIMESTAMPSEC(timestamp_sec_t::infinity());
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TIMESTAMPMS(timestamp_ms_t::infinity());
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TIMESTAMPNS(timestamp_ns_t::infinity());
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t::infinity());
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t::infinity());
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(std::numeric_limits<float>::infinity());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(std::numeric_limits<double>::infinity());
	default:
		throw InvalidTypeException(type, "Infinity requires numeric type");
	}
}

Value Value::NegativeInfinity(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DATE:
		return Value::DATE(date_t::ninfinity());
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t::ninfinity());
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TIMESTAMPSEC(timestamp_sec_t::ninfinity());
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TIMESTAMPMS(timestamp_ms_t::ninfinity());
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TIMESTAMPNS(timestamp_ns_t::ninfinity());
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t::ninfinity());
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t::ninfinity());
	case LogicalTypeId::FLOAT:
		return Value::FLOAT(-std::numeric_limits<float>::infinity());
	case LogicalTypeId::DOUBLE:
		return Value::DOUBLE(-std::numeric_limits<double>::infinity());
	default:
		throw InvalidTypeException(type, "NegativeInfinity requires numeric type");
	}
}

Value Value::BOOLEAN(bool value) {
	Value result(LogicalType::BOOLEAN);
	result.value_.boolean = value;
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

Value Value::UHUGEINT(uhugeint_t value) {
	Value result(LogicalType::UHUGEINT);
	result.value_.uhugeint = value;
	result.is_null = false;
	return result;
}

Value Value::UUID(hugeint_t value) {
	Value result(LogicalType::UUID);
	result.value_.hugeint = value;
	result.is_null = false;
	return result;
}

Value Value::UUID(const string &value) {
	Value result(LogicalType::UUID);
	result.value_.hugeint = UUID::FromString(value);
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

bool Value::FloatIsFinite(float value) {
	return !(std::isnan(value) || std::isinf(value));
}

bool Value::DoubleIsFinite(double value) {
	return !(std::isnan(value) || std::isinf(value));
}

template <>
bool Value::IsFinite(float input) {
	return Value::FloatIsFinite(input);
}

template <>
bool Value::IsFinite(double input) {
	return Value::DoubleIsFinite(input);
}

template <>
bool Value::IsFinite(date_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_sec_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_ms_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_ns_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_tz_t input) {
	return input.IsFinite();
}

template <>
bool Value::IsFinite(timestamp_tz_ns_t input) {
	return input.IsFinite();
}

bool Value::StringIsValid(const char *str, idx_t length) {
	auto utf_type = Utf8Proc::Analyze(str, length);
	return utf_type != UnicodeType::INVALID;
}

Value Value::DECIMAL(int16_t value, uint8_t width, uint8_t scale) {
	return Value::DECIMAL(int64_t(value), width, scale);
}

Value Value::DECIMAL(int32_t value, uint8_t width, uint8_t scale) {
	return Value::DECIMAL(int64_t(value), width, scale);
}

Value Value::DECIMAL(int64_t value, uint8_t width, uint8_t scale) {
	auto decimal_type = LogicalType::DECIMAL(width, scale);
	Value result(decimal_type);
	switch (decimal_type.InternalType()) {
	case PhysicalType::INT16:
		result.value_.smallint = NumericCast<int16_t>(value);
		break;
	case PhysicalType::INT32:
		result.value_.integer = NumericCast<int32_t>(value);
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
	Value result(LogicalType::FLOAT);
	result.value_.float_ = value;
	result.is_null = false;
	return result;
}

Value Value::DOUBLE(double value) {
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

Value Value::TIME_NS(dtime_ns_t value) {
	Value result(LogicalType::TIME_NS);
	result.value_.time_ns = value;
	result.is_null = false;
	return result;
}

Value Value::TIMETZ(dtime_tz_t value) {
	Value result(LogicalType::TIME_TZ);
	result.value_.timetz = value;
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

Value Value::TIMESTAMPSEC(timestamp_sec_t timestamp) {
	Value result(LogicalType::TIMESTAMP_S);
	result.value_.timestamp_s = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMPMS(timestamp_ms_t timestamp) {
	Value result(LogicalType::TIMESTAMP_MS);
	result.value_.timestamp_ms = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMPNS(timestamp_ns_t timestamp) {
	Value result(LogicalType::TIMESTAMP_NS);
	result.value_.timestamp_ns = timestamp;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMPTZ(timestamp_tz_t value) {
	Value result(LogicalType::TIMESTAMP_TZ);
	result.value_.timestamp_tz = value;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMPTZNS(timestamp_tz_ns_t value) {
	Value result(LogicalType::TIMESTAMP_TZ_NS);
	result.value_.timestamp_tz_ns = value;
	result.is_null = false;
	return result;
}

Value Value::TIMESTAMP(date_t date, dtime_t time) {
	return Value::TIMESTAMP(Timestamp::FromDatetime(date, time));
}

Value Value::TIMESTAMP(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t min, int32_t sec,
                       int32_t micros) {
	auto date = Date::FromDate(year, month, day);
	auto time = Time::FromTime(hour, min, sec, micros);
	auto val = Value::TIMESTAMP(date, time);
	val.type_ = LogicalType::TIMESTAMP;
	return val;
}

Value Value::STRUCT(const LogicalType &type, vector<Value> struct_values) {
	Value result;
	auto child_types = StructType::GetChildTypes(type);
	for (size_t i = 0; i < struct_values.size(); i++) {
		struct_values[i] = struct_values[i].DefaultCastAs(child_types[i].second);
	}
	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(struct_values));
	result.type_ = type;
	result.is_null = false;
	return result;
}

Value Value::STRUCT(child_list_t<Value> values) {
	child_list_t<LogicalType> child_types;
	vector<Value> struct_values;
	for (auto &child : values) {
		child_types.push_back(make_pair(std::move(child.first), child.second.type()));
		struct_values.push_back(std::move(child.second));
	}
	return Value::STRUCT(LogicalType::STRUCT(child_types), std::move(struct_values));
}

Value Value::TUPLE(vector<Value> values) {
	vector<LogicalType> child_types;
	child_types.reserve(values.size());
	for (auto &child : values) {
		child_types.push_back(child.type());
	}
	return Value::STRUCT(LogicalType::TUPLE(std::move(child_types)), std::move(values));
}

Value Value::VARIANT(vector<Value> val) {
	D_ASSERT(val.size() == 4);
	D_ASSERT(val[0].type().id() == LogicalTypeId::LIST);
	D_ASSERT(val[1].type().id() == LogicalTypeId::LIST);
	D_ASSERT(val[2].type().id() == LogicalTypeId::LIST);
	D_ASSERT(val[3].type().id() == LogicalTypeId::BLOB);
	return Value::STRUCT(LogicalType::VARIANT(), std::move(val));
}

void MapKeyCheck(value_set_t &unique_keys, const Value &key) {
	// NULL key check.
	if (key.IsNull()) {
		MapVector::EvalMapInvalidReason(MapInvalidReason::NULL_KEY);
	}

	// Duplicate key check.
	if (unique_keys.find(key) != unique_keys.end()) {
		MapVector::EvalMapInvalidReason(MapInvalidReason::DUPLICATE_KEY);
	}
	unique_keys.insert(key);
}

Value Value::MAP(const LogicalType &child_type, vector<Value> values) { // NOLINT
	vector<Value> map_keys;
	vector<Value> map_values;
	value_set_t unique_keys;

	for (auto &val : values) {
		D_ASSERT(val.type().InternalType() == PhysicalType::STRUCT);
		auto &children = StructValue::GetChildren(val);
		D_ASSERT(children.size() == 2);

		auto &key = children[0];
		MapKeyCheck(unique_keys, key);

		map_keys.push_back(key);
		map_values.push_back(children[1]);
	}

	auto &key_type = StructType::GetChildType(child_type, 0);
	auto &value_type = StructType::GetChildType(child_type, 1);
	return Value::MAP(key_type, value_type, std::move(map_keys), std::move(map_values));
}

Value Value::MAP(const LogicalType &key_type, const LogicalType &value_type, vector<Value> keys, vector<Value> values) {
	D_ASSERT(keys.size() == values.size());
	Value result;

	result.type_ = LogicalType::MAP(key_type, value_type);
	result.is_null = false;
	value_set_t unique_keys;

	for (idx_t i = 0; i < keys.size(); i++) {
		child_list_t<LogicalType> struct_types;
		vector<Value> new_children;
		struct_types.reserve(2);
		new_children.reserve(2);

		struct_types.emplace_back(make_pair("key", key_type));
		struct_types.emplace_back(make_pair("value", value_type));

		auto key = keys[i].DefaultCastAs(key_type);
		MapKeyCheck(unique_keys, key);

		new_children.push_back(key);
		new_children.push_back(values[i]);
		auto struct_type = LogicalType::STRUCT(std::move(struct_types));
		values[i] = Value::STRUCT(struct_type, std::move(new_children));
	}

	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(values));
	return result;
}

Value Value::MAP(const InsertionOrderPreservingMap<string> &kv_pairs) {
	Value result;
	result.type_ = LogicalType::MAP(LogicalType::VARCHAR, LogicalType::VARCHAR);
	result.is_null = false;
	vector<Value> pairs;
	for (auto &kv : kv_pairs) {
		pairs.push_back(Value::STRUCT({{"key", Value(kv.first)}, {"value", Value(kv.second)}}));
	}
	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(pairs));
	return result;
}

Value Value::UNION(child_list_t<LogicalType> members, uint8_t tag, Value value) {
	D_ASSERT(!members.empty());
	D_ASSERT(members.size() <= UnionType::MAX_UNION_MEMBERS);
	D_ASSERT(members.size() > tag);

	D_ASSERT(value.type() == members[tag].second);

	Value result;
	result.is_null = false;
	// add the tag to the front of the struct
	vector<Value> union_values;
	union_values.emplace_back(Value::UTINYINT(tag));
	for (idx_t i = 0; i < members.size(); i++) {
		if (i != tag) {
			union_values.emplace_back(members[i].second);
		} else {
			union_values.emplace_back(nullptr);
		}
	}
	union_values[tag + 1] = std::move(value);
	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(union_values));
	result.type_ = LogicalType::UNION(std::move(members));
	return result;
}

Value Value::LIST(const LogicalType &child_type, vector<Value> values) {
	Value result;
	result.type_ = LogicalType::LIST(child_type);
	result.is_null = false;
	for (auto &val : values) {
		val = val.DefaultCastAs(child_type);
	}
	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(values));
	return result;
}

Value Value::LIST(vector<Value> values) {
	if (values.empty()) {
		throw InternalException(
		    "Value::LIST(values) cannot be used to make an empty list - use Value::LIST(type, values) instead");
	}
	auto &type = values[0].type();
	return Value::LIST(type, std::move(values));
}

Value Value::ARRAY(const LogicalType &child_type, vector<Value> values) {
	Value result;
	result.type_ = LogicalType::ARRAY(child_type, values.size());
	for (auto &val : values) {
		val = val.DefaultCastAs(child_type);
	}
	result.value_info_ = make_shared_ptr<NestedValueInfo>(std::move(values));
	result.is_null = false;
	return result;
}

Value Value::BLOB(const_data_ptr_t data, idx_t len) {
	Value result(LogicalType::BLOB);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(const_char_ptr_cast(data), len));
	return result;
}

Value Value::BIGNUM(const_data_ptr_t data, idx_t len) {
	return BIGNUM(string(const_char_ptr_cast(data), len));
}

Value Value::BIGNUM(const string &data) {
	Value result(LogicalType::BIGNUM);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(data);
	return result;
}

Value Value::GEOMETRY(const_data_ptr_t data, idx_t len, const CoordinateReferenceSystem &crs) {
	Value result;
	result.type_ = LogicalType::GEOMETRY(crs); // construct type explicitly so that we get the LogicalTypeInfo
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(const_char_ptr_cast(data), len));
	return result;
}

Value Value::GEOMETRY(const_data_ptr_t data, idx_t len) {
	Value result;
	result.type_ = LogicalType::GEOMETRY(); // construct type explicitly so that we get the LogicalTypeInfo
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(const_char_ptr_cast(data), len));
	return result;
}

Value Value::TYPE(const LogicalType &type) {
	MemoryStream stream;
	SerializationOptions options;
	options.storage_compatibility = StorageCompatibility::Latest();
	BinarySerializer::Serialize(type, stream, options);
	auto data_ptr = const_char_ptr_cast(stream.GetData());
	auto data_len = stream.GetPosition();

	Value result(LogicalType::TYPE());
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(data_ptr, data_len));
	return result;
}

Value Value::TYPE(const string_t &serialized_type) {
	Value result(LogicalType::TYPE());
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(serialized_type.GetString());
	return result;
}

Value Value::BLOB(const string &data) {
	Value result(LogicalType::BLOB);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(Blob::ToBlob(string_t(data)));
	return result;
}

Value Value::LEGACY_AGGREGATE_STATE(const LogicalType &type, const_data_ptr_t data, idx_t len) { // NOLINT
	Value result(type);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(const_char_ptr_cast(data), len));
	return result;
}

Value Value::BIT(const_data_ptr_t data, idx_t len) {
	Value result(LogicalType::BIT);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(string(const_char_ptr_cast(data), len));
	return result;
}

Value Value::BIT(const string &data) {
	Value result(LogicalType::BIT);
	result.is_null = false;
	result.value_info_ = make_shared_ptr<StringValueInfo>(Bit::ToBit(string_t(data)));
	return result;
}

Value Value::ENUM(uint64_t value, const LogicalType &original_type) {
	D_ASSERT(original_type.id() == LogicalTypeId::ENUM);
	Value result(original_type);
	switch (original_type.InternalType()) {
	case PhysicalType::UINT8:
		result.value_.utinyint = NumericCast<uint8_t>(value);
		break;
	case PhysicalType::UINT16:
		result.value_.usmallint = NumericCast<uint16_t>(value);
		break;
	case PhysicalType::UINT32:
		result.value_.uinteger = NumericCast<uint32_t>(value);
		break;
	default:
		throw InternalException("Incorrect Physical Type for ENUM");
	}
	result.is_null = false;
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
Value Value::CreateValue(uhugeint_t value) {
	return Value::UHUGEINT(value);
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
Value Value::CreateValue(dtime_ns_t value) {
	return Value::TIME_NS(value);
}

template <>
Value Value::CreateValue(dtime_tz_t value) {
	return Value::TIMETZ(value);
}

template <>
Value Value::CreateValue(timestamp_t value) {
	return Value::TIMESTAMP(value);
}

template <>
Value Value::CreateValue(timestamp_sec_t value) {
	return Value::TIMESTAMPSEC(value);
}

template <>
Value Value::CreateValue(timestamp_ms_t value) {
	return Value::TIMESTAMPMS(value);
}

template <>
Value Value::CreateValue(timestamp_ns_t value) {
	return Value::TIMESTAMPNS(value);
}

template <>
Value Value::CreateValue(timestamp_tz_t value) {
	return Value::TIMESTAMPTZ(value);
}

template <>
Value Value::CreateValue(timestamp_tz_ns_t value) {
	return Value::TIMESTAMPTZNS(value);
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
	if (IsNull()) {
		InternalException::Throw("Calling GetValueInternal on a value that is NULL");
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
	case LogicalTypeId::UUID:
		return Cast::Operation<hugeint_t, T>(value_.hugeint);
	case LogicalTypeId::UHUGEINT:
		return Cast::Operation<uhugeint_t, T>(value_.uhugeint);
	case LogicalTypeId::DATE:
		return Cast::Operation<date_t, T>(value_.date);
	case LogicalTypeId::TIME:
		return Cast::Operation<dtime_t, T>(value_.time);
	case LogicalTypeId::TIME_NS:
		return Cast::Operation<dtime_ns_t, T>(value_.time_ns);
	case LogicalTypeId::TIME_TZ:
		return Cast::Operation<dtime_tz_t, T>(value_.timetz);
	case LogicalTypeId::TIMESTAMP:
		return Cast::Operation<timestamp_t, T>(value_.timestamp);
	case LogicalTypeId::TIMESTAMP_SEC:
		return Cast::Operation<timestamp_sec_t, T>(value_.timestamp_s);
	case LogicalTypeId::TIMESTAMP_MS:
		return Cast::Operation<timestamp_ms_t, T>(value_.timestamp_ms);
	case LogicalTypeId::TIMESTAMP_NS:
		return Cast::Operation<timestamp_ns_t, T>(value_.timestamp_ns);
	case LogicalTypeId::TIMESTAMP_TZ:
		return Cast::Operation<timestamp_tz_t, T>(value_.timestamp_tz);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Cast::Operation<timestamp_tz_ns_t, T>(value_.timestamp_tz_ns);
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
		return Cast::Operation<string_t, T>(StringValue::Get(*this).c_str());
	case LogicalTypeId::INTERVAL:
		return Cast::Operation<interval_t, T>(value_.interval);
	case LogicalTypeId::DECIMAL:
		return DefaultCastAs(LogicalType::DOUBLE).GetValueInternal<T>();
	case LogicalTypeId::ENUM: {
		switch (type_.InternalType()) {
		case PhysicalType::UINT8:
			return Cast::Operation<uint8_t, T>(value_.utinyint);
		case PhysicalType::UINT16:
			return Cast::Operation<uint16_t, T>(value_.usmallint);
		case PhysicalType::UINT32:
			return Cast::Operation<uint32_t, T>(value_.uinteger);
		default:
			InternalException::Throw("Invalid Internal Type for ENUMs");
		}
	}
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
	if (IsNull()) {
		throw InternalException("Calling GetValue on a value that is NULL");
	}
	switch (type_.id()) {
	case LogicalTypeId::TIMESTAMP:
		return value_.timestamp.value;
	case LogicalTypeId::TIMESTAMP_SEC:
		return value_.timestamp_s.value;
	case LogicalTypeId::TIMESTAMP_MS:
		return value_.timestamp_ms.value;
	case LogicalTypeId::TIMESTAMP_NS:
		return value_.timestamp_ns.value;
	case LogicalTypeId::TIMESTAMP_TZ:
		return value_.timestamp_tz.value;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return value_.timestamp_tz_ns.value;
	case LogicalTypeId::TIME:
		return value_.bigint;
	default:
		return GetValueInternal<int64_t>();
	}
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
uhugeint_t Value::GetValue() const {
	return GetValueInternal<uhugeint_t>();
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
dtime_ns_t Value::GetValue() const {
	return GetValueInternal<dtime_ns_t>();
}

template <>
timestamp_t Value::GetValue() const {
	return GetValueInternal<timestamp_t>();
}

template <>
timestamp_sec_t Value::GetValue() const {
	return GetValueInternal<timestamp_sec_t>();
}

template <>
timestamp_ms_t Value::GetValue() const {
	return GetValueInternal<timestamp_ms_t>();
}

template <>
timestamp_ns_t Value::GetValue() const {
	return GetValueInternal<timestamp_ns_t>();
}

template <>
timestamp_tz_t Value::GetValue() const {
	return GetValueInternal<timestamp_tz_t>();
}

template <>
timestamp_tz_ns_t Value::GetValue() const {
	return GetValueInternal<timestamp_tz_ns_t>();
}

template <>
dtime_tz_t Value::GetValue() const {
	return GetValueInternal<dtime_tz_t>();
}

template <>
DUCKDB_API interval_t Value::GetValue() const {
	return GetValueInternal<interval_t>();
}

template <>
DUCKDB_API Value Value::GetValue() const {
	return Value(*this);
}

template <>
DUCKDB_API Identifier Value::GetValue() const {
	return Identifier(GetValue<string>());
}

uintptr_t Value::GetPointer() const {
	D_ASSERT(type() == LogicalType::POINTER);
	return value_.pointer;
}

Value Value::Numeric(const LogicalType &type, int64_t value) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
		D_ASSERT(value == 0 || value == 1);
		return Value::BOOLEAN(value ? true : false);
	case LogicalTypeId::TINYINT:
		D_ASSERT(value >= NumericLimits<int8_t>::Minimum() && value <= NumericLimits<int8_t>::Maximum());
		return Value::TINYINT((int8_t)value);
	case LogicalTypeId::SMALLINT:
		D_ASSERT(value >= NumericLimits<int16_t>::Minimum() && value <= NumericLimits<int16_t>::Maximum());
		return Value::SMALLINT((int16_t)value);
	case LogicalTypeId::INTEGER:
		D_ASSERT(value >= NumericLimits<int32_t>::Minimum() && value <= NumericLimits<int32_t>::Maximum());
		return Value::INTEGER((int32_t)value);
	case LogicalTypeId::BIGINT:
		return Value::BIGINT(value);
	case LogicalTypeId::UTINYINT:
		D_ASSERT(value >= NumericLimits<uint8_t>::Minimum() && value <= NumericLimits<uint8_t>::Maximum());
		return Value::UTINYINT((uint8_t)value);
	case LogicalTypeId::USMALLINT:
		D_ASSERT(value >= NumericLimits<uint16_t>::Minimum() && value <= NumericLimits<uint16_t>::Maximum());
		return Value::USMALLINT((uint16_t)value);
	case LogicalTypeId::UINTEGER:
		D_ASSERT(value >= NumericLimits<uint32_t>::Minimum() && value <= NumericLimits<uint32_t>::Maximum());
		return Value::UINTEGER((uint32_t)value);
	case LogicalTypeId::UBIGINT:
		D_ASSERT(value >= 0);
		return Value::UBIGINT(NumericCast<uint64_t>(value));
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(value);
	case LogicalTypeId::UHUGEINT:
		return Value::UHUGEINT(NumericCast<uint64_t>(value));
	case LogicalTypeId::DECIMAL:
		return Value::DECIMAL(value, DecimalType::GetWidth(type), DecimalType::GetScale(type));
	case LogicalTypeId::FLOAT:
		return Value((float)value);
	case LogicalTypeId::DOUBLE:
		return Value((double)value);
	case LogicalTypeId::POINTER:
		return Value::POINTER(NumericCast<uintptr_t>(value));
	case LogicalTypeId::DATE:
		D_ASSERT(value >= NumericLimits<int32_t>::Minimum() && value <= NumericLimits<int32_t>::Maximum());
		return Value::DATE(date_t(NumericCast<int32_t>(value)));
	case LogicalTypeId::TIME:
		return Value::TIME(dtime_t(value));
	case LogicalTypeId::TIME_NS:
		return Value::TIME_NS(dtime_ns_t(value));
	case LogicalTypeId::TIMESTAMP:
		return Value::TIMESTAMP(timestamp_t(value));
	case LogicalTypeId::TIMESTAMP_SEC:
		return Value::TIMESTAMPSEC(timestamp_sec_t(value));
	case LogicalTypeId::TIMESTAMP_MS:
		return Value::TIMESTAMPMS(timestamp_ms_t(value));
	case LogicalTypeId::TIMESTAMP_NS:
		return Value::TIMESTAMPNS(timestamp_ns_t(value));
	case LogicalTypeId::TIMESTAMP_TZ:
		return Value::TIMESTAMPTZ(timestamp_tz_t(value));
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return Value::TIMESTAMPTZNS(timestamp_tz_ns_t(value));
	case LogicalTypeId::ENUM:
		return Value::ENUM(NumericCast<uint64_t>(value), type);
	default:
		throw InvalidTypeException(type, "Numeric requires numeric type");
	}
}

Value Value::Numeric(const LogicalType &type, hugeint_t value) {
#ifdef DEBUG
	// perform a throwing cast to verify that the type fits
	Value::HUGEINT(value).DefaultCastAs(type);
#endif
	switch (type.id()) {
	case LogicalTypeId::HUGEINT:
		return Value::HUGEINT(value);
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(Hugeint::Cast<uint64_t>(value));
	default:
		return Value::Numeric(type, Hugeint::Cast<int64_t>(value));
	}
}

Value Value::Numeric(const LogicalType &type, uhugeint_t value) {
#ifdef DEBUG
	// perform a throwing cast to verify that the type fits
	Value::UHUGEINT(value).DefaultCastAs(type);
#endif
	switch (type.id()) {
	case LogicalTypeId::UHUGEINT:
		return Value::UHUGEINT(value);
	case LogicalTypeId::UBIGINT:
		return Value::UBIGINT(Uhugeint::Cast<uint64_t>(value));
	default:
		return Value::Numeric(type, Uhugeint::Cast<int64_t>(value));
	}
}

//===--------------------------------------------------------------------===//
// GetValueUnsafe
//===--------------------------------------------------------------------===//
template <>
DUCKDB_API bool Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::BOOL);
	return value_.boolean;
}

template <>
int8_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT8 || type_.InternalType() == PhysicalType::BOOL);
	return value_.tinyint;
}

template <>
int16_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT16);
	return value_.smallint;
}

template <>
int32_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT32);
	return value_.integer;
}

template <>
int64_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.bigint;
}

template <>
hugeint_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT128);
	return value_.hugeint;
}

template <>
uint8_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT8);
	return value_.utinyint;
}

template <>
uint16_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT16);
	return value_.usmallint;
}

template <>
uint32_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT32);
	return value_.uinteger;
}

template <>
uint64_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT64);
	return value_.ubigint;
}

template <>
uhugeint_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::UINT128);
	return value_.uhugeint;
}

template <>
string Value::GetValueUnsafe() const {
	return StringValue::Get(*this);
}

template <>
DUCKDB_API string_t Value::GetValueUnsafe() const {
	return string_t(StringValue::Get(*this));
}

template <>
DUCKDB_API bignum_t Value::GetValueUnsafe() const {
	return bignum_t(StringValue::Get(*this));
}

template <>
float Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::FLOAT);
	return value_.float_;
}

template <>
double Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::DOUBLE);
	return value_.double_;
}

template <>
date_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT32);
	return value_.date;
}

template <>
dtime_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.time;
}

template <>
dtime_ns_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.time_ns;
}

template <>
dtime_tz_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timetz;
}

template <>
timestamp_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp;
}

template <>
timestamp_sec_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp_s;
}

template <>
timestamp_ms_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp_ms;
}

template <>
timestamp_ns_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp_ns;
}

template <>
timestamp_tz_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp_tz;
}

template <>
timestamp_tz_ns_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INT64);
	return value_.timestamp_tz_ns;
}

template <>
interval_t Value::GetValueUnsafe() const {
	D_ASSERT(type_.InternalType() == PhysicalType::INTERVAL);
	return value_.interval;
}

//===--------------------------------------------------------------------===//
// GetPointerToData
//===--------------------------------------------------------------------===//
const_data_ptr_t Value::GetPointerToData() const {
	D_ASSERT(TypeIsConstantSize(type_.InternalType()));
	return const_data_ptr_cast(&value_);
}

//===--------------------------------------------------------------------===//
// Hash
//===--------------------------------------------------------------------===//
hash_t Value::Hash() const {
	if (IsNull()) {
		return 0;
	}
	Vector input(*this, count_t(1));
	Vector result(LogicalType::HASH, 1);
	VectorOperations::Hash(input, result);

	if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		return *ConstantVector::GetData<hash_t>(result);
	}
	return FlatVector::GetData<hash_t>(result)[0];
}

string Value::ToString() const {
	if (IsNull()) {
		return "NULL";
	}
	return StringValue::Get(DefaultCastAs(LogicalType::VARCHAR));
}

string Value::ToSQLString() const {
	if (IsNull()) {
		auto res = ToString();
		if (type_.id() != LogicalTypeId::SQLNULL) {
			res += "::" + type_.ToString();
		}
		return res;
	}
	switch (type_.id()) {
	case LogicalTypeId::UUID:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::GEOMETRY:
		return "'" + ToString() + "'::" + type_.ToString();
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM: {
		auto str_val = ToString();
		if (str_val.size() == 1 && str_val[0] == '\0') {
			return "chr(0)";
		}
		return "'" + StringUtil::Replace(ToString(), "'", "''") + "'";
	}
	case LogicalTypeId::VARIANT: {
		Vector tmp(*this, count_t(1));
		RecursiveUnifiedVectorFormat format;
		Vector::RecursiveToUnifiedFormat(tmp, format);
		UnifiedVariantVectorData vector_data(format);
		auto val = VariantUtils::ConvertVariantToValue(vector_data, 0, 0);
		if (val.type().id() == LogicalTypeId::STRUCT) {
			child_list_t<Value> children;
			auto &values = StructValue::GetChildren(val);
			for (idx_t i = 0; i < values.size(); i++) {
				children.emplace_back(StructType::GetChildName(val.type(), i),
				                      values[i].DefaultCastAs(LogicalType::VARIANT()));
			}
			val = Value::STRUCT(std::move(children));
			return "CAST(" + val.ToSQLString() + " AS VARIANT)";
		}
		if (val.type().id() == LogicalTypeId::LIST) {
			val = Value::LIST(LogicalType::VARIANT(), ListValue::GetChildren(val));
			return "CAST(" + val.ToSQLString() + " AS VARIANT)";
		}
		// Preserve the payload's type as well as its value (e.g. SMALLINT versus INTEGER).
		return "CAST(CAST(" + val.ToSQLString() + " AS " + val.type().ToString() + ") AS VARIANT)";
	}
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
		return NestedToSQLString(*this, [](const Value &child) { return child.ToSQLString(); });
	case LogicalTypeId::FLOAT:
		if (!FloatIsFinite(FloatValue::Get(*this)) ||
		    (FloatValue::Get(*this) == 0 && std::signbit(FloatValue::Get(*this)))) {
			return "'" + ToString() + "'::" + type_.ToString();
		}
		return ToString();
	case LogicalTypeId::DOUBLE: {
		double val = DoubleValue::Get(*this);
		// A numeric -0.0 literal is parsed as DECIMAL, losing the floating-point sign.
		if (val == 0 && std::signbit(val)) {
			return "'" + ToString() + "'::" + type_.ToString();
		}
		if (!DoubleIsFinite(val)) {
			if (!Value::IsNan(val)) {
				// to infinity and beyond
				return val < 0 ? "-1e1000" : "1e1000";
			}
			return "'" + ToString() + "'::" + type_.ToString();
		}
		return ToString();
	}
	default:
		return ToString();
	}
}

string Value::NestedToSQLString(const Value &value, const std::function<string(const Value &)> &child_to_sql) {
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::STRUCT: {
		bool is_unnamed = type.id() == LogicalTypeId::TUPLE;
		string ret = is_unnamed ? "(" : "{";
		auto &child_types = StructType::GetChildTypes(type);
		auto &struct_values = StructValue::GetChildren(value);
		for (idx_t i = 0; i < struct_values.size(); i++) {
			auto &name = child_types[i].first;
			auto &child = struct_values[i];
			if (is_unnamed) {
				ret += child_to_sql(child);
			} else {
				ret += "'" + StringUtil::Replace(name.GetIdentifierName(), "'", "''") + "': " + child_to_sql(child);
			}
			if (i < struct_values.size() - 1) {
				ret += ", ";
			}
		}
		// a single-element tuple needs a trailing comma to round-trip: (1,) - otherwise (1) is just grouping
		if (is_unnamed && struct_values.size() == 1) {
			ret += ",";
		}
		ret += is_unnamed ? ")" : "}";
		return ret;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY: {
		auto &children =
		    type.id() == LogicalTypeId::LIST ? ListValue::GetChildren(value) : ArrayValue::GetChildren(value);
		return "[" + StringUtil::Join(children, children.size(), ", ", child_to_sql) + "]";
	}
	case LogicalTypeId::MAP: {
		// A bare `MAP {...}` literal infers its element types from the entries
		// (and `MAP {}` infers MAP(INTEGER, INTEGER)), so it does not faithfully
		// round-trip on its own. Append an explicit cast to the real type
		auto &entries = MapValue::GetChildren(value);
		string ret = "MAP {";
		for (idx_t i = 0; i < entries.size(); i++) {
			auto &kv = StructValue::GetChildren(entries[i]);
			if (i > 0) {
				ret += ", ";
			}
			ret += child_to_sql(kv[0]);
			ret += ": ";
			ret += child_to_sql(kv[1]);
		}
		ret += "}::" + type.ToString();
		return ret;
	}
	case LogicalTypeId::UNION: {
		string ret = "union_value(";
		auto union_tag = UnionValue::GetTag(value);
		auto &tag_name = UnionType::GetMemberName(type, union_tag);
		ret += SQLIdentifier(tag_name) + " := ";
		ret += child_to_sql(UnionValue::GetValue(value));
		ret += ")::" + type.ToString();
		return ret;
	}
	default:
		throw InternalException("Value::NestedToSQLString called on non-nested type %s", type.ToString());
	}
}

//===--------------------------------------------------------------------===//
// Type-specific getters
//===--------------------------------------------------------------------===//
bool BooleanValue::Get(const Value &value) {
	return value.GetValueUnsafe<bool>();
}

int8_t TinyIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<int8_t>();
}

int16_t SmallIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<int16_t>();
}

int32_t IntegerValue::Get(const Value &value) {
	return value.GetValueUnsafe<int32_t>();
}

int64_t BigIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<int64_t>();
}

hugeint_t HugeIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<hugeint_t>();
}

uint8_t UTinyIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<uint8_t>();
}

uint16_t USmallIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<uint16_t>();
}

uint32_t UIntegerValue::Get(const Value &value) {
	return value.GetValueUnsafe<uint32_t>();
}

uint64_t UBigIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<uint64_t>();
}

uhugeint_t UhugeIntValue::Get(const Value &value) {
	return value.GetValueUnsafe<uhugeint_t>();
}

float FloatValue::Get(const Value &value) {
	return value.GetValueUnsafe<float>();
}

double DoubleValue::Get(const Value &value) {
	return value.GetValueUnsafe<double>();
}

const string &StringValue::Get(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling StringValue::Get on a NULL value");
	}
	D_ASSERT(value.type().InternalType() == PhysicalType::VARCHAR);
	D_ASSERT(value.value_info_);
	return value.value_info_->Get<StringValueInfo>().GetString();
}

LogicalType TypeValue::GetType(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling TypeValue::GetType on a NULL value");
	}
	D_ASSERT(value.type().id() == LogicalTypeId::TYPE);
	D_ASSERT(value.value_info_);
	auto &type_str = value.value_info_->Get<StringValueInfo>().GetString();
	auto str = string_t(type_str);
	MemoryStream stream(data_ptr_cast(str.GetDataWriteable()), str.GetSize());
	BinaryDeserializer deserializer(stream);
	return LogicalType::Deserialize(deserializer);
}

date_t DateValue::Get(const Value &value) {
	return value.GetValueUnsafe<date_t>();
}

dtime_t TimeValue::Get(const Value &value) {
	return value.GetValueUnsafe<dtime_t>();
}

timestamp_t TimestampValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_t>();
}

timestamp_sec_t TimestampSValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_sec_t>();
}

timestamp_ms_t TimestampMSValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_ms_t>();
}

timestamp_ns_t TimestampNSValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_ns_t>();
}

timestamp_tz_t TimestampTZValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_tz_t>();
}

timestamp_tz_ns_t TimestampTZNSValue::Get(const Value &value) {
	return value.GetValueUnsafe<timestamp_tz_ns_t>();
}

interval_t IntervalValue::Get(const Value &value) {
	return value.GetValueUnsafe<interval_t>();
}

const vector<Value> &StructValue::GetChildren(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling StructValue::GetChildren on a NULL value");
	}
	D_ASSERT(value.type().InternalType() == PhysicalType::STRUCT);
	D_ASSERT(value.value_info_);
	return value.value_info_->Get<NestedValueInfo>().GetValues();
}

const vector<Value> &MapValue::GetChildren(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling MapValue::GetChildren on a NULL value");
	}
	D_ASSERT(value.type().id() == LogicalTypeId::MAP);
	D_ASSERT(value.type().InternalType() == PhysicalType::LIST);
	D_ASSERT(value.value_info_);
	return value.value_info_->Get<NestedValueInfo>().GetValues();
}

const vector<Value> &ListValue::GetChildren(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling ListValue::GetChildren on a NULL value");
	}
	D_ASSERT(value.type().InternalType() == PhysicalType::LIST);
	D_ASSERT(value.value_info_);
	return value.value_info_->Get<NestedValueInfo>().GetValues();
}

const vector<Value> &ArrayValue::GetChildren(const Value &value) {
	if (value.is_null) {
		throw InternalException("Calling ArrayValue::GetChildren on a NULL value");
	}
	D_ASSERT(value.type().InternalType() == PhysicalType::ARRAY);
	D_ASSERT(value.value_info_);
	return value.value_info_->Get<NestedValueInfo>().GetValues();
}

const Value &UnionValue::GetValue(const Value &value) {
	D_ASSERT(value.type().id() == LogicalTypeId::UNION);
	auto &children = StructValue::GetChildren(value);
	auto tag = children[0].GetValueUnsafe<union_tag_t>();
	D_ASSERT(tag < children.size() - 1);
	return children[tag + 1];
}

union_tag_t UnionValue::GetTag(const Value &value) {
	D_ASSERT(value.type().id() == LogicalTypeId::UNION);
	auto children = StructValue::GetChildren(value);
	auto tag = children[0].GetValueUnsafe<union_tag_t>();
	D_ASSERT(tag < children.size() - 1);
	return tag;
}

const LogicalType &UnionValue::GetType(const Value &value) {
	return UnionType::GetMemberType(value.type(), UnionValue::GetTag(value));
}

Value VariantValue::GetValue(const Value &variant_val) {
	D_ASSERT(variant_val.type().id() == LogicalTypeId::VARIANT && !variant_val.IsNull());
	Vector tmp(variant_val, count_t(1));
	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(tmp, format);
	UnifiedVariantVectorData vector_data(format);
	return VariantUtils::ConvertVariantToValue(vector_data, 0, 0);
}

hugeint_t IntegralValue::Get(const Value &value) {
	switch (value.type().InternalType()) {
	case PhysicalType::INT8:
		return TinyIntValue::Get(value);
	case PhysicalType::INT16:
		return SmallIntValue::Get(value);
	case PhysicalType::INT32:
		return IntegerValue::Get(value);
	case PhysicalType::INT64:
		return BigIntValue::Get(value);
	case PhysicalType::INT128:
		return HugeIntValue::Get(value);
	case PhysicalType::UINT8:
		return UTinyIntValue::Get(value);
	case PhysicalType::UINT16:
		return USmallIntValue::Get(value);
	case PhysicalType::UINT32:
		return UIntegerValue::Get(value);
	case PhysicalType::UINT64:
		return NumericCast<int64_t>(UBigIntValue::Get(value));
	case PhysicalType::UINT128:
		return static_cast<hugeint_t>(UhugeIntValue::Get(value));
	default:
		throw InternalException("Invalid internal type \"%s\" for IntegralValue::Get", value.type().ToString());
	}
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

optional<Value> Value::TryCastAs(CastFunctionSet &set, GetCastFunctionInput &get_input, const LogicalType &target_type,
                                 string *error_message, bool strict) const {
	if (type_ == target_type) {
		return Copy();
	}
	Vector input(*this, count_t(1));
	Vector result(target_type);
	// always pass an error string - VectorOperations::TryCast throws instead of returning false without one
	string local_error;
	if (!VectorOperations::TryCast(set, get_input, input, result, 1, error_message ? error_message : &local_error,
	                               strict)) {
		return nullopt;
	}
	return result.GetValue(0);
}

optional<Value> Value::TryCastAs(ClientContext &context, const LogicalType &target_type, string *error_message,
                                 bool strict) const {
	GetCastFunctionInput get_input(context);
	return TryCastAs(CastFunctionSet::Get(context), get_input, target_type, error_message, strict);
}

optional<Value> Value::DefaultTryCastAs(const LogicalType &target_type, string *error_message, bool strict) const {
	CastFunctionSet set;
	GetCastFunctionInput get_input;
	return TryCastAs(set, get_input, target_type, error_message, strict);
}

Value Value::CastAs(CastFunctionSet &set, GetCastFunctionInput &get_input, const LogicalType &target_type,
                    bool strict) const {
	if (target_type.id() == LogicalTypeId::ANY) {
		return *this;
	}
	string error_message;
	auto new_value = TryCastAs(set, get_input, target_type, &error_message, strict);
	if (!new_value) {
		throw InvalidInputException("Failed to cast value: %s", error_message);
	}
	return std::move(*new_value);
}

Value Value::CastAs(ClientContext &context, const LogicalType &target_type, bool strict) const {
	GetCastFunctionInput get_input(context);
	return CastAs(CastFunctionSet::Get(context), get_input, target_type, strict);
}

Value Value::DefaultCastAs(const LogicalType &target_type, bool strict) const {
	CastFunctionSet set;
	GetCastFunctionInput get_input;
	return CastAs(set, get_input, target_type, strict);
}

Value Value::WithType(LogicalType new_type) const {
	Value result = *this;
	result.type_ = std::move(new_type);
	return result;
}

static const LogicalType &GetChildType(const LogicalType &parent_type, idx_t i) {
	switch (parent_type.InternalType()) {
	case PhysicalType::LIST:
		return ListType::GetChildType(parent_type);
	case PhysicalType::STRUCT:
		return StructType::GetChildType(parent_type, i);
	case PhysicalType::ARRAY:
		return ArrayType::GetChildType(parent_type);
	default:
		throw InternalException("Parent type is not a nested type");
	}
}

static bool SerializeTypeMatches(const LogicalType &expected_type, const LogicalType &actual_type) {
	if (expected_type.id() != actual_type.id()) {
		// type id needs to be the same
		return false;
	}
	if (expected_type.IsNested()) {
		// for nested types that is enough - we will recurse into the children and check there again anyway
		return true;
	}
	// otherwise we do a deep comparison of the type (e.g. decimal flags need to be consistent)
	return expected_type == actual_type;
}

void Value::SerializeChildren(Serializer &serializer, const vector<Value> &children, const LogicalType &parent_type) {
	serializer.WriteObject(102, "value", [&](Serializer &child_serializer) {
		child_serializer.WriteList(100, "children", children.size(), [&](Serializer::List &list, idx_t i) {
			auto &value_type = GetChildType(parent_type, i);
			bool serialize_type = value_type.InternalType() == PhysicalType::INVALID;
			if (!serialize_type && !SerializeTypeMatches(value_type, children[i].type())) {
				throw InternalException("Error when serializing type - serializing a child of a nested value with type "
				                        "%s, but expected type %s",
				                        children[i].type(), value_type);
			}
			list.WriteObject([&](Serializer &element_serializer) {
				children[i].SerializeInternal(element_serializer, serialize_type);
			});
		});
	});
}

void Value::SerializeInternal(Serializer &serializer, bool serialize_type) const {
	if (serialize_type || !serializer.ShouldSerialize(StorageVersion::V1_2_0)) {
		// only the root value needs to serialize its type
		// for forwards compatibility reasons, we also serialize the type always when targeting versions < v1.2.0
		serializer.WriteProperty(100, "type", type_);
	}
	serializer.WriteProperty(101, "is_null", is_null);
	if (IsNull()) {
		return;
	}

	if (type_.id() == LogicalTypeId::TYPE) {
		// special case for TYPE values: serialize the type as a nested object
		auto type_value = TypeValue::GetType(*this);
		serializer.WriteProperty(102, "value", type_value);
		return;
	}

	switch (type_.InternalType()) {
	case PhysicalType::BIT:
		throw InternalException("BIT type should not be serialized");
	case PhysicalType::BOOL:
		serializer.WriteProperty(102, "value", value_.boolean);
		break;
	case PhysicalType::INT8:
		serializer.WriteProperty(102, "value", value_.tinyint);
		break;
	case PhysicalType::INT16:
		serializer.WriteProperty(102, "value", value_.smallint);
		break;
	case PhysicalType::INT32:
		serializer.WriteProperty(102, "value", value_.integer);
		break;
	case PhysicalType::INT64:
		serializer.WriteProperty(102, "value", value_.bigint);
		break;
	case PhysicalType::UINT8:
		serializer.WriteProperty(102, "value", value_.utinyint);
		break;
	case PhysicalType::UINT16:
		serializer.WriteProperty(102, "value", value_.usmallint);
		break;
	case PhysicalType::UINT32:
		serializer.WriteProperty(102, "value", value_.uinteger);
		break;
	case PhysicalType::UINT64:
		serializer.WriteProperty(102, "value", value_.ubigint);
		break;
	case PhysicalType::INT128:
		serializer.WriteProperty(102, "value", value_.hugeint);
		break;
	case PhysicalType::UINT128:
		serializer.WriteProperty(102, "value", value_.uhugeint);
		break;
	case PhysicalType::FLOAT:
		serializer.WriteProperty(102, "value", value_.float_);
		break;
	case PhysicalType::DOUBLE:
		serializer.WriteProperty(102, "value", value_.double_);
		break;
	case PhysicalType::INTERVAL:
		serializer.WriteProperty(102, "value", value_.interval);
		break;
	case PhysicalType::VARCHAR: {
		if (type_.id() == LogicalTypeId::BLOB) {
			auto blob_str = Blob::ToString(StringValue::Get(*this));
			serializer.WriteProperty(102, "value", blob_str);
		} else if (type_.id() == LogicalTypeId::GEOMETRY) {
			if (!serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
				// Write as old-style SPATIAL format
				string blob;
				Geometry::ToSpatialGeometry(StringValue::Get(*this), blob);
				auto text = Blob::ToString(blob);
				serializer.WriteProperty(102, "value", text);
			} else {
				// Otherwise, write as WKB with an explicit format property so that we recognize during deserialization.
				auto text = Blob::ToString(StringValue::Get(*this));
				serializer.WriteProperty(102, "value", text);
				serializer.WriteProperty(103, "geometry_format", GeometryStorageType::WKB);
			}
		} else {
			serializer.WriteProperty(102, "value", StringValue::Get(*this));
		}
	} break;
	case PhysicalType::LIST:
		SerializeChildren(serializer, ListValue::GetChildren(*this), type_);
		break;
	case PhysicalType::STRUCT:
		SerializeChildren(serializer, StructValue::GetChildren(*this), type_);
		break;
	case PhysicalType::ARRAY:
		SerializeChildren(serializer, ArrayValue::GetChildren(*this), type_);
		break;
	default:
		throw NotImplementedException("Unimplemented type for Serialize");
	}
}

void Value::Serialize(Serializer &serializer) const {
	// serialize the value - the top-level value always needs to serialize its type
	SerializeInternal(serializer, true);
}

//! Whether the value (excluding its children) is within the domain of its type
//! Whether a child value has the type its parent expects - ANY accepts values of any type (e.g. in secrets)
static bool ChildTypeMatches(const Value &child, const LogicalType &expected_type) {
	return expected_type.id() == LogicalTypeId::ANY || child.type() == expected_type;
}

static bool ValueIsValidShallow(const Value &value) {
	if (value.IsNull()) {
		return true;
	}
	auto &type = value.type();
	switch (type.id()) {
	case LogicalTypeId::VARCHAR:
		return Value::StringIsValid(StringValue::Get(value));
	case LogicalTypeId::ENUM: {
		uint64_t index;
		switch (type.InternalType()) {
		case PhysicalType::UINT8:
			index = value.GetValueUnsafe<uint8_t>();
			break;
		case PhysicalType::UINT16:
			index = value.GetValueUnsafe<uint16_t>();
			break;
		case PhysicalType::UINT32:
			index = value.GetValueUnsafe<uint32_t>();
			break;
		default:
			throw InternalException("Invalid physical type for ENUM");
		}
		return index < EnumType::GetSize(type);
	}
	case LogicalTypeId::TIME: {
		const auto micros = value.GetValueUnsafe<dtime_t>().value;
		return micros >= 0 && micros <= Interval::MICROS_PER_DAY;
	}
	case LogicalTypeId::TIME_NS: {
		const auto nanos = value.GetValueUnsafe<dtime_ns_t>().value;
		return nanos >= 0 && nanos <= Interval::NANOS_PER_DAY;
	}
	case LogicalTypeId::TIME_TZ: {
		const auto time_tz = value.GetValueUnsafe<dtime_tz_t>();
		const auto micros = time_tz.time().value;
		const auto offset = time_tz.offset();
		return micros >= 0 && micros <= Interval::MICROS_PER_DAY && offset >= dtime_tz_t::MIN_OFFSET &&
		       offset <= dtime_tz_t::MAX_OFFSET;
	}
	case LogicalTypeId::DECIMAL: {
		const auto width = DecimalType::GetWidth(type);
		switch (type.InternalType()) {
		case PhysicalType::INT16:
		case PhysicalType::INT32:
		case PhysicalType::INT64: {
			int64_t decimal_value;
			if (type.InternalType() == PhysicalType::INT16) {
				decimal_value = value.GetValueUnsafe<int16_t>();
			} else if (type.InternalType() == PhysicalType::INT32) {
				decimal_value = value.GetValueUnsafe<int32_t>();
			} else {
				decimal_value = value.GetValueUnsafe<int64_t>();
			}
			return decimal_value > -NumericHelper::POWERS_OF_TEN[width] &&
			       decimal_value < NumericHelper::POWERS_OF_TEN[width];
		}
		case PhysicalType::INT128: {
			const auto decimal_value = value.GetValueUnsafe<hugeint_t>();
			return decimal_value > -Hugeint::POWERS_OF_TEN[width] && decimal_value < Hugeint::POWERS_OF_TEN[width];
		}
		default:
			throw InternalException("Invalid physical type for DECIMAL");
		}
	}
	case LogicalTypeId::BIT: {
		// a padding byte followed by at least one data byte, with the padding bits set
		auto &bits = StringValue::Get(value);
		if (bits.size() < 2) {
			return false;
		}
		const auto padding = static_cast<uint8_t>(bits[0]);
		if (padding >= 8) {
			return false;
		}
		const auto padding_mask = static_cast<uint8_t>(0xFF << (8 - padding));
		return padding == 0 || (static_cast<uint8_t>(bits[1]) & padding_mask) == padding_mask;
	}
	case LogicalTypeId::UNION: {
		auto &children = StructValue::GetChildren(value);
		if (children.size() != UnionType::GetMemberCount(type) + 1 ||
		    children[0].type().id() != LogicalTypeId::UTINYINT || children[0].IsNull() ||
		    children[0].GetValueUnsafe<union_tag_t>() >= UnionType::GetMemberCount(type)) {
			return false;
		}
		for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(type); member_idx++) {
			if (!ChildTypeMatches(children[member_idx + 1], UnionType::GetMemberType(type, member_idx))) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::STRUCT: {
		auto &children = StructValue::GetChildren(value);
		if (children.size() != StructType::GetChildCount(type)) {
			return false;
		}
		for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
			if (!ChildTypeMatches(children[child_idx], StructType::GetChildType(type, child_idx))) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP: {
		auto &child_type = ListType::GetChildType(type);
		for (auto &child : ListValue::GetChildren(value)) {
			if (!ChildTypeMatches(child, child_type)) {
				return false;
			}
		}
		return true;
	}
	case LogicalTypeId::ARRAY: {
		auto &children = ArrayValue::GetChildren(value);
		if (children.size() != ArrayType::GetSize(type)) {
			return false;
		}
		auto &child_type = ArrayType::GetChildType(type);
		for (auto &child : children) {
			if (!ChildTypeMatches(child, child_type)) {
				return false;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

bool Value::IsValid() const {
	if (!ValueIsValidShallow(*this)) {
		return false;
	}
	if (is_null) {
		return true;
	}
	switch (type_.InternalType()) {
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		for (auto &child : value_info_->Get<NestedValueInfo>().GetValues()) {
			if (!child.IsValid()) {
				return false;
			}
		}
		break;
	default:
		break;
	}
	return true;
}

Value Value::Deserialize(Deserializer &deserializer) {
	auto type = deserializer.ReadPropertyWithExplicitDefault<LogicalType>(100, "type", LogicalTypeId::INVALID);
	if (type.InternalType() == PhysicalType::INVALID) {
		if (type.id() != LogicalTypeId::INVALID) {
			throw SerializationException("Failed to deserialize value: type %s is missing its type info",
			                             EnumUtil::ToString(type.id()));
		}
		type = deserializer.Get<const LogicalType &>();
	}
	auto is_null = deserializer.ReadProperty<bool>(101, "is_null");
	Value new_value = Value(type);
	if (is_null) {
		return new_value;
	}
	new_value.is_null = false;
	if (type.IsNested() && !type.HasParameters()) {
		throw SerializationException("Failed to deserialize value: type %s is missing its type info", type.ToString());
	}

	if (type.id() == LogicalTypeId::TYPE) {
		// special case for TYPE values: deserialize the type as a nested object
		auto type_value = deserializer.ReadProperty<LogicalType>(102, "value");
		return Value::TYPE(type_value);
	}

	switch (type.InternalType()) {
	case PhysicalType::BIT:
		throw InternalException("BIT type should not be deserialized");
	case PhysicalType::BOOL:
		new_value.value_.boolean = deserializer.ReadProperty<bool>(102, "value");
		break;
	case PhysicalType::UINT8:
		new_value.value_.utinyint = deserializer.ReadProperty<uint8_t>(102, "value");
		break;
	case PhysicalType::INT8:
		new_value.value_.tinyint = deserializer.ReadProperty<int8_t>(102, "value");
		break;
	case PhysicalType::UINT16:
		new_value.value_.usmallint = deserializer.ReadProperty<uint16_t>(102, "value");
		break;
	case PhysicalType::INT16:
		new_value.value_.smallint = deserializer.ReadProperty<int16_t>(102, "value");
		break;
	case PhysicalType::UINT32:
		new_value.value_.uinteger = deserializer.ReadProperty<uint32_t>(102, "value");
		break;
	case PhysicalType::INT32:
		new_value.value_.integer = deserializer.ReadProperty<int32_t>(102, "value");
		break;
	case PhysicalType::UINT64:
		new_value.value_.ubigint = deserializer.ReadProperty<uint64_t>(102, "value");
		break;
	case PhysicalType::INT64:
		new_value.value_.bigint = deserializer.ReadProperty<int64_t>(102, "value");
		break;
	case PhysicalType::UINT128:
		new_value.value_.uhugeint = deserializer.ReadProperty<uhugeint_t>(102, "value");
		break;
	case PhysicalType::INT128:
		new_value.value_.hugeint = deserializer.ReadProperty<hugeint_t>(102, "value");
		break;
	case PhysicalType::FLOAT:
		new_value.value_.float_ = deserializer.ReadProperty<float>(102, "value");
		break;
	case PhysicalType::DOUBLE:
		new_value.value_.double_ = deserializer.ReadProperty<double>(102, "value");
		break;
	case PhysicalType::INTERVAL:
		new_value.value_.interval = deserializer.ReadProperty<interval_t>(102, "value");
		break;
	case PhysicalType::VARCHAR: {
		if (type.id() == LogicalTypeId::BLOB) {
			auto str = deserializer.ReadProperty<string>(102, "value");
			new_value.value_info_ = make_shared_ptr<StringValueInfo>(Blob::ToBlob(str));
		} else if (type.id() == LogicalTypeId::GEOMETRY) {
			auto text = deserializer.ReadProperty<string>(102, "value");
			auto type = deserializer.ReadPropertyWithExplicitDefault<GeometryStorageType>(103, "geometry_format",
			                                                                              GeometryStorageType::SPATIAL);

			auto blob = Blob::ToBlob(text);
			if (type == GeometryStorageType::WKB) {
				new_value.value_info_ = make_shared_ptr<StringValueInfo>(std::move(blob));
			} else if (type == GeometryStorageType::SPATIAL) {
				string geom;
				Geometry::FromSpatialGeometry(blob, geom);
				new_value.value_info_ = make_shared_ptr<StringValueInfo>(std::move(geom));
			} else {
				throw InternalException("Unknown geometry format in value deserialization");
			}
		} else {
			auto str = deserializer.ReadProperty<string>(102, "value");
			new_value.value_info_ = make_shared_ptr<StringValueInfo>(std::move(str));
		}
	} break;
	case PhysicalType::LIST: {
		deserializer.Set<const LogicalType &>(ListType::GetChildType(type));
		deserializer.ReadObject(102, "value", [&](Deserializer &obj) {
			auto children = obj.ReadProperty<vector<Value>>(100, "children");
			new_value.value_info_ = make_shared_ptr<NestedValueInfo>(children);
		});
		deserializer.Unset<LogicalType>();
	} break;
	case PhysicalType::STRUCT: {
		deserializer.ReadObject(102, "value", [&](Deserializer &obj) {
			vector<Value> children;
			obj.ReadList(100, "children", [&](Deserializer::List &list, idx_t i) {
				if (i >= StructType::GetChildCount(type)) {
					throw SerializationException("Failed to deserialize value: too many children for type %s",
					                             type.ToString());
				}
				deserializer.Set<const LogicalType &>(StructType::GetChildType(type, i));
				auto child = list.ReadElement<Value>();
				deserializer.Unset<LogicalType>();
				children.push_back(std::move(child));
			});
			new_value.value_info_ = make_shared_ptr<NestedValueInfo>(children);
		});
	} break;
	case PhysicalType::ARRAY: {
		deserializer.Set<const LogicalType &>(ArrayType::GetChildType(type));
		deserializer.ReadObject(102, "value", [&](Deserializer &obj) {
			auto children = obj.ReadProperty<vector<Value>>(100, "children");
			new_value.value_info_ = make_shared_ptr<NestedValueInfo>(children);
		});
		deserializer.Unset<LogicalType>();
	} break;
	default:
		throw NotImplementedException("Unimplemented type for Deserialize");
	}
	// the children are deserialized (and checked) through Value::Deserialize as well
	if (!ValueIsValidShallow(new_value)) {
		throw SerializationException("Failed to deserialize value: value is not valid for type %s", type.ToString());
	}
	return new_value;
}

void Value::Print() const {
	Printer::Print(ToString());
}

bool Value::NotDistinctFrom(const Value &lvalue, const Value &rvalue) {
	return ValueOperations::NotDistinctFrom(lvalue, rvalue);
}

static string SanitizeValue(string input) {
	// some results might contain padding spaces, e.g. when rendering
	// VARCHAR(10) and the string only has 6 characters, they will be padded
	// with spaces to 10 in the rendering. We don't do that here yet as we
	// are looking at internal structures. So just ignore any extra spaces
	// on the right
	StringUtil::RTrim(input);
	// for result checking code, replace null bytes with their escaped value (\0)
	return StringUtil::Replace(input, string("\0", 1), "\\0");
}

bool Value::ValuesAreEqual(CastFunctionSet &set, GetCastFunctionInput &get_input, const Value &result_value,
                           const Value &value) {
	if (result_value.IsNull() != value.IsNull()) {
		return false;
	}
	if (result_value.IsNull() && value.IsNull()) {
		// NULL = NULL in checking code
		return true;
	}
	switch (value.type_.id()) {
	case LogicalTypeId::FLOAT: {
		auto other = result_value.CastAs(set, get_input, LogicalType::FLOAT);
		float ldecimal = value.value_.float_;
		float rdecimal = other.value_.float_;
		return ApproxEqual(ldecimal, rdecimal);
	}
	case LogicalTypeId::DOUBLE: {
		auto other = result_value.CastAs(set, get_input, LogicalType::DOUBLE);
		double ldecimal = value.value_.double_;
		double rdecimal = other.value_.double_;
		return ApproxEqual(ldecimal, rdecimal);
	}
	case LogicalTypeId::VARCHAR: {
		auto other = result_value.CastAs(set, get_input, LogicalType::VARCHAR);
		string left = SanitizeValue(StringValue::Get(other));
		string right = SanitizeValue(StringValue::Get(value));
		return left == right;
	}
	default:
		if (result_value.type_.id() == LogicalTypeId::FLOAT || result_value.type_.id() == LogicalTypeId::DOUBLE) {
			return Value::ValuesAreEqual(set, get_input, value, result_value);
		}
		return value == result_value;
	}
}

bool Value::ValuesAreEqual(ClientContext &context, const Value &result_value, const Value &value) {
	GetCastFunctionInput get_input(context);
	return Value::ValuesAreEqual(CastFunctionSet::Get(context), get_input, result_value, value);
}
bool Value::DefaultValuesAreEqual(const Value &result_value, const Value &value) {
	CastFunctionSet set;
	GetCastFunctionInput get_input;
	return Value::ValuesAreEqual(set, get_input, result_value, value);
}

} // namespace duckdb
