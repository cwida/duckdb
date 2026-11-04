#include "duckdb/main/capi/capi_internal.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

void PrepareCAPIVector(Vector &result) {
	switch (result.GetType().InternalType()) {
	case PhysicalType::STRUCT:
		for (auto &child : StructVector::GetEntries(result)) {
			PrepareCAPIVector(child);
		}
		return;
	case PhysicalType::ARRAY:
		PrepareCAPIVector(ArrayVector::GetChildMutable(result));
		return;
	case PhysicalType::LIST:
		PrepareCAPIVector(ListVector::GetChildMutable(result));
		break;
	default:
		break;
	}
	FlatVector::GetDataMutable(result);
}

LogicalTypeId LogicalTypeIdFromC(const duckdb_type type) {
	switch (type) {
	case DUCKDB_TYPE_INVALID:
		return LogicalTypeId::INVALID;
	case DUCKDB_TYPE_BOOLEAN:
		return LogicalTypeId::BOOLEAN;
	case DUCKDB_TYPE_TINYINT:
		return LogicalTypeId::TINYINT;
	case DUCKDB_TYPE_SMALLINT:
		return LogicalTypeId::SMALLINT;
	case DUCKDB_TYPE_INTEGER:
		return LogicalTypeId::INTEGER;
	case DUCKDB_TYPE_BIGINT:
		return LogicalTypeId::BIGINT;
	case DUCKDB_TYPE_UTINYINT:
		return LogicalTypeId::UTINYINT;
	case DUCKDB_TYPE_USMALLINT:
		return LogicalTypeId::USMALLINT;
	case DUCKDB_TYPE_UINTEGER:
		return LogicalTypeId::UINTEGER;
	case DUCKDB_TYPE_UBIGINT:
		return LogicalTypeId::UBIGINT;
	case DUCKDB_TYPE_FLOAT:
		return LogicalTypeId::FLOAT;
	case DUCKDB_TYPE_DOUBLE:
		return LogicalTypeId::DOUBLE;
	case DUCKDB_TYPE_TIMESTAMP:
		return LogicalTypeId::TIMESTAMP;
	case DUCKDB_TYPE_DATE:
		return LogicalTypeId::DATE;
	case DUCKDB_TYPE_TIME:
		return LogicalTypeId::TIME;
	case DUCKDB_TYPE_INTERVAL:
		return LogicalTypeId::INTERVAL;
	case DUCKDB_TYPE_HUGEINT:
		return LogicalTypeId::HUGEINT;
	case DUCKDB_TYPE_UHUGEINT:
		return LogicalTypeId::UHUGEINT;
	case DUCKDB_TYPE_VARCHAR:
		return LogicalTypeId::VARCHAR;
	case DUCKDB_TYPE_BLOB:
		return LogicalTypeId::BLOB;
	case DUCKDB_TYPE_DECIMAL:
		return LogicalTypeId::DECIMAL;
	case DUCKDB_TYPE_TIMESTAMP_S:
		return LogicalTypeId::TIMESTAMP_SEC;
	case DUCKDB_TYPE_TIMESTAMP_MS:
		return LogicalTypeId::TIMESTAMP_MS;
	case DUCKDB_TYPE_TIMESTAMP_NS:
		return LogicalTypeId::TIMESTAMP_NS;
	case DUCKDB_TYPE_ENUM:
		return LogicalTypeId::ENUM;
	case DUCKDB_TYPE_LIST:
		return LogicalTypeId::LIST;
	case DUCKDB_TYPE_STRUCT:
		return LogicalTypeId::STRUCT;
	case DUCKDB_TYPE_MAP:
		return LogicalTypeId::MAP;
	case DUCKDB_TYPE_ARRAY:
		return LogicalTypeId::ARRAY;
	case DUCKDB_TYPE_UUID:
		return LogicalTypeId::UUID;
	case DUCKDB_TYPE_UNION:
		return LogicalTypeId::UNION;
	case DUCKDB_TYPE_BIT:
		return LogicalTypeId::BIT;
	case DUCKDB_TYPE_TIME_TZ:
		return LogicalTypeId::TIME_TZ;
	case DUCKDB_TYPE_TIMESTAMP_TZ:
		return LogicalTypeId::TIMESTAMP_TZ;
	case DUCKDB_TYPE_TIMESTAMP_TZ_NS:
		return LogicalTypeId::TIMESTAMP_TZ_NS;
	case DUCKDB_TYPE_ANY:
		return LogicalTypeId::ANY;
	case DUCKDB_TYPE_BIGNUM:
		return LogicalTypeId::BIGNUM;
	case DUCKDB_TYPE_SQLNULL:
		return LogicalTypeId::SQLNULL;
	case DUCKDB_TYPE_STRING_LITERAL:
		return LogicalTypeId::STRING_LITERAL;
	case DUCKDB_TYPE_INTEGER_LITERAL:
		return LogicalTypeId::INTEGER_LITERAL;
	case DUCKDB_TYPE_TIME_NS:
		return LogicalTypeId::TIME_NS;
	case DUCKDB_TYPE_GEOMETRY:
		return LogicalTypeId::GEOMETRY;
	case DUCKDB_TYPE_GEOGRAPHY:
		return LogicalTypeId::GEOGRAPHY;
	case DUCKDB_TYPE_VARIANT:
		return LogicalTypeId::VARIANT;
	default: // LCOV_EXCL_START
		D_ASSERT(0);
		return LogicalTypeId::INVALID;
	} // LCOV_EXCL_STOP
}

