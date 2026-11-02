#include "duckdb/common/types.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/function/cast_rules.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/common/types/type_manager.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/keyword_helper.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

#include <cmath>
#include "duckdb/common/types/geometry.hpp"

namespace duckdb {

constexpr idx_t ArrayType::MAX_ARRAY_SIZE;
const idx_t UnionType::MAX_UNION_MEMBERS;

//! Computes the physical type of a type with the given id and no parameters - returns false for an unknown id
static bool TryGetBuiltinPhysicalType(LogicalTypeId id, PhysicalType &result) noexcept {
	switch (id) {
	case LogicalTypeId::BOOLEAN:
		result = PhysicalType::BOOL;
		return true;
	case LogicalTypeId::TINYINT:
		result = PhysicalType::INT8;
		return true;
	case LogicalTypeId::UTINYINT:
		result = PhysicalType::UINT8;
		return true;
	case LogicalTypeId::SMALLINT:
		result = PhysicalType::INT16;
		return true;
	case LogicalTypeId::USMALLINT:
		result = PhysicalType::UINT16;
		return true;
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTEGER:
		result = PhysicalType::INT32;
		return true;
	case LogicalTypeId::UINTEGER:
		result = PhysicalType::UINT32;
		return true;
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		result = PhysicalType::INT64;
		return true;
	case LogicalTypeId::UBIGINT:
		result = PhysicalType::UINT64;
		return true;
	case LogicalTypeId::UHUGEINT:
		result = PhysicalType::UINT128;
		return true;
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UUID:
		result = PhysicalType::INT128;
		return true;
	case LogicalTypeId::FLOAT:
		result = PhysicalType::FLOAT;
		return true;
	case LogicalTypeId::DOUBLE:
		result = PhysicalType::DOUBLE;
		return true;
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::CHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::GEOMETRY:
		result = PhysicalType::VARCHAR;
		return true;
	case LogicalTypeId::INTERVAL:
		result = PhysicalType::INTERVAL;
		return true;
	case LogicalTypeId::UNION:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		result = PhysicalType::STRUCT;
		return true;
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		result = PhysicalType::LIST;
		return true;
	case LogicalTypeId::ARRAY:
		result = PhysicalType::ARRAY;
		return true;
	case LogicalTypeId::POINTER:
		static_assert(sizeof(uintptr_t) == sizeof(uint32_t) || sizeof(uintptr_t) == sizeof(uint64_t),
		              "Unsupported pointer size");
		result = sizeof(uintptr_t) == sizeof(uint32_t) ? PhysicalType::UINT32 : PhysicalType::UINT64;
		return true;
	case LogicalTypeId::VALIDITY:
		result = PhysicalType::BIT;
		return true;
	case LogicalTypeId::LAMBDA:
		// a lambda has no value of its own - it occupies an argument slot that holds a constant placeholder
		result = PhysicalType::UINT8;
		return true;
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::TABLE:
	case LogicalTypeId::ANY:
	case LogicalTypeId::INVALID:
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::STRING_LITERAL:
	case LogicalTypeId::INTEGER_LITERAL:
	case LogicalTypeId::TEMPLATE:
		result = PhysicalType::INVALID;
		return true;
	case LogicalTypeId::UNBOUND:
		result = PhysicalType::UNKNOWN;
		return true;
	default:
		return false;
	}
}

//! Computes the physical type of a type with the given id and info - returns false for an unknown id
static bool TryGetPhysicalType(LogicalTypeId id, const LogicalTypeInfo &info, PhysicalType &result) {
	switch (id) {
	case LogicalTypeId::DECIMAL: {
		if (info.type != LogicalTypeInfoType::DECIMAL_TYPE_INFO) {
			break;
		}
		auto width = info.Cast<DecimalTypeInfo>().width;
		if (width <= Decimal::MAX_WIDTH_INT16) {
			result = PhysicalType::INT16;
		} else if (width <= Decimal::MAX_WIDTH_INT32) {
			result = PhysicalType::INT32;
		} else if (width <= Decimal::MAX_WIDTH_INT64) {
			result = PhysicalType::INT64;
		} else if (width <= Decimal::MAX_WIDTH_INT128) {
			result = PhysicalType::INT128;
		} else {
			throw InternalException("Decimal has a width of %d which is bigger than the maximum supported width of %d",
			                        width, DecimalType::MaxWidth());
		}
		return true;
	}
	case LogicalTypeId::ENUM:
		if (info.type != LogicalTypeInfoType::ENUM_TYPE_INFO) {
			break;
		}
		result = EnumTypeInfo::DictType(info.Cast<EnumTypeInfo>().GetDictSize());
		return true;
	default:
		break;
	}
	return TryGetBuiltinPhysicalType(id, result);
}

namespace {

constexpr idx_t BUILTIN_TYPE_INFO_COUNT = NumericLimits<uint8_t>::Maximum() + 1;

struct BuiltinTypeInfos {
	//! The info for each id - nullptr for an invalid id
	const LogicalTypeInfo *infos[BUILTIN_TYPE_INFO_COUNT] = {};
	//! The infos are created in place, so creating them never allocates
	alignas(LogicalTypeInfo) char storage[BUILTIN_TYPE_INFO_COUNT][sizeof(LogicalTypeInfo)];
};

} // namespace

const LogicalTypeInfo *LogicalType::TryGetBuiltinTypeInfo(LogicalTypeId id) noexcept {
	static const auto *builtins = []() noexcept {
		// zero-initialized and never destroyed - types can still be destroyed during static destruction
		static BuiltinTypeInfos result;
		for (idx_t i = 0; i < BUILTIN_TYPE_INFO_COUNT; i++) {
			auto builtin_id = static_cast<LogicalTypeId>(i);
			auto &info = *new (result.storage[i]) LogicalTypeInfo(LogicalTypeInfoType::INVALID_TYPE_INFO);
			if (TryGetBuiltinPhysicalType(builtin_id, info.physical_type)) {
				info.id = builtin_id;
				info.immortal = true;
				result.infos[i] = &info;
			}
		}
		return &result;
	}();
	return builtins->infos[static_cast<uint8_t>(id)];
}

const LogicalTypeInfo &LogicalType::GetBuiltinTypeInfo(LogicalTypeId id) {
	auto info = TryGetBuiltinTypeInfo(id);
	if (!info) {
		throw InternalException("Invalid LogicalType %d", static_cast<uint8_t>(id));
	}
	return *info;
}

LogicalType::LogicalType() : LogicalType(LogicalTypeId::INVALID) {
}

LogicalType::LogicalType(LogicalTypeId id) : type_info_(GetBuiltinTypeInfo(id)) {
}

const LogicalTypeInfo &LogicalType::AdoptNewTypeInfo(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info) {
	// validates the id - and ensures the builtin infos exist before any type does
	auto &builtin_type_info = GetBuiltinTypeInfo(id);
	if (!type_info) {
		return builtin_type_info;
	}
	if (!TryGetPhysicalType(id, *type_info, type_info->physical_type)) {
		throw InternalException("Invalid LogicalType %d", static_cast<uint8_t>(id));
	}
	type_info->id = id;
	type_info->ref_count = 1;
	return *type_info.release();
}

LogicalType::LogicalType(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info)
    : type_info_(AdoptNewTypeInfo(id, std::move(type_info))) {
}

LogicalType::LogicalType(LogicalType &&other) noexcept : type_info_(other.type_info_) {
	// the moved-from type keeps its id, but loses its parameters
	// it may come from another copy of DuckDB (e.g. the host of a loadable extension that statically links its own
	// copy), so this can be the first use of the builtins of this copy
	other.type_info_ = *TryGetBuiltinTypeInfo(type_info_.get().id);
}

const LogicalTypeInfo &LogicalType::ReleaseTypeInfo() && {
	auto &result = type_info_.get();
	type_info_ = *TryGetBuiltinTypeInfo(result.id);
	return result;
}

LogicalType LogicalType::AdoptTypeInfo(const LogicalTypeInfo &type_info) {
	return LogicalType(type_info);
}

LogicalType LogicalType::CreateImmortal(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info) {
	type_info->immortal = true;
	return LogicalType(id, std::move(type_info));
}

hash_t LogicalType::Hash() const {
	return duckdb::Hash<uint8_t>((uint8_t)id());
}

bool LogicalType::HasParameters() const {
	auto info_type = type_info_.get().type;
	switch (id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return info_type == LogicalTypeInfoType::LIST_TYPE_INFO;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::UNION:
	case LogicalTypeId::VARIANT:
		return info_type == LogicalTypeInfoType::STRUCT_TYPE_INFO;
	case LogicalTypeId::ARRAY:
		return info_type == LogicalTypeInfoType::ARRAY_TYPE_INFO;
	case LogicalTypeId::DECIMAL:
		return info_type == LogicalTypeInfoType::DECIMAL_TYPE_INFO;
	case LogicalTypeId::ENUM:
		return info_type == LogicalTypeInfoType::ENUM_TYPE_INFO;
	case LogicalTypeId::VARCHAR:
		return info_type == LogicalTypeInfoType::STRING_TYPE_INFO;
	case LogicalTypeId::ANY:
		return info_type == LogicalTypeInfoType::ANY_TYPE_INFO;
	case LogicalTypeId::INTEGER_LITERAL:
		return info_type == LogicalTypeInfoType::INTEGER_LITERAL_TYPE_INFO;
	case LogicalTypeId::TEMPLATE:
		return info_type == LogicalTypeInfoType::TEMPLATE_TYPE_INFO;
	case LogicalTypeId::GEOMETRY:
		return info_type == LogicalTypeInfoType::GEO_TYPE_INFO;
	case LogicalTypeId::UNBOUND:
		return info_type == LogicalTypeInfoType::UNBOUND_TYPE_INFO;
	default:
		return false;
	}
}

// **DEPRECATED**: Use EnumUtil directly instead.
string LogicalTypeIdToString(LogicalTypeId type) {
	return EnumUtil::ToString(type);
}

constexpr const LogicalTypeId LogicalType::INVALID;
constexpr const LogicalTypeId LogicalType::SQLNULL;
constexpr const LogicalTypeId LogicalType::UNKNOWN;
constexpr const LogicalTypeId LogicalType::BOOLEAN;
constexpr const LogicalTypeId LogicalType::TINYINT;
constexpr const LogicalTypeId LogicalType::UTINYINT;
constexpr const LogicalTypeId LogicalType::SMALLINT;
constexpr const LogicalTypeId LogicalType::USMALLINT;
constexpr const LogicalTypeId LogicalType::INTEGER;
constexpr const LogicalTypeId LogicalType::UINTEGER;
constexpr const LogicalTypeId LogicalType::BIGINT;
constexpr const LogicalTypeId LogicalType::UBIGINT;
constexpr const LogicalTypeId LogicalType::HUGEINT;
constexpr const LogicalTypeId LogicalType::UHUGEINT;
constexpr const LogicalTypeId LogicalType::UUID;
constexpr const LogicalTypeId LogicalType::FLOAT;
constexpr const LogicalTypeId LogicalType::DOUBLE;
constexpr const LogicalTypeId LogicalType::DATE;

constexpr const LogicalTypeId LogicalType::TIMESTAMP;
constexpr const LogicalTypeId LogicalType::TIMESTAMP_MS;
constexpr const LogicalTypeId LogicalType::TIMESTAMP_NS;
constexpr const LogicalTypeId LogicalType::TIMESTAMP_S;

constexpr const LogicalTypeId LogicalType::TIME;
constexpr const LogicalTypeId LogicalType::TIME_NS;

constexpr const LogicalTypeId LogicalType::TIME_TZ;
constexpr const LogicalTypeId LogicalType::TIMESTAMP_TZ;
constexpr const LogicalTypeId LogicalType::TIMESTAMP_TZ_NS;

constexpr const LogicalTypeId LogicalType::HASH;
constexpr const LogicalTypeId LogicalType::POINTER;

constexpr const LogicalTypeId LogicalType::VARCHAR;

constexpr const LogicalTypeId LogicalType::BLOB;
constexpr const LogicalTypeId LogicalType::BIT;
constexpr const LogicalTypeId LogicalType::BIGNUM;

constexpr const LogicalTypeId LogicalType::INTERVAL;
constexpr const LogicalTypeId LogicalType::ROW_TYPE;

// TODO these are incomplete and should maybe not exist as such
constexpr const LogicalTypeId LogicalType::TABLE;
constexpr const LogicalTypeId LogicalType::LAMBDA;

constexpr const LogicalTypeId LogicalType::ANY;

const vector<LogicalType> LogicalType::Numeric() {
	vector<LogicalType> types = {LogicalType::TINYINT,   LogicalType::SMALLINT,  LogicalType::INTEGER,
	                             LogicalType::BIGINT,    LogicalType::HUGEINT,   LogicalType::FLOAT,
	                             LogicalType::DOUBLE,    LogicalTypeId::DECIMAL, LogicalType::UTINYINT,
	                             LogicalType::USMALLINT, LogicalType::UINTEGER,  LogicalType::UBIGINT,
	                             LogicalType::UHUGEINT};
	return types;
}

const vector<LogicalType> LogicalType::Integral() {
	vector<LogicalType> types = {LogicalType::TINYINT,   LogicalType::SMALLINT, LogicalType::INTEGER,
	                             LogicalType::BIGINT,    LogicalType::HUGEINT,  LogicalType::UTINYINT,
	                             LogicalType::USMALLINT, LogicalType::UINTEGER, LogicalType::UBIGINT,
	                             LogicalType::UHUGEINT};
	return types;
}

const vector<LogicalType> LogicalType::Real() {
	vector<LogicalType> types = {LogicalType::FLOAT, LogicalType::DOUBLE};
	return types;
}

const vector<LogicalType> LogicalType::AllTypes() {
	vector<LogicalType> types = {
	    LogicalTypeId::BOOLEAN,   LogicalTypeId::TINYINT,       LogicalTypeId::SMALLINT,
	    LogicalTypeId::INTEGER,   LogicalTypeId::BIGINT,        LogicalTypeId::DATE,
	    LogicalTypeId::TIME,      LogicalTypeId::TIMESTAMP_SEC, LogicalTypeId::TIMESTAMP_MS,
	    LogicalTypeId::TIMESTAMP, LogicalTypeId::TIMESTAMP_NS,  LogicalTypeId::DECIMAL,
	    LogicalTypeId::FLOAT,     LogicalTypeId::DOUBLE,        LogicalTypeId::CHAR,
	    LogicalTypeId::VARCHAR,   LogicalTypeId::BLOB,          LogicalTypeId::INTERVAL,
	    LogicalTypeId::UTINYINT,  LogicalTypeId::USMALLINT,     LogicalTypeId::UINTEGER,
	    LogicalTypeId::UBIGINT,   LogicalTypeId::TIMESTAMP_TZ,  LogicalTypeId::TIMESTAMP_TZ_NS,
	    LogicalTypeId::TIME_TZ,   LogicalTypeId::TIME_NS,       LogicalTypeId::BIT,
	    LogicalTypeId::BIGNUM,    LogicalTypeId::UHUGEINT,      LogicalTypeId::HUGEINT,
	    LogicalTypeId::UUID,      LogicalTypeId::GEOMETRY,      LogicalTypeId::STRUCT,
	    LogicalTypeId::TUPLE,     LogicalTypeId::LIST,          LogicalTypeId::MAP,
	    LogicalTypeId::ENUM,      LogicalTypeId::UNION,         LogicalTypeId::ARRAY,
	    LogicalTypeId::VARIANT,
	};
	return types;
}

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
	case PhysicalType::UINT128:
		return "UINT128";
	case PhysicalType::FLOAT:
		return "FLOAT";
	case PhysicalType::DOUBLE:
		return "DOUBLE";
	case PhysicalType::VARCHAR:
		return "VARCHAR";
	case PhysicalType::INTERVAL:
		return "INTERVAL";
	case PhysicalType::STRUCT:
		return "STRUCT";
	case PhysicalType::LIST:
		return "LIST";
	case PhysicalType::ARRAY:
		return "ARRAY";
	case PhysicalType::INVALID:
		return "INVALID";
	case PhysicalType::BIT:
		return "BIT";
	case PhysicalType::UNKNOWN:
		return "UNKNOWN";
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
	case PhysicalType::UINT128:
		return sizeof(uhugeint_t);
	case PhysicalType::FLOAT:
		return sizeof(float);
	case PhysicalType::DOUBLE:
		return sizeof(double);
	case PhysicalType::VARCHAR:
		return sizeof(string_t);
	case PhysicalType::INTERVAL:
		return sizeof(interval_t);
	case PhysicalType::STRUCT:
	case PhysicalType::UNKNOWN:
	case PhysicalType::ARRAY:
		return 0; // no own payload
	case PhysicalType::LIST:
		return sizeof(list_entry_t); // offset + len
	default:
		throw InternalException("Invalid PhysicalType for GetTypeIdSize");
	}
}

