#include "duckdb/catalog/catalog_entry/window_function_catalog_entry.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_window_function_info.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

WindowFunctionCatalogEntry::WindowFunctionCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema,
                                                       CreateWindowFunctionInfo &info)
    : FunctionEntry(Type, catalog, schema, info), functions(info.functions) {
	functions.ApplyToFunctions(
	    [&](WindowFunction &function) { function.SetQualifiedName(schema.GetQualifiedName(name)); });
}

} // namespace duckdb
