#include "duckdb/storage/checkpoint/table_index_writer.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/checkpoint_manager.hpp"
#include "duckdb/common/thread.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

TableIndexWriter::TableIndexWriter(PartialBlockManager &partial_block_manager, StorageVersion version)
    : partial_block_manager(partial_block_manager), storage_version(version) {
}

TableIndexWriter::~TableIndexWriter() {
}

StorageVersion TableIndexWriter::GetStorageVersion() const {
	return storage_version;
}

SingleFileTableIndexWriter::SingleFileTableIndexWriter(SingleFileCheckpointWriter &checkpoint_manager,
                                                       const StorageVersion version)
    : TableIndexWriter(checkpoint_manager.index_partial_block_manager, version),
      checkpoint_manager(checkpoint_manager) {
}

PartialBlockManager SingleFileTableIndexWriter::CreatePerIndexPartialBlockManager() {
	return checkpoint_manager.CreatePerIndexPartialBlockManager();
}

const unordered_set<idx_t> &SingleFileTableIndexWriter::GetIndexOids() const {
	return checkpoint_manager.index_oids;
}

void SingleFileTableIndexWriter::Flush() {
	auto sleep_ms = Settings::Get<DebugCheckpointIndexSleepMsSetting>(checkpoint_manager.db.GetDatabase());
	if (sleep_ms > 0) {
		ThreadUtil::SleepMs(sleep_ms);
	}
	partial_block_manager.FlushPartialBlocks();
}

} // namespace duckdb