bool TypeIsConstantSize(PhysicalType type) {
	return (type >= PhysicalType::BOOL && type <= PhysicalType::DOUBLE) || type == PhysicalType::INTERVAL ||
	       type == PhysicalType::INT128 || type == PhysicalType::UINT128;
}
bool TypeIsIntegral(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::INT64) || type == PhysicalType::INT128 ||
	       type == PhysicalType::UINT128;
}
bool TypeIsNumeric(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::DOUBLE) || type == PhysicalType::INT128 ||
	       type == PhysicalType::UINT128;
}
bool TypeIsInteger(PhysicalType type) {
	return (type >= PhysicalType::UINT8 && type <= PhysicalType::INT64) || type == PhysicalType::INT128 ||
	       type == PhysicalType::UINT128;
}

static string TypeModifierListToString(const vector<LogicalTypeModifier> &mod_list) {
	string result;
	if (mod_list.empty()) {
		return result;
	}
	result = "(";
	for (idx_t i = 0; i < mod_list.size(); i++) {
		result += mod_list[i].ToString();
		if (i < mod_list.size() - 1) {
			result += ", ";
		}
	}
	result += ")";
	return result;
}

string LogicalType::ToString() const {
	// nested types are rendered with an explicit stack to avoid unbounded recursion
	struct StackEntry {
		optional_ptr<const LogicalType> type;
		string text;
	};
	string result;
	vector<StackEntry> stack;
	auto push_text = [&](string text) {
		stack.push_back(StackEntry {nullptr, std::move(text)});
	};
	auto push_type = [&](const LogicalType &type) {
		stack.push_back(StackEntry {&type, string()});
	};
	push_type(*this);

	while (!stack.empty()) {
		auto entry = std::move(stack.back());
		stack.pop_back();
		if (!entry.type) {
			result += entry.text;
			continue;
		}
		auto &type = *entry.type;
		if (type.id() != LogicalTypeId::UNBOUND) {
			auto alias = type.GetAlias();
			if (!alias.empty()) {
				if (type.HasExtensionInfo()) {
					auto &ext_info = *type.GetExtensionInfo();
					alias += TypeModifierListToString(ext_info.modifiers);
				}
				result += alias;
				continue;
			}
		}
		switch (type.id()) {
		case LogicalTypeId::TUPLE:
		case LogicalTypeId::STRUCT: {
			string name = type.id() == LogicalTypeId::TUPLE ? "TUPLE" : "STRUCT";
			if (!type.HasParameters() || StructType::GetChildTypes(type).empty()) {
				result += name;
				break;
			}
			auto &child_types = StructType::GetChildTypes(type);
			auto print_names = type.id() == LogicalTypeId::STRUCT;
			push_text(")");
			for (idx_t i = child_types.size(); i > 0; i--) {
				auto &child = child_types[i - 1];
				push_type(child.second);
				if (print_names) {
					push_text(SQLIdentifier(child.first) + " ");
				}
				if (i > 1) {
					push_text(", ");
				}
			}
			push_text(name + "(");
			break;
		}
		case LogicalTypeId::LIST: {
			if (!type.HasParameters()) {
				result += "LIST";
				break;
			}
			push_text("[]");
			push_type(ListType::GetChildType(type));
			break;
		}
		case LogicalTypeId::ARRAY: {
			if (!type.HasParameters()) {
				result += "ARRAY";
				break;
			}
			auto size = ArrayType::GetSize(type);
			push_text(size == 0 ? "[ANY]" : "[" + to_string(size) + "]");
			push_type(ArrayType::GetChildType(type));
			break;
		}
		case LogicalTypeId::MAP: {
			if (!type.HasParameters()) {
				result += "MAP";
				break;
			}
			push_text(")");
			push_type(MapType::ValueType(type));
			push_text(", ");
			push_type(MapType::KeyType(type));
			push_text("MAP(");
			break;
		}
		case LogicalTypeId::UNION: {
			if (!type.HasParameters()) {
				result += "UNION";
				break;
			}
			auto count = UnionType::GetMemberCount(type);
			push_text(")");
			for (idx_t i = count; i > 0; i--) {
				push_type(UnionType::GetMemberType(type, i - 1));
				push_text(SQLIdentifier(UnionType::GetMemberName(type, i - 1)) + " ");
				if (i > 1) {
					push_text(", ");
				}
			}
			push_text("UNION(");
			break;
		}
		case LogicalTypeId::DECIMAL: {
			if (!type.HasParameters()) {
				result += "DECIMAL";
				break;
			}
			auto width = DecimalType::GetWidth(type);
			auto scale = DecimalType::GetScale(type);
			if (width == 0) {
				result += "DECIMAL";
				break;
			}
			result += StringUtil::Format("DECIMAL(%d,%d)", width, scale);
			break;
		}
		case LogicalTypeId::ENUM: {
			result += "ENUM(";
			for (idx_t i = 0; i < EnumType::GetSize(type); i++) {
				if (i > 0) {
					result += ", ";
				}
				result += SQLString(EnumType::GetString(type, i).GetString());
			}
			result += ")";
			break;
		}
		case LogicalTypeId::UNBOUND: {
			if (!type.HasParameters()) {
				result += "UNBOUND";
				break;
			}
			auto &expr = UnboundType::GetTypeExpression(type);
			if (expr->GetExpressionType() != ExpressionType::TYPE) {
				result += "(" + expr->ToString() + ")";
			} else {
				result += expr->ToString();
			}
			break;
		}
		case LogicalTypeId::SQLNULL: {
			result += "\"NULL\"";
			break;
		}
		case LogicalTypeId::TEMPLATE: {
			if (!type.HasParameters()) {
				result += "T";
				break;
			}
			result += TemplateType::GetName(type);
			break;
		}
		case LogicalTypeId::GEOMETRY: {
			if (!GeoType::HasCRS(type)) {
				result += "GEOMETRY";
				break;
			}
			auto &crs = GeoType::GetCRS(type);
			result += StringUtil::Format("GEOMETRY(%s)", SQLString(crs.GetDefinition()));
			break;
		}
		case LogicalTypeId::VARCHAR: {
			// a collation is part of the type's identity: the constructor form parses in type position
			auto &string_type_info = type.GetTypeInfo();
			if (string_type_info.type == LogicalTypeInfoType::STRING_TYPE_INFO) {
				auto &string_info = string_type_info.Cast<StringTypeInfo>();
				if (!string_info.collation.empty()) {
					result += StringUtil::Format("VARCHAR(%s)", SQLString(string_info.collation));
					break;
				}
			}
			result += "VARCHAR";
			break;
		}
		default:
			result += EnumUtil::ToString(type.id());
			break;
		}
	}
	return result;
}

