//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry/function_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

namespace duckdb {

//! A scalar function in the catalog
class ScalarFunctionCatalogEntry : public FunctionEntry {
public:
	static constexpr const CatalogType Type = CatalogType::SCALAR_FUNCTION_ENTRY;
	static constexpr const char *Name = "scalar function";

public:
	ScalarFunctionCatalogEntry(Catalog &catalog, SchemaCatalogEntry &schema, CreateScalarFunctionInfo &info);

	//! The scalar functions
	ScalarFunctionSet functions;

public:
	unique_ptr<CatalogEntry> AlterEntry(CatalogTransaction transaction, AlterInfo &info) override;

private:
	friend class ExtensionLoader;
	shared_ptr<const ScalarFunction> FinalizeFunction(ScalarFunction function) const;
};
} // namespace duckdb
