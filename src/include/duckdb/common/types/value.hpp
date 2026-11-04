//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/value.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/datetime.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/insertion_order_preserving_map.hpp"

#include <cmath>
#include <functional>

namespace duckdb {

class String;
class CastFunctionSet;
struct GetCastFunctionInput;
struct ExtraValueInfo;

//! The Value object holds a single arbitrary value of any type that can be
//! stored in the database.
class Value {
	friend struct StringValue;
	friend struct StructValue;
	friend struct ListValue;
	friend struct UnionValue;
	friend struct ArrayValue;
	friend struct MapValue;
	friend struct TypeValue;

public:
	//! Create an empty NULL value of the specified type
	DUCKDB_API explicit Value(LogicalType type = LogicalType::SQLNULL);
	//! Create an INTEGER value
	DUCKDB_API Value(int32_t val); // NOLINT: Allow implicit conversion from `int32_t`
	//! Create a BOOLEAN value
	explicit DUCKDB_API Value(bool val);
	//! Create a BIGINT value
	DUCKDB_API Value(int64_t val); // NOLINT: Allow implicit conversion from `int64_t`
	//! Create a FLOAT value
	DUCKDB_API Value(float val); // NOLINT: Allow implicit conversion from `float`
	//! Create a DOUBLE value
	DUCKDB_API Value(double val); // NOLINT: Allow implicit conversion from `double`
	//! Create a VARCHAR value
	DUCKDB_API Value(const char *val);       // NOLINT: Allow implicit conversion from `const char *`
	DUCKDB_API Value(const Identifier &val); // NOLINT: Allow implicit conversion from `Identifier`
	//! Create a NULL value
	DUCKDB_API Value(std::nullptr_t val); // NOLINT: Allow implicit conversion from `nullptr_t`
	//! Create a VARCHAR value
	DUCKDB_API Value(string_t val); // NOLINT: Allow implicit conversion from `string_t`
	//! Create a VARCHAR value
	DUCKDB_API Value(string val); // NOLINT: Allow implicit conversion from `string`
	//! Create a VARCHAR value
	DUCKDB_API Value(String val); // NOLINT: Allow implicit conversion from `string`
	//! Create a VARCHAR value
	DUCKDB_API Value(std::string_view val); // NOLINT: Allow implicit conversion from `std::string_view`
	//! Copy constructor
	DUCKDB_API Value(const Value &other);
	//! Move constructor
	DUCKDB_API Value(Value &&other) noexcept;
	//! Destructor
	DUCKDB_API ~Value();

	// copy assignment
	DUCKDB_API Value &operator=(const Value &other);
	// move assignment
	DUCKDB_API Value &operator=(Value &&other) noexcept;

	inline const LogicalType &type() const { // NOLINT
		return type_;
	}
	inline bool IsNull() const {
		return is_null;
	}
	//! Whether the value is within the domain of its type (e.g. valid UTF-8, ENUM index within the dictionary)
	DUCKDB_API bool IsValid() const;

	//! Create the lowest possible value of a given type (numeric only)
	DUCKDB_API static Value MinimumValue(const LogicalType &type);
	//! Create the highest possible value of a given type (numeric only)
	DUCKDB_API static Value MaximumValue(const LogicalType &type);
	//! Create the negative infinite value of a given type (numeric only)
	DUCKDB_API static Value NegativeInfinity(const LogicalType &type);
	//! Create the positive infinite value of a given type (numeric only)
	DUCKDB_API static Value Infinity(const LogicalType &type);
	//! Create a Numeric value of the specified type with the specified value
	DUCKDB_API static Value Numeric(const LogicalType &type, int64_t value);
	DUCKDB_API static Value Numeric(const LogicalType &type, hugeint_t value);
	DUCKDB_API static Value Numeric(const LogicalType &type, uhugeint_t value);

