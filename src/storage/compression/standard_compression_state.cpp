#include "duckdb/storage/compression/standard_compression_state.hpp"
#include "duckdb/storage/storage_manager.hpp"
#include "duckdb/storage/table/column_data_checkpointer.hpp"

namespace duckdb {

CompressionState::CompressionState(ColumnDataCheckpointData &checkpoint_data_p, CompressionType compression_type)
    : checkpoint_data(checkpoint_data_p), function(checkpoint_data.GetCompressionFunction(compression_type)),
      block_manager(checkpoint_data.GetStorageManager().GetBlockManager()), info(block_manager) {
}

unique_ptr<ColumnSegment> CompressionState::CreateNewSegment() {
	return ColumnSegment::CreateTransientSegment(checkpoint_data.GetDatabase(), function, checkpoint_data.GetType(),
	                                             info.GetBlockSize(), info.GetBlockManager());
}

const LogicalType &CompressionState::GetType() {
	return checkpoint_data.GetType();
}

StandardCompressionState::~StandardCompressionState() {
}

void StandardCompressionState::CreateAndPinNewSegment() {
	auto compressed_segment = CreateNewSegment();
	current_segment = std::move(compressed_segment);

	auto &buffer_manager = BufferManager::GetBufferManager(current_segment->GetDatabase());
	handle = buffer_manager.Pin(current_segment->GetBlockHandle());
}

void StandardCompressionState::FlushCurrentSegment(idx_t segment_size) {
	auto &state = checkpoint_data.GetCheckpointState();
	state.FlushSegment(std::move(current_segment), std::move(handle), segment_size);
}

} // namespace duckdb
