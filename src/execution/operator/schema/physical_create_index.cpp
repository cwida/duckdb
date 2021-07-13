#include "duckdb/execution/operator/schema/physical_create_index.hpp"

#include "duckdb/catalog/catalog_entry/index_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

void PhysicalCreateIndex::GetChunkInternal(ExecutionContext &context, DataChunk &chunk,
                                           PhysicalOperatorState *state) const {
	if (column_ids.empty()) {
		throw BinderException("CREATE INDEX does not refer to any columns in the base table!");
	}

	auto &schema = *table.schema;
	auto index_entry = (IndexCatalogEntry *)schema.CreateIndex(context.client, info.get(), &table);
	if (!index_entry) {
		// index already exists, but error ignored because of IF NOT EXISTS
		return;
	}

	unique_ptr<Index> index;
	switch (info->index_type) {
	case IndexType::ART: {
		index = make_unique<ART>(column_ids, unbound_expressions, info->unique);
		break;
	}
	default:
		throw InternalException("Unimplemented index type");
	}
	index_entry->index = index.get();
	index_entry->info = table.storage->info;
	table.storage->AddIndex(move(index), expressions);

	chunk.SetCardinality(0);
	state->finished = true;
}

} // namespace duckdb