duckdb_type LogicalTypeIdToC(const LogicalTypeId type) {
	switch (type) {
	case LogicalTypeId::INVALID:
		return DUCKDB_TYPE_INVALID;
	case LogicalTypeId::UNKNOWN:
		return DUCKDB_TYPE_INVALID;
	case LogicalTypeId::BOOLEAN:
		return DUCKDB_TYPE_BOOLEAN;
	case LogicalTypeId::TINYINT:
		return DUCKDB_TYPE_TINYINT;
	case LogicalTypeId::SMALLINT:
		return DUCKDB_TYPE_SMALLINT;
	case LogicalTypeId::INTEGER:
		return DUCKDB_TYPE_INTEGER;
	case LogicalTypeId::BIGINT:
		return DUCKDB_TYPE_BIGINT;
	case LogicalTypeId::UTINYINT:
		return DUCKDB_TYPE_UTINYINT;
	case LogicalTypeId::USMALLINT:
		return DUCKDB_TYPE_USMALLINT;
	case LogicalTypeId::UINTEGER:
		return DUCKDB_TYPE_UINTEGER;
	case LogicalTypeId::UBIGINT:
		return DUCKDB_TYPE_UBIGINT;
	case LogicalTypeId::HUGEINT:
		return DUCKDB_TYPE_HUGEINT;
	case LogicalTypeId::UHUGEINT:
		return DUCKDB_TYPE_UHUGEINT;
	case LogicalTypeId::FLOAT:
		return DUCKDB_TYPE_FLOAT;
	case LogicalTypeId::DOUBLE:
		return DUCKDB_TYPE_DOUBLE;
	case LogicalTypeId::TIMESTAMP:
		return DUCKDB_TYPE_TIMESTAMP;
	case LogicalTypeId::TIMESTAMP_TZ:
		return DUCKDB_TYPE_TIMESTAMP_TZ;
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return DUCKDB_TYPE_TIMESTAMP_TZ_NS;
	case LogicalTypeId::TIMESTAMP_SEC:
		return DUCKDB_TYPE_TIMESTAMP_S;
	case LogicalTypeId::TIMESTAMP_MS:
		return DUCKDB_TYPE_TIMESTAMP_MS;
	case LogicalTypeId::TIMESTAMP_NS:
		return DUCKDB_TYPE_TIMESTAMP_NS;
	case LogicalTypeId::DATE:
		return DUCKDB_TYPE_DATE;
	case LogicalTypeId::TIME:
		return DUCKDB_TYPE_TIME;
	case LogicalTypeId::TIME_TZ:
		return DUCKDB_TYPE_TIME_TZ;
	case LogicalTypeId::VARCHAR:
		return DUCKDB_TYPE_VARCHAR;
	case LogicalTypeId::BLOB:
		return DUCKDB_TYPE_BLOB;
	case LogicalTypeId::BIT:
		return DUCKDB_TYPE_BIT;
	case LogicalTypeId::BIGNUM:
		return DUCKDB_TYPE_BIGNUM;
	case LogicalTypeId::INTERVAL:
		return DUCKDB_TYPE_INTERVAL;
	case LogicalTypeId::DECIMAL:
		return DUCKDB_TYPE_DECIMAL;
	case LogicalTypeId::ENUM:
		return DUCKDB_TYPE_ENUM;
	case LogicalTypeId::LIST:
		return DUCKDB_TYPE_LIST;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		// TUPLE is an unnamed struct - reported as DUCKDB_TYPE_STRUCT over the C-API (child names are empty)
		return DUCKDB_TYPE_STRUCT;
	case LogicalTypeId::MAP:
		return DUCKDB_TYPE_MAP;
	case LogicalTypeId::UNION:
		return DUCKDB_TYPE_UNION;
	case LogicalTypeId::UUID:
		return DUCKDB_TYPE_UUID;
	case LogicalTypeId::ARRAY:
		return DUCKDB_TYPE_ARRAY;
	case LogicalTypeId::ANY:
		return DUCKDB_TYPE_ANY;
	case LogicalTypeId::SQLNULL:
		return DUCKDB_TYPE_SQLNULL;
	case LogicalTypeId::STRING_LITERAL:
		return DUCKDB_TYPE_STRING_LITERAL;
	case LogicalTypeId::INTEGER_LITERAL:
		return DUCKDB_TYPE_INTEGER_LITERAL;
	case LogicalTypeId::TIME_NS:
		return DUCKDB_TYPE_TIME_NS;
	case LogicalTypeId::GEOMETRY:
		return DUCKDB_TYPE_GEOMETRY;
	case LogicalTypeId::GEOGRAPHY:
		return DUCKDB_TYPE_GEOGRAPHY;
	case LogicalTypeId::VARIANT:
		return DUCKDB_TYPE_VARIANT;
	default: // LCOV_EXCL_START
		D_ASSERT(0);
		return DUCKDB_TYPE_INVALID;
	} // LCOV_EXCL_STOP
}