LogicalTypeId TransformStringToLogicalTypeId(const string &str) {
	auto type = DefaultTypeGenerator::GetDefaultType(Identifier(str));
	if (type == LogicalTypeId::INVALID) {
		// This is a User Type, at this point we don't know if its one of the User Defined Types or an error
		// It is checked in the binder
		type = LogicalTypeId::UNBOUND;
	}
	return type;
}

LogicalType TransformStringToLogicalType(const string &str, ClientContext &context) {
	return TypeManager::Get(context).ParseLogicalType(str, context);
}

bool LogicalType::IsIntegral() const {
	switch (id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

bool LogicalType::IsSigned() const {
	switch (id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
		return true;
	default:
		break;
	}
	return false;
}

bool LogicalType::IsUnsigned() const {
	switch (id()) {
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		break;
	}
	return false;
}

bool LogicalType::IsFloating() const {
	switch (id()) {
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
		return true;
	default:
		return false;
	}
}

bool LogicalType::IsNumeric() const {
	return IsNumeric(id());
}

bool LogicalType::IsNumeric(const LogicalTypeId type) {
	switch (type) {
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
	case LogicalTypeId::UHUGEINT:
		return true;
	default:
		return false;
	}
}

bool LogicalType::IsTemporal() const {
	switch (id()) {
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
		return true;
	default:
		return false;
	}
}

bool LogicalType::IsValid() const {
	return id() != LogicalTypeId::INVALID && id() != LogicalTypeId::UNKNOWN;
}

bool LogicalType::IsComplete() const {
	// Check if type does not contain incomplete types
	return !TypeVisitor::Contains(*this, [](const LogicalType &type) {
		switch (type.id()) {
		case LogicalTypeId::INVALID:
		case LogicalTypeId::UNKNOWN:
		case LogicalTypeId::ANY:
		case LogicalTypeId::TEMPLATE:
			return true; // These are incomplete by default
		case LogicalTypeId::LIST:
		case LogicalTypeId::MAP:
		case LogicalTypeId::STRUCT:
		case LogicalTypeId::TUPLE:
		case LogicalTypeId::UNION:
		case LogicalTypeId::VARIANT:
		case LogicalTypeId::ARRAY:
		case LogicalTypeId::DECIMAL:
		case LogicalTypeId::ENUM:
			if (!type.HasParameters()) {
				return true; // Missing or incorrect type info
			}
			break;
		default:
			return false;
		}

		// Type has type info, check if it is complete
		switch (type.GetTypeInfo().type) {
		case LogicalTypeInfoType::STRUCT_TYPE_INFO:
			// empty STRUCTs/TUPLEs are complete (children, if any, are checked by recursion)
			// UNION/VARIANT (which also use STRUCT_TYPE_INFO) cannot be empty
			if (type.id() == LogicalTypeId::STRUCT || type.id() == LogicalTypeId::TUPLE) {
				return false;
			}
			return type.GetTypeInfo().Cast<StructTypeInfo>().child_types.empty();
		case LogicalTypeInfoType::DECIMAL_TYPE_INFO: {
			// A well-formed decimal is not incomplete
			auto is_well_formed = DecimalType::GetWidth(type) >= 1 &&
			                      DecimalType::GetWidth(type) <= Decimal::MAX_WIDTH_DECIMAL &&
			                      DecimalType::GetScale(type) <= DecimalType::GetWidth(type);
			return !is_well_formed;
		}
		default:
			return false; // Nested types are checked by TypeVisitor recursion
		}
	});
}

bool LogicalType::IsTemplated() const {
	return TypeVisitor::Contains(*this, LogicalTypeId::TEMPLATE);
}

bool LogicalType::SupportsRegularUpdate() const {
	switch (id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::MAP:
	case LogicalTypeId::UNION:
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::GEOMETRY: // If geometry is shredded, its parts (lists/structs) can't be regularly updated.
		return false;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto &child_types = StructType::GetChildTypes(*this);
		for (auto &entry : child_types) {
			if (!entry.second.SupportsRegularUpdate()) {
				return false;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

bool LogicalType::GetDecimalProperties(uint8_t &width, uint8_t &scale) const {
	switch (id()) {
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
	case LogicalTypeId::UHUGEINT:
		// hugeint: max size decimal (38, 0)
		// note that a uhugeint is not guaranteed to fit in this
		width = 38;
		scale = 0;
		break;
	case LogicalTypeId::DECIMAL:
		width = DecimalType::GetWidth(*this);
		scale = DecimalType::GetScale(*this);
		break;
	case LogicalTypeId::INTEGER_LITERAL:
		return IntegerLiteral::GetType(*this).GetDecimalProperties(width, scale);
	default:
		// Nonsense values to ensure initialization
		width = 255u;
		scale = 255u;
		// FIXME(carlo): This should be probably a throw, requires checking the various call-sites
		return false;
	}
	return true;
}

//! Grows Decimal width/scale when appropriate
static LogicalType DecimalSizeCheck(const LogicalType &left, const LogicalType &right) {
	D_ASSERT(left.id() == LogicalTypeId::DECIMAL || right.id() == LogicalTypeId::DECIMAL);
	D_ASSERT(left.id() != right.id());

	//! Make sure the 'right' is the DECIMAL type
	if (left.id() == LogicalTypeId::DECIMAL) {
		return DecimalSizeCheck(right, left);
	}
	auto width = DecimalType::GetWidth(right);
	auto scale = DecimalType::GetScale(right);

	uint8_t other_width;
	uint8_t other_scale;
	bool success = left.GetDecimalProperties(other_width, other_scale);
	if (!success) {
		throw InternalException("Type provided to DecimalSizeCheck was not a numeric type");
	}
	D_ASSERT(other_scale == 0);
	const auto effective_width = width - scale;
	if (other_width > effective_width) {
		auto new_width = NumericCast<uint8_t>(other_width + scale);
		//! Cap the width at max, if an actual value exceeds this, an exception will be thrown later
		if (new_width > DecimalType::MaxWidth()) {
			new_width = DecimalType::MaxWidth();
		}
		return LogicalType::DECIMAL(new_width, scale);
	}
	return right;
}

static LogicalType CombineNumericTypes(const LogicalType &left, const LogicalType &right) {
	D_ASSERT(left.id() != right.id());
	if (left.id() > right.id()) {
		// this method is symmetric
		// arrange it so the left type is smaller to limit the number of options we need to check
		return CombineNumericTypes(right, left);
	}
	// we can't cast implicitly either way and types are not equal
	// this happens when left is signed and right is unsigned
	// e.g. INTEGER and UINTEGER
	// in this case we need to upcast to make sure the types fit

	if (left.id() == LogicalTypeId::BIGINT || right.id() == LogicalTypeId::UBIGINT) {
		return LogicalType::HUGEINT;
	}
	if (left.id() == LogicalTypeId::INTEGER || right.id() == LogicalTypeId::UINTEGER) {
		return LogicalType::BIGINT;
	}
	if (left.id() == LogicalTypeId::SMALLINT || right.id() == LogicalTypeId::USMALLINT) {
		return LogicalType::INTEGER;
	}
	if (left.id() == LogicalTypeId::TINYINT || right.id() == LogicalTypeId::UTINYINT) {
		return LogicalType::SMALLINT;
	}

	// No type is larger than (u)hugeint, so casting to double is required
	// UHUGEINT is on the left because the enum is lower
	if (left.id() == LogicalTypeId::UHUGEINT || right.id() == LogicalTypeId::HUGEINT) {
		return LogicalType::DOUBLE;
	}
	throw InternalException("Cannot combine these numeric types (%s & %s)", left.ToString(), right.ToString());
}

LogicalType LogicalType::NormalizeType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::STRING_LITERAL:
		return LogicalType::VARCHAR;
	case LogicalTypeId::INTEGER_LITERAL:
		return IntegerLiteral::GetType(type);
	case LogicalTypeId::UNKNOWN:
		throw ParameterNotResolvedException();
	default:
		return type;
	}
}

static bool TryGetMaxLogicalTypeInternal(LogicalTypeResolver &logical_type_resolver, const LogicalType &left,
                                         const LogicalType &right, LogicalType &result);

class ForceLogicalTypeResolver : public LogicalTypeResolver {
public:
	using LogicalTypeResolver::LogicalTypeResolver;

	bool Operation(const LogicalType &left, const LogicalType &right, LogicalType &result) override {
		result = context ? LogicalType::ForceMaxLogicalType(*context, left, right)
		                 : LogicalType::DefaultForceMaxLogicalType(left, right);
		return true;
	}
};

class TryGetLogicalTypeResolver : public LogicalTypeResolver {
public:
	using LogicalTypeResolver::LogicalTypeResolver;

	bool Operation(const LogicalType &left, const LogicalType &right, LogicalType &result) override {
		return TryGetMaxLogicalTypeInternal(*this, left, right, result);
	}
};

// Generic combine engine: try the implicit cast rules in both directions and keep the cheaper one. With a
// context we go through CastFunctionSet so that any registered casts are honored; without a context we
// consult the built-in CastRules table directly. Returns false when neither direction can be cast.
static bool TryCombineViaImplicitCast(LogicalTypeResolver &logical_type_resolver, const LogicalType &left,
                                      const LogicalType &right, LogicalType &result) {
	auto left_to_right_cost = logical_type_resolver.context
	                              ? CastFunctionSet::ImplicitCastCost(*logical_type_resolver.context, left, right)
	                              : CastRules::ImplicitCast(left, right);
	auto right_to_left_cost = logical_type_resolver.context
	                              ? CastFunctionSet::ImplicitCastCost(*logical_type_resolver.context, right, left)
	                              : CastRules::ImplicitCast(right, left);
	if (left_to_right_cost >= 0 && (left_to_right_cost < right_to_left_cost || right_to_left_cost < 0)) {
		// we can implicitly cast left to right, return right
		//! Depending on the type, we might need to grow the `width` of the DECIMAL type
		if (right.id() == LogicalTypeId::DECIMAL) {
			if (left.id() == LogicalTypeId::VARIANT) {
				//! Prefer VARIANT always
				result = left;
			} else {
				result = DecimalSizeCheck(left, right);
			}
		} else {
			result = right;
		}
		return true;
	}
	if (right_to_left_cost >= 0) {
		// we can implicitly cast right to left, return left
		//! Depending on the type, we might need to grow the `width` of the DECIMAL type
		if (left.id() == LogicalTypeId::DECIMAL) {
			if (right.id() == LogicalTypeId::VARIANT) {
				//! Prefer VARIANT always
				result = right;
			} else {
				result = DecimalSizeCheck(right, left);
			}
		} else {
			result = left;
		}
		return true;
	}
	return false;
}

static bool TryGetMaxLogicalTypeInternal(LogicalTypeResolver &logical_type_resolver, const LogicalType &left,
                                         const LogicalType &right, LogicalType &result) {
	auto &optional_context = logical_type_resolver.context;
	// with a context, check all combine rules, including ones registered by extensions
	// without one, check only the built-in rules
	bool success;
	bool matched = optional_context
	                   ? CastFunctionSet::Get(*optional_context)
	                         .TryCombineTypes(logical_type_resolver, left, right, result, success)
	                   : CastFunctionSet::TryCombineTypes(DefaultCombineTypesRules(), logical_type_resolver, left,
	                                                      right, result, success);
	if (matched) {
		return success;
	}
	// unequal types fallthrough
	if (left.id() != right.id()) {
		// for other types - try to combine through the generic implicit cast rules
		if (TryCombineViaImplicitCast(logical_type_resolver, left, right, result)) {
			return true;
		}
		// for integer literals - rerun the operation with the underlying type
		if (left.id() == LogicalTypeId::INTEGER_LITERAL) {
			return logical_type_resolver.Operation(IntegerLiteral::GetType(left), right, result);
		}
		if (right.id() == LogicalTypeId::INTEGER_LITERAL) {
			return logical_type_resolver.Operation(left, IntegerLiteral::GetType(right), result);
		}
		// for unsigned/signed comparisons we have a few fallbacks
		if (left.IsNumeric() && right.IsNumeric()) {
			result = CombineNumericTypes(left, right);
			return true;
		}
		if (left.id() == LogicalTypeId::BOOLEAN && right.IsIntegral()) {
			result = right;
			return true;
		}
		if (right.id() == LogicalTypeId::BOOLEAN && left.IsIntegral()) {
			result = left;
			return true;
		}
		return false;
	}
	// equal types fallthrough
	result = left;
	return true;
}

bool LogicalType::TryGetMaxLogicalType(ClientContext &context, const LogicalType &left, const LogicalType &right,
                                       LogicalType &result) {
	if (Settings::Get<OldImplicitCastingSetting>(context)) {
		result = LogicalType::ForceMaxLogicalType(context, left, right);
		return true;
	}
	return TryGetMaxLogicalTypeUnchecked(context, left, right, result);
}

bool LogicalType::TryGetMaxLogicalTypeUnchecked(ClientContext &context, const LogicalType &left,
                                                const LogicalType &right, LogicalType &result) {
	TryGetLogicalTypeResolver logical_type_resolver(&context);
	return TryGetMaxLogicalTypeInternal(logical_type_resolver, left, right, result);
}

bool LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(const LogicalType &left, const LogicalType &right,
                                                       LogicalType &result) {
	TryGetLogicalTypeResolver logical_type_resolver(nullptr);
	return TryGetMaxLogicalTypeInternal(logical_type_resolver, left, right, result);
}

static idx_t GetLogicalTypeScore(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::INVALID:
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::ANY:
	case LogicalTypeId::TEMPLATE:
	case LogicalTypeId::STRING_LITERAL:
	case LogicalTypeId::INTEGER_LITERAL:
		return 0;
	// numerics
	case LogicalTypeId::BOOLEAN:
		return 10;
	case LogicalTypeId::UTINYINT:
		return 11;
	case LogicalTypeId::TINYINT:
		return 12;
	case LogicalTypeId::USMALLINT:
		return 13;
	case LogicalTypeId::SMALLINT:
		return 14;
	case LogicalTypeId::UINTEGER:
		return 15;
	case LogicalTypeId::INTEGER:
		return 16;
	case LogicalTypeId::UBIGINT:
		return 17;
	case LogicalTypeId::BIGINT:
		return 18;
	case LogicalTypeId::UHUGEINT:
		return 19;
	case LogicalTypeId::HUGEINT:
		return 20;
	case LogicalTypeId::DECIMAL:
		return 21;
	case LogicalTypeId::FLOAT:
		return 22;
	case LogicalTypeId::DOUBLE:
		return 23;
	// date/time/timestamp
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
		return 50;
	case LogicalTypeId::TIME_NS:
		return 51;
	case LogicalTypeId::DATE:
		return 52;
	case LogicalTypeId::TIMESTAMP_SEC:
		return 53;
	case LogicalTypeId::TIMESTAMP_MS:
		return 54;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
		return 55;
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return 56;
	case LogicalTypeId::INTERVAL:
		return 58;
	// text/character strings
	case LogicalTypeId::CHAR:
		return 75;
	case LogicalTypeId::VARCHAR:
		return 77;
	case LogicalTypeId::ENUM:
		return 78;
	// blob/complex types
	case LogicalTypeId::BIT:
		return 100;
	case LogicalTypeId::BLOB:
		return 101;
	case LogicalTypeId::UUID:
		return 102;
	case LogicalTypeId::BIGNUM:
		return 103;
	case LogicalTypeId::GEOMETRY:
		return 104;
	// nested types
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		return 125;
	case LogicalTypeId::LIST:
	case LogicalTypeId::ARRAY:
		return 126;
	case LogicalTypeId::MAP:
		return 127;
	case LogicalTypeId::VARIANT:
	case LogicalTypeId::UNION:
	case LogicalTypeId::TABLE:
		return 150;
	// weirdo types
	case LogicalTypeId::LAMBDA:
	case LogicalTypeId::POINTER:
	case LogicalTypeId::VALIDITY:
	case LogicalTypeId::UNBOUND:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::LEGACY_AGGREGATE_STATE:
		break;
	}
	return 1000;
}

static LogicalType ForceMaxLogicalTypeInternal(optional_ptr<ClientContext> context, const LogicalType &left,
                                               const LogicalType &right) {
	LogicalType result;
	ForceLogicalTypeResolver logical_type_resolver(context);
	if (TryGetMaxLogicalTypeInternal(logical_type_resolver, left, right, result)) {
		return result;
	}
	// we prefer the type with the highest score
	auto left_score = GetLogicalTypeScore(left);
	auto right_score = GetLogicalTypeScore(right);
	if (left_score < right_score) {
		return right;
	} else {
		return left;
	}
}

LogicalType LogicalType::ForceMaxLogicalType(ClientContext &context, const LogicalType &left,
                                             const LogicalType &right) {
	return ForceMaxLogicalTypeInternal(&context, left, right);
}

LogicalType LogicalType::DefaultForceMaxLogicalType(const LogicalType &left, const LogicalType &right) {
	return ForceMaxLogicalTypeInternal(nullptr, left, right);
}

LogicalType LogicalType::MaxLogicalType(ClientContext &context, const LogicalType &left, const LogicalType &right) {
	LogicalType result;
	if (!TryGetMaxLogicalType(context, left, right, result)) {
		throw NotImplementedException("Cannot combine types %s and %s - an explicit cast is required", left.ToString(),
		                              right.ToString());
	}
	return result;
}

void LogicalType::Verify() const {
#ifdef DEBUG
	switch (id()) {
	case LogicalTypeId::DECIMAL:
		D_ASSERT(DecimalType::GetWidth(*this) >= 1 && DecimalType::GetWidth(*this) <= Decimal::MAX_WIDTH_DECIMAL);
		D_ASSERT(DecimalType::GetScale(*this) <= DecimalType::GetWidth(*this));
		break;
	case LogicalTypeId::STRUCT: {
		// verify child types
		identifier_set_t child_names;
		bool all_empty = true;
		for (auto &entry : StructType::GetChildTypes(*this)) {
			if (entry.first.empty()) {
				D_ASSERT(all_empty);
			} else {
				// check for duplicate struct names
				all_empty = false;
				auto existing_entry = child_names.find(entry.first);
				D_ASSERT(existing_entry == child_names.end());
				child_names.insert(entry.first);
			}
			entry.second.Verify();
		}
		break;
	}
	case LogicalTypeId::TUPLE: {
		// verify child types - all names must be empty
		for (auto &entry : StructType::GetChildTypes(*this)) {
			D_ASSERT(entry.first.empty());
			entry.second.Verify();
		}
		break;
	}
	case LogicalTypeId::LIST:
		ListType::GetChildType(*this).Verify();
		break;
	case LogicalTypeId::MAP: {
		MapType::KeyType(*this).Verify();
		MapType::ValueType(*this).Verify();
		break;
	}
	default:
		break;
	}
#endif
}

bool ApproxEqual(float ldecimal, float rdecimal) {
	if (Value::IsNan(ldecimal) && Value::IsNan(rdecimal)) {
		return true;
	}
	if (!Value::FloatIsFinite(ldecimal) || !Value::FloatIsFinite(rdecimal)) {
		return ldecimal == rdecimal;
	}
	float epsilon = static_cast<float>(std::fabs(rdecimal) * 0.01 + 0.00000001);
	return std::fabs(ldecimal - rdecimal) <= epsilon;
}

bool ApproxEqual(double ldecimal, double rdecimal) {
	if (Value::IsNan(ldecimal) && Value::IsNan(rdecimal)) {
		return true;
	}
	if (!Value::DoubleIsFinite(ldecimal) || !Value::DoubleIsFinite(rdecimal)) {
		return ldecimal == rdecimal;
	}
	double epsilon = std::fabs(rdecimal) * 0.01 + 0.00000001;
	return std::fabs(ldecimal - rdecimal) <= epsilon;
}

void LogicalType::Serialize(Serializer &serializer) const {
	// Serialize geometry as old extension geometry type if required
	if (id() == LogicalTypeId::GEOMETRY && !serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		// This will drop the CRS information, but that's better than throwing an error.
		auto legacy_geom = Geometry::GetSpatialGeometryType();
		legacy_geom.Serialize(serializer);
		return;
	}

	// This is a UNBOUND type and we are writing to older storage.
	// 1. try to default-bind into a concrete logical type, and serialize that
	// 2. if that fails, serialize normally, in which case the UNBOUND_TYPE_INFO will try to
	//    write itself as an old-style USER type.
	if (id() == LogicalTypeId::UNBOUND && !serializer.ShouldSerialize(StorageVersion::V1_5_0)) {
		try {
			auto bound_type = UnboundType::TryDefaultBind(*this);
			if (bound_type.id() != LogicalTypeId::INVALID && bound_type.id() != LogicalTypeId::UNBOUND) {
				bound_type.Serialize(serializer);
				return;
			}
		} catch (const InternalException &) {
			throw;
		} catch (const OutOfMemoryException &) {
			// binding allocates, and a failure to allocate is not a failure to bind: falling back here reports it
			// as "Cannot serialize non-constant type parameter", which says nothing about what went wrong
			throw;
		} catch (const InterruptException &) {
			throw;
		} catch (...) {
			// Ignore errors, just try to write as a USER type instead
		}
	}
	// TUPLE is a recent type. We store its own id when targeting a recent storage version (V2_0_0+), but for older
	// formats we write it as an unnamed STRUCT for backwards compatibility (older engines read it as an unnamed
	// struct - its children carry empty names - and we convert unnamed structs back to TUPLE on deserialize).
	auto serialized_id = id();
	if (id() == LogicalTypeId::TUPLE && !serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serialized_id = LogicalTypeId::STRUCT;
	}
	serializer.WriteProperty<LogicalTypeId>(100, "id", serialized_id);
	// types without parameters are written without type info
	optional_ptr<const LogicalTypeInfo> type_info;
	if (type_info_.get().type != LogicalTypeInfoType::INVALID_TYPE_INFO) {
		type_info = type_info_.get();
	}
	serializer.WritePropertyWithDefault<optional_ptr<const LogicalTypeInfo>>(101, "type_info", type_info);
}

LogicalType LogicalType::Deserialize(Deserializer &deserializer) {
	auto id = deserializer.ReadProperty<LogicalTypeId>(100, "id");
	auto type_info = deserializer.ReadPropertyWithDefault<unique_ptr<LogicalTypeInfo>>(101, "type_info");
	if (id == LogicalTypeId::LEGACY_AGGREGATE_STATE) {
		// convert legacy aggregate state to blob on deserialize
		return LogicalType::BLOB;
	}

	// Convert unnamed (non-empty) STRUCTs back to TUPLE
	if (id == LogicalTypeId::STRUCT && type_info && type_info->type == LogicalTypeInfoType::STRUCT_TYPE_INFO &&
	    StructType::AllUnnamed(type_info->Cast<StructTypeInfo>().child_types)) {
		id = LogicalTypeId::TUPLE;
	}

	auto info_type = type_info ? type_info->type : LogicalTypeInfoType::INVALID_TYPE_INFO;
	if (info_type == LogicalTypeInfoType::DECIMAL_TYPE_INFO) {
		auto &decimal_info = type_info->Cast<DecimalTypeInfo>();
		if (!Decimal::IsValidWidthScale(decimal_info.width, decimal_info.scale)) {
			throw SerializationException("Failed to deserialize type: invalid DECIMAL(%d, %d)", decimal_info.width,
			                             decimal_info.scale);
		}
	}
	LogicalType result(id, std::move(type_info));
	if (info_type != LogicalTypeInfoType::INVALID_TYPE_INFO && info_type != LogicalTypeInfoType::GENERIC_TYPE_INFO &&
	    !result.HasParameters()) {
		throw SerializationException("Failed to deserialize type %s: type info %s does not match the type",
		                             EnumUtil::ToString(id), EnumUtil::ToString(info_type));
	}
	if (Geometry::IsSpatialGeometryType(result)) {
		// This is a legacy geometry type, deserialize as geometry
		return LogicalType::GEOMETRY();
	}
	return result;
}

//===--------------------------------------------------------------------===//
// Extra Type Info
//===--------------------------------------------------------------------===//
LogicalType LogicalType::Copy() const {
	if (type_info_.get().type == LogicalTypeInfoType::INVALID_TYPE_INFO ||
	    type_info_.get().type == LogicalTypeInfoType::ENUM_TYPE_INFO) {
		// enum type info is kept shared to avoid rebuilding the dictionary lookup map
		return *this;
	}
	return LogicalType(id(), type_info_.get().Copy());
}

//! Copies the type info so it can be modified before being adopted by a new type
static unique_ptr<LogicalTypeInfo> CopyTypeInfo(const LogicalTypeInfo &info) {
	if (info.type == LogicalTypeInfoType::INVALID_TYPE_INFO) {
		return make_uniq<LogicalTypeInfo>(LogicalTypeInfoType::GENERIC_TYPE_INFO);
	}
	return info.Copy();
}

LogicalType LogicalType::WithAlias(string alias) const {
	if (type_info_.get().alias == alias) {
		// avoid copying the (potentially expensive) type info if the alias does not change
		return *this;
	}
	auto new_info = CopyTypeInfo(type_info_.get());
	new_info->alias = std::move(alias);
	return LogicalType(id(), std::move(new_info));
}

string LogicalType::GetAlias() const {
	return type_info_.get().alias;
}

bool LogicalType::HasAlias() const {
	return !type_info_.get().alias.empty();
}

bool LogicalType::HasExtensionInfo() const {
	return type_info_.get().extension_info != nullptr;
}

optional_ptr<const ExtensionTypeInfo> LogicalType::GetExtensionInfo() const {
	return type_info_.get().extension_info.get();
}

LogicalType LogicalType::WithExtensionInfo(unique_ptr<ExtensionTypeInfo> info) const {
	auto new_info = CopyTypeInfo(type_info_.get());
	new_info->extension_info = std::move(info);
	return LogicalType(id(), std::move(new_info));
}

//===--------------------------------------------------------------------===//
// Decimal Type
//===--------------------------------------------------------------------===//
uint8_t DecimalType::GetWidth(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
	auto &info = type.GetTypeInfo();
	return info.Cast<DecimalTypeInfo>().width;
}

uint8_t DecimalType::GetScale(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::DECIMAL);
	auto &info = type.GetTypeInfo();
	return info.Cast<DecimalTypeInfo>().scale;
}

uint8_t DecimalType::MaxWidth() {
	return DecimalWidth<hugeint_t>::max;
}

LogicalType LogicalType::DECIMAL(uint8_t width, uint8_t scale) {
	D_ASSERT(width >= scale);
	auto type_info = make_uniq<DecimalTypeInfo>(width, scale);
	return LogicalType(LogicalTypeId::DECIMAL, std::move(type_info));
}

//===--------------------------------------------------------------------===//
// String Type
//===--------------------------------------------------------------------===//
string StringType::GetCollation(const LogicalType &type) {
	if (type.id() != LogicalTypeId::VARCHAR) {
		return string();
	}
	auto &info = type.GetTypeInfo();
	if (info.type != LogicalTypeInfoType::STRING_TYPE_INFO) {
		return string();
	}
	return info.Cast<StringTypeInfo>().collation;
}

LogicalType LogicalType::VARCHAR_COLLATION(string collation) { // NOLINT
	auto string_info = make_uniq<StringTypeInfo>(std::move(collation));
	return LogicalType(LogicalTypeId::VARCHAR, std::move(string_info));
}

//===--------------------------------------------------------------------===//
// List Type
//===--------------------------------------------------------------------===//
const LogicalType &ListType::GetChildType(const LogicalType &type) {
	if (type.id() != LogicalTypeId::LIST && type.id() != LogicalTypeId::MAP) {
		throw InternalException("ListType::GetChildType called on a non-LIST type: %s", type.ToString());
	}
	auto &info = type.GetTypeInfo();
	return info.Cast<ListTypeInfo>().child_type;
}

LogicalType LogicalType::LIST(const LogicalType &child) {
	auto info = make_uniq<ListTypeInfo>(child);
	return LogicalType(LogicalTypeId::LIST, std::move(info));
}

//===--------------------------------------------------------------------===//
// Struct Type
//===--------------------------------------------------------------------===//
const child_list_t<LogicalType> &StructType::GetChildTypes(const LogicalType &type) {
	if (type.id() != LogicalTypeId::STRUCT && type.id() != LogicalTypeId::TUPLE && type.id() != LogicalTypeId::UNION &&
	    type.id() != LogicalTypeId::VARIANT) {
		throw InternalException("StructType::GetChildTypes called on a non-STRUCT type: %s", type.ToString());
	}

	auto &info = type.GetTypeInfo();
	return info.Cast<StructTypeInfo>().child_types;
}

const LogicalType &StructType::GetChildType(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	return child_types[index].second;
}

const Identifier &StructType::GetChildName(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	return child_types[index].first;
}

idx_t StructType::GetChildIndexUnsafe(const LogicalType &type, const string &name) {
	auto &child_types = StructType::GetChildTypes(type);
	for (idx_t i = 0; i < child_types.size(); i++) {
		if (child_types[i].first == name) {
			return i;
		}
	}
	throw InternalException("Could not find child with name \"%s\" in struct type \"%s\"", name, type.ToString());
}

idx_t StructType::GetChildCount(const LogicalType &type) {
	return StructType::GetChildTypes(type).size();
}

bool StructType::IsUnnamed(const LogicalType &type) {
	return type.id() == LogicalTypeId::TUPLE;
}

bool StructType::AllUnnamed(const child_list_t<LogicalType> &children) {
	if (children.empty()) {
		return false;
	}
	for (auto &child : children) {
		if (!child.first.empty()) {
			return false;
		}
	}
	return true;
}

LogicalType LogicalType::STRUCT(child_list_t<LogicalType> children) {
	if (StructType::AllUnnamed(children)) {
		// a struct without any member names is a TUPLE
		return LogicalType::TUPLE(std::move(children));
	}
	auto info = make_uniq<StructTypeInfo>(std::move(children));
	return LogicalType(LogicalTypeId::STRUCT, std::move(info));
}

bool StructType::IsStruct(const LogicalType &type) {
	return StructType::IsStruct(type.id());
}

bool StructType::IsStruct(LogicalTypeId id) {
	return id == LogicalTypeId::STRUCT || id == LogicalTypeId::TUPLE;
}

string TupleType::GetChildName(idx_t index) {
	return "element" + to_string(index + 1);
}

child_list_t<LogicalType> TupleType::NamedChildren(const LogicalType &type) {
	auto &child_types = StructType::GetChildTypes(type);
	child_list_t<LogicalType> result;
	result.reserve(child_types.size());
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &child = child_types[i];
		auto name = child.first.empty() ? TupleType::GetChildName(i) : child.first.GetIdentifierName();
		result.emplace_back(Identifier(std::move(name)), child.second);
	}
	return result;
}

child_list_t<LogicalType> LogicalType::GetNamedChildTypes(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
		return {{"element", ListType::GetChildType(type)}};
	case LogicalTypeId::ARRAY:
		return {{"element", ArrayType::GetChildType(type)}};
	case LogicalTypeId::MAP:
		return {{"key", MapType::KeyType(type)}, {"value", MapType::ValueType(type)}};
	case LogicalTypeId::STRUCT:
		return StructType::GetChildTypes(type);
	case LogicalTypeId::TUPLE:
		return TupleType::NamedChildren(type);
	default:
		throw InternalException("Unsupported type %s in GetNamedChildTypes", type.ToString());
	}
}

