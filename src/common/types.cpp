#include "duckdb/common/types.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/decimal.hpp"

#include <cmath>

namespace duckdb {

LogicalType::LogicalType() : LogicalType(LogicalTypeId::INVALID) {
}

LogicalType::LogicalType(LogicalTypeId id) : id_(id) {
	physical_type_ = GetInternalType();
}
LogicalType::LogicalType(LogicalTypeId id, shared_ptr<ExtraTypeInfo> type_info_p)
    : id_(id), type_info_(move(type_info_p)) {
	physical_type_ = GetInternalType();
}

hash_t LogicalType::Hash() const {
	return duckdb::Hash<uint8_t>((uint8_t)id_);
}

PhysicalType LogicalType::GetInternalType() {
	switch (id_) {
	case LogicalTypeId::BOOLEAN:
		return PhysicalType::BOOL;
	case LogicalTypeId::TINYINT:
		return PhysicalType::INT8;
	case LogicalTypeId::UTINYINT:
		return PhysicalType::UINT8;
	case LogicalTypeId::SMALLINT:
		return PhysicalType::INT16;
	case LogicalTypeId::USMALLINT:
		return PhysicalType::UINT16;
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTEGER:
		return PhysicalType::INT32;
	case LogicalTypeId::UINTEGER:
		return PhysicalType::UINT32;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
		return PhysicalType::INT64;
	case LogicalTypeId::UBIGINT:
		return PhysicalType::UINT64;
	case LogicalTypeId::HUGEINT:
		return PhysicalType::INT128;
	case LogicalTypeId::FLOAT:
		return PhysicalType::FLOAT;
	case LogicalTypeId::DOUBLE:
		return PhysicalType::DOUBLE;
	case LogicalTypeId::DECIMAL: {
		if (!type_info_) {
			return PhysicalType::INVALID;
		}
		auto width = DecimalType::GetWidth(*this);
		if (width <= Decimal::MAX_WIDTH_INT16) {
			return PhysicalType::INT16;
		} else if (width <= Decimal::MAX_WIDTH_INT32) {
			return PhysicalType::INT32;
		} else if (width <= Decimal::MAX_WIDTH_INT64) {
			return PhysicalType::INT64;
		} else if (width <= Decimal::MAX_WIDTH_INT128) {
			return PhysicalType::INT128;
		} else {
			throw InternalException("Widths bigger than 38 are not supported");
		}
	}
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::CHAR:
	case LogicalTypeId::BLOB:
		return PhysicalType::VARCHAR;
	case LogicalTypeId::INTERVAL:
		return PhysicalType::INTERVAL;
	case LogicalTypeId::MAP:
	case LogicalTypeId::STRUCT:
		return PhysicalType::STRUCT;
	case LogicalTypeId::LIST:
		return PhysicalType::LIST;
	case LogicalTypeId::HASH:
		return PhysicalType::HASH;
	case LogicalTypeId::POINTER:
		return PhysicalType::POINTER;
	case LogicalTypeId::VALIDITY:
		return PhysicalType::BIT;
	case LogicalTypeId::TABLE:
	case LogicalTypeId::ANY:
	case LogicalTypeId::INVALID:
	case LogicalTypeId::UNKNOWN:
		return PhysicalType::INVALID;
	default:
		throw InternalException("Invalid LogicalType %s", ToString());
	}
}

const LogicalType LogicalType::INVALID = LogicalType(LogicalTypeId::INVALID);
const LogicalType LogicalType::SQLNULL = LogicalType(LogicalTypeId::SQLNULL);
const LogicalType LogicalType::BOOLEAN = LogicalType(LogicalTypeId::BOOLEAN);
const LogicalType LogicalType::TINYINT = LogicalType(LogicalTypeId::TINYINT);
const LogicalType LogicalType::UTINYINT = LogicalType(LogicalTypeId::UTINYINT);
const LogicalType LogicalType::SMALLINT = LogicalType(LogicalTypeId::SMALLINT);
const LogicalType LogicalType::USMALLINT = LogicalType(LogicalTypeId::USMALLINT);
const LogicalType LogicalType::INTEGER = LogicalType(LogicalTypeId::INTEGER);
const LogicalType LogicalType::UINTEGER = LogicalType(LogicalTypeId::UINTEGER);
const LogicalType LogicalType::BIGINT = LogicalType(LogicalTypeId::BIGINT);
const LogicalType LogicalType::UBIGINT = LogicalType(LogicalTypeId::UBIGINT);
const LogicalType LogicalType::HUGEINT = LogicalType(LogicalTypeId::HUGEINT);
const LogicalType LogicalType::FLOAT = LogicalType(LogicalTypeId::FLOAT);
const LogicalType LogicalType::DOUBLE = LogicalType(LogicalTypeId::DOUBLE);
const LogicalType LogicalType::DATE = LogicalType(LogicalTypeId::DATE);

const LogicalType LogicalType::TIMESTAMP = LogicalType(LogicalTypeId::TIMESTAMP);
const LogicalType LogicalType::TIMESTAMP_MS = LogicalType(LogicalTypeId::TIMESTAMP_MS);
const LogicalType LogicalType::TIMESTAMP_NS = LogicalType(LogicalTypeId::TIMESTAMP_NS);
const LogicalType LogicalType::TIMESTAMP_S = LogicalType(LogicalTypeId::TIMESTAMP_SEC);

const LogicalType LogicalType::TIME = LogicalType(LogicalTypeId::TIME);
const LogicalType LogicalType::HASH = LogicalType(LogicalTypeId::HASH);
const LogicalType LogicalType::POINTER = LogicalType(LogicalTypeId::POINTER);

const LogicalType LogicalType::VARCHAR = LogicalType(LogicalTypeId::VARCHAR);

const LogicalType LogicalType::BLOB = LogicalType(LogicalTypeId::BLOB);
const LogicalType LogicalType::INTERVAL = LogicalType(LogicalTypeId::INTERVAL);

// TODO these are incomplete and should maybe not exist as such
const LogicalType LogicalType::TABLE = LogicalType(LogicalTypeId::TABLE);

const LogicalType LogicalType::ANY = LogicalType(LogicalTypeId::ANY);

const vector<LogicalType> LogicalType::NUMERIC = {LogicalType::TINYINT,   LogicalType::SMALLINT,  LogicalType::INTEGER,
                                                  LogicalType::BIGINT,    LogicalType::HUGEINT,   LogicalType::FLOAT,
                                                  LogicalType::DOUBLE,    LogicalTypeId::DECIMAL, LogicalType::UTINYINT,
                                                  LogicalType::USMALLINT, LogicalType::UINTEGER,  LogicalType::UBIGINT};

const vector<LogicalType> LogicalType::INTEGRAL = {LogicalType::TINYINT,   LogicalType::SMALLINT, LogicalType::INTEGER,
                                                   LogicalType::BIGINT,    LogicalType::HUGEINT,  LogicalType::UTINYINT,
                                                   LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT};

const vector<LogicalType> LogicalType::ALL_TYPES = {
    LogicalType::BOOLEAN,  LogicalType::TINYINT,   LogicalType::SMALLINT,  LogicalType::INTEGER,
    LogicalType::BIGINT,   LogicalType::DATE,      LogicalType::TIMESTAMP, LogicalType::DOUBLE,
    LogicalType::FLOAT,    LogicalType::VARCHAR,   LogicalType::BLOB,      LogicalType::INTERVAL,
    LogicalType::HUGEINT,  LogicalTypeId::DECIMAL, LogicalType::UTINYINT,  LogicalType::USMALLINT,
    LogicalType::UINTEGER, LogicalType::UBIGINT,   LogicalType::TIME,      LogicalTypeId::LIST,
    LogicalTypeId::STRUCT, LogicalTypeId::MAP};

const LogicalType LOGICAL_ROW_TYPE = LogicalType::BIGINT;
const PhysicalType ROW_TYPE = PhysicalType::INT64;

// LCOV_EXCL_START
string TypeIdToString(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL:
		return "BOOL";
	case PhysicalType::INT8:
		return "INT8";
	case PhysicalType::INT16:
		return "INT16";
	case PhysicalType::INT32:
		return "INT32";
	case PhysicalType::INT64:
		return "INT64";
	case PhysicalType::UINT8:
		return "UINT8";
	case PhysicalType::UINT16:
		return "UINT16";
	case PhysicalType::UINT32:
		return "UINT32";
	case PhysicalType::UINT64:
		return "UINT64";
	case PhysicalType::INT128:
		return "INT128";
	case PhysicalType::HASH:
		return "HASH";
	case PhysicalType::POINTER:
		return "POINTER";
	case PhysicalType::FLOAT:
		return "FLOAT";
	case PhysicalType::DOUBLE:
		return "DOUBLE";
	case PhysicalType::VARCHAR:
		return "VARCHAR";
	case PhysicalType::INTERVAL:
		return "INTERVAL";
	case PhysicalType::STRUCT:
		return "STRUCT<?>";
	case PhysicalType::LIST:
		return "LIST<?>";
	case PhysicalType::INVALID:
		return "INVALID";
	case PhysicalType::BIT:
		return "BIT";
	case PhysicalType::NA:
		return "NA";
	case PhysicalType::HALF_FLOAT:
		return "HALF_FLOAT";
	case PhysicalType::STRING:
		return "ARROW_STRING";
	case PhysicalType::BINARY:
		return "BINARY";
	case PhysicalType::FIXED_SIZE_BINARY:
		return "FIXED_SIZE_BINARY";
	case PhysicalType::DATE32:
		return "DATE32";
	case PhysicalType::DATE64:
		return "DATE64";
	case PhysicalType::TIMESTAMP:
		return "TIMESTAMP";
	case PhysicalType::TIME32:
		return "TIME32";
	case PhysicalType::TIME64:
		return "TIME64";
	case PhysicalType::UNION:
		return "UNION";
	case PhysicalType::DICTIONARY:
		return "DICTIONARY";
	case PhysicalType::MAP:
		return "MAP";
	case PhysicalType::EXTENSION:
		return "EXTENSION";
	case PhysicalType::FIXED_SIZE_LIST:
		return "FIXED_SIZE_LIST";
	case PhysicalType::DURATION:
		return "DURATION";
	case PhysicalType::LARGE_STRING:
		return "LARGE_STRING";
	case PhysicalType::LARGE_BINARY:
		return "LARGE_BINARY";
	case PhysicalType::LARGE_LIST:
		return "LARGE_LIST";
	}
	return "INVALID";
}
// LCOV_EXCL_STOP