idx_t GetCTypeSize(const duckdb_type type) {
	switch (type) {
	case DUCKDB_TYPE_BOOLEAN:
		return sizeof(bool);
	case DUCKDB_TYPE_TINYINT:
		return sizeof(int8_t);
	case DUCKDB_TYPE_SMALLINT:
		return sizeof(int16_t);
	case DUCKDB_TYPE_INTEGER:
	case DUCKDB_TYPE_SQLNULL:
		return sizeof(int32_t);
	case DUCKDB_TYPE_BIGINT:
		return sizeof(int64_t);
	case DUCKDB_TYPE_UTINYINT:
		return sizeof(uint8_t);
	case DUCKDB_TYPE_USMALLINT:
		return sizeof(uint16_t);
	case DUCKDB_TYPE_UINTEGER:
		return sizeof(uint32_t);
	case DUCKDB_TYPE_UBIGINT:
		return sizeof(uint64_t);
	case DUCKDB_TYPE_UHUGEINT:
	case DUCKDB_TYPE_HUGEINT:
	case DUCKDB_TYPE_UUID:
		return sizeof(duckdb_hugeint);
	case DUCKDB_TYPE_FLOAT:
		return sizeof(float);
	case DUCKDB_TYPE_DOUBLE:
		return sizeof(double);
	case DUCKDB_TYPE_DATE:
		return sizeof(duckdb_date);
	case DUCKDB_TYPE_TIME:
		return sizeof(duckdb_time);
	case DUCKDB_TYPE_TIME_NS:
		return sizeof(duckdb_time_ns);
	case DUCKDB_TYPE_TIMESTAMP:
	case DUCKDB_TYPE_TIMESTAMP_TZ:
	case DUCKDB_TYPE_TIMESTAMP_S:
	case DUCKDB_TYPE_TIMESTAMP_MS:
	case DUCKDB_TYPE_TIMESTAMP_NS:
	case DUCKDB_TYPE_TIMESTAMP_TZ_NS:
		return sizeof(duckdb_timestamp);
	case DUCKDB_TYPE_VARCHAR:
		return sizeof(const char *);
	case DUCKDB_TYPE_BLOB:
		return sizeof(duckdb_blob);
	case DUCKDB_TYPE_INTERVAL:
		return sizeof(duckdb_interval);
	case DUCKDB_TYPE_DECIMAL:
		return sizeof(duckdb_hugeint);
	default: // LCOV_EXCL_START
		// Unsupported nested or complex type. Internally, we set the null mask to NULL.
		// This is a deprecated code path. Use the Vector Interface for nested and complex types.
		return 0;
	} // LCOV_EXCL_STOP
}