LogicalType LogicalType::ConstructNestedType(const LogicalType &type, child_list_t<LogicalType> children) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
		D_ASSERT(children.size() == 1);
		return LogicalType::LIST(children[0].second);
	case LogicalTypeId::MAP:
		D_ASSERT(children.size() == 2);
		return LogicalType::MAP(children[0].second, children[1].second);
	case LogicalTypeId::STRUCT:
		return LogicalType::STRUCT(std::move(children));
	default:
		throw InternalException("Unsupported type %s in ConstructNestedType", type.ToString());
	}
}

LogicalType LogicalType::TUPLE(child_list_t<LogicalType> children) {
	// a TUPLE is an unnamed struct - all member names are empty
	for (auto &child : children) {
		child.first.clear();
	}
	auto info = make_uniq<StructTypeInfo>(std::move(children));
	return LogicalType(LogicalTypeId::TUPLE, std::move(info));
}

LogicalType LogicalType::TUPLE(vector<LogicalType> children) {
	child_list_t<LogicalType> child_list;
	child_list.reserve(children.size());
	for (auto &child : children) {
		child_list.emplace_back(string(), std::move(child));
	}
	return LogicalType::TUPLE(std::move(child_list));
}

//===--------------------------------------------------------------------===//
// Map Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::MAP(const LogicalType &child_p) {
	D_ASSERT(child_p.id() == LogicalTypeId::STRUCT);
	auto &children = StructType::GetChildTypes(child_p);
	D_ASSERT(children.size() == 2);

	// We do this to enforce that for every MAP created, the keys are called "key"
	// and the values are called "value"

	// This is done because for Vector the keys of the STRUCT are used in equality checks.
	// Vector::Reference will throw if the types don't match
	child_list_t<LogicalType> new_children(2);
	new_children[0] = children[0];
	new_children[0].first = "key";

	new_children[1] = children[1];
	new_children[1].first = "value";

	auto child = LogicalType::STRUCT(std::move(new_children));
	auto info = make_uniq<ListTypeInfo>(child);
	return LogicalType(LogicalTypeId::MAP, std::move(info));
}

