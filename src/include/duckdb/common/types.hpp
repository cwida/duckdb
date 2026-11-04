//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/identifier.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/atomic.hpp"

#include <limits>

namespace duckdb {

class Serializer;
class Deserializer;
class Value;
class TypeCatalogEntry;
class Vector;
class ClientContext;
class ParsedExpression;
class CoordinateReferenceSystem;

struct string_t; // NOLINT: mimic std casing

template <class T>
using child_list_t = vector<std::pair<Identifier, T>>;
template <class T>
using buffer_ptr = shared_ptr<T>;

template <class... Args>
struct VectorStructType;

template <class T>
struct VectorListType;

//! Marker type selecting the VARIANT specialization of VectorIterator (see variant_iterator.hpp)
struct VectorVariantType;

template <class T, typename... ARGS>
buffer_ptr<T> make_buffer(ARGS &&...args) { // NOLINT: mimic std casing
	return make_shared_ptr<T>(std::forward<ARGS>(args)...);
}

struct list_entry_t { // NOLINT: mimic std casing
	list_entry_t() = default;
	list_entry_t(uint64_t offset, uint64_t length) : offset(offset), length(length) {
	}
	inline constexpr bool operator!=(const list_entry_t &other) const {
		return !(*this == other);
	}
	inline constexpr bool operator==(const list_entry_t &other) const {
		return offset == other.offset && length == other.length;
	}

	uint64_t offset;
	uint64_t length;
};

using union_tag_t = uint8_t;

//===--------------------------------------------------------------------===//
// Internal Types
//===--------------------------------------------------------------------===//

// taken from arrow's type.h
enum class PhysicalType : uint8_t {
	///// A NULL type having no physical storage
	// NA = 0,

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

	///// 2-byte floating point value
	// HALF_FLOAT = 10,

	/// 4-byte floating point value
	FLOAT = 11,

	/// 8-byte floating point value
	DOUBLE = 12,

	///// UTF8 variable-length string as List<Char>
	// STRING = 13,

	///// Variable-length bytes (no guarantee of UTF8-ness)
	// BINARY = 14,

	///// Fixed-size binary. Each value occupies the same number of bytes
	// FIXED_SIZE_BINARY = 15,

	///// int32_t days since the UNIX epoch
	// DATE32 = 16,

	///// int64_t milliseconds since the UNIX epoch
	// DATE64 = 17,

	///// Exact timestamp encoded with int64 since UNIX epoch
	///// Default unit millisecond
	// TIMESTAMP = 18,

	///// Time as signed 32-bit integer, representing either seconds or
	///// milliseconds since midnight
	// TIME32 = 19,

	///// Time as signed 64-bit integer, representing either microseconds or
	///// nanoseconds since midnight
	// TIME64 = 20,

	/// YEAR_MONTH or DAY_TIME interval in SQL style
	INTERVAL = 21,

	/// Precision- and scale-based decimal type. Storage type depends on the
	/// parameters.
	// DECIMAL = 22,

	/// A list of some logical data type
	LIST = 23,

	/// Struct of logical types
	STRUCT = 24,

	///// Unions of logical types
	// UNION = 25,

	///// Dictionary-encoded type, also called "categorical" or "factor"
	///// in other programming languages. Holds the dictionary value
	///// type but not the dictionary itself, which is part of the
	///// ArrayData struct
	// DICTIONARY = 26,

	///// Custom data type, implemented by user
	// EXTENSION = 28,

	///// Array with fixed length of some logical type (a fixed-size list)
	ARRAY = 29,

	///// Measure of elapsed time in either seconds, milliseconds, microseconds
	///// or nanoseconds.
	// DURATION = 30,

	///// Like STRING, but with 64-bit offsets
	// LARGE_STRING = 31,

	///// Like BINARY, but with 64-bit offsets
	// LARGE_BINARY = 32,

	///// Like LIST, but with 64-bit offsets
	// LARGE_LIST = 33,

	/// DuckDB Extensions
	VARCHAR = 200, // our own string representation, different from STRING and LARGE_STRING above
	UINT128 = 203, // 128-bit unsigned integers
	INT128 = 204,  // 128-bit integers
	UNKNOWN = 205, // Unknown physical type of user defined types
	/// Boolean as 1 bit, LSB bit-packed ordering
	BIT = 206,

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
	UNBOUND = 4, /* A parsed but unbound type, used during query planning */

