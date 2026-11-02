#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_type_info.hpp"

namespace duckdb::capiv2 {

class CV2CustomType {
public:
	explicit CV2CustomType(DatabaseInstance &db) : db(db) {
	}

	// Validates the configuration and returns the type to install: the base type carrying the custom type's name as
	// its alias, which is what makes it logically distinct from the base type.
	LogicalType Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "custom type");
		if (name.empty()) {
			throw InvalidInputException("Type name cannot be empty.");
		}
		if (base_type.id() == LogicalTypeId::INVALID) {
			throw InvalidInputException("Base type must be set for the type.");
		}
		if (!base_type.IsComplete()) {
			throw InvalidInputException("Base type must be a fully defined concrete type");
		}
		return base_type.WithAlias(name.GetIdentifierName());
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	Identifier name;
	LogicalType base_type;
};

static auto Convert(duckdb_v2_custom_type_handle type) -> CV2CustomType * {
	return reinterpret_cast<CV2CustomType *>(type);
}
static auto Convert(CV2CustomType *type) -> duckdb_v2_custom_type_handle {
	return reinterpret_cast<duckdb_v2_custom_type_handle>(type);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_custom_type_create(duckdb_v2_factory_handle factory, duckdb_v2_custom_type_handle *type,
                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2CustomType>(Convert(factory)->GetDatabase());
		*type = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_type_set_name(duckdb_v2_custom_type_handle type, const duckdb_v2_identifier_t *name,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(type)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_custom_type_set_base_type(duckdb_v2_custom_type_handle type,
                                                    duckdb_v2_logical_type_handle base_type,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(type);
	DUCKDB_CHECK_ARG(base_type);
	return WithErrorHandler(err, [&]() {
		auto base = Convert(base_type);
		Convert(type)->base_type = *base;
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_custom_type(duckdb_v2_connection_handle conn,
                                                          duckdb_v2_custom_type_handle type,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto type_value = Convert(type)->Build(*context.db);
			// Read the name before the type is moved out: sibling arguments have no evaluation order.
			auto name = type_value.GetAlias();
			duckdb::CreateTypeInfo info(std::move(name), std::move(type_value));
			info.temporary = true;
			info.internal = true;
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			duckdb::Catalog::GetSystemCatalog(context).CreateType(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_custom_type(duckdb_v2_extension_handle extension,
                                                         duckdb_v2_custom_type_handle type,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		auto type_value = Convert(type)->Build(loader.GetDatabaseInstance());
		auto name = type_value.GetAlias();
		loader.RegisterType(std::move(name), std::move(type_value));
	});
}

DUCKDB_V2_ERROR duckdb_v2_custom_type_destroy(duckdb_v2_custom_type_handle *type) {
	return WithErrorHandler(nullptr, [&]() {
		if (!type) {
			return;
		}
		if (*type) {
			delete Convert(*type);
			*type = nullptr;
		}
	});
}