LogicalType LogicalType::MAP(LogicalType key, LogicalType value) {
	child_list_t<LogicalType> child_types;
	child_types.emplace_back("key", std::move(key));
	child_types.emplace_back("value", std::move(value));
	return LogicalType::MAP(LogicalType::STRUCT(child_types));
}

const LogicalType &MapType::KeyType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::MAP);
	return StructType::GetChildTypes(ListType::GetChildType(type))[0].second;
}

const LogicalType &MapType::ValueType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::MAP);
	return StructType::GetChildTypes(ListType::GetChildType(type))[1].second;
}

//===--------------------------------------------------------------------===//
// Union Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::UNION(child_list_t<LogicalType> members) {
	D_ASSERT(!members.empty());
	D_ASSERT(members.size() <= UnionType::MAX_UNION_MEMBERS);
	// union types always have a hidden "tag" field in front
	members.insert(members.begin(), {"", LogicalType::UTINYINT});
	auto info = make_uniq<StructTypeInfo>(std::move(members));
	return LogicalType(LogicalTypeId::UNION, std::move(info));
}

const LogicalType &UnionType::GetMemberType(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	// skip the "tag" field
	return child_types[index + 1].second;
}

const Identifier &UnionType::GetMemberName(const LogicalType &type, idx_t index) {
	auto &child_types = StructType::GetChildTypes(type);
	D_ASSERT(index < child_types.size());
	// skip the "tag" field
	return child_types[index + 1].first;
}

