#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/default/default_types.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/logical_type_info.hpp"
#include "duckdb/common/types/geometry_crs.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

#include <cstdlib>
#include <cstring>

namespace duckdb::capiv2 {
namespace {

// The set of LogicalTypeIds a parameterless duckdb_v2_*_create_type_from_id
// instantiates directly, without touching the catalog. Includes ANY, the
// function-signature wildcard: it is constructible so it can be passed to
// function parameter / varargs setters, while data-creating surfaces reject
// it. Excludes:
//  - INVALID (sentinel),
//  - the remaining bind-time-only ids (SQLNULL, UNKNOWN) which only exist
//    inside the planner / UDF binding paths,
//  - parameterised types (DECIMAL, LIST, STRUCT, TUPLE, MAP, ARRAY, UNION,
//    ENUM, VARIANT, GEOMETRY), which need parameters to bind.
bool IsPrimitiveCreatable(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::ANY:
	case LogicalTypeId::BOOLEAN:
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
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::UUID:
		return true;
	default:
		return false;
	}
}

// Ids that never name a constructible type, with or without parameters:
// the zero sentinel plus the ids that only exist inside the planner / UDF
// binding paths. TYPE is reachable only through create_type_from_text.
bool IsNonConstructible(LogicalTypeId id) {
	switch (id) {
	case LogicalTypeId::INVALID:
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::TYPE:
		return true;
	default:
		return false;
	}
}

bool IsValidTypeEntry(optional_ptr<CatalogEntry> entry) {
	if (!entry) {
		return false;
	}
	return entry->Cast<TypeCatalogEntry>().user_type.id() != LogicalTypeId::INVALID;
}

// Hand-mirrors ExpressionBinder::BindExpression(TypeExpression&) in
// src/planner/binder/expression/bind_type_expression.cpp: search path
// first, then the system catalog (where custom_type_builder registers).
// Intentional omissions vs the mirrored source, per the unqualified-only
// contract: qualified names (catalog/schema splitting and the 4-step
// lookup), expression-valued params (values arrive pre-folded here), and
// query-location error context. If engine type binding changes, this
// mirror must follow. Runs inside a transaction; the caller provides it.
LogicalType BindTypeByNameV2(ClientContext &context, const QualifiedName &name, const vector<TypeArgument> &args) {
	EntryLookupInfo lookup(CatalogType::TYPE_ENTRY, name);
	CatalogEntryRetriever retriever(context);
	optional_ptr<CatalogEntry> entry;
	if (name.Path().size() > 1) {
		// The caller named a catalog or schema: resolve exactly that, without falling back to the system catalog.
		entry = retriever.GetEntry(lookup, OnEntryNotFound::THROW_EXCEPTION);
	} else if (!DatabaseManager::Get(context).HasAttachedDatabase()) {
		entry = retriever.GetEntry(
		    EntryLookupInfo(lookup, QualifiedName(Identifier::SystemCatalog(), Identifier::InvalidSchema(),
		                                          lookup.GetEntryIdentifier())));
	} else {
		entry = retriever.GetEntry(lookup, OnEntryNotFound::RETURN_NULL);
		if (!IsValidTypeEntry(entry)) {
			entry = retriever.GetEntry(
			    EntryLookupInfo(lookup, QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(),
			                                          lookup.GetEntryIdentifier())),
			    OnEntryNotFound::THROW_EXCEPTION);
		}
	}
	auto &type_entry = entry->Cast<TypeCatalogEntry>();
	return type_entry.constructors.Bind(context, type_entry.user_type, args);
}

