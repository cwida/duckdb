#include "duckdb/storage/checkpoint/table_index_writer.hpp"
#include "duckdb/storage/checkpoint/pending_publication.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/checkpoint_manager.hpp"

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
    : TableIndexWriter(checkpoint_manager.partial_block_manager, version), checkpoint_manager(checkpoint_manager) {
}

PartialBlockManager SingleFileTableIndexWriter::CreatePerIndexPartialBlockManager() {
	return checkpoint_manager.CreatePerIndexPartialBlockManager();
}

void SingleFileTableIndexWriter::AddPendingPublication(unique_ptr<PendingPublication> publication) {
	checkpoint_manager.AddPendingPublication(std::move(publication));
}

} // namespace duckdb