idx_t GetTypeIdSize(PhysicalType type) {
	switch (type) {
	case PhysicalType::BIT:
	case PhysicalType::BOOL:
		return sizeof(bool);
	case PhysicalType::INT8:
		return sizeof(int8_t);
	case PhysicalType::INT16:
		return sizeof(int16_t);
	case PhysicalType::INT32:
		return sizeof(int32_t);
	case PhysicalType::INT64:
		return sizeof(int64_t);
	case PhysicalType::UINT8:
		return sizeof(uint8_t);
	case PhysicalType::UINT16:
		return sizeof(uint16_t);
	case PhysicalType::UINT32:
		return sizeof(uint32_t);
	case PhysicalType::UINT64:
		return sizeof(uint64_t);
	case PhysicalType::INT128:
		return sizeof(hugeint_t);
	case PhysicalType::FLOAT:
		return sizeof(float);
	case PhysicalType::DOUBLE:
		return sizeof(double);
	case PhysicalType::HASH:
		return sizeof(hash_t);
	case PhysicalType::POINTER:
		return sizeof(uintptr_t);
	case PhysicalType::VARCHAR:
		return sizeof(string_t);
	case PhysicalType::INTERVAL:
		return sizeof(interval_t);
	case PhysicalType::STRUCT:
		return 0; // no own payload
	case PhysicalType::LIST:
		return sizeof(list_entry_t); // offset + len
	default:
		throw InternalException("Invalid PhysicalType for GetTypeIdSize");
	}
}