	//! Create a boolean Value from a specified value
	DUCKDB_API static Value BOOLEAN(bool value);
	//! Create a tinyint Value from a specified value
	DUCKDB_API static Value TINYINT(int8_t value);
	//! Create a smallint Value from a specified value
	DUCKDB_API static Value SMALLINT(int16_t value);
	//! Create an integer Value from a specified value
	DUCKDB_API static Value INTEGER(int32_t value);
	//! Create a bigint Value from a specified value
	DUCKDB_API static Value BIGINT(int64_t value);
	//! Create an unsigned tinyint Value from a specified value
	DUCKDB_API static Value UTINYINT(uint8_t value);
	//! Create an unsigned smallint Value from a specified value
	DUCKDB_API static Value USMALLINT(uint16_t value);
	//! Create an unsigned integer Value from a specified value
	DUCKDB_API static Value UINTEGER(uint32_t value);
	//! Create an unsigned bigint Value from a specified value
	DUCKDB_API static Value UBIGINT(uint64_t value);
	//! Create a hugeint Value from a specified value
	DUCKDB_API static Value HUGEINT(hugeint_t value);
	//! Create a uhugeint Value from a specified value
	DUCKDB_API static Value UHUGEINT(uhugeint_t value);
	//! Create a uuid Value from a specified value
	DUCKDB_API static Value UUID(const string &value);
	//! Create a uuid Value from a specified value
	DUCKDB_API static Value UUID(hugeint_t value);
	//! Create a hash Value from a specified value
	DUCKDB_API static Value HASH(hash_t value);
	//! Create a pointer Value from a specified value
	DUCKDB_API static Value POINTER(uintptr_t value);
	//! Create a date Value from a specified date
	DUCKDB_API static Value DATE(date_t date);
	//! Create a date Value from a specified date
	DUCKDB_API static Value DATE(int32_t year, int32_t month, int32_t day);
	//! Create a time Value from a specified time
	DUCKDB_API static Value TIME(dtime_t time);
	DUCKDB_API static Value TIME_NS(dtime_ns_t time);
	DUCKDB_API static Value TIMETZ(dtime_tz_t time);
	//! Create a time Value from a specified time
	DUCKDB_API static Value TIME(int32_t hour, int32_t min, int32_t sec, int32_t micros);
	//! Create a timestamp Value from a specified date/time combination.
	DUCKDB_API static Value TIMESTAMP(date_t date, dtime_t time);
	//! Create a timestamp Value from a specified value.
	DUCKDB_API static Value TIMESTAMP(timestamp_t timestamp);
	//! Create a timestamp_s Value from a specified value.
	DUCKDB_API static Value TIMESTAMPSEC(timestamp_sec_t timestamp);
	//! Create a timestamp_ms Value from a specified value.
	DUCKDB_API static Value TIMESTAMPMS(timestamp_ms_t timestamp);
	//! Create a timestamp_ns Value from a specified value.
	DUCKDB_API static Value TIMESTAMPNS(timestamp_ns_t timestamp);
	//! Create a timestamp_tz Value from a specified value.
	DUCKDB_API static Value TIMESTAMPTZ(timestamp_tz_t timestamp);
	//! Create a timestamp_tz_ns Value from a specified value.
	DUCKDB_API static Value TIMESTAMPTZNS(timestamp_tz_ns_t timestamp);
	//! Create a timestamp Value from a specified timestamp in separate values
	DUCKDB_API static Value TIMESTAMP(int32_t year, int32_t month, int32_t day, int32_t hour, int32_t min, int32_t sec,
	                                  int32_t micros);
	DUCKDB_API static Value INTERVAL(int32_t months, int32_t days, int64_t micros);
	DUCKDB_API static Value INTERVAL(interval_t interval);

	// Create a enum Value from a specified uint value
	DUCKDB_API static Value ENUM(uint64_t value, const LogicalType &original_type);

	// Decimal values
	DUCKDB_API static Value DECIMAL(int16_t value, uint8_t width, uint8_t scale);
	DUCKDB_API static Value DECIMAL(int32_t value, uint8_t width, uint8_t scale);
	DUCKDB_API static Value DECIMAL(int64_t value, uint8_t width, uint8_t scale);
	DUCKDB_API static Value DECIMAL(hugeint_t value, uint8_t width, uint8_t scale);
	//! Create a float Value from a specified value
	DUCKDB_API static Value FLOAT(float value);
	//! Create a double Value from a specified value
	DUCKDB_API static Value DOUBLE(double value);
	//! Create a struct value with given list of entries
	DUCKDB_API static Value STRUCT(child_list_t<Value> values);
	DUCKDB_API static Value STRUCT(const LogicalType &type, vector<Value> struct_values);
	//! Create an unnamed TUPLE value of the given child values
	DUCKDB_API static Value TUPLE(vector<Value> values);
	DUCKDB_API static Value AGGREGATE_STATE(const LogicalType &type, vector<Value> underlying_struct_values);
	//! Create a variant value with given list of internal variant data (keys/children/values/data)
	DUCKDB_API static Value VARIANT(vector<Value> children);
	//! Create a list value with the given entries
	DUCKDB_API static Value LIST(const LogicalType &child_type, vector<Value> values);
	//! Create a list value with the given entries
	//! The type of the first value determines the list type. The list cannot be empty.
	DUCKDB_API static Value LIST(vector<Value> values);
	// Create an array value with the given entries
	DUCKDB_API static Value ARRAY(const LogicalType &type, vector<Value> values);
	//! Create a map value with the given entries
	DUCKDB_API static Value MAP(const LogicalType &child_type, vector<Value> values);
	//! Create a map value with the given entries
	DUCKDB_API static Value MAP(const LogicalType &key_type, const LogicalType &value_type, vector<Value> keys,
	                            vector<Value> values);
	//! Create a map value from a set of key-value pairs
	DUCKDB_API static Value MAP(const InsertionOrderPreservingMap<string> &kv_pairs);