	// A "template" type functions as a "placeholder" type for function arguments and return types.
	// Templates only exist during the binding phase, in the scope of a function, and are replaced with concrete types
	// before execution. When defining a template, you provide a name to distinguish between different template types,
	// specifying to the binder that they dont need to resolve to the same concrete type. Two templates with the same
	// name are always resolved to the same concrete type.
	TEMPLATE = 5,

	TYPE = 6, /* Type type, used for type parameters */

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
	TIMESTAMP_TZ = 32,
	TIMESTAMP_TZ_NS = 33,
	TIME_TZ = 34,
	TIME_NS = 35,
	BIT = 36,
	STRING_LITERAL = 37,  /* string literals, used for constant strings - only exists while binding */
	INTEGER_LITERAL = 38, /* integer literals, used for constant integers - only exists while binding */
	BIGNUM = 39,
	UHUGEINT = 49,
	HUGEINT = 50,
	POINTER = 51,
	VALIDITY = 53,
	UUID = 54,

	GEOMETRY = 60,

	STRUCT = 100,
	LIST = 101,
	MAP = 102,
	TABLE = 103,
	ENUM = 104,
	LEGACY_AGGREGATE_STATE = 105,
	LAMBDA = 106,
	UNION = 107,
	ARRAY = 108,
	VARIANT = 109,
	TUPLE = 110 /* unnamed struct - shares the physical representation of STRUCT */
};

struct ExtensionTypeInfo;

//! The kind of a LogicalTypeInfo
enum class LogicalTypeInfoType : uint8_t {
	INVALID_TYPE_INFO = 0,
	GENERIC_TYPE_INFO = 1,
	DECIMAL_TYPE_INFO = 2,
	STRING_TYPE_INFO = 3,
	LIST_TYPE_INFO = 4,
	STRUCT_TYPE_INFO = 5,
	ENUM_TYPE_INFO = 6,
	UNBOUND_TYPE_INFO = 7,
	LEGACY_AGGREGATE_STATE_TYPE_INFO = 8,
	ARRAY_TYPE_INFO = 9,
	ANY_TYPE_INFO = 10,
	INTEGER_LITERAL_TYPE_INFO = 11,
	TEMPLATE_TYPE_INFO = 12,
	GEO_TYPE_INFO = 13
};

//! The intrusively ref-counted data behind a LogicalType. Types without parameters use an INVALID_TYPE_INFO.
//! Once adopted by a LogicalType the info is shared and only ever accessed as const.
struct LogicalTypeInfo {
	explicit LogicalTypeInfo(LogicalTypeInfoType type) noexcept;
	explicit LogicalTypeInfo(LogicalTypeInfoType type, string alias);
	virtual ~LogicalTypeInfo();

	// the 1-byte fields and the ref count are declared first so they pack into one word after the vtable pointer
	const LogicalTypeInfoType type;

private:
	friend struct LogicalType;
	//! The id and physical type of the LogicalType that owns this info - set when a LogicalType adopts it
	LogicalTypeId id = LogicalTypeId::INVALID;
	PhysicalType physical_type = PhysicalType::INVALID;
	//! Immortal infos (e.g. the one behind LogicalType::INTEGER) are shared without ref-counting and never freed.
	bool immortal = false;
	//! Atomic intrusive ref-count. This comes after the other members to avoid extra padding.
	mutable atomic<uint32_t> ref_count {0};

public:
	string alias;
	unique_ptr<ExtensionTypeInfo> extension_info;

protected:
	// copy	constructor (protected)
	LogicalTypeInfo(const LogicalTypeInfo &other);

public:
	LogicalTypeInfo &operator=(const LogicalTypeInfo &other) = delete;

	inline void AddRef() const {
		if (immortal) {
			return;
		}
		auto previous = ref_count.fetch_add(1, std::memory_order_relaxed);
		D_ASSERT(previous < std::numeric_limits<uint32_t>::max());
		(void)previous;
	}
	inline void Release() const {
		if (immortal) {
			return;
		}
		auto previous = ref_count.fetch_sub(1, std::memory_order_acq_rel);
		D_ASSERT(previous > 0);
		if (previous == 1) {
			delete this;
		}
	}

	bool Equals(const LogicalTypeInfo &other) const;