bool TypeIsConstantSize(PhysicalType type) {
	return (type >= PhysicalType::BOOL && type <= PhysicalType::DOUBLE) ||
	       (type >= PhysicalType::FIXED_SIZE_BINARY && type <= PhysicalType::INTERVAL) || type == PhysicalType::HASH ||
	       type == PhysicalType::POINTER || type == PhysicalType::INTERVAL || type == PhysicalType::INT128;
}
bool TypeIsIntegral(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::INT64) || type == PhysicalType::HASH ||
	       type == PhysicalType::POINTER || type == PhysicalType::INT128;
}
bool TypeIsNumeric(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::DOUBLE) || type == PhysicalType::INT128;
}
bool TypeIsInteger(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::INT64) || type == PhysicalType::INT128;
}

// LCOV_EXCL_START
string LogicalTypeIdToString(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::TINYINT:
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::INTEGER:
		return "INTEGER";
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::HUGEINT:
		return "HUGEINT";
	case LogicalTypeId::UTINYINT:
		return "UTINYINT";
	case LogicalTypeId::USMALLINT:
		return "USMALLINT";
	case LogicalTypeId::UINTEGER:
		return "UINTEGER";
	case LogicalTypeId::UBIGINT:
		return "UBIGINT";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME";
	case LogicalTypeId::TIMESTAMP:
		return "TIMESTAMP";
	case LogicalTypeId::TIMESTAMP_MS:
		return "TIMESTAMP (MS)";
	case LogicalTypeId::TIMESTAMP_NS:
		return "TIMESTAMP (NS)";
	case LogicalTypeId::TIMESTAMP_SEC:
		return "TIMESTAMP (SEC)";
	case LogicalTypeId::FLOAT:
		return "FLOAT";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case LogicalTypeId::DECIMAL:
		return "DECIMAL";
	case LogicalTypeId::VARCHAR:
		return "VARCHAR";
	case LogicalTypeId::BLOB:
		return "BLOB";
	case LogicalTypeId::CHAR:
		return "CHAR";
	case LogicalTypeId::INTERVAL:
		return "INTERVAL";
	case LogicalTypeId::SQLNULL:
		return "NULL";
	case LogicalTypeId::ANY:
		return "ANY";
	case LogicalTypeId::VALIDITY:
		return "VALIDITY";
	case LogicalTypeId::STRUCT:
		return "STRUCT<?>";
	case LogicalTypeId::LIST:
		return "LIST<?>";
	case LogicalTypeId::MAP:
		return "MAP<?>";
	case LogicalTypeId::HASH:
		return "HASH";
	case LogicalTypeId::POINTER:
		return "POINTER";
	case LogicalTypeId::TABLE:
		return "TABLE";
	case LogicalTypeId::INVALID:
		return "INVALID";
	case LogicalTypeId::UNKNOWN:
		return "UNKNOWN";
	}
	return "UNDEFINED";
}

