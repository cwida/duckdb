//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_table_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"

namespace duckdb {
class SchemaCatalogEntry;

struct DefaultNamedParameter {
	const char *name;
	const char *default_value;
};

struct DefaultTableMacro {
	const char *schema;
	const char *name;
	const char *parameters[8];
	DefaultNamedParameter named_parameters[8];
	const char *macro;
};

class DefaultTableFunctionGenerator : public DefaultGenerator {
public:
	DefaultTableFunctionGenerator(Catalog &catalog, SchemaCatalogEntry &schema);

	SchemaCatalogEntry &schema;

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;

	static unique_ptr<CreateMacroInfo> CreateTableMacroInfo(const DefaultTableMacro &default_macro);
	//! Finds a macro in a nullptr-terminated list
	static optional_ptr<const DefaultTableMacro> FindTableMacro(const DefaultTableMacro macros[],
	                                                            const Identifier &name,
	                                                            optional_ptr<const Identifier> schema_name = nullptr);
	static unique_ptr<CatalogEntry> CreateTableMacroEntry(Catalog &catalog, SchemaCatalogEntry &schema,
	                                                      const DefaultTableMacro &default_macro);

private:
	static unique_ptr<CreateMacroInfo> CreateInternalTableMacroInfo(const DefaultTableMacro &default_macro,
	                                                                unique_ptr<MacroFunction> function);
};

} // namespace duckdb
