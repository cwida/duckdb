//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/default/default_coordinate_systems.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/common/array_ptr.hpp"
#include "duckdb/catalog/default/default_table_functions.hpp"

namespace duckdb {
class SchemaCatalogEntry;

class DefaultCoordinateSystemGenerator : public DefaultGenerator {
public:
	DefaultCoordinateSystemGenerator(Catalog &catalog, SchemaCatalogEntry &schema);

	SchemaCatalogEntry &schema;

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
};

} // namespace duckdb