string LogicalType::ToString() const {
	switch (id_) {
	case LogicalTypeId::STRUCT: {
		if (!type_info_) {
			return "STRUCT";
		}
		auto &child_types = StructType::GetChildTypes(*this);
		string ret = "STRUCT<";
		for (size_t i = 0; i < child_types.size(); i++) {
			ret += child_types[i].first + ": " + child_types[i].second.ToString();
			if (i < child_types.size() - 1) {
				ret += ", ";
			}
		}
		ret += ">";
		return ret;
	}
	case LogicalTypeId::LIST: {
		if (!type_info_) {
			return "LIST";
		}
		return "LIST<" + ListType::GetChildType(*this).ToString() + ">";
	}
	case LogicalTypeId::MAP: {
		if (!type_info_) {
			return "MAP";
		}
		auto &child_types = StructType::GetChildTypes(*this);
		if (child_types.empty()) {
			return "MAP<?>";
		}
		if (child_types.size() != 2) {
			throw InternalException("Map needs exactly two child elements");
		}
		return "MAP<" + ListType::GetChildType(child_types[0].second).ToString() + ", " +
		       ListType::GetChildType(child_types[1].second).ToString() + ">";
	}
	case LogicalTypeId::DECIMAL: {
		if (!type_info_) {
			return "DECIMAL";
		}
		auto width = DecimalType::GetWidth(*this);
		auto scale = DecimalType::GetScale(*this);
		if (width == 0) {
			return "DECIMAL";
		}
		return StringUtil::Format("DECIMAL(%d,%d)", width, scale);
	}
	default:
		return LogicalTypeIdToString(id_);
	}
}
// LCOV_EXCL_STOP