idx_t UnionType::GetMemberCount(const LogicalType &type) {
	// don't count the "tag" field
	return StructType::GetChildTypes(type).size() - 1;
}
const child_list_t<LogicalType> UnionType::CopyMemberTypes(const LogicalType &type) {
	auto child_types = StructType::GetChildTypes(type);
	child_types.erase(child_types.begin());
	return child_types;
}

//===--------------------------------------------------------------------===//
// Unbound Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::UNBOUND(unique_ptr<ParsedExpression> expr) {
	auto info = make_uniq<UnboundTypeInfo>(std::move(expr));
	return LogicalType(LogicalTypeId::UNBOUND, std::move(info));
}

//===--------------------------------------------------------------------===//
// Type Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::TYPE() {
	return LogicalType(LogicalTypeId::TYPE);
}

//===--------------------------------------------------------------------===//
// Enum Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::ENUM(const Vector &ordered_data, idx_t size) {
	return EnumTypeInfo::CreateType(ordered_data, size);
}

LogicalType LogicalType::ENUM(const string &enum_name, const Vector &ordered_data, idx_t size) {
	return LogicalType::ENUM(ordered_data, size);
}

const string EnumType::GetValue(const Value &val) {
	auto &info = val.type().GetTypeInfo();
	auto &values_insert_order = info.Cast<EnumTypeInfo>().GetValuesInsertOrder();
	return StringValue::Get(values_insert_order.GetValue(val.GetValue<uint32_t>()));
}