duckdb_statement_type StatementTypeToC(const StatementType type) {
	switch (type) {
	case StatementType::SELECT_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_SELECT;
	case StatementType::INVALID_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_INVALID;
	case StatementType::INSERT_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_INSERT;
	case StatementType::UPDATE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_UPDATE;
	case StatementType::EXPLAIN_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_EXPLAIN;
	case StatementType::DELETE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_DELETE;
	case StatementType::PREPARE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_PREPARE;
	case StatementType::CREATE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_CREATE;
	case StatementType::EXECUTE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_EXECUTE;
	case StatementType::ALTER_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_ALTER;
	case StatementType::TRANSACTION_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_TRANSACTION;
	case StatementType::COPY_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_COPY;
	case StatementType::ANALYZE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_ANALYZE;
	case StatementType::VARIABLE_SET_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_VARIABLE_SET;
	case StatementType::CREATE_FUNC_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_CREATE_FUNC;
	case StatementType::DROP_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_DROP;
	case StatementType::EXPORT_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_EXPORT;
	case StatementType::PRAGMA_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_PRAGMA;
	case StatementType::VACUUM_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_VACUUM;
	case StatementType::CALL_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_CALL;
	case StatementType::SET_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_SET;
	case StatementType::LOAD_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_LOAD;
	case StatementType::RELATION_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_RELATION;
	case StatementType::EXTENSION_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_EXTENSION;
	case StatementType::LOGICAL_PLAN_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_LOGICAL_PLAN;
	case StatementType::ATTACH_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_ATTACH;
	case StatementType::DETACH_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_DETACH;
	case StatementType::MULTI_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_MULTI;
	case StatementType::COPY_DATABASE_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_COPY_DATABASE;
	case StatementType::UPDATE_EXTENSIONS_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_UPDATE_EXTENSIONS;
	case StatementType::MERGE_INTO_STATEMENT:
		return DUCKDB_STATEMENT_TYPE_MERGE_INTO;
	default:
		return DUCKDB_STATEMENT_TYPE_INVALID;
	}
}

