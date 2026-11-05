//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"

namespace duckdb {
class SchemaCatalogEntry;

struct DefaultMacro {
	const char *schema;
	const char *name;
	const char *macro_definition; // e.g. "(param1, param2) AS expr" or "(x, sep := ',') AS expr"
};

class DefaultFunctionGenerator : public DefaultGenerator {
public:
	DefaultFunctionGenerator(Catalog &catalog, SchemaCatalogEntry &schema);

	SchemaCatalogEntry &schema;

	DUCKDB_API static unique_ptr<CreateMacroInfo> CreateInternalMacroInfo(const DefaultMacro &default_macro);
	//! Whether the built-in macro list defines a macro of this name in this schema
	static bool IsDefaultMacro(const Identifier &schema, const Identifier &name);

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