LogicalTypeId TransformStringToLogicalType(const string &str) {
	auto lower_str = StringUtil::Lower(str);
	// Transform column type
	if (lower_str == "int" || lower_str == "int4" || lower_str == "signed" || lower_str == "integer" ||
	    lower_str == "integral" || lower_str == "int32") {
		return LogicalTypeId::INTEGER;
	} else if (lower_str == "varchar" || lower_str == "bpchar" || lower_str == "text" || lower_str == "string" ||
	           lower_str == "char") {
		return LogicalTypeId::VARCHAR;
	} else if (lower_str == "bytea" || lower_str == "blob" || lower_str == "varbinary" || lower_str == "binary") {
		return LogicalTypeId::BLOB;
	} else if (lower_str == "int8" || lower_str == "bigint" || lower_str == "int64" || lower_str == "long" ||
	           lower_str == "oid") {
		return LogicalTypeId::BIGINT;
	} else if (lower_str == "int2" || lower_str == "smallint" || lower_str == "short" || lower_str == "int16") {
		return LogicalTypeId::SMALLINT;
	} else if (lower_str == "timestamp" || lower_str == "datetime" || lower_str == "timestamp_us") {
		return LogicalTypeId::TIMESTAMP;
	} else if (lower_str == "timestamp_ms") {
		return LogicalTypeId::TIMESTAMP_MS;
	} else if (lower_str == "timestamp_ns") {
		return LogicalTypeId::TIMESTAMP_NS;
	} else if (lower_str == "timestamp_s") {
		return LogicalTypeId::TIMESTAMP_SEC;
	} else if (lower_str == "bool" || lower_str == "boolean" || lower_str == "logical") {
		return LogicalTypeId::BOOLEAN;
	} else if (lower_str == "real" || lower_str == "float4" || lower_str == "float") {
		return LogicalTypeId::FLOAT;
	} else if (lower_str == "decimal" || lower_str == "dec" || lower_str == "numeric") {
		return LogicalTypeId::DECIMAL;
	} else if (lower_str == "double" || lower_str == "float8" || lower_str == "decimal") {
		return LogicalTypeId::DOUBLE;
	} else if (lower_str == "tinyint" || lower_str == "int1") {
		return LogicalTypeId::TINYINT;
	} else if (lower_str == "date") {
		return LogicalTypeId::DATE;
	} else if (lower_str == "time") {
		return LogicalTypeId::TIME;
	} else if (lower_str == "interval") {
		return LogicalTypeId::INTERVAL;
	} else if (lower_str == "hugeint" || lower_str == "int128") {
		return LogicalTypeId::HUGEINT;
	} else if (lower_str == "struct" || lower_str == "row") {
		return LogicalTypeId::STRUCT;
	} else if (lower_str == "map") {
		return LogicalTypeId::MAP;
	} else if (lower_str == "utinyint" || lower_str == "uint8") {
		return LogicalTypeId::UTINYINT;
	} else if (lower_str == "usmallint" || lower_str == "uint16") {
		return LogicalTypeId::USMALLINT;
	} else if (lower_str == "uinteger" || lower_str == "uint32") {
		return LogicalTypeId::UINTEGER;
	} else if (lower_str == "ubigint" || lower_str == "uint64") {
		return LogicalTypeId::UBIGINT;
	} else {
		throw NotImplementedException("DataType %s not supported yet...\n", str);
	}
}

bool LogicalType::IsIntegral() const {
	switch (id_) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
		return true;
	default:
		return false;
	}
}

bool LogicalType::IsNumeric() const {
	switch (id_) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
		return true;
	default:
		return false;
	}
}

bool LogicalType::GetDecimalProperties(uint8_t &width, uint8_t &scale) const {
	switch (id_) {
	case LogicalTypeId::SQLNULL:
		width = 0;
		scale = 0;
		break;
	case LogicalTypeId::BOOLEAN:
		width = 1;
		scale = 0;
		break;
	case LogicalTypeId::TINYINT:
		// tinyint: [-127, 127] = DECIMAL(3,0)
		width = 3;
		scale = 0;
		break;
	case LogicalTypeId::SMALLINT:
		// smallint: [-32767, 32767] = DECIMAL(5,0)
		width = 5;
		scale = 0;
		break;
	case LogicalTypeId::INTEGER:
		// integer: [-2147483647, 2147483647] = DECIMAL(10,0)
		width = 10;
		scale = 0;
		break;
	case LogicalTypeId::BIGINT:
		// bigint: [-9223372036854775807, 9223372036854775807] = DECIMAL(19,0)
		width = 19;
		scale = 0;
		break;
	case LogicalTypeId::UTINYINT:
		// UInt8 — [0 : 255]
		width = 3;
		scale = 0;
		break;
	case LogicalTypeId::USMALLINT:
		// UInt16 — [0 : 65535]
		width = 5;
		scale = 0;
		break;
	case LogicalTypeId::UINTEGER:
		// UInt32 — [0 : 4294967295]
		width = 10;
		scale = 0;
		break;
	case LogicalTypeId::UBIGINT:
		// UInt64 — [0 : 18446744073709551615]
		width = 20;
		scale = 0;
		break;
	case LogicalTypeId::HUGEINT:
		// hugeint: max size decimal (38, 0)
		// note that a hugeint is not guaranteed to fit in this
		width = 38;
		scale = 0;
		break;
	case LogicalTypeId::DECIMAL:
		width = DecimalType::GetWidth(*this);
		scale = DecimalType::GetScale(*this);
		break;
	default:
		return false;
	}
	return true;
}

