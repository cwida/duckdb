#include "duckdb/storage/block.hpp"
#include "duckdb/storage/table/column_data.hpp"
#include "duckdb/storage/table/column_checkpoint_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/partial_block_manager.hpp"
#include "duckdb/storage/table/row_group.hpp"

namespace duckdb {

ColumnCheckpointState::ColumnCheckpointState(const RowGroup &row_group, ColumnData &original_column,
                                             PartialBlockManager &partial_block_manager)
    : row_group(row_group), original_column(original_column), partial_block_manager(partial_block_manager),
      original_column_mutable(original_column) {
}

ColumnCheckpointState::~ColumnCheckpointState() {
}

unique_ptr<BaseStatistics> ColumnCheckpointState::GetStatistics() {
	D_ASSERT(global_stats);
	return std::move(global_stats);
}

shared_ptr<ColumnData> ColumnCheckpointState::CreateEmptyColumnData() {
	throw InternalException("CreateEmptyColumnData not implemented for this column checkpoint state");
}

ColumnData &ColumnCheckpointState::GetResultColumn() {
	if (!result_column) {
		result_column = CreateEmptyColumnData();
	}
	return *result_column;
}

shared_ptr<ColumnData> ColumnCheckpointState::GetFinalResult() {
	if (!result_column) {
		// no result column instantiated - that means we haven't changed anything and can directly return the
		// original column
		return original_column_mutable.shared_from_this();
	}
	result_column->SetCount(original_column.count.load());
	return result_column;
}

void ColumnCheckpointState::FlushSegment(unique_ptr<ColumnSegment> segment, BufferHandle handle, idx_t segment_size) {
	handle.Destroy();
	FlushSegmentInternal(std::move(segment), segment_size);
}

void ColumnCheckpointState::FlushSegmentInternal(unique_ptr<ColumnSegment> segment, idx_t segment_size) {
	auto block_size = partial_block_manager.GetBlockManager().GetBlockSize();
	if (segment_size > block_size) {
		throw InternalException("segment size exceeds block size in ColumnCheckpointState::FlushSegmentInternal");
	}

	auto tuple_count = segment->count.load();
	if (tuple_count == 0) { // LCOV_EXCL_START
		return;
	} // LCOV_EXCL_STOP
	auto byte_size = NumericCast<uint32_t>(segment_size);
	segment->SetByteSize(byte_size);

	// Merge the segment statistics into the global statistics.
	global_stats->Merge(segment->GetStats());

	BlockPointer block_ptr;

	if (segment->GetStats().IsConstant()) {
		// Constant block.
		segment->ConvertToPersistentConstant();
	} else if (segment_size != 0) {
		// Non-constant block with data that has to go to disk.
		auto &db = original_column.GetDatabase();
		auto &buffer_manager = BufferManager::GetBufferManager(db);
		auto source = buffer_manager.Pin(segment->GetBlockHandle());

		if (auto new_block_ptr = partial_block_manager.Write(*segment, std::move(source), byte_size)) {
			block_ptr = *new_block_ptr;
		};
	} else {
		// Empty segment, which does not have to go to disk.
		// We still need to change its type to persistent, because we need to write its metadata.
		segment->SetSegmentType(ColumnSegmentType::PERSISTENT);
		segment->GetBlockHandle().reset();
	}

	// construct the data pointer
	DataPointer data_pointer(segment->GetStats().Copy());
	data_pointer.block_pointer = block_ptr;
	data_pointer.row_start = 0;
	if (!data_pointers.empty()) {
		auto &last_pointer = data_pointers.back();
		data_pointer.row_start = last_pointer.row_start + last_pointer.tuple_count;
	}
	data_pointer.tuple_count = tuple_count;
	auto &compression_function = segment->GetCompressionFunction();
	data_pointer.compression_type = compression_function.type;
	data_pointer.byte_size = byte_size;
	if (compression_function.serialize_state) {
		data_pointer.segment_state = compression_function.serialize_state(*segment);
	}

	// append the segment to the new segment tree
	GetResultColumn().GetSegmentTree().AppendSegment(std::move(segment));
	data_pointers.push_back(std::move(data_pointer));
}

PersistentColumnData ColumnCheckpointState::ToPersistentData() {
	auto &type = result_column ? result_column->type : original_column.type;
	PersistentColumnData data(type);
	data.pointers = std::move(data_pointers);
	return data;
}

} // namespace duckdb