const Vector &EnumType::GetValuesInsertOrder(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ENUM);
	auto &info = type.GetTypeInfo();
	return info.Cast<EnumTypeInfo>().GetValuesInsertOrder();
}

idx_t EnumType::GetSize(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ENUM);
	auto &info = type.GetTypeInfo();
	return info.Cast<EnumTypeInfo>().GetDictSize();
}

PhysicalType EnumType::GetPhysicalType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ENUM);
	auto &info = type.GetTypeInfo().Cast<EnumTypeInfo>();
	D_ASSERT(info.GetEnumDictType() == EnumDictType::VECTOR_DICT);
	return EnumTypeInfo::DictType(info.GetDictSize());
}

//===--------------------------------------------------------------------===//
// JSON Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::JSON() {
	static const auto json_type = CreateImmortal(
	    LogicalTypeId::VARCHAR, make_uniq<LogicalTypeInfo>(LogicalTypeInfoType::GENERIC_TYPE_INFO, JSON_TYPE_NAME));
	return json_type;
}

bool LogicalType::IsJSONType() const {
	return id() == LogicalTypeId::VARCHAR && HasAlias() && GetAlias() == JSON_TYPE_NAME;
}

bool LogicalType::IsAggregateState() const {
	return HasAlias() && GetAlias() == "AGGREGATE_STATE";
}
//===--------------------------------------------------------------------===//
// Array Type
//===--------------------------------------------------------------------===//

const LogicalType &ArrayType::GetChildType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ARRAY);
	auto &info = type.GetTypeInfo();
	return info.Cast<ArrayTypeInfo>().child_type;
}

idx_t ArrayType::GetSize(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ARRAY);
	auto &info = type.GetTypeInfo();
	return info.Cast<ArrayTypeInfo>().size;
}

bool ArrayType::IsAnySize(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ARRAY);
	auto &info = type.GetTypeInfo();
	return info.Cast<ArrayTypeInfo>().size == 0;
}

LogicalType ArrayType::ConvertToList(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::ARRAY: {
		return LogicalType::LIST(ConvertToList(ArrayType::GetChildType(type)));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(ConvertToList(ListType::GetChildType(type)));
	case LogicalTypeId::STRUCT: {
		auto children = StructType::GetChildTypes(type);
		for (auto &child : children) {
			child.second = ConvertToList(child.second);
		}
		return LogicalType::STRUCT(children);
	}
	case LogicalTypeId::TUPLE: {
		auto children = StructType::GetChildTypes(type);
		for (auto &child : children) {
			child.second = ConvertToList(child.second);
		}
		return LogicalType::TUPLE(children);
	}
	case LogicalTypeId::MAP: {
		auto key_type = ConvertToList(MapType::KeyType(type));
		auto value_type = ConvertToList(MapType::ValueType(type));
		return LogicalType::MAP(key_type, value_type);
	}
	case LogicalTypeId::UNION: {
		auto children = UnionType::CopyMemberTypes(type);
		for (auto &child : children) {
			child.second = ConvertToList(child.second);
		}
		return LogicalType::UNION(children);
	}
	default:
		return type;
	}
}

LogicalType LogicalType::ARRAY(const LogicalType &child, optional_idx size) {
	if (!size.IsValid()) {
		// Create an incomplete ARRAY type, used for binding
		auto info = make_uniq<ArrayTypeInfo>(child, uint32_t(0));
		return LogicalType(LogicalTypeId::ARRAY, std::move(info));
	} else {
		auto array_size = size.GetIndex();
		D_ASSERT(array_size > 0);
		D_ASSERT(array_size <= ArrayType::MAX_ARRAY_SIZE);
		auto info = make_uniq<ArrayTypeInfo>(child, NumericCast<uint32_t>(array_size));
		return LogicalType(LogicalTypeId::ARRAY, std::move(info));
	}
}

//===--------------------------------------------------------------------===//
// Any Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::ANY_PARAMS(LogicalType target, idx_t cast_score) { // NOLINT
	auto type_info = make_uniq<AnyTypeInfo>(std::move(target), cast_score);
	return LogicalType(LogicalTypeId::ANY, std::move(type_info));
}

LogicalType AnyType::GetTargetType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ANY);
	auto &info = type.GetTypeInfo();
	if (info.type != LogicalTypeInfoType::ANY_TYPE_INFO) {
		return LogicalType::ANY;
	}
	return info.Cast<AnyTypeInfo>().target_type;
}

idx_t AnyType::GetCastScore(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::ANY);
	auto &info = type.GetTypeInfo();
	if (info.type != LogicalTypeInfoType::ANY_TYPE_INFO) {
		return 5;
	}
	return info.Cast<AnyTypeInfo>().cast_score;
}

