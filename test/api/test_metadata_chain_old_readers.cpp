#include "catch.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/storage/block_manager.hpp"
#include "duckdb/storage/database_size.hpp"
#include "duckdb/storage/metadata/metadata_manager.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/table/row_group_collection.hpp"
#include "test_helpers.hpp"

#include <set>

using namespace duckdb;

namespace {

struct MetadataBlock {
	block_id_t block_id;
	idx_t index;

	bool operator<(const MetadataBlock &other) const {
		return block_id < other.block_id || (block_id == other.block_id && index < other.index);
	}
};

//! The blocks of the table data that readers before v1.4.2 enter and whose next pointer leads to a block that is
//! not in use. Those readers resolve the next pointer of a block as soon as they enter it, so such a pointer makes
//! them fail to open the file.
idx_t CountNextPointersToFreeBlocks(Connection &con, idx_t table_count) {
	con.BeginTransaction();
	auto &catalog = Catalog::GetCatalog(*con.context, "");
	auto &metadata_manager = StorageManager::Get(catalog).GetBlockManager().GetMetadataManager();
	std::set<MetadataBlock> in_use;
	for (auto &info : metadata_manager.GetMetadataInfo()) {
		std::set<idx_t> free_blocks(info.free_list.begin(), info.free_list.end());
		for (idx_t i = 0; i < info.total_blocks; i++) {
			if (free_blocks.find(i) == free_blocks.end()) {
				in_use.insert(MetadataBlock {info.block_id, i});
			}
		}
	}
	idx_t result = 0;
	for (idx_t t = 0; t < table_count; t++) {
		auto &table = Catalog::GetEntry<TableCatalogEntry>(
		    *con.context, QualifiedName(INVALID_CATALOG, DEFAULT_SCHEMA, Identifier("candles_" + std::to_string(t))));
		auto &collection = *table.GetStorage().GetRowGroupCollection();
		// loading every row group adds the rest of the table metadata to the pointers
		REQUIRE(collection.GetRowGroupCount() > 0);
		auto &pointers = collection.GetMetadataPointers();
		REQUIRE(!pointers.empty());
		for (auto &pointer : pointers) {
			auto next_block = metadata_manager.GetNextBlock(pointer);
			if (!next_block.IsValid()) {
				continue;
			}
			if (in_use.find(MetadataBlock {next_block.GetBlockId(), next_block.GetBlockIndex()}) == in_use.end()) {
				result++;
			}
		}
	}
	con.Commit();
	return result;
}

} // namespace

TEST_CASE("Metadata of a file for older versions has no next pointers into free blocks", "[storage]") {
	// every storage format below v1.5.0
	auto storage_version = GENERATE(as<string> {}, "v1.0.0", "v1.2.0", "v1.3.0", "v1.4.0");
	INFO("storage version " << storage_version);
	auto path = TestCreatePath("metadata_chain_old_readers.db");
	DeleteDatabase(path);
	auto config = GetTestConfig();
	config->SetOptionByName("storage_compatibility_version", Value(storage_version));
	// only the explicit checkpoints below write the file, automatic ones would rewrite the tables this relies on
	config->options.checkpoint_wal_size = idx_t(1) << 40;
	config->options.checkpoint_on_shutdown = false;
	const idx_t table_count = 9;
	// the database that creates the file drops the first tables, the database that opens it again drops the others
	const idx_t dropped_before_reopen = 4;
	{
		DuckDB db(path, config.get());
		Connection con(db);
		REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE wide AS PIVOT (SELECT i, j, i * j AS v FROM range(1000) t(i), "
		                          "range(100) s(j)) ON j USING first(v) ORDER BY i"));
		REQUIRE_NO_FAIL(con.Query("CREATE TEMP TABLE narrow AS SELECT i, i * 2 AS b, i * 3 AS c, i * 4 AS d, i * 5 AS "
		                          "e FROM range(1000) t(i)"));
		for (idx_t i = 0; i < table_count; i++) {
			auto suffix = std::to_string(i);
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE candles_" + suffix + " AS FROM narrow"));
			REQUIRE_NO_FAIL(con.Query("CREATE TABLE other_" + suffix + " AS FROM wide"));
			REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
		}
		for (idx_t i = 0; i < dropped_before_reopen; i++) {
			REQUIRE_NO_FAIL(con.Query("DROP TABLE other_" + std::to_string(i)));
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	{
		DuckDB db(path, config.get());
		Connection con(db);
		for (idx_t i = dropped_before_reopen; i < table_count; i++) {
			REQUIRE_NO_FAIL(con.Query("DROP TABLE other_" + std::to_string(i)));
		}
		REQUIRE_NO_FAIL(con.Query("CHECKPOINT"));
	}
	DuckDB db(path, config.get());
	Connection con(db);
	auto result = con.Query("SELECT count(*) FROM duckdb_tables()");
	REQUIRE(CHECK_COLUMN(result, 0, {Value::UBIGINT(table_count)}));
	REQUIRE(CountNextPointersToFreeBlocks(con, table_count) == 0);
	DeleteDatabase(path);
}