// Binds a built-in type by name, without a client: constructors that consult the catalog (collations, coordinate
// systems) skip that check or refuse.
LogicalType BindBuiltinTypeV2(const QualifiedName &name, const vector<TypeArgument> &args) {
	if (name.Path().size() > 1) {
		throw InvalidInputException("a qualified type name requires a catalog: use the factory of a connection or a "
		                            "context");
	}
	vector<pair<string, Value>> params;
	params.reserve(args.size());
	for (auto &arg : args) {
		params.emplace_back(arg.GetName(), arg.GetValue());
	}
	auto type = DefaultTypeGenerator::TryDefaultBind(name.Name().GetIdentifierName(), params);
	if (type.id() == LogicalTypeId::INVALID) {
		throw InvalidInputException("type \"%s\" is not a built-in type", name.Name().GetIdentifierName());
	}
	return type;
}

// Binds a type by name in the factory's scope: the client's catalog, or the built-in types only.
LogicalType BindTypeV2(CV2Factory &factory, const QualifiedName &name, const vector<TypeArgument> &args) {
	if (!factory.TryGetClientContext()) {
		return BindBuiltinTypeV2(name, args);
	}
	LogicalType result;
	factory.WithTransaction([&](ClientContext &context) { result = BindTypeByNameV2(context, name, args); });
	return result;
}

// The value-parameter view of a bound type: the exact dual of
// logical_type_create. Kinds without retained parameters report 0.
idx_t TypeParamCount(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::MAP:
		return 2;
	case LogicalTypeId::LIST:
		return 1;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		return StructType::GetChildCount(type);
	case LogicalTypeId::UNION:
		return UnionType::GetMemberCount(type);
	case LogicalTypeId::ENUM:
		return EnumType::GetSize(type);
	case LogicalTypeId::VARCHAR:
		return StringType::GetCollation(type).empty() ? 0 : 1;
	case LogicalTypeId::GEOMETRY:
		return GeoType::HasCRS(type) ? 1 : 0;
	default:
		return 0;
	}
}

// Produces param `index` of `type`: the owned value is returned, the
// borrowed name (off the type, or a static literal) goes into out_name,
// which arrives pre-set to the positional {NULL, 0}. The caller has
// bounds-checked index against TypeParamCount.
Value TypeParamValue(const LogicalType &type, idx_t index, duckdb_v2_identifier_t &out_name) {
	switch (type.id()) {
	case LogicalTypeId::DECIMAL:
		return index == 0 ? Value::UTINYINT(DecimalType::GetWidth(type)) : Value::UTINYINT(DecimalType::GetScale(type));
	case LogicalTypeId::LIST:
		return Value::TYPE(ListType::GetChildType(type));
	case LogicalTypeId::ARRAY:
		return index == 0 ? Value::TYPE(ArrayType::GetChildType(type))
		                  : Value::BIGINT(NumericCast<int64_t>(ArrayType::GetSize(type)));
	case LogicalTypeId::MAP:
		return Value::TYPE(index == 0 ? MapType::KeyType(type) : MapType::ValueType(type));
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
		if (!StructType::IsUnnamed(type)) {
			out_name = Convert(StructType::GetChildName(type, index));
		}
		return Value::TYPE(StructType::GetChildType(type, index));
	case LogicalTypeId::UNION:
		out_name = Convert(UnionType::GetMemberName(type, index));
		return Value::TYPE(UnionType::GetMemberType(type, index));
	case LogicalTypeId::ENUM:
		return Value(EnumType::GetString(type, index).GetString());
	case LogicalTypeId::VARCHAR:
		out_name = duckdb_v2_identifier_t {"collation", 9};
		return Value(StringType::GetCollation(type));
	case LogicalTypeId::GEOMETRY:
		return Value(GeoType::GetCRS(type).GetDefinition());
	default:
		throw InternalException("TypeParamValue called for a kind without parameters");
	}
}

} // anonymous namespace