LogicalType LogicalType::MaxLogicalType(const LogicalType &left, const LogicalType &right) {
	if (left.id() < right.id()) {
		return right;
	} else if (right.id() < left.id()) {
		return left;
	} else {
		if (left.id() == LogicalTypeId::VARCHAR) {
			// varchar: use type that has collation (if any)
			if (StringType::GetCollation(right).empty()) {
				return left;
			} else {
				return right;
			}
		} else if (left.id() == LogicalTypeId::DECIMAL) {
			// use max width/scale of the two types
			auto width = MaxValue<uint8_t>(DecimalType::GetWidth(left), DecimalType::GetWidth(right));
			auto scale = MaxValue<uint8_t>(DecimalType::GetScale(left), DecimalType::GetScale(right));
			return LogicalType::DECIMAL(width, scale);
		} else if (left.id() == LogicalTypeId::LIST) {
			// list: perform max recursively on child type
			auto new_child = MaxLogicalType(ListType::GetChildType(left), ListType::GetChildType(right));
			return LogicalType::LIST(move(new_child));
		} else if (left.id() == LogicalTypeId::STRUCT) {
			// struct: perform recursively
			auto &left_child_types = StructType::GetChildTypes(left);
			auto &right_child_types = StructType::GetChildTypes(right);
			if (left_child_types.size() != right_child_types.size()) {
				// child types are not of equal size, we can't cast anyway
				// just return the left child
				return left;
			}
			child_list_t<LogicalType> child_types;
			for (idx_t i = 0; i < left_child_types.size(); i++) {
				auto child_type = MaxLogicalType(left_child_types[i].second, right_child_types[i].second);
				child_types.push_back(make_pair(left_child_types[i].first, move(child_type)));
			}
			return LogicalType::STRUCT(move(child_types));
		} else {
			// types are equal but no extra specifier: just return the type
			return left;
		}
	}
}

void LogicalType::Verify() const {
#ifdef DEBUG
	if (id_ == LogicalTypeId::DECIMAL) {
		D_ASSERT(DecimalType::GetWidth(*this) >= 1 && DecimalType::GetWidth(*this) <= Decimal::MAX_WIDTH_DECIMAL);
		D_ASSERT(DecimalType::GetScale(*this) >= 0 && DecimalType::GetScale(*this) <= DecimalType::GetWidth(*this));
	}
#endif
}

bool ApproxEqual(float ldecimal, float rdecimal) {
	float epsilon = std::fabs(rdecimal) * 0.01;
	return std::fabs(ldecimal - rdecimal) <= epsilon;
}

bool ApproxEqual(double ldecimal, double rdecimal) {
	double epsilon = std::fabs(rdecimal) * 0.01;
	return std::fabs(ldecimal - rdecimal) <= epsilon;
}

//===--------------------------------------------------------------------===//
// Extra Type Info
//===--------------------------------------------------------------------===//
enum class ExtraTypeInfoType : uint8_t {
	INVALID_TYPE_INFO = 0,
	DECIMAL_TYPE_INFO = 1,
	STRING_TYPE_INFO = 2,
	LIST_TYPE_INFO = 3,
	STRUCT_TYPE_INFO = 4
};

struct ExtraTypeInfo {
	explicit ExtraTypeInfo(ExtraTypeInfoType type) : type(type) {
	}
	virtual ~ExtraTypeInfo() {
	}

	ExtraTypeInfoType type;

public:
	virtual bool Equals(ExtraTypeInfo *other) = 0;
	//! Serializes a ExtraTypeInfo to a stand-alone binary blob
	virtual void Serialize(Serializer &serializer) const = 0;
	//! Serializes a ExtraTypeInfo to a stand-alone binary blob
	static void Serialize(ExtraTypeInfo *info, Serializer &serializer);
	//! Deserializes a blob back into an ExtraTypeInfo
	static shared_ptr<ExtraTypeInfo> Deserialize(Deserializer &source);
};

//===--------------------------------------------------------------------===//
// Decimal Type
//===--------------------------------------------------------------------===//
struct DecimalTypeInfo : public ExtraTypeInfo {
	DecimalTypeInfo(uint8_t width_p, uint8_t scale_p)
	    : ExtraTypeInfo(ExtraTypeInfoType::DECIMAL_TYPE_INFO), width(width_p), scale(scale_p) {
	}