	virtual void Serialize(Serializer &serializer) const;
	static unique_ptr<LogicalTypeInfo> Deserialize(Deserializer &source);
	//! Creates an unshared copy of the info - nested types are shared with the original
	virtual unique_ptr<LogicalTypeInfo> Copy() const;
	//! Copy the base fields (alias, extension info) into "target" - used by Copy implementations that
	//! reconstruct the type info instead of copy-constructing it
	void CopyBaseInfo(LogicalTypeInfo &target) const;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

protected:
	virtual bool EqualsInternal(const LogicalTypeInfo *other_p) const;
};

struct LogicalType {
	DUCKDB_API LogicalType();
	DUCKDB_API LogicalType(LogicalTypeId id); // NOLINT: Allow implicit conversion from `LogicalTypeId`
	DUCKDB_API LogicalType(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info);
	inline LogicalType(const LogicalType &other) : type_info_(other.type_info_) {
		type_info_.get().AddRef();
	}
	DUCKDB_API LogicalType(LogicalType &&other) noexcept;

	inline ~LogicalType() {
		type_info_.get().Release();
	}

	inline LogicalTypeId id() const { // NOLINT: mimic std casing
		return type_info_.get().id;
	}
	inline PhysicalType InternalType() const {
		return type_info_.get().physical_type;
	}
	//! The type info - always present, of type INVALID_TYPE_INFO for types without parameters
	inline const LogicalTypeInfo &GetTypeInfo() const {
		return type_info_.get();
	}
	inline bool IsNested() const {
		auto internal = InternalType();
		if (internal == PhysicalType::STRUCT) {
			return true;
		}
		if (internal == PhysicalType::LIST) {
			return true;
		}
		if (internal == PhysicalType::ARRAY) {
			return true;
		}
		return false;
	}
	inline bool IsUnknown() const {
		return id() == LogicalTypeId::UNKNOWN;
	}
	inline bool IsUnbound() const {
		return id() == LogicalTypeId::UNBOUND;
	}
	//! Gives up this type's reference to its info without releasing it - the reference must be taken back with
	//! AdoptTypeInfo. Used to hand types across an ABI boundary (e.g. the C API) without copying them.
	DUCKDB_API const LogicalTypeInfo &ReleaseTypeInfo() &&;
	//! Creates a type that takes over a reference given up by ReleaseTypeInfo
	DUCKDB_API static LogicalType AdoptTypeInfo(const LogicalTypeInfo &type_info);
	//! Whether this type carries the type info its id is parameterized by (e.g. the child type of a LIST). Does not
	//! look at child types - false for a bare LogicalTypeId::LIST, true for LIST(ANY).
	DUCKDB_API bool HasParameters() const;

	//! Copies the logical type, making a new (unshared) LogicalTypeInfo
	LogicalType Copy() const;

	bool EqualTypeInfo(const LogicalType &rhs) const;

	// copy assignment
	inline LogicalType &operator=(const LogicalType &other) {
		other.type_info_.get().AddRef();
		type_info_.get().Release();
		type_info_ = other.type_info_;
		return *this;
	}
	// move assignment
	inline LogicalType &operator=(LogicalType &&other) noexcept {
		std::swap(type_info_, other.type_info_);
		return *this;
	}

	DUCKDB_API bool operator==(const LogicalType &rhs) const;
	//! Logical type equality including collations in this type and its children.
	DUCKDB_API bool EqualsIncludingCollation(const LogicalType &rhs) const;
	inline bool operator!=(const LogicalType &rhs) const {
		return !(*this == rhs);
	}

	DUCKDB_API void Serialize(Serializer &serializer) const;
	DUCKDB_API static LogicalType Deserialize(Deserializer &deserializer);

	static bool TypeIsTimestamp(LogicalTypeId id) {
		return (id == LogicalTypeId::TIMESTAMP || id == LogicalTypeId::TIMESTAMP_MS ||
		        id == LogicalTypeId::TIMESTAMP_NS || id == LogicalTypeId::TIMESTAMP_SEC ||
		        id == LogicalTypeId::TIMESTAMP_TZ);
	}
	static bool TypeIsTimestamp(const LogicalType &type) {
		return TypeIsTimestamp(type.id());
	}
	DUCKDB_API string ToString() const;
	DUCKDB_API bool IsIntegral() const;
	DUCKDB_API bool IsFloating() const;
	DUCKDB_API bool IsNumeric() const;
	DUCKDB_API static bool IsNumeric(LogicalTypeId type);
	DUCKDB_API bool IsTemporal() const;
	DUCKDB_API hash_t Hash() const;
	//! Returns a copy of this type with "alias" as its alias - never modifies types that share our type info
	DUCKDB_API LogicalType WithAlias(string alias) const;
	DUCKDB_API bool HasAlias() const;
	DUCKDB_API string GetAlias() const;