auto ResolveCommonType(CV2Factory &factory, const vector<LogicalType> &types) -> LogicalType {
	D_ASSERT(!types.empty());
	auto context = factory.TryGetClientContext();
	auto result = types[0];
	for (idx_t i = 1; i < types.size(); i++) {
		auto &type = types[i];
		if (context) {
			result = LogicalType::MaxLogicalType(*context, result, type);
			continue;
		}
		LogicalType max_type;
		if (!LogicalType::DefaultTryGetMaxLogicalTypeUnchecked(result, type, max_type)) {
			throw NotImplementedException("Cannot combine types %s and %s - an explicit cast is required",
			                              result.ToString(), type.ToString());
		}
		result = std::move(max_type);
	}
	return result;
}

} // namespace duckdb::capiv2

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

using namespace duckdb::capiv2;

// Collects the parallel (name, value) arrays into bind arguments. An absent
// or {NULL, 0} name makes that parameter positional.
static duckdb::vector<duckdb::TypeArgument> CollectTypeArgsV2(const duckdb_v2_identifier_t *param_names,
                                                              const duckdb_v2_value_handle *param_values,
                                                              idx_t param_count) {
	duckdb::vector<duckdb::TypeArgument> args;
	args.reserve(param_count);
	for (idx_t i = 0; i < param_count; i++) {
		auto param_name = param_names ? param_names[i] : duckdb_v2_str {nullptr, 0};
		if (!param_name.ptr && param_name.len > 0) {
			throw duckdb::InvalidInputException("null parameter name at index %llu", i);
		}
		if (!param_values[i]) {
			throw duckdb::InvalidInputException("null parameter value at index %llu", i);
		}
		args.emplace_back(duckdb::string(ConvertIdentifierName(param_name)), *Convert(param_values[i]));
	}
	return args;
}

// A parameterless call instantiates the primitive directly; with parameters
// the id resolves to its canonical name and binds through the same path as
// create_type_from_name.
static void CreateLogicalTypeFromIdV2(CV2Factory &factory, DUCKDB_V2_LOGICAL_TYPE_ID type_id,
                                      const duckdb_v2_identifier_t *param_names,
                                      const duckdb_v2_value_handle *param_values, idx_t param_count,
                                      duckdb_v2_logical_type_handle *out_type, const char *fn) {
	if (param_count > 0 && !param_values) {
		throw duckdb::InvalidInputException("'param_values' cannot be null when param_count > 0");
	}
	*out_type = nullptr;
	auto id = static_cast<duckdb::LogicalTypeId>(type_id);
	if (IsNonConstructible(id)) {
		throw duckdb::InvalidInputException("%s does not accept this type id", fn);
	}
	if (param_count == 0) {
		if (!IsPrimitiveCreatable(id)) {
			throw duckdb::InvalidInputException("%s requires type parameters for this type id", fn);
		}
		*out_type = Convert(duckdb::LogicalType(id));
		return;
	}
	// Bind errors propagate.
	auto args = CollectTypeArgsV2(param_names, param_values, param_count);
	auto bound = BindTypeV2(factory, duckdb::QualifiedName(duckdb::Identifier(duckdb::EnumUtil::ToString(id))), args);
	*out_type = Convert(std::move(bound));
}

DUCKDB_V2_ERROR duckdb_v2_factory_create_type_from_id(duckdb_v2_factory_handle factory,
                                                      DUCKDB_V2_LOGICAL_TYPE_ID type_id,
                                                      const duckdb_v2_identifier_t *param_names,
                                                      const duckdb_v2_value_handle *param_values, idx_t param_count,
                                                      duckdb_v2_logical_type_handle *out_type,
                                                      duckdb_v2_error_info_handle *err) {
	static const char *fn = "duckdb_v2_factory_create_type_from_id";
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_type);
	return WithErrorHandler(err, [&]() {
		CreateLogicalTypeFromIdV2(*Convert(factory), type_id, param_names, param_values, param_count, out_type, fn);
	});
}