//===--------------------------------------------------------------------===//
// Integer Literal Type
//===--------------------------------------------------------------------===//
LogicalType IntegerLiteral::GetType(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::INTEGER_LITERAL);
	auto &info = type.GetTypeInfo();
	D_ASSERT(info.type == LogicalTypeInfoType::INTEGER_LITERAL_TYPE_INFO);
	return info.Cast<IntegerLiteralTypeInfo>().constant_value.type();
}

bool IntegerLiteral::FitsInType(const LogicalType &type, const LogicalType &target) {
	D_ASSERT(type.id() == LogicalTypeId::INTEGER_LITERAL);
	// we can always cast integer literals to float and double
	if (target.id() == LogicalTypeId::FLOAT || target.id() == LogicalTypeId::DOUBLE) {
		return true;
	}
	if (!target.IsIntegral()) {
		return false;
	}
	// we can cast to integral types if the constant value fits within that type
	auto &info = type.GetTypeInfo();
	D_ASSERT(info.type == LogicalTypeInfoType::INTEGER_LITERAL_TYPE_INFO);
	auto &literal_info = info.Cast<IntegerLiteralTypeInfo>();
	return literal_info.constant_value.DefaultTryCastAs(target).has_value();
}

LogicalType LogicalType::INTEGER_LITERAL(const Value &constant) { // NOLINT
	if (!constant.type().IsIntegral()) {
		throw InternalException("INTEGER_LITERAL can only be made from literals of integer types");
	}
	auto type_info = make_uniq<IntegerLiteralTypeInfo>(constant);
	return LogicalType(LogicalTypeId::INTEGER_LITERAL, std::move(type_info));
}

//===--------------------------------------------------------------------===//
// Template Type
//===--------------------------------------------------------------------===//
LogicalType LogicalType::TEMPLATE(const string &name) {
	D_ASSERT(!name.empty());
	auto type_info = make_uniq<TemplateTypeInfo>(name);
	return LogicalType(LogicalTypeId::TEMPLATE, std::move(type_info));
}

const string &TemplateType::GetName(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::TEMPLATE);
	auto &info = type.GetTypeInfo();
	D_ASSERT(info.type == LogicalTypeInfoType::TEMPLATE_TYPE_INFO);
	return info.Cast<TemplateTypeInfo>().name;
}

//===--------------------------------------------------------------------===//
// Variant Type
//===--------------------------------------------------------------------===//

static unique_ptr<LogicalTypeInfo> CreateVariantTypeInfo() {
	child_list_t<LogicalType> children;
	//! keys
	children.emplace_back("keys", LogicalType::LIST(LogicalTypeId::VARCHAR));
	//! children
	children.emplace_back("children",
	                      LogicalType::LIST(LogicalType::STRUCT(
	                          {{"keys_index", LogicalType::UINTEGER}, {"values_index", LogicalType::UINTEGER}})));
	//! values
	children.emplace_back("values", LogicalType::LIST(LogicalType::STRUCT(
	                                    {{"type_id", LogicalType::UTINYINT}, {"byte_offset", LogicalType::UINTEGER}})));
	//! data
	children.emplace_back("data", LogicalType::BLOB);

	return make_uniq<StructTypeInfo>(std::move(children));
}

LogicalType LogicalType::VARIANT() {
	static const auto variant_type = CreateImmortal(LogicalTypeId::VARIANT, CreateVariantTypeInfo());
	return variant_type;
}

//===--------------------------------------------------------------------===//
// Spatial Types
//===--------------------------------------------------------------------===//

LogicalType LogicalType::GEOMETRY() {
	return LogicalType(LogicalTypeId::GEOMETRY);
}

LogicalType LogicalType::GEOMETRY(const string &crs) {
	if (crs.empty()) {
		return LogicalType::GEOMETRY();
	}
	auto info = make_uniq<GeoTypeInfo>();
	info->crs = CoordinateReferenceSystem(crs);
	return LogicalType(LogicalTypeId::GEOMETRY, std::move(info));
}

LogicalType LogicalType::GEOMETRY(const CoordinateReferenceSystem &crs) {
	auto info = make_uniq<GeoTypeInfo>();
	info->crs = crs;
	return LogicalType(LogicalTypeId::GEOMETRY, std::move(info));
}

bool GeoType::HasCRS(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::GEOMETRY);
	auto &info = type.GetTypeInfo();
	if (info.type != LogicalTypeInfoType::GEO_TYPE_INFO) {
		// a GEOMETRY type without geo type info has no CRS - this can happen when an alias is set on a geometry
		// type that was created without a CRS (WithAlias attaches a generic type info)
		return false;
	}
	const auto &geo_info = info.Cast<GeoTypeInfo>();

	return geo_info.crs.GetType() != CoordinateReferenceSystemType::INVALID;
}

const CoordinateReferenceSystem &GeoType::GetCRS(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::GEOMETRY);
	auto &info = type.GetTypeInfo();
	if (info.type != LogicalTypeInfoType::GEO_TYPE_INFO) {
		throw InternalException("Geometry type has no CRS information");
	}
	auto &geo_info = info.Cast<GeoTypeInfo>();

	return geo_info.crs;
}

//===--------------------------------------------------------------------===//
// Unbound Types
//===--------------------------------------------------------------------===//

const unique_ptr<ParsedExpression> &UnboundType::GetTypeExpression(const LogicalType &type) {
	D_ASSERT(type.id() == LogicalTypeId::UNBOUND);
	auto &info = type.GetTypeInfo();
	D_ASSERT(info.type == LogicalTypeInfoType::UNBOUND_TYPE_INFO);
	return info.Cast<UnboundTypeInfo>().expr;
}

LogicalType UnboundType::TryParseAndDefaultBind(const string &type_str) {
	if (type_str.empty()) {
		return LogicalType::INVALID;
	}
	try {
		ColumnList list = Parser::GetBuiltinParser().ParseColumnList("dummy " + type_str);
		auto unbound = list.GetColumn(LogicalIndex(0)).Type();
		return TryDefaultBind(unbound);
	} catch (const std::runtime_error &e) {
		throw InvalidInputException("Could not parse type string '%s'", type_str);
	}
}

static LogicalType TryDefaultBindTypeExpression(const ParsedExpression &expr) {
	if (expr.GetExpressionType() != ExpressionType::TYPE) {
		throw InvalidInputException("Cannot default bind unbound type with non-type expression");
	}
	const auto &type_expr = expr.Cast<TypeExpression>();

	// Now we try to bind the unbound type to a default type
	auto &name = type_expr.GetTypeName();
	auto &args = type_expr.GetChildren();

	vector<pair<string, Value>> bound_args;
	for (auto &arg : args) {
		switch (arg->GetExpressionType()) {
		case ExpressionType::TYPE: {
			auto type = TryDefaultBindTypeExpression(*arg);
			bound_args.emplace_back(arg->GetAlias().GetIdentifierName(), Value::TYPE(type));
		} break;
		case ExpressionType::VALUE_CONSTANT: {
			auto &const_expr = arg->Cast<ConstantExpression>();
			bound_args.emplace_back(arg->GetAlias().GetIdentifierName(), const_expr.GetLiteral().ToValue());
		} break;
		default:
			throw InvalidInputException("Cannot default bind unbound type with non-type, non-expression parameter");
			break;
		}
	}

	// Try to bind as far as we can
	auto result = DefaultTypeGenerator::TryDefaultBind(name.GetIdentifierName(), bound_args);
	if (result.id() != LogicalTypeId::INVALID) {
		return result;
	}

	// Otherwise, wrap this as an unbound type
	auto copy = expr.Copy();
	return LogicalType::UNBOUND(std::move(copy));
	// return LogicalType::INVALID;
}

LogicalType UnboundType::TryDefaultBind(const LogicalType &unbound_type) {
	if (!unbound_type.IsUnbound()) {
		return unbound_type;
	}
	auto &expr = UnboundType::GetTypeExpression(unbound_type);
	return TryDefaultBindTypeExpression(*expr);
}

LogicalType UnboundType::TryDefaultBind(const ParsedExpression &type_expr) {
	return TryDefaultBindTypeExpression(type_expr);
}

//===--------------------------------------------------------------------===//
// Logical Type
//===--------------------------------------------------------------------===//

bool LogicalType::EqualTypeInfo(const LogicalType &rhs) const {
	if (RefersToSameObject(type_info_, rhs.type_info_)) {
		return true;
	}
	return type_info_.get().Equals(rhs.type_info_.get());
}

bool LogicalType::EqualsIncludingCollation(const LogicalType &rhs) const {
	if (*this != rhs) {
		return false;
	}
	vector<string> collations;
	TypeVisitor::Contains(*this, [&](const LogicalType &child) {
		if (child.id() == LogicalTypeId::VARCHAR) {
			collations.push_back(StringType::GetCollation(child));
		}
		return false;
	});
	idx_t index = 0;
	auto mismatch = TypeVisitor::Contains(rhs, [&](const LogicalType &child) {
		if (child.id() != LogicalTypeId::VARCHAR) {
			return false;
		}
		if (index >= collations.size()) {
			return true;
		}
		auto collation_mismatch = collations[index] != StringType::GetCollation(child);
		index++;
		return collation_mismatch;
	});
	return !mismatch && index == collations.size();
}

bool LogicalType::operator==(const LogicalType &rhs) const {
	if (RefersToSameObject(type_info_, rhs.type_info_)) {
		return true;
	}
	if (id() != rhs.id()) {
		return false;
	}
	return EqualTypeInfo(rhs);
}

} // namespace duckdb