duckdb_error_type ErrorTypeToC(const ExceptionType type) {
	switch (type) {
	case ExceptionType::INVALID:
		return DUCKDB_ERROR_INVALID;
	case ExceptionType::OUT_OF_RANGE:
		return DUCKDB_ERROR_OUT_OF_RANGE;
	case ExceptionType::CONVERSION:
		return DUCKDB_ERROR_CONVERSION;
	case ExceptionType::UNKNOWN_TYPE:
		return DUCKDB_ERROR_UNKNOWN_TYPE;
	case ExceptionType::DECIMAL:
		return DUCKDB_ERROR_DECIMAL;
	case ExceptionType::MISMATCH_TYPE:
		return DUCKDB_ERROR_MISMATCH_TYPE;
	case ExceptionType::DIVIDE_BY_ZERO:
		return DUCKDB_ERROR_DIVIDE_BY_ZERO;
	case ExceptionType::OBJECT_SIZE:
		return DUCKDB_ERROR_OBJECT_SIZE;
	case ExceptionType::INVALID_TYPE:
		return DUCKDB_ERROR_INVALID_TYPE;
	case ExceptionType::SERIALIZATION:
		return DUCKDB_ERROR_SERIALIZATION;
	case ExceptionType::TRANSACTION:
		return DUCKDB_ERROR_TRANSACTION;
	case ExceptionType::NOT_IMPLEMENTED:
		return DUCKDB_ERROR_NOT_IMPLEMENTED;
	case ExceptionType::EXPRESSION:
		return DUCKDB_ERROR_EXPRESSION;
	case ExceptionType::CATALOG:
		return DUCKDB_ERROR_CATALOG;
	case ExceptionType::PARSER:
		return DUCKDB_ERROR_PARSER;
	case ExceptionType::PLANNER:
		return DUCKDB_ERROR_PLANNER;
	case ExceptionType::SCHEDULER:
		return DUCKDB_ERROR_SCHEDULER;
	case ExceptionType::EXECUTOR:
		return DUCKDB_ERROR_EXECUTOR;
	case ExceptionType::CONSTRAINT:
		return DUCKDB_ERROR_CONSTRAINT;
	case ExceptionType::INDEX:
		return DUCKDB_ERROR_INDEX;
	case ExceptionType::STAT:
		return DUCKDB_ERROR_STAT;
	case ExceptionType::CONNECTION:
		return DUCKDB_ERROR_CONNECTION;
	case ExceptionType::SYNTAX:
		return DUCKDB_ERROR_SYNTAX;
	case ExceptionType::SETTINGS:
		return DUCKDB_ERROR_SETTINGS;
	case ExceptionType::BINDER:
		return DUCKDB_ERROR_BINDER;
	case ExceptionType::NETWORK:
		return DUCKDB_ERROR_NETWORK;
	case ExceptionType::OPTIMIZER:
		return DUCKDB_ERROR_OPTIMIZER;
	case ExceptionType::NULL_POINTER:
		return DUCKDB_ERROR_NULL_POINTER;
	case ExceptionType::IO:
		return DUCKDB_ERROR_IO;
	case ExceptionType::INTERRUPT:
		return DUCKDB_ERROR_INTERRUPT;
	case ExceptionType::FATAL:
		return DUCKDB_ERROR_FATAL;
	case ExceptionType::INTERNAL:
		return DUCKDB_ERROR_INTERNAL;
	case ExceptionType::INVALID_INPUT:
		return DUCKDB_ERROR_INVALID_INPUT;
	case ExceptionType::OUT_OF_MEMORY:
		return DUCKDB_ERROR_OUT_OF_MEMORY;
	case ExceptionType::PERMISSION:
		return DUCKDB_ERROR_PERMISSION;
	case ExceptionType::PARAMETER_NOT_RESOLVED:
		return DUCKDB_ERROR_PARAMETER_NOT_RESOLVED;
	case ExceptionType::PARAMETER_NOT_ALLOWED:
		return DUCKDB_ERROR_PARAMETER_NOT_ALLOWED;
	case ExceptionType::DEPENDENCY:
		return DUCKDB_ERROR_DEPENDENCY;
	case ExceptionType::HTTP:
		return DUCKDB_ERROR_HTTP;
	case ExceptionType::MISSING_EXTENSION:
		return DUCKDB_ERROR_MISSING_EXTENSION;
	case ExceptionType::AUTOLOAD:
		return DUCKDB_ERROR_AUTOLOAD;
	case ExceptionType::SEQUENCE:
		return DUCKDB_ERROR_SEQUENCE;
	case ExceptionType::DATA_CORRUPTION:
		return DUCKDB_ERROR_DATA_CORRUPTION;
	case ExceptionType::INVALID_CONFIGURATION:
		return DUCKDB_INVALID_CONFIGURATION;
	case ExceptionType::RESOURCE_IN_USE:
		return DUCKDB_ERROR_RESOURCE_IN_USE;
	default:
		return DUCKDB_ERROR_INVALID;
	}
}

