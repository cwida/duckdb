//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/single_thread_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

class Serializer;
class Deserializer;

//! Type used to represent dates (days since 1970-01-01)
struct date_t {
	int32_t days;

	date_t() = default;
	explicit inline date_t(int32_t days_p) : days(days_p) {}

	// explicit conversion
	explicit inline operator int32_t() const {return days;}

	// comparison operators
	inline bool operator==(const date_t &rhs) const {return days == rhs.days;};
	inline bool operator!=(const date_t &rhs) const {return days != rhs.days;};
	inline bool operator<=(const date_t &rhs) const {return days <= rhs.days;};
	inline bool operator<(const date_t &rhs) const {return days < rhs.days;};
	inline bool operator>(const date_t &rhs) const {return days > rhs.days;};
	inline bool operator>=(const date_t &rhs) const {return days >= rhs.days;};

	// arithmetic operators
	inline date_t operator+(const int32_t &days) const {return date_t(this->days + days);};
	inline date_t operator-(const int32_t &days) const {return date_t(this->days - days);};

	// in-place operators
	inline date_t &operator+=(const int32_t &days) {this->days += days; return *this;};
	inline date_t &operator-=(const int32_t &days) {this->days -= days; return *this;};
};

//! Type used to represent time (microseconds)
struct dtime_t {
    int64_t micros;

	dtime_t() = default;
	explicit inline dtime_t(int64_t micros_p) : micros(micros_p) {}
	inline dtime_t& operator=(int64_t micros_p) {micros = micros_p; return *this;}

	// explicit conversion
	explicit inline operator int64_t() const {return micros;}
	explicit inline operator double() const {return micros;}

	// comparison operators
	inline bool operator==(const dtime_t &rhs) const {return micros == rhs.micros;};
	inline bool operator!=(const dtime_t &rhs) const {return micros != rhs.micros;};
	inline bool operator<=(const dtime_t &rhs) const {return micros <= rhs.micros;};
	inline bool operator<(const dtime_t &rhs) const {return micros < rhs.micros;};
	inline bool operator>(const dtime_t &rhs) const {return micros > rhs.micros;};
	inline bool operator>=(const dtime_t &rhs) const {return micros >= rhs.micros;};

	// arithmetic operators
	inline dtime_t operator+(const int64_t &micros) const {return dtime_t(this->micros + micros);};
	inline dtime_t operator+(const double &micros) const {return dtime_t(this->micros + int64_t(micros));};
	inline dtime_t operator-(const int64_t &micros) const {return dtime_t(this->micros - micros);};
	inline dtime_t operator*(const idx_t &copies) const {return dtime_t(this->micros * copies);};
	inline dtime_t operator/(const idx_t &copies) const {return dtime_t(this->micros / copies);};
	inline int64_t operator-(const dtime_t &other) const {return this->micros - other.micros;};

	// in-place operators
	inline dtime_t &operator+=(const int64_t &micros) {this->micros += micros; return *this;};
	inline dtime_t &operator-=(const int64_t &micros) {this->micros -= micros; return *this;};
	inline dtime_t &operator+=(const dtime_t &other) {this->micros += other.micros; return *this;};
};

//! Type used to represent timestamps (seconds,microseconds,milliseconds or nanoseconds since 1970-01-01)
struct timestamp_t {
    int64_t value;

	timestamp_t() = default;
	explicit inline timestamp_t(int64_t value_p) : value(value_p) {}
	inline timestamp_t& operator=(int64_t value_p) {value = value_p; return *this;}

	// explicit conversion
	explicit inline operator int64_t() const {return value;}

	// comparison operators
	inline bool operator==(const timestamp_t &rhs) const {return value == rhs.value;};
	inline bool operator!=(const timestamp_t &rhs) const {return value != rhs.value;};
	inline bool operator<=(const timestamp_t &rhs) const {return value <= rhs.value;};
	inline bool operator<(const timestamp_t &rhs) const {return value < rhs.value;};
	inline bool operator>(const timestamp_t &rhs) const {return value > rhs.value;};
	inline bool operator>=(const timestamp_t &rhs) const {return value >= rhs.value;};