	uint8_t width;
	uint8_t scale;

public:
	bool Equals(ExtraTypeInfo *other_p) override {
		if (!other_p) {
			return false;
		}
		if (type != other_p->type) {
			return false;
		}
		auto &other = (DecimalTypeInfo &)*other_p;
		return width == other.width && scale == other.scale;
	}

	void Serialize(Serializer &serializer) const override {
		serializer.Write<uint8_t>(width);
		serializer.Write<uint8_t>(scale);
	}

	static shared_ptr<ExtraTypeInfo> Deserialize(Deserializer &source) {
		auto width = source.Read<uint8_t>();
		auto scale = source.Read<uint8_t>();
		return make_shared<DecimalTypeInfo>(width, scale);
	}
};

uint8_t DecimalType::GetWidth(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
	auto info = type.AuxInfo();
	D_ASSERT(info);
	return ((DecimalTypeInfo &)*info).width;
}

uint8_t DecimalType::GetScale(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
	auto info = type.AuxInfo();
	D_ASSERT(info);
	return ((DecimalTypeInfo &)*info).scale;
}

LogicalType LogicalType::DECIMAL(int width, int scale) {
	auto type_info = make_shared<DecimalTypeInfo>(width, scale);
	return LogicalType(LogicalTypeId::DECIMAL, move(type_info));
}

//===--------------------------------------------------------------------===//
// String Type
//===--------------------------------------------------------------------===//
struct StringTypeInfo : public ExtraTypeInfo {
	explicit StringTypeInfo(string collation_p)
	    : ExtraTypeInfo(ExtraTypeInfoType::STRING_TYPE_INFO), collation(move(collation_p)) {
	}

	string collation;

public:
	bool Equals(ExtraTypeInfo *other_p) override {
		// collation info has no impact on equality
		return true;
	}

	void Serialize(Serializer &serializer) const override {
		serializer.WriteString(collation);
	}

	static shared_ptr<ExtraTypeInfo> Deserialize(Deserializer &source) {
		auto collation = source.Read<string>();
		return make_shared<StringTypeInfo>(move(collation));
	}
};

string StringType::GetCollation(const LogicalType &type) {
	if (type.id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	auto info = type.AuxInfo();
	if (!info) {
		return string();
	}
	return ((StringTypeInfo &)*info).collation;
}

LogicalType LogicalType::VARCHAR_COLLATION(string collation) { // NOLINT
	auto string_info = make_shared<StringTypeInfo>(move(collation));
	return LogicalType(LogicalTypeId::VARCHAR, move(string_info));
}

//===--------------------------------------------------------------------===//
// List Type
//===--------------------------------------------------------------------===//
struct ListTypeInfo : public ExtraTypeInfo {
	explicit ListTypeInfo(LogicalType child_type_p)
	    : ExtraTypeInfo(ExtraTypeInfoType::LIST_TYPE_INFO), child_type(move(child_type_p)) {
	}

	LogicalType child_type;

public:
	bool Equals(ExtraTypeInfo *other_p) override {
		if (!other_p) {
			return false;
		}
		if (type != other_p->type) {
			return false;
		}
		auto &other = (ListTypeInfo &)*other_p;
		return child_type == other.child_type;
	}

	void Serialize(Serializer &serializer) const override {
		child_type.Serialize(serializer);
	}

	static shared_ptr<ExtraTypeInfo> Deserialize(Deserializer &source) {
		auto child_type = LogicalType::Deserialize(source);
		return make_shared<ListTypeInfo>(move(child_type));
	}
};

const LogicalType &ListType::GetChildType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::LIST);
	auto info = type.AuxInfo();
	D_ASSERT(info);
	return ((ListTypeInfo &)*info).child_type;
}

LogicalType LogicalType::LIST(LogicalType child) {
	auto info = make_shared<ListTypeInfo>(move(child));
	return LogicalType(LogicalTypeId::LIST, move(info));
}

//===--------------------------------------------------------------------===//
// Struct Type
//===--------------------------------------------------------------------===//
struct StructTypeInfo : public ExtraTypeInfo {
	explicit StructTypeInfo(child_list_t<LogicalType> child_types_p)
	    : ExtraTypeInfo(ExtraTypeInfoType::STRUCT_TYPE_INFO), child_types(move(child_types_p)) {
	}