ExceptionType ErrorTypeFromC(const duckdb_error_type type) {
	switch (type) {
	case DUCKDB_ERROR_INVALID:
		return ExceptionType::INVALID;
	case DUCKDB_ERROR_OUT_OF_RANGE:
		return ExceptionType::OUT_OF_RANGE;
	case DUCKDB_ERROR_CONVERSION:
		return ExceptionType::CONVERSION;
	case DUCKDB_ERROR_UNKNOWN_TYPE:
		return ExceptionType::UNKNOWN_TYPE;
	case DUCKDB_ERROR_DECIMAL:
		return ExceptionType::DECIMAL;
	case DUCKDB_ERROR_MISMATCH_TYPE:
		return ExceptionType::MISMATCH_TYPE;
	case DUCKDB_ERROR_DIVIDE_BY_ZERO:
		return ExceptionType::DIVIDE_BY_ZERO;
	case DUCKDB_ERROR_OBJECT_SIZE:
		return ExceptionType::OBJECT_SIZE;
	case DUCKDB_ERROR_INVALID_TYPE:
		return ExceptionType::INVALID_TYPE;
	case DUCKDB_ERROR_SERIALIZATION:
		return ExceptionType::SERIALIZATION;
	case DUCKDB_ERROR_TRANSACTION:
		return ExceptionType::TRANSACTION;
	case DUCKDB_ERROR_NOT_IMPLEMENTED:
		return ExceptionType::NOT_IMPLEMENTED;
	case DUCKDB_ERROR_EXPRESSION:
		return ExceptionType::EXPRESSION;
	case DUCKDB_ERROR_CATALOG:
		return ExceptionType::CATALOG;
	case DUCKDB_ERROR_PARSER:
		return ExceptionType::PARSER;
	case DUCKDB_ERROR_PLANNER:
		return ExceptionType::PLANNER;
	case DUCKDB_ERROR_SCHEDULER:
		return ExceptionType::SCHEDULER;
	case DUCKDB_ERROR_EXECUTOR:
		return ExceptionType::EXECUTOR;
	case DUCKDB_ERROR_CONSTRAINT:
		return ExceptionType::CONSTRAINT;
	case DUCKDB_ERROR_INDEX:
		return ExceptionType::INDEX;
	case DUCKDB_ERROR_STAT:
		return ExceptionType::STAT;
	case DUCKDB_ERROR_CONNECTION:
		return ExceptionType::CONNECTION;
	case DUCKDB_ERROR_SYNTAX:
		return ExceptionType::SYNTAX;
	case DUCKDB_ERROR_SETTINGS:
		return ExceptionType::SETTINGS;
	case DUCKDB_ERROR_BINDER:
		return ExceptionType::BINDER;
	case DUCKDB_ERROR_NETWORK:
		return ExceptionType::NETWORK;
	case DUCKDB_ERROR_OPTIMIZER:
		return ExceptionType::OPTIMIZER;
	case DUCKDB_ERROR_NULL_POINTER:
		return ExceptionType::NULL_POINTER;
	case DUCKDB_ERROR_IO:
		return ExceptionType::IO;
	case DUCKDB_ERROR_INTERRUPT:
		return ExceptionType::INTERRUPT;
	case DUCKDB_ERROR_FATAL:
		return ExceptionType::FATAL;
	case DUCKDB_ERROR_INTERNAL:
		return ExceptionType::INTERNAL;
	case DUCKDB_ERROR_INVALID_INPUT:
		return ExceptionType::INVALID_INPUT;
	case DUCKDB_ERROR_OUT_OF_MEMORY:
		return ExceptionType::OUT_OF_MEMORY;
	case DUCKDB_ERROR_PERMISSION:
		return ExceptionType::PERMISSION;
	case DUCKDB_ERROR_PARAMETER_NOT_RESOLVED:
		return ExceptionType::PARAMETER_NOT_RESOLVED;
	case DUCKDB_ERROR_PARAMETER_NOT_ALLOWED:
		return ExceptionType::PARAMETER_NOT_ALLOWED;
	case DUCKDB_ERROR_DEPENDENCY:
		return ExceptionType::DEPENDENCY;
	case DUCKDB_ERROR_HTTP:
		return ExceptionType::HTTP;
	case DUCKDB_ERROR_MISSING_EXTENSION:
		return ExceptionType::MISSING_EXTENSION;
	case DUCKDB_ERROR_AUTOLOAD:
		return ExceptionType::AUTOLOAD;
	case DUCKDB_ERROR_SEQUENCE:
		return ExceptionType::SEQUENCE;
	case DUCKDB_ERROR_DATA_CORRUPTION:
		return ExceptionType::DATA_CORRUPTION;
	case DUCKDB_INVALID_CONFIGURATION:
		return ExceptionType::INVALID_CONFIGURATION;
	case DUCKDB_ERROR_RESOURCE_IN_USE:
		return ExceptionType::RESOURCE_IN_USE;
	default:
		return ExceptionType::INVALID;
	}
}

} // namespace duckdb