static void CreateLogicalTypeFromTextV2(CV2Factory &factory, const duckdb_v2_str *text,
                                        duckdb_v2_logical_type_handle *out_type) {
	*out_type = nullptr;
	// Parse and bind errors propagate.
	auto string = duckdb::string(Convert(text));
	if (!factory.TryGetClientContext()) {
		*out_type = Convert(duckdb::TypeManager::Get(factory.GetDatabase()).ParseLogicalType(string));
		return;
	}
	duckdb::LogicalType parsed;
	factory.WithTransaction(
	    [&](duckdb::ClientContext &context) { parsed = duckdb::TransformStringToLogicalType(string, context); });
	*out_type = Convert(std::move(parsed));
}

DUCKDB_V2_ERROR duckdb_v2_factory_create_type_from_text(duckdb_v2_factory_handle factory, const duckdb_v2_str *text,
                                                        duckdb_v2_logical_type_handle *out_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_type);
	DUCKDB_CHECK_ARG(text);
	return WithErrorHandler(err, [&]() { CreateLogicalTypeFromTextV2(*Convert(factory), text, out_type); });
}

static void CreateLogicalTypeFromArgsV2(CV2Factory &factory, duckdb_v2_qname_handle name,
                                        const duckdb_v2_identifier_t *param_names,
                                        const duckdb_v2_value_handle *param_values, idx_t param_count,
                                        duckdb_v2_logical_type_handle *out_type) {
	if (param_count > 0 && !param_values) {
		throw duckdb::InvalidInputException("'param_values' cannot be null when param_count > 0");
	}
	*out_type = nullptr;
	auto args = CollectTypeArgsV2(param_names, param_values, param_count);
	// Bind errors propagate.
	auto bound = BindTypeV2(factory, *Convert(name), args);
	*out_type = Convert(std::move(bound));
}

DUCKDB_V2_ERROR duckdb_v2_factory_create_type_from_name(duckdb_v2_factory_handle factory, duckdb_v2_qname_handle name,
                                                        const duckdb_v2_identifier_t *param_names,
                                                        const duckdb_v2_value_handle *param_values, idx_t param_count,
                                                        duckdb_v2_logical_type_handle *out_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_type);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		CreateLogicalTypeFromArgsV2(*Convert(factory), name, param_names, param_values, param_count, out_type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_copy(duckdb_v2_logical_type_handle type, duckdb_v2_logical_type_handle *out_type,
                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_type);
	return WithErrorHandler(err, [&]() {
		auto lt = Convert(type);
		*out_type = Convert(*lt);
	});
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_destroy(duckdb_v2_logical_type_handle *type) {
	return WithErrorHandler(nullptr, [&]() {
		if (!type) {
			return;
		}
		if (*type) {
			TakeOwnership(*type);
			*type = nullptr;
		}
	});
}

// ---------------------------------------------------------------------------
// Common introspection
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_logical_type_is_equal(duckdb_v2_logical_type_handle left, duckdb_v2_logical_type_handle right,
                                                bool *result, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(left);
	DUCKDB_CHECK_ARG(right);
	DUCKDB_CHECK_ARG(result);
	return WithErrorHandler(err, [&]() {
		auto left_lt = Convert(left);
		auto right_lt = Convert(right);

		*result = *left_lt == *right_lt;
	});
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_get_id(duckdb_v2_logical_type_handle type, DUCKDB_V2_LOGICAL_TYPE_ID *out_id,
                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_id);
	return WithErrorHandler(err, [&]() { *out_id = static_cast<DUCKDB_V2_LOGICAL_TYPE_ID>(Convert(type)->id()); });
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_get_name(duckdb_v2_logical_type_handle type, duckdb_v2_identifier_t *out_name,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_name);
	return WithErrorHandler(err, [&]() {
		auto lt = Convert(type);
		auto &info = lt->GetTypeInfo();
		if (!info.alias.empty()) {
			*out_name = Convert(info.alias);
			return;
		}
		// Canonical fixed name of the id: static storage, so the borrowed
		// view outlives even the handle.
		const char *canonical = duckdb::EnumUtil::ToChars<duckdb::LogicalTypeId>(lt->id());
		*out_name = duckdb_v2_str {canonical, std::strlen(canonical)};
	});
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_to_text(duckdb_v2_logical_type_handle type, char *out_text, idx_t out_capacity,
                                               idx_t *out_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		FillCallerText(out_text, out_capacity, out_length, Convert(type)->ToString(), "duckdb_v2_logical_type_to_text");
	});
}