	//! Create a union value from a selected value and a tag from a set of alternatives.
	DUCKDB_API static Value UNION(child_list_t<LogicalType> members, uint8_t tag, Value value);

	//! Create a blob Value from a data pointer and a length: no bytes are interpreted
	DUCKDB_API static Value BLOB(const_data_ptr_t data, idx_t len);
	static Value BLOB_RAW(const string &data) { // NOLINT
		return Value::BLOB(const_data_ptr_cast(data.c_str()), data.size());
	}
	static Value BLOB_RAW(std::string_view data) {
		return Value::BLOB(
		    const_data_ptr_cast(data.data()), // NOLINT(bugprone-suspicious-stringview-data-usage): length is passed
		    data.size());
	}
	//! Creates a blob by casting a specified string to a blob (i.e. interpreting \x characters)
	DUCKDB_API static Value BLOB(const string &data);
	//! Creates a bitstring by casting a specified string to a bitstring
	DUCKDB_API static Value BIT(const_data_ptr_t data, idx_t len);
	DUCKDB_API static Value BIT(const string &data);
	DUCKDB_API static Value BIGNUM(const_data_ptr_t data, idx_t len);
	DUCKDB_API static Value BIGNUM(const string &data);

	DUCKDB_API static Value GEOMETRY(const_data_ptr_t data, idx_t len);
	DUCKDB_API static Value GEOMETRY(const_data_ptr_t data, idx_t len, const CoordinateReferenceSystem &crs);

	DUCKDB_API static Value TYPE(const LogicalType &type);
	DUCKDB_API static Value TYPE(const string_t &serialized_type);

	//! Creates an aggregate state
	DUCKDB_API static Value LEGACY_AGGREGATE_STATE(const LogicalType &type, const_data_ptr_t data, idx_t len); // NOLINT

	template <class T>
	T GetValue() const;
	template <class T>
	static Value CreateValue(T value) {
		static_assert(AlwaysFalse<T>::VALUE, "No specialization exists for this type");
		return Value(nullptr);
	}
	// Returns the internal value. Unlike GetValue(), this method does not perform casting, and assumes T matches the
	// type of the value. Only use this if you know what you are doing.
	template <class T>
	T GetValueUnsafe() const;

	//! Pointer to the inline-stored payload bytes. Only valid for constant-size physical types; the
	//! pointer borrows the Value's storage and is valid until the Value is destroyed.
	DUCKDB_API const_data_ptr_t GetPointerToData() const;

	//! Return a copy of this value
	Value Copy() const {
		return Value(*this);
	}

	//! Hashes the Value
	DUCKDB_API hash_t Hash() const;
	//! Convert this value to a string
	DUCKDB_API string ToString() const;
	//! Convert this value to a SQL-parseable string
	DUCKDB_API string ToSQLString() const;
	//! Render a nested value as SQL via child_to_sql
	DUCKDB_API static string NestedToSQLString(const Value &value,
	                                           const std::function<string(const Value &)> &child_to_sql);

	DUCKDB_API uintptr_t GetPointer() const;

