#include "duckdb/storage/index.hpp"
#include "duckdb/storage/checkpoint/table_index_writer.hpp"

namespace duckdb {

Index::Index(const vector<column_t> &column_ids, TableIOManager &table_io_manager, AttachedDatabase &db)

    : column_ids(column_ids), table_io_manager(table_io_manager), db(db) {
	// create the column id set
	column_id_set.insert(column_ids.begin(), column_ids.end());
}

IndexCheckpointResult Index::Checkpoint(PartialBlockManager &partial_block_manager, const StorageVersion version) {
	throw NotImplementedException("Checkpoint not implemented for index type \"%s\"", GetIndexType());
}

} // namespace duckdb
