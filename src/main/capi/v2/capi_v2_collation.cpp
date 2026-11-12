#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parsed_data/create_collation_info.hpp"

namespace duckdb::capiv2 {

class CV2CollationInfo {
public:
	explicit CV2CollationInfo(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the collation, looking up its function in `schema` of `target`.
	auto Build(DatabaseInstance &target, CatalogTransaction transaction, const Identifier &schema)
	    -> CreateCollationInfo {
		CheckRegistrationTarget(db, target, "collation");
		if (name.empty()) {
			throw InvalidInputException("Collation name cannot be empty.");
		}
		if (function_name.empty()) {
			throw InvalidInputException("A function name must be set for the collation.");
		}
		auto &catalog = Catalog::GetSystemCatalog(target);
		auto &schema_entry = catalog.GetSchema(transaction, schema);
		auto entry = schema_entry.GetEntry(transaction, CatalogType::SCALAR_FUNCTION_ENTRY, function_name);
		if (!entry) {
			throw InvalidInputException("Scalar function %s of collation %s does not exist.", function_name, name);
		}
		auto &functions = entry->Cast<ScalarFunctionCatalogEntry>().functions;
		for (idx_t i = 0; i < functions.Size(); i++) {
			auto &function = *functions.GetFunctionByOffset(i);
			const auto &signature = function.GetSignature();
			if (signature.GetParameterCount() != 1 || signature.IsVariadic() ||
			    signature.GetParameter(0).GetType().id() != LogicalTypeId::VARCHAR) {
				continue;
			}
			const auto return_id = signature.GetReturnType().id();
			if (return_id != LogicalTypeId::VARCHAR && return_id != LogicalTypeId::BLOB) {
				continue;
			}
			return CreateCollationInfo(name, function, combinable, not_required_for_equality);
		}
		throw InvalidInputException(
		    "Scalar function %s of collation %s has no overload taking one VARCHAR and returning VARCHAR or BLOB.",
		    function_name, name);
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	Identifier name;
	Identifier function_name;
	bool combinable = false;
	bool not_required_for_equality = false;
};

static auto Convert(duckdb_v2_collation_info_handle info) -> CV2CollationInfo * {
	return reinterpret_cast<CV2CollationInfo *>(info);
}
static auto Convert(CV2CollationInfo *info) -> duckdb_v2_collation_info_handle {
	return reinterpret_cast<duckdb_v2_collation_info_handle>(info);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------
using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_collation_info_create(duckdb_v2_factory_handle factory, duckdb_v2_collation_info_handle *info,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(info);
	*info = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2CollationInfo>(Convert(factory)->GetDatabase());
		*info = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_collation_info_set_name(duckdb_v2_collation_info_handle info, const char *name,
                                                  idx_t name_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(
	    err, [&]() { Convert(info)->name = duckdb::Identifier(ConvertIdentifierName(name, name_length)); });
}

DUCKDB_V2_ERROR duckdb_v2_collation_info_set_function_name(duckdb_v2_collation_info_handle info, const char *name,
                                                           idx_t name_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_STR_ARG(name, name_length);
	return WithErrorHandler(
	    err, [&]() { Convert(info)->function_name = duckdb::Identifier(ConvertIdentifierName(name, name_length)); });
}

DUCKDB_V2_ERROR duckdb_v2_collation_info_set_combinable(duckdb_v2_collation_info_handle info, bool combinable,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->combinable = combinable; });
}

DUCKDB_V2_ERROR duckdb_v2_collation_info_set_not_required_for_equality(duckdb_v2_collation_info_handle info,
                                                                       bool not_required_for_equality,
                                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->not_required_for_equality = not_required_for_equality; });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_collation(duckdb_v2_connection_handle conn,
                                                        duckdb_v2_collation_info_handle info,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			auto collation = Convert(info)->Build(*context.db, duckdb::CatalogTransaction(catalog, context),
			                                      duckdb::Identifier::DefaultSchema());
			collation.on_conflict = duckdb::OnCreateConflict::IGNORE_ON_CONFLICT;
			catalog.CreateCollation(context, collation);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_collation(duckdb_v2_extension_handle extension,
                                                       duckdb_v2_collation_info_handle info,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		auto &db = loader.GetDatabaseInstance();
		auto collation =
		    Convert(info)->Build(db, duckdb::CatalogTransaction::GetSystemTransaction(db), loader.GetExtensionSchema());
		loader.RegisterCollation(collation);
	});
}

DUCKDB_V2_ERROR duckdb_v2_collation_info_destroy(duckdb_v2_collation_info_handle *info) {
	return WithErrorHandler(nullptr, [&]() {
		if (!info) {
			return;
		}
		if (*info) {
			delete Convert(*info);
			*info = nullptr;
		}
	});
}