	DUCKDB_API bool HasExtensionInfo() const;
	DUCKDB_API optional_ptr<const ExtensionTypeInfo> GetExtensionInfo() const;
	//! Returns a copy of this type with "info" as its extension info - never modifies types that share our type info
	DUCKDB_API LogicalType WithExtensionInfo(unique_ptr<ExtensionTypeInfo> info) const;

	//! Returns the maximum logical type when combining the two types - or throws an exception if combining is not
	//! possible
	DUCKDB_API static LogicalType MaxLogicalType(ClientContext &context, const LogicalType &left,
	                                             const LogicalType &right);
	DUCKDB_API static bool TryGetMaxLogicalType(ClientContext &context, const LogicalType &left,
	                                            const LogicalType &right, LogicalType &result);
	DUCKDB_API static bool TryGetMaxLogicalTypeUnchecked(ClientContext &context, const LogicalType &left,
	                                                     const LogicalType &right, LogicalType &result);
	//! Variant of TryGetMaxLogicalTypeUnchecked for call-sites that have no ClientContext available; uses only the
	//! built-in CastRules (no extension-registered casts).
	DUCKDB_API static bool DefaultTryGetMaxLogicalTypeUnchecked(const LogicalType &left, const LogicalType &right,
	                                                            LogicalType &result);
	//! Forcibly returns a maximum logical type - similar to MaxLogicalType but never throws. As a fallback either left
	//! or right are returned.
	DUCKDB_API static LogicalType ForceMaxLogicalType(ClientContext &context, const LogicalType &left,
	                                                  const LogicalType &right);
	//! Variant of ForceMaxLogicalType for call-sites that have no ClientContext available; uses only the built-in
	//! CastRules (no extension-registered casts).
	DUCKDB_API static LogicalType DefaultForceMaxLogicalType(const LogicalType &left, const LogicalType &right);
	//! Normalize a type - removing literals
	DUCKDB_API static LogicalType NormalizeType(const LogicalType &type);
	//! Nested type children with Parquet style names
	DUCKDB_API static child_list_t<LogicalType> GetNamedChildTypes(const LogicalType &type);
	//! Rebuild a nested type with new children
	DUCKDB_API static LogicalType ConstructNestedType(const LogicalType &type, child_list_t<LogicalType> children);

	//! Gets the decimal properties of a numeric type. Fails if the type is not numeric.
	DUCKDB_API bool GetDecimalProperties(uint8_t &width, uint8_t &scale) const;

	DUCKDB_API void Verify() const;

	DUCKDB_API bool IsSigned() const;
	DUCKDB_API bool IsUnsigned() const;

	DUCKDB_API bool IsValid() const;
	DUCKDB_API bool IsComplete() const;
	DUCKDB_API bool IsTemplated() const;

	//! True, if this type supports in-place updates.
	bool SupportsRegularUpdate() const;

private:
	//! Takes over a reference to "type_info"
	explicit LogicalType(const LogicalTypeInfo &type_info) : type_info_(type_info) {
	}