// ---------------------------------------------------------------------------
// Generic parameter inspection
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_logical_type_get_param_count(duckdb_v2_logical_type_handle type, idx_t *out_count,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() {
		auto lt = Convert(type);
		*out_count = TypeParamCount(*lt);
	});
}

DUCKDB_V2_ERROR duckdb_v2_logical_type_get_param(duckdb_v2_logical_type_handle type, idx_t index,
                                                 duckdb_v2_identifier_t *out_name, duckdb_v2_value_handle *out_value,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(out_name);
	DUCKDB_CHECK_ARG(out_value);
	return WithErrorHandler(err, [&]() {
		*out_name = duckdb_v2_identifier_t {nullptr, 0};
		*out_value = nullptr;
		auto lt_ref = Convert(type);
		auto &lt = *lt_ref;
		if (index >= TypeParamCount(lt)) {
			throw duckdb::InvalidInputException("parameter index out of range in duckdb_v2_logical_type_get_param");
		}
		duckdb_v2_identifier_t name {nullptr, 0};
		auto *value = new duckdb::Value(TypeParamValue(lt, index, name));
		*out_name = name;
		*out_value = Convert(value);
	});
}

namespace {

// The alias keeps the base type's internal representation; only the name and
// its catalog identity change.
duckdb::LogicalType AliasOf(duckdb_v2_logical_type_handle base_type, const duckdb_v2_identifier_t *alias_name,
                            duckdb_v2_logical_type_handle *out_type) {
	*out_type = nullptr;
	auto name = ConvertIdentifierName(alias_name);
	if (name.empty()) {
		throw duckdb::InvalidInputException("alias name cannot be empty");
	}
	return Convert(base_type)->WithAlias(std::string(name));
}

} // namespace

DUCKDB_V2_ERROR duckdb_v2_factory_create_type_with_alias(duckdb_v2_factory_handle factory,
                                                         duckdb_v2_logical_type_handle base_type,
                                                         const duckdb_v2_identifier_t *alias_name,
                                                         duckdb_v2_logical_type_handle *out_type,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(base_type);
	DUCKDB_CHECK_ARG(alias_name);
	DUCKDB_CHECK_ARG(out_type);
	return WithErrorHandler(err, [&]() { *out_type = Convert(AliasOf(base_type, alias_name, out_type)); });
}

DUCKDB_V2_ERROR duckdb_v2_factory_create_common_type(duckdb_v2_factory_handle factory,
                                                     const duckdb_v2_logical_type_handle *types, idx_t type_count,
                                                     duckdb_v2_logical_type_handle *out_type,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(out_type);
	*out_type = nullptr;
	return WithErrorHandler(err, [&]() {
		if (type_count == 0 || !types) {
			throw duckdb::InvalidInputException("cannot resolve the common type of an empty set");
		}
		duckdb::vector<duckdb::LogicalType> input_types;
		input_types.reserve(type_count);
		for (idx_t i = 0; i < type_count; i++) {
			if (!types[i]) {
				throw duckdb::InvalidInputException("type %llu is null", i);
			}
			auto type = Convert(types[i]);
			if (type->id() == duckdb::LogicalTypeId::ANY) {
				throw duckdb::InvalidInputException("cannot resolve the common type of ANY");
			}
			input_types.push_back(*type);
		}
		*out_type = Convert(ResolveCommonType(*Convert(factory), input_types));
	});
}