	// arithmetic operators
	inline timestamp_t operator+(const double &value) const {return timestamp_t(this->value + int64_t(value));};
	inline int64_t operator-(const timestamp_t &other) const {return this->value - other.value;};

	// in-place operators
	inline timestamp_t &operator+=(const int64_t &value) {this->value += value; return *this;};
	inline timestamp_t &operator-=(const int64_t &value) {this->value -= value; return *this;};
};

struct interval_t {
	int32_t months;
	int32_t days;
	int64_t micros;

	inline bool operator==(const interval_t &rhs) const {
		return this->days == rhs.days && this->months == rhs.months && this->micros == rhs.micros;
	}
};

struct hugeint_t {
public:
	uint64_t lower;
	int64_t upper;

public:
	hugeint_t() = default;
	hugeint_t(int64_t value); // NOLINT: Allow implicit conversion from `int64_t`
	hugeint_t(const hugeint_t &rhs) = default;
	hugeint_t(hugeint_t &&rhs) = default;
	hugeint_t &operator=(const hugeint_t &rhs) = default;
	hugeint_t &operator=(hugeint_t &&rhs) = default;

	string ToString() const;

	// comparison operators
	bool operator==(const hugeint_t &rhs) const;
	bool operator!=(const hugeint_t &rhs) const;
	bool operator<=(const hugeint_t &rhs) const;
	bool operator<(const hugeint_t &rhs) const;
	bool operator>(const hugeint_t &rhs) const;
	bool operator>=(const hugeint_t &rhs) const;

	// arithmetic operators
	hugeint_t operator+(const hugeint_t &rhs) const;
	hugeint_t operator-(const hugeint_t &rhs) const;
	hugeint_t operator*(const hugeint_t &rhs) const;
	hugeint_t operator/(const hugeint_t &rhs) const;
	hugeint_t operator%(const hugeint_t &rhs) const;
	hugeint_t operator-() const;

	// bitwise operators
	hugeint_t operator>>(const hugeint_t &rhs) const;
	hugeint_t operator<<(const hugeint_t &rhs) const;
	hugeint_t operator&(const hugeint_t &rhs) const;
	hugeint_t operator|(const hugeint_t &rhs) const;
	hugeint_t operator^(const hugeint_t &rhs) const;
	hugeint_t operator~() const;

	// in-place operators
	hugeint_t &operator+=(const hugeint_t &rhs);
	hugeint_t &operator-=(const hugeint_t &rhs);
	hugeint_t &operator*=(const hugeint_t &rhs);
	hugeint_t &operator/=(const hugeint_t &rhs);
	hugeint_t &operator%=(const hugeint_t &rhs);
	hugeint_t &operator>>=(const hugeint_t &rhs);
	hugeint_t &operator<<=(const hugeint_t &rhs);
	hugeint_t &operator&=(const hugeint_t &rhs);
	hugeint_t &operator|=(const hugeint_t &rhs);
	hugeint_t &operator^=(const hugeint_t &rhs);
};

struct string_t;

template <class T>
using child_list_t = std::vector<std::pair<std::string, T>>;
// we should be using single_thread_ptr here but cross-thread access to ChunkCollections currently prohibits this.
template <class T>
using buffer_ptr = shared_ptr<T>;

template <class T, typename... Args>
buffer_ptr<T> make_buffer(Args &&...args) {
	return make_shared<T>(std::forward<Args>(args)...);
}

struct list_entry_t {
	list_entry_t() = default;
	list_entry_t(uint64_t offset, uint64_t length) : offset(offset), length(length) {
	}

	uint64_t offset;
	uint64_t length;
};

//===--------------------------------------------------------------------===//
// Internal Types
//===--------------------------------------------------------------------===//

// taken from arrow's type.h
enum class PhysicalType : uint8_t {
	/// A NULL type having no physical storage
	NA = 0,

	/// Boolean as 8 bit "bool" value
	BOOL = 1,

	/// Unsigned 8-bit little-endian integer
	UINT8 = 2,

	/// Signed 8-bit little-endian integer
	INT8 = 3,

	/// Unsigned 16-bit little-endian integer
	UINT16 = 4,

	/// Signed 16-bit little-endian integer
	INT16 = 5,