	//! Cast this value to another type, throws exception if its not possible
	DUCKDB_API Value CastAs(CastFunctionSet &set, GetCastFunctionInput &get_input, const LogicalType &target_type,
	                        bool strict = false) const;
	DUCKDB_API Value CastAs(ClientContext &context, const LogicalType &target_type, bool strict = false) const;
	DUCKDB_API Value DefaultCastAs(const LogicalType &target_type, bool strict = false) const;
	//! Tries to cast this value to another type - returns the result, or nullopt if the cast is not possible.
	//! Never throws on a failed cast; pass "error_message" to obtain the reason for the failure
	DUCKDB_API optional<Value> TryCastAs(CastFunctionSet &set, GetCastFunctionInput &get_input,
	                                     const LogicalType &target_type, string *error_message = nullptr,
	                                     bool strict = false) const;
	DUCKDB_API optional<Value> TryCastAs(ClientContext &context, const LogicalType &target_type,
	                                     string *error_message = nullptr, bool strict = false) const;
	DUCKDB_API optional<Value> DefaultTryCastAs(const LogicalType &target_type, string *error_message = nullptr,
	                                            bool strict = false) const;

	//! Returns a copy of this value with its type replaced - the underlying value is reinterpreted, not cast
	DUCKDB_API Value WithType(LogicalType new_type) const;

	//! Serializes a Value to a stand-alone binary blob
	DUCKDB_API void Serialize(Serializer &serializer) const;
	//! Deserializes a Value from a blob
	DUCKDB_API static Value Deserialize(Deserializer &deserializer);

	//===--------------------------------------------------------------------===//
	// Comparison Operators
	//===--------------------------------------------------------------------===//
	DUCKDB_API bool operator==(const Value &rhs) const;
	DUCKDB_API bool operator!=(const Value &rhs) const;
	DUCKDB_API bool operator<(const Value &rhs) const;
	DUCKDB_API bool operator>(const Value &rhs) const;
	DUCKDB_API bool operator<=(const Value &rhs) const;
	DUCKDB_API bool operator>=(const Value &rhs) const;

	DUCKDB_API bool operator==(const int64_t &rhs) const;
	DUCKDB_API bool operator!=(const int64_t &rhs) const;
	DUCKDB_API bool operator<(const int64_t &rhs) const;
	DUCKDB_API bool operator>(const int64_t &rhs) const;
	DUCKDB_API bool operator<=(const int64_t &rhs) const;
	DUCKDB_API bool operator>=(const int64_t &rhs) const;

	DUCKDB_API static bool FloatIsFinite(float value);
	DUCKDB_API static bool DoubleIsFinite(double value);
	template <class T>
	static bool IsNan(T value) {
		throw InternalException("Unimplemented template type for Value::IsNan");
	}
	template <class T>
	static bool IsFinite(T value) {
		return true;
	}
	DUCKDB_API static bool StringIsValid(const char *str, idx_t length);
	static bool StringIsValid(const string &str) {
		return StringIsValid(str.c_str(), str.size());
	}

	//! Returns true if the values are (approximately) equivalent. Note this is NOT the SQL equivalence. For this
	//! function, NULL values are equivalent and floating point values that are close are equivalent.
	DUCKDB_API static bool ValuesAreEqual(CastFunctionSet &set, GetCastFunctionInput &get_input,
	                                      const Value &result_value, const Value &value);
	DUCKDB_API static bool ValuesAreEqual(ClientContext &context, const Value &result_value, const Value &value);
	DUCKDB_API static bool DefaultValuesAreEqual(const Value &result_value, const Value &value);
	//! Returns true if the values are not distinct from each other, following SQL semantics for NOT DISTINCT FROM.
	DUCKDB_API static bool NotDistinctFrom(const Value &lvalue, const Value &rvalue);

	friend std::ostream &operator<<(std::ostream &out, const Value &val) {
		out << val.ToString();
		return out;
	}
	DUCKDB_API void Print() const;

private:
	void SerializeInternal(Serializer &serializer, bool serialize_type) const;
	static void SerializeChildren(Serializer &serializer, const vector<Value> &children,
	                              const LogicalType &parent_type);

private:
	//! The logical of the value
	LogicalType type_; // NOLINT

	//! Whether or not the value is NULL
	bool is_null;

	//! The value of the object, if it is of a constant size Type
	union Val {
		bool boolean;
		int8_t tinyint;
		int16_t smallint;
		int32_t integer;
		int64_t bigint;
		uint8_t utinyint;
		uint16_t usmallint;
		uint32_t uinteger;
		uint64_t ubigint;
		hugeint_t hugeint;
		uhugeint_t uhugeint;
		float float_;   // NOLINT
		double double_; // NOLINT
		uintptr_t pointer;
		uint64_t hash;
		date_t date;
		dtime_t time;
		dtime_ns_t time_ns;
		dtime_tz_t timetz;
		timestamp_t timestamp;
		timestamp_sec_t timestamp_s;
		timestamp_ms_t timestamp_ms;
		timestamp_ns_t timestamp_ns;
		timestamp_tz_t timestamp_tz;
		timestamp_tz_ns_t timestamp_tz_ns;
		interval_t interval;
	} value_; // NOLINT

