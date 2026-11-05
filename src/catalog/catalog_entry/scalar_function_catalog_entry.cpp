#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"

#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

constexpr const char *ScalarFunctionCatalogEntry::Name;

ScalarFunctionCatalogEntry::ScalarFunctionCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                                       CreateScalarFunctionInfo &info)
    : FunctionEntry(CatalogType::SCALAR_FUNCTION_ENTRY, catalog, schema, info), functions(info.functions) {
	for (auto &function : functions.functions) {
		function = FinalizeFunction(*function);
	}
}

shared_ptr<const ScalarFunction> ScalarFunctionCatalogEntry::FinalizeFunction(ScalarFunction function) const {
	auto result = make_shared_ptr<ScalarFunction>(std::move(function));
	result->SetQualifiedName(schema.GetQualifiedName(name));
	return result;
}

unique_ptr<CatalogEntry> ScalarFunctionCatalogEntry::AlterEntry(CatalogTransaction transaction, AlterInfo &info) {
	if (info.type != AlterType::ALTER_SCALAR_FUNCTION) {
		throw InternalException("Attempting to alter ScalarFunctionCatalogEntry with unsupported alter type");
	}
	auto &function_info = info.Cast<AlterScalarFunctionInfo>();
	if (function_info.alter_scalar_function_type != AlterScalarFunctionType::ADD_FUNCTION_OVERLOADS) {
		throw InternalException(
		    "Attempting to alter ScalarFunctionCatalogEntry with unsupported alter scalar function type");
	}
	auto &add_overloads = function_info.Cast<AddScalarFunctionOverloadInfo>();

	ScalarFunctionSet new_set = functions;
	if (!new_set.MergeFunctionSet(add_overloads.new_overloads->functions, true)) {
		throw BinderException("Failed to add new function overloads to function %s: function overload already exists",
		                      name);
	}
	CreateScalarFunctionInfo new_info(std::move(new_set));
	new_info.internal = internal;
	new_info.descriptions = descriptions;
	new_info.descriptions.insert(new_info.descriptions.end(), add_overloads.new_overloads->descriptions.begin(),
	                             add_overloads.new_overloads->descriptions.end());
	return make_uniq<ScalarFunctionCatalogEntry>(catalog, schema, new_info);
}

} // namespace duckdb