	/// Unsigned 32-bit little-endian integer
	UINT32 = 6,

	/// Signed 32-bit little-endian integer
	INT32 = 7,

	/// Unsigned 64-bit little-endian integer
	UINT64 = 8,

	/// Signed 64-bit little-endian integer
	INT64 = 9,

	/// 2-byte floating point value
	HALF_FLOAT = 10,

	/// 4-byte floating point value
	FLOAT = 11,

	/// 8-byte floating point value
	DOUBLE = 12,

	/// UTF8 variable-length string as List<Char>
	STRING = 13,

	/// Variable-length bytes (no guarantee of UTF8-ness)
	BINARY = 14,

	/// Fixed-size binary. Each value occupies the same number of bytes
	FIXED_SIZE_BINARY = 15,

	/// int32_t days since the UNIX epoch
	DATE32 = 16,

	/// int64_t milliseconds since the UNIX epoch
	DATE64 = 17,

	/// Exact timestamp encoded with int64 since UNIX epoch
	/// Default unit millisecond
	TIMESTAMP = 18,

	/// Time as signed 32-bit integer, representing either seconds or
	/// milliseconds since midnight
	TIME32 = 19,

	/// Time as signed 64-bit integer, representing either microseconds or
	/// nanoseconds since midnight
	TIME64 = 20,

	/// YEAR_MONTH or DAY_TIME interval in SQL style
	INTERVAL = 21,

	/// Precision- and scale-based decimal type. Storage type depends on the
	/// parameters.
	// DECIMAL = 22,

	/// A list of some logical data type
	LIST = 23,

	/// Struct of logical types
	STRUCT = 24,

	/// Unions of logical types
	UNION = 25,

	/// Dictionary-encoded type, also called "categorical" or "factor"
	/// in other programming languages. Holds the dictionary value
	/// type but not the dictionary itself, which is part of the
	/// ArrayData struct
	DICTIONARY = 26,

	/// Map, a repeated struct logical type
	MAP = 27,

	/// Custom data type, implemented by user
	EXTENSION = 28,

	/// Fixed size list of some logical type
	FIXED_SIZE_LIST = 29,

	/// Measure of elapsed time in either seconds, milliseconds, microseconds
	/// or nanoseconds.
	DURATION = 30,

	/// Like STRING, but with 64-bit offsets
	LARGE_STRING = 31,

	/// Like BINARY, but with 64-bit offsets
	LARGE_BINARY = 32,

	/// Like LIST, but with 64-bit offsets
	LARGE_LIST = 33,

	// DuckDB Extensions
	VARCHAR = 200, // our own string representation, different from STRING and LARGE_STRING above
	POINTER = 202,
	HASH = 203,
	INT128 = 204, // 128-bit integers

	/// Boolean as 1 bit, LSB bit-packed ordering
	BIT = 205,

	INVALID = 255
};

//===--------------------------------------------------------------------===//
// SQL Types
//===--------------------------------------------------------------------===//
enum class LogicalTypeId : uint8_t {
	INVALID = 0,
	SQLNULL = 1, /* NULL type, used for constant NULL */
	UNKNOWN = 2, /* unknown type, used for parameter expressions */
	ANY = 3,     /* ANY type, used for functions that accept any type as parameter */

	BOOLEAN = 10,
	TINYINT = 11,
	SMALLINT = 12,
	INTEGER = 13,
	BIGINT = 14,
	DATE = 15,
	TIME = 16,
	TIMESTAMP_SEC = 17,
	TIMESTAMP_MS = 18,
	TIMESTAMP = 19, //! us
	TIMESTAMP_NS = 20,
	DECIMAL = 21,
	FLOAT = 22,
	DOUBLE = 23,
	CHAR = 24,
	VARCHAR = 25,
	BLOB = 26,
	INTERVAL = 27,
	UTINYINT = 28,
	USMALLINT = 29,
	UINTEGER = 30,
	UBIGINT = 31,


	HUGEINT = 50,
	POINTER = 51,
	HASH = 52,
	VALIDITY = 53,

	STRUCT = 100,
	LIST = 101,
	MAP = 102,
	TABLE = 103
};

struct ExtraTypeInfo;