	const_reference<LogicalTypeInfo> type_info_; // NOLINT: allow this naming for legacy reasons

private:
	//! The immortal type info of a type without parameters - created once per id
	DUCKDB_API static const LogicalTypeInfo &GetBuiltinTypeInfo(LogicalTypeId id);
	//! The builtin info for an id, or nullptr for an invalid id - creating the builtins never allocates, so cannot fail
	static const LogicalTypeInfo *TryGetBuiltinTypeInfo(LogicalTypeId id) noexcept;
	//! Takes ownership of a new info for a type with the given id - or returns the builtin info if there is none
	static const LogicalTypeInfo &AdoptNewTypeInfo(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info);
	//! Creates a type whose info is never ref-counted or freed - for types that are built once and shared
	static LogicalType CreateImmortal(LogicalTypeId id, unique_ptr<LogicalTypeInfo> type_info);

public:
	static constexpr const LogicalTypeId SQLNULL = LogicalTypeId::SQLNULL;
	static constexpr const LogicalTypeId UNKNOWN = LogicalTypeId::UNKNOWN;
	static constexpr const LogicalTypeId BOOLEAN = LogicalTypeId::BOOLEAN;
	static constexpr const LogicalTypeId TINYINT = LogicalTypeId::TINYINT;
	static constexpr const LogicalTypeId UTINYINT = LogicalTypeId::UTINYINT;
	static constexpr const LogicalTypeId SMALLINT = LogicalTypeId::SMALLINT;
	static constexpr const LogicalTypeId USMALLINT = LogicalTypeId::USMALLINT;
	static constexpr const LogicalTypeId INTEGER = LogicalTypeId::INTEGER;
	static constexpr const LogicalTypeId UINTEGER = LogicalTypeId::UINTEGER;
	static constexpr const LogicalTypeId BIGINT = LogicalTypeId::BIGINT;
	static constexpr const LogicalTypeId UBIGINT = LogicalTypeId::UBIGINT;
	static constexpr const LogicalTypeId FLOAT = LogicalTypeId::FLOAT;
	static constexpr const LogicalTypeId DOUBLE = LogicalTypeId::DOUBLE;
	static constexpr const LogicalTypeId DATE = LogicalTypeId::DATE;
	static constexpr const LogicalTypeId TIMESTAMP = LogicalTypeId::TIMESTAMP;
	static constexpr const LogicalTypeId TIMESTAMP_S = LogicalTypeId::TIMESTAMP_SEC;
	static constexpr const LogicalTypeId TIMESTAMP_MS = LogicalTypeId::TIMESTAMP_MS;
	static constexpr const LogicalTypeId TIMESTAMP_NS = LogicalTypeId::TIMESTAMP_NS;
	static constexpr const LogicalTypeId TIME = LogicalTypeId::TIME;
	static constexpr const LogicalTypeId TIME_NS = LogicalTypeId::TIME_NS;
	static constexpr const LogicalTypeId TIMESTAMP_TZ = LogicalTypeId::TIMESTAMP_TZ;
	static constexpr const LogicalTypeId TIMESTAMP_TZ_NS = LogicalTypeId::TIMESTAMP_TZ_NS;
	static constexpr const LogicalTypeId TIME_TZ = LogicalTypeId::TIME_TZ;
	static constexpr const LogicalTypeId VARCHAR = LogicalTypeId::VARCHAR;
	static constexpr const LogicalTypeId ANY = LogicalTypeId::ANY;
	static constexpr const LogicalTypeId BLOB = LogicalTypeId::BLOB;
	static constexpr const LogicalTypeId BIT = LogicalTypeId::BIT;
	static constexpr const LogicalTypeId BIGNUM = LogicalTypeId::BIGNUM;

	static constexpr const LogicalTypeId INTERVAL = LogicalTypeId::INTERVAL;
	static constexpr const LogicalTypeId HUGEINT = LogicalTypeId::HUGEINT;
	static constexpr const LogicalTypeId UHUGEINT = LogicalTypeId::UHUGEINT;
	static constexpr const LogicalTypeId UUID = LogicalTypeId::UUID;
	static constexpr const LogicalTypeId HASH = LogicalTypeId::UBIGINT;
	static constexpr const LogicalTypeId POINTER = LogicalTypeId::POINTER;
	static constexpr const LogicalTypeId TABLE = LogicalTypeId::TABLE;
	static constexpr const LogicalTypeId LAMBDA = LogicalTypeId::LAMBDA;
	static constexpr const LogicalTypeId INVALID = LogicalTypeId::INVALID;
	static constexpr const LogicalTypeId ROW_TYPE = LogicalTypeId::BIGINT;