void *duckdb_malloc(size_t size) {
	return malloc(size);
}

void duckdb_free(void *ptr) {
	free(ptr);
}

idx_t duckdb_vector_size() {
	return STANDARD_VECTOR_SIZE;
}

bool duckdb_string_is_inlined(duckdb_string_t string_p) {
	static_assert(sizeof(duckdb_string_t) == sizeof(duckdb::string_t),
	              "duckdb_string_t should have the same memory layout as duckdb::string_t");
	auto &string = *reinterpret_cast<duckdb::string_t *>(&string_p);
	return string.IsInlined();
}

uint32_t duckdb_string_t_length(duckdb_string_t string_p) {
	static_assert(sizeof(duckdb_string_t) == sizeof(duckdb::string_t),
	              "duckdb_string_t should have the same memory layout as duckdb::string_t");
	auto &string = *reinterpret_cast<duckdb::string_t *>(&string_p);
	return static_cast<uint32_t>(string.GetSize());
}

const char *duckdb_string_t_data(duckdb_string_t *string_p) {
	static_assert(sizeof(duckdb_string_t) == sizeof(duckdb::string_t),
	              "duckdb_string_t should have the same memory layout as duckdb::string_t");
	auto &string = *reinterpret_cast<duckdb::string_t *>(string_p);
	return string.GetData();
}

duckdb_error_data duckdb_valid_utf8_check(const char *str, idx_t len) {
	duckdb::UnicodeInvalidReason reason;
	size_t pos;
	auto utf_type = duckdb::Utf8Proc::Analyze(str, len, &reason, &pos);
	if (utf_type == duckdb::UnicodeType::INVALID) {
		return duckdb_create_error_data(DUCKDB_ERROR_INVALID_INPUT,
		                                "invalid Unicode detected, str must be valid UTF-8");
	}
	return nullptr;
}