struct LogicalType {
	DUCKDB_API LogicalType();
	DUCKDB_API LogicalType(LogicalTypeId id); // NOLINT: Allow implicit conversion from `LogicalTypeId`
	DUCKDB_API LogicalType(LogicalTypeId id, shared_ptr<ExtraTypeInfo> type_info);
	DUCKDB_API LogicalType(const LogicalType &other) :
		id_(other.id_), physical_type_(other.physical_type_), type_info_(other.type_info_) {}

	DUCKDB_API LogicalType(LogicalType &&other) :
		id_(other.id_), physical_type_(other.physical_type_), type_info_(move(other.type_info_)) {}

	DUCKDB_API ~LogicalType();

	LogicalTypeId id() const {
		return id_;
	}
	PhysicalType InternalType() const {
		return physical_type_;
	}
	const ExtraTypeInfo *AuxInfo() const {
		return type_info_.get();
	}

	// copy assignment
	LogicalType& operator=(const LogicalType &other) {
		id_ = other.id_;
		physical_type_ = other.physical_type_;
		type_info_ = other.type_info_;
		return *this;
	}
	// move assignment
	LogicalType& operator=(LogicalType&& other) {
		id_ = other.id_;
		physical_type_ = other.physical_type_;
		type_info_ = move(other.type_info_);
		return *this;
	}

	bool operator==(const LogicalType &rhs) const;
	bool operator!=(const LogicalType &rhs) const {
		return !(*this == rhs);
	}

	//! Serializes a LogicalType to a stand-alone binary blob
	DUCKDB_API void Serialize(Serializer &serializer) const;
	//! Deserializes a blob back into an LogicalType
	DUCKDB_API static LogicalType Deserialize(Deserializer &source);

	DUCKDB_API string ToString() const;
	DUCKDB_API bool IsIntegral() const;
	DUCKDB_API bool IsNumeric() const;
	DUCKDB_API bool IsMoreGenericThan(LogicalType &other) const;
	DUCKDB_API hash_t Hash() const;

	DUCKDB_API static LogicalType MaxLogicalType(const LogicalType &left, const LogicalType &right);

	//! Gets the decimal properties of a numeric type. Fails if the type is not numeric.
	DUCKDB_API bool GetDecimalProperties(uint8_t &width, uint8_t &scale) const;

	DUCKDB_API void Verify() const;

private:
	LogicalTypeId id_;
	PhysicalType physical_type_;
	shared_ptr<ExtraTypeInfo> type_info_;

private:
	PhysicalType GetInternalType();

public:
	DUCKDB_API static const LogicalType SQLNULL;
	DUCKDB_API static const LogicalType BOOLEAN;
	DUCKDB_API static const LogicalType TINYINT;
	DUCKDB_API static const LogicalType UTINYINT;
	DUCKDB_API static const LogicalType SMALLINT;
	DUCKDB_API static const LogicalType USMALLINT;
	DUCKDB_API static const LogicalType INTEGER;
	DUCKDB_API static const LogicalType UINTEGER;
	DUCKDB_API static const LogicalType BIGINT;
	DUCKDB_API static const LogicalType UBIGINT;
	DUCKDB_API static const LogicalType FLOAT;
	DUCKDB_API static const LogicalType DOUBLE;
	DUCKDB_API static const LogicalType DATE;
	DUCKDB_API static const LogicalType TIMESTAMP;
	DUCKDB_API static const LogicalType TIMESTAMP_S;
	DUCKDB_API static const LogicalType TIMESTAMP_MS;
	DUCKDB_API static const LogicalType TIMESTAMP_NS;
	DUCKDB_API static const LogicalType TIME;
	DUCKDB_API static const LogicalType VARCHAR;
	DUCKDB_API static const LogicalType ANY;
	DUCKDB_API static const LogicalType BLOB;
	DUCKDB_API static const LogicalType INTERVAL;
	DUCKDB_API static const LogicalType HUGEINT;
	DUCKDB_API static const LogicalType HASH;
	DUCKDB_API static const LogicalType POINTER;
	DUCKDB_API static const LogicalType TABLE;
	DUCKDB_API static const LogicalType INVALID;