	child_list_t<LogicalType> child_types;

public:
	bool Equals(ExtraTypeInfo *other_p) override {
		if (!other_p) {
			return false;
		}
		if (type != other_p->type) {
			return false;
		}
		auto &other = (StructTypeInfo &)*other_p;
		return child_types == other.child_types;
	}

	void Serialize(Serializer &serializer) const override {
		serializer.Write<uint32_t>(child_types.size());
		for (idx_t i = 0; i < child_types.size(); i++) {
			serializer.WriteString(child_types[i].first);
			child_types[i].second.Serialize(serializer);
		}
	}

	static shared_ptr<ExtraTypeInfo> Deserialize(Deserializer &source) {
		child_list_t<LogicalType> child_list;
		auto child_types_size = source.Read<uint32_t>();
		for (uint32_t i = 0; i < child_types_size; i++) {
			auto name = source.Read<string>();
			auto type = LogicalType::Deserialize(source);
			child_list.push_back(make_pair(move(name), move(type)));
		}
		return make_shared<StructTypeInfo>(move(child_list));
	}
};

const child_list_t<LogicalType> &StructType::GetChildTypes(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::STRUCT || type.id() == LogicalTypeId::MAP);
	auto info = type.AuxInfo();
	D_ASSERT(info);
	return ((StructTypeInfo &)*info).child_types;
}

const LogicalType &StructType::GetChildType(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	return child_types[index].second;
}

const string &StructType::GetChildName(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	return child_types[index].first;
}

idx_t StructType::GetChildCount(const LogicalType &type) {
	return StructType::GetChildTypes(type).size();
}

LogicalType LogicalType::STRUCT(child_list_t<LogicalType> children) {
	auto info = make_shared<StructTypeInfo>(move(children));
	return LogicalType(LogicalTypeId::STRUCT, move(info));
}

LogicalType LogicalType::MAP(child_list_t<LogicalType> children) {
	auto info = make_shared<StructTypeInfo>(move(children));
	return LogicalType(LogicalTypeId::MAP, move(info));
}

//===--------------------------------------------------------------------===//
// Extra Type Info
//===--------------------------------------------------------------------===//
void ExtraTypeInfo::Serialize(ExtraTypeInfo *info, Serializer &serializer) {
	if (!info) {
		serializer.Write<ExtraTypeInfoType>(ExtraTypeInfoType::INVALID_TYPE_INFO);
	} else {
		serializer.Write<ExtraTypeInfoType>(info->type);
		info->Serialize(serializer);
	}
}

shared_ptr<ExtraTypeInfo> ExtraTypeInfo::Deserialize(Deserializer &source) {
	auto type = source.Read<ExtraTypeInfoType>();
	switch (type) {
	case ExtraTypeInfoType::INVALID_TYPE_INFO:
		return nullptr;
	case ExtraTypeInfoType::DECIMAL_TYPE_INFO:
		return DecimalTypeInfo::Deserialize(source);
	case ExtraTypeInfoType::STRING_TYPE_INFO:
		return StringTypeInfo::Deserialize(source);
	case ExtraTypeInfoType::LIST_TYPE_INFO:
		return ListTypeInfo::Deserialize(source);
	case ExtraTypeInfoType::STRUCT_TYPE_INFO:
		return StructTypeInfo::Deserialize(source);
	default:
		throw InternalException("Unimplemented type info in ExtraTypeInfo::Deserialize");
	}
}

//===--------------------------------------------------------------------===//
// Logical Type
//===--------------------------------------------------------------------===//

// the destructor needs to know about the extra type info
LogicalType::~LogicalType() {
}

void LogicalType::Serialize(Serializer &serializer) const {
	serializer.Write<LogicalTypeId>(id_);
	ExtraTypeInfo::Serialize(type_info_.get(), serializer);
}

LogicalType LogicalType::Deserialize(Deserializer &source) {
	auto id = source.Read<LogicalTypeId>();
	auto info = ExtraTypeInfo::Deserialize(source);

	return LogicalType(id, move(info));
}

bool LogicalType::operator==(const LogicalType &rhs) const {
	if (id_ != rhs.id_) {
		return false;
	}
	if (type_info_.get() == rhs.type_info_.get()) {
		return true;
	}
	if (type_info_) {
		return type_info_->Equals(rhs.type_info_.get());
	} else {
		D_ASSERT(rhs.type_info_);
		return rhs.type_info_->Equals(type_info_.get());
	}
}

} // namespace duckdb