	// explicitly allowing these functions to be capitalized to be in-line with the remaining functions
	DUCKDB_API static LogicalType DECIMAL(uint8_t width, uint8_t scale);      // NOLINT
	DUCKDB_API static LogicalType VARCHAR_COLLATION(string collation);        // NOLINT
	DUCKDB_API static LogicalType LIST(const LogicalType &child);             // NOLINT
	DUCKDB_API static LogicalType STRUCT(child_list_t<LogicalType> children); // NOLINT
	//! Unnamed struct - shares the physical representation of STRUCT but is a distinct logical type
	DUCKDB_API static LogicalType TUPLE(child_list_t<LogicalType> children);           // NOLINT
	DUCKDB_API static LogicalType TUPLE(vector<LogicalType> children);                 // NOLINT
	DUCKDB_API static LogicalType MAP(const LogicalType &child);                       // NOLINT
	DUCKDB_API static LogicalType MAP(LogicalType key, LogicalType value);             // NOLINT
	DUCKDB_API static LogicalType UNION(child_list_t<LogicalType> members);            // NOLINT
	DUCKDB_API static LogicalType ARRAY(const LogicalType &child, optional_idx index); // NOLINT
	DUCKDB_API static LogicalType ENUM(const Vector &ordered_data, idx_t size);        // NOLINT
	DUCKDB_API static LogicalType GEOMETRY();                                          // NOLINT
	DUCKDB_API static LogicalType GEOMETRY(const string &crs);
	DUCKDB_API static LogicalType GEOMETRY(const CoordinateReferenceSystem &crs);
	// ANY but with special rules (default is LogicalType::ANY, 5)
	DUCKDB_API static LogicalType ANY_PARAMS(LogicalType target, idx_t cast_score = 5); // NOLINT
	DUCKDB_API static LogicalType TEMPLATE(const string &name);                         // NOLINT
	DUCKDB_API static LogicalType VARIANT();                                            // NOLINT
	//! Integer literal of the specified value
	DUCKDB_API static LogicalType INTEGER_LITERAL(const Value &constant); // NOLINT
	// DEPRECATED - provided for backwards compatibility
	DUCKDB_API static LogicalType ENUM(const string &enum_name, const Vector &ordered_data, idx_t size); // NOLINT
	DUCKDB_API static LogicalType UNBOUND(unique_ptr<ParsedExpression> expr);                            // NOLINT
	DUCKDB_API static LogicalType TYPE();                                                                // NOLINT
	//! A list of all NUMERIC types (integral and floating point types)
	DUCKDB_API static const vector<LogicalType> Numeric();
	//! A list of all INTEGRAL types
	DUCKDB_API static const vector<LogicalType> Integral();
	//! A list of all REAL types
	DUCKDB_API static const vector<LogicalType> Real();
	//! A list of ALL SQL types
	DUCKDB_API static const vector<LogicalType> AllTypes();

public:
	//! The JSON type lives in the JSON extension, but we need to define this here for special handling
	static constexpr auto JSON_TYPE_NAME = "JSON";
	DUCKDB_API static LogicalType JSON(); // NOLINT
	DUCKDB_API bool IsJSONType() const;
	DUCKDB_API bool IsAggregateState() const;
};

struct DecimalType {
	DUCKDB_API static uint8_t GetWidth(const LogicalType &type);
	DUCKDB_API static uint8_t GetScale(const LogicalType &type);
	DUCKDB_API static uint8_t MaxWidth();
};

struct StringType {
	DUCKDB_API static string GetCollation(const LogicalType &type);
};

struct ListType {
	DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type);
};

struct UnboundType {
	// Try to bind the unbound type into a concrete type, using just the built in types
	DUCKDB_API static LogicalType TryParseAndDefaultBind(const string &type_str);
	DUCKDB_API static LogicalType TryDefaultBind(const LogicalType &unbound_type);
	DUCKDB_API static LogicalType TryDefaultBind(const ParsedExpression &type_expr);
	DUCKDB_API static const unique_ptr<ParsedExpression> &GetTypeExpression(const LogicalType &type);
};

struct EnumType {
	DUCKDB_API static int64_t GetPos(const LogicalType &type, const string_t &key);
	DUCKDB_API static const Vector &GetValuesInsertOrder(const LogicalType &type);
	DUCKDB_API static idx_t GetSize(const LogicalType &type);
	DUCKDB_API static const string GetValue(const Value &val);
	DUCKDB_API static PhysicalType GetPhysicalType(const LogicalType &type);
	DUCKDB_API static string_t GetString(const LogicalType &type, idx_t pos);
};