	// explicitly allowing these functions to be capitalized to be in-line with the remaining functions
	DUCKDB_API static LogicalType DECIMAL(int width, int scale);                 // NOLINT
	DUCKDB_API static LogicalType VARCHAR_COLLATION(string collation);           // NOLINT
	DUCKDB_API static LogicalType LIST(LogicalType child);                       // NOLINT
	DUCKDB_API static LogicalType STRUCT(child_list_t<LogicalType> children);    // NOLINT
	DUCKDB_API static LogicalType MAP(child_list_t<LogicalType> children);       // NOLINT

	//! A list of all NUMERIC types (integral and floating point types)
	DUCKDB_API static const vector<LogicalType> NUMERIC;
	//! A list of all INTEGRAL types
	DUCKDB_API static const vector<LogicalType> INTEGRAL;
	//! A list of ALL SQL types
	DUCKDB_API static const vector<LogicalType> ALL_TYPES;
};

struct DecimalType {
	DUCKDB_API static uint8_t GetWidth(const LogicalType &type);
	DUCKDB_API static uint8_t GetScale(const LogicalType &type);
};

struct StringType {
	DUCKDB_API static string GetCollation(const LogicalType &type);
};

struct ListType {
	DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type);
};

struct StructType {
	DUCKDB_API static const child_list_t<LogicalType> &GetChildTypes(const LogicalType &type);
	DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type, idx_t index);
	DUCKDB_API static const string &GetChildName(const LogicalType &type, idx_t index);
	DUCKDB_API static idx_t GetChildCount(const LogicalType &type);
};


string LogicalTypeIdToString(LogicalTypeId type);

LogicalTypeId TransformStringToLogicalType(const string &str);

//! Returns the PhysicalType for the given type
template <class T>
PhysicalType GetTypeId() {
	if (std::is_same<T, bool>()) {
		return PhysicalType::BOOL;
	} else if (std::is_same<T, int8_t>()) {
		return PhysicalType::INT8;
	} else if (std::is_same<T, int16_t>()) {
		return PhysicalType::INT16;
	} else if (std::is_same<T, int32_t>()) {
		return PhysicalType::INT32;
	} else if (std::is_same<T, int64_t>()) {
		return PhysicalType::INT64;
	} else if (std::is_same<T, uint8_t>()) {
		return PhysicalType::UINT8;
	} else if (std::is_same<T, uint16_t>()) {
		return PhysicalType::UINT16;
	} else if (std::is_same<T, uint32_t>()) {
		return PhysicalType::UINT32;
	} else if (std::is_same<T, uint64_t>()) {
		return PhysicalType::UINT64;
	} else if (std::is_same<T, hugeint_t>()) {
		return PhysicalType::INT128;
	} else if (std::is_same<T, uint64_t>()) {
		return PhysicalType::HASH;
	} else if (std::is_same<T, uintptr_t>()) {
		return PhysicalType::POINTER;
	} else if (std::is_same<T, float>()) {
		return PhysicalType::FLOAT;
	} else if (std::is_same<T, double>()) {
		return PhysicalType::DOUBLE;
	} else if (std::is_same<T, const char *>() || std::is_same<T, char *>()) {
		return PhysicalType::VARCHAR;
	} else if (std::is_same<T, interval_t>()) {
		return PhysicalType::INTERVAL;
	} else {
		return PhysicalType::INVALID;
	}
}

template <class T>
bool IsValidType() {
	return GetTypeId<T>() != PhysicalType::INVALID;
}

//! The PhysicalType used by the row identifiers column
extern const LogicalType LOGICAL_ROW_TYPE;
extern const PhysicalType ROW_TYPE;

string TypeIdToString(PhysicalType type);
idx_t GetTypeIdSize(PhysicalType type);
bool TypeIsConstantSize(PhysicalType type);
bool TypeIsIntegral(PhysicalType type);
bool TypeIsNumeric(PhysicalType type);
bool TypeIsInteger(PhysicalType type);
bool TypeIsNested(PhysicalType type);

template <class T>
bool IsIntegerType() {
	return TypeIsIntegral(GetTypeId<T>());
}

bool ApproxEqual(float l, float r);
bool ApproxEqual(double l, double r);

} // namespace duckdb