	shared_ptr<ExtraValueInfo> value_info_; // NOLINT

private:
	template <class T>
	T GetValueInternal() const;
};

//===--------------------------------------------------------------------===//
// Type-specific getters
//===--------------------------------------------------------------------===//
// Note that these are equivalent to calling GetValueUnsafe<X>, meaning no cast will be performed
// instead, an assertion will be triggered if the value is not of the correct type
struct BooleanValue {
	DUCKDB_API static bool Get(const Value &value);
};

struct TinyIntValue {
	DUCKDB_API static int8_t Get(const Value &value);
};

struct SmallIntValue {
	DUCKDB_API static int16_t Get(const Value &value);
};

struct IntegerValue {
	DUCKDB_API static int32_t Get(const Value &value);
};

struct BigIntValue {
	DUCKDB_API static int64_t Get(const Value &value);
};

struct HugeIntValue {
	DUCKDB_API static hugeint_t Get(const Value &value);
};

struct UTinyIntValue {
	DUCKDB_API static uint8_t Get(const Value &value);
};

struct USmallIntValue {
	DUCKDB_API static uint16_t Get(const Value &value);
};

struct UIntegerValue {
	DUCKDB_API static uint32_t Get(const Value &value);
};

struct UBigIntValue {
	DUCKDB_API static uint64_t Get(const Value &value);
};

struct UhugeIntValue {
	DUCKDB_API static uhugeint_t Get(const Value &value);
};

struct FloatValue {
	DUCKDB_API static float Get(const Value &value);
};

struct DoubleValue {
	DUCKDB_API static double Get(const Value &value);
};

struct StringValue {
	DUCKDB_API static const string &Get(const Value &value);
};

struct DateValue {
	DUCKDB_API static date_t Get(const Value &value);
};

struct TimeValue {
	DUCKDB_API static dtime_t Get(const Value &value);
};

struct TimestampValue {
	DUCKDB_API static timestamp_t Get(const Value &value);
};

struct TimestampSValue {
	DUCKDB_API static timestamp_sec_t Get(const Value &value);
};

struct TimestampMSValue {
	DUCKDB_API static timestamp_ms_t Get(const Value &value);
};

struct TimestampNSValue {
	DUCKDB_API static timestamp_ns_t Get(const Value &value);
};

struct TimestampTZValue {
	DUCKDB_API static timestamp_tz_t Get(const Value &value);
};

struct TimestampTZNSValue {
	DUCKDB_API static timestamp_tz_ns_t Get(const Value &value);
};

struct IntervalValue {
	DUCKDB_API static interval_t Get(const Value &value);
};

struct StructValue {
	DUCKDB_API static const vector<Value> &GetChildren(const Value &value);
};

struct MapValue {
	DUCKDB_API static const vector<Value> &GetChildren(const Value &value);
};

struct ListValue {
	DUCKDB_API static const vector<Value> &GetChildren(const Value &value);
};

struct ArrayValue {
	DUCKDB_API static const vector<Value> &GetChildren(const Value &value);
};

struct UnionValue {
	DUCKDB_API static const Value &GetValue(const Value &value);
	DUCKDB_API static uint8_t GetTag(const Value &value);
	DUCKDB_API static const LogicalType &GetType(const Value &value);
};

struct TypeValue {
	DUCKDB_API static LogicalType GetType(const Value &value);
};

struct VariantValue {
	//! Convert a (non-null) VARIANT-typed Value back to a plain Value
	DUCKDB_API static Value GetValue(const Value &variant_val);
};

//! Return the internal integral value for any type that is stored as an integral value internally
//! This can be used on values of type integer, uinteger, but also date, timestamp, decimal, etc
struct IntegralValue {
	static hugeint_t Get(const Value &value);
};

template <>
Value DUCKDB_API Value::CreateValue(bool value);
template <>
Value DUCKDB_API Value::CreateValue(uint8_t value);
template <>
Value DUCKDB_API Value::CreateValue(uint16_t value);
template <>
Value DUCKDB_API Value::CreateValue(uint32_t value);
template <>
Value DUCKDB_API Value::CreateValue(uint64_t value);
template <>
Value DUCKDB_API Value::CreateValue(int8_t value);
template <>
Value DUCKDB_API Value::CreateValue(int16_t value);
template <>
Value DUCKDB_API Value::CreateValue(int32_t value);
template <>
Value DUCKDB_API Value::CreateValue(int64_t value);
template <>
Value DUCKDB_API Value::CreateValue(hugeint_t value);
template <>
Value DUCKDB_API Value::CreateValue(uhugeint_t value);
template <>
Value DUCKDB_API Value::CreateValue(date_t value);
template <>
Value DUCKDB_API Value::CreateValue(dtime_t value);
template <>
Value DUCKDB_API Value::CreateValue(dtime_ns_t value);
template <>
Value DUCKDB_API Value::CreateValue(dtime_tz_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_sec_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_ms_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_ns_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_tz_t value);
template <>
Value DUCKDB_API Value::CreateValue(timestamp_tz_ns_t value);
template <>
Value DUCKDB_API Value::CreateValue(const char *value);
template <>
Value DUCKDB_API Value::CreateValue(string value);
template <>
Value DUCKDB_API Value::CreateValue(string_t value);
template <>
Value DUCKDB_API Value::CreateValue(float value);
template <>
Value DUCKDB_API Value::CreateValue(double value);
template <>
Value DUCKDB_API Value::CreateValue(interval_t value);
template <>
Value DUCKDB_API Value::CreateValue(Value value);

template <>
DUCKDB_API bool Value::GetValue() const;
template <>
DUCKDB_API int8_t Value::GetValue() const;
template <>
DUCKDB_API int16_t Value::GetValue() const;
template <>
DUCKDB_API int32_t Value::GetValue() const;
template <>
DUCKDB_API int64_t Value::GetValue() const;
template <>
DUCKDB_API uint8_t Value::GetValue() const;
template <>
DUCKDB_API uint16_t Value::GetValue() const;
template <>
DUCKDB_API uint32_t Value::GetValue() const;
template <>
DUCKDB_API uint64_t Value::GetValue() const;
template <>
DUCKDB_API hugeint_t Value::GetValue() const;
template <>
DUCKDB_API uhugeint_t Value::GetValue() const;
template <>
DUCKDB_API string Value::GetValue() const;
template <>
DUCKDB_API float Value::GetValue() const;
template <>
DUCKDB_API double Value::GetValue() const;
template <>
DUCKDB_API date_t Value::GetValue() const;
template <>
DUCKDB_API dtime_t Value::GetValue() const;
template <>
DUCKDB_API dtime_ns_t Value::GetValue() const;
template <>
DUCKDB_API dtime_tz_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_sec_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_ms_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_ns_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_tz_t Value::GetValue() const;
template <>
DUCKDB_API timestamp_tz_ns_t Value::GetValue() const;
template <>
DUCKDB_API interval_t Value::GetValue() const;
template <>
DUCKDB_API Value Value::GetValue() const;
// Not a logical type like the specializations above, but a semantic wrapper around the string value. It mirrors the
// implicit Value(const Identifier &) constructor, so identifiers can round-trip through a Value.
template <>
DUCKDB_API Identifier Value::GetValue() const;

template <>
DUCKDB_API bool Value::GetValueUnsafe() const;
template <>
DUCKDB_API int8_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API int16_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API int32_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API int64_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API hugeint_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API uhugeint_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API uint8_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API uint16_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API uint32_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API uint64_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API string Value::GetValueUnsafe() const;
template <>
DUCKDB_API string_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API float Value::GetValueUnsafe() const;
template <>
DUCKDB_API double Value::GetValueUnsafe() const;
template <>
DUCKDB_API date_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API dtime_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API dtime_ns_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API dtime_tz_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_sec_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_ms_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_ns_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_tz_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API timestamp_tz_ns_t Value::GetValueUnsafe() const;
template <>
DUCKDB_API interval_t Value::GetValueUnsafe() const;

template <>
inline bool Value::IsNan(float input) {
	return std::isnan(input);
}
template <>
inline bool Value::IsNan(double input) {
	return std::isnan(input);
}

template <>
DUCKDB_API bool Value::IsFinite(float input);
template <>
DUCKDB_API bool Value::IsFinite(double input);
template <>
DUCKDB_API bool Value::IsFinite(date_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_sec_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_ms_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_ns_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_tz_t input);
template <>
DUCKDB_API bool Value::IsFinite(timestamp_tz_ns_t input);

} // namespace duckdb