struct StructType {
	DUCKDB_API static const child_list_t<LogicalType> &GetChildTypes(const LogicalType &type);
	DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type, idx_t index);
	DUCKDB_API static const Identifier &GetChildName(const LogicalType &type, idx_t index);
	DUCKDB_API static idx_t GetChildIndexUnsafe(const LogicalType &type, const string &name);
	DUCKDB_API static idx_t GetChildCount(const LogicalType &type);
	//! Whether the type is an unnamed struct, i.e. a TUPLE
	DUCKDB_API static bool IsUnnamed(const LogicalType &type);
	//! Whether all (and at least one) of the children are unnamed - LogicalType::STRUCT turns these into a TUPLE
	DUCKDB_API static bool AllUnnamed(const child_list_t<LogicalType> &children);
	//! Whether the type is backed by StructTypeInfo and laid out as a struct (STRUCT or TUPLE)
	DUCKDB_API static bool IsStruct(const LogicalType &type);
	DUCKDB_API static bool IsStruct(LogicalTypeId id);
};

//! Helpers for the unnamed-struct TUPLE type. A TUPLE has no member names; when a name is required (e.g. when
//! converting to a format that mandates field names), positional names "element1", "element2", ... are synthesized -
//! matching the names produced by UNNEST of a struct.
struct TupleType {
	//! The synthesized positional name for the i-th (0-based) member: "element1", "element2", ...
	DUCKDB_API static string GetChildName(idx_t index);
	//! Returns the child types of a struct-like type, with synthesized names for any unnamed (empty-named) members.
	//! Members that already have a name keep it.
	DUCKDB_API static child_list_t<LogicalType> NamedChildren(const LogicalType &type);
};

struct MapType {
	DUCKDB_API static const LogicalType &KeyType(const LogicalType &type);
	DUCKDB_API static const LogicalType &ValueType(const LogicalType &type);
};

struct UnionType {
	DUCKDB_API static const idx_t MAX_UNION_MEMBERS = 255;
	DUCKDB_API static idx_t GetMemberCount(const LogicalType &type);
	DUCKDB_API static const LogicalType &GetMemberType(const LogicalType &type, idx_t index);
	DUCKDB_API static const Identifier &GetMemberName(const LogicalType &type, idx_t index);
	DUCKDB_API static const child_list_t<LogicalType> CopyMemberTypes(const LogicalType &type);
};

struct ArrayType {
	DUCKDB_API static const LogicalType &GetChildType(const LogicalType &type);
	DUCKDB_API static idx_t GetSize(const LogicalType &type);
	DUCKDB_API static bool IsAnySize(const LogicalType &type);
	DUCKDB_API static constexpr idx_t MAX_ARRAY_SIZE = 100000; // 100k for now
	//! Recursively replace all ARRAY types to LIST types within the given type
	DUCKDB_API static LogicalType ConvertToList(const LogicalType &type);
};

struct AnyType {
	DUCKDB_API static LogicalType GetTargetType(const LogicalType &type);
	DUCKDB_API static idx_t GetCastScore(const LogicalType &type);
};

struct IntegerLiteral {
	//! Returns the type that this integer literal "prefers"
	DUCKDB_API static LogicalType GetType(const LogicalType &type);
	//! Whether or not the integer literal fits into the target numeric type
	DUCKDB_API static bool FitsInType(const LogicalType &type, const LogicalType &target);
};

struct TemplateType {
	// Get the name of the template type
	DUCKDB_API static const string &GetName(const LogicalType &type);
};

struct GeoType {
	DUCKDB_API static bool HasCRS(const LogicalType &type);
	DUCKDB_API static const CoordinateReferenceSystem &GetCRS(const LogicalType &type);
};

// **DEPRECATED**: Use EnumUtil directly instead.
DUCKDB_API string LogicalTypeIdToString(LogicalTypeId type);

DUCKDB_API LogicalTypeId TransformStringToLogicalTypeId(const string &str);

DUCKDB_API LogicalType TransformStringToLogicalType(const string &str, ClientContext &context);

//! The PhysicalType used by the row identifiers column
extern const PhysicalType ROW_TYPE;

DUCKDB_API string TypeIdToString(PhysicalType type);
DUCKDB_API idx_t GetTypeIdSize(PhysicalType type);
DUCKDB_API bool TypeIsConstantSize(PhysicalType type);
DUCKDB_API bool TypeIsIntegral(PhysicalType type);
DUCKDB_API bool TypeIsNumeric(PhysicalType type);
DUCKDB_API bool TypeIsInteger(PhysicalType type);

bool ApproxEqual(float l, float r);
bool ApproxEqual(double l, double r);

} // namespace duckdb
