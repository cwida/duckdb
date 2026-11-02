#include "duckdb/execution/operator/persistent/copy_batch_slicer.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/row/tuple_data_collection.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"

namespace duckdb {

static constexpr idx_t BATCH_SIZE_BYTES_SLACK_DIVISOR = 8;

//! Column collections store fixed-width struct children without tuple headers.
static idx_t FixedRowBytes(const LogicalType &type) {
	if (type.InternalType() != PhysicalType::STRUCT) {
		return GetTypeIdSize(type.InternalType());
	}
	idx_t result = 0;
	for (const auto &child_type : StructType::GetChildTypes(type)) {
		result += FixedRowBytes(child_type.second);
	}
	return result;
}

CopyBatchSlicer::CopyBatchSlicer(const vector<LogicalType> &types, const optional_idx &batch_size_bytes_p)
    : batch_size_bytes(batch_size_bytes_p) {
	if (!batch_size_bytes.IsValid()) {
		return;
	}
	layout.Initialize(types, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	for (const auto &type : types) {
		fixed_row_bytes += FixedRowBytes(type);
	}
	partial.InitializeEmpty(types);
	row_bytes_state = make_uniq<TupleDataChunkState>();
	TupleDataCollection::InitializeChunkState(*row_bytes_state, types, layout.GetVariableColumns());
}

CopyBatchSlicer::~CopyBatchSlicer() {
}

const Vector &CopyBatchSlicer::ComputeRowSizes(DataChunk &chunk) {
	D_ASSERT(row_bytes_state);
	auto &row_sizes = row_bytes_state->heap_sizes;
	if (layout.AllConstant()) {
		auto writer = FlatVector::Writer<idx_t>(row_sizes, chunk.size());
		for (idx_t i = 0; i < chunk.size(); i++) {
			writer.WriteValue(fixed_row_bytes);
		}
	} else {
		TupleDataCollection::ToUnifiedFormat(*row_bytes_state, chunk);
		TupleDataCollection::ComputeHeapSizes(*row_bytes_state, chunk, *FlatVector::IncrementalSelectionVector(),
		                                      chunk.size());
		auto heap_sizes = row_sizes.Values<idx_t>();
		auto writer = FlatVector::Writer<idx_t>(row_sizes, chunk.size());
		for (auto entry : heap_sizes) {
			writer.WriteValue(fixed_row_bytes + entry.GetValue());
		}
	}
	return row_sizes;
}

idx_t CopyBatchSlicer::RowsThatFit(idx_t count, idx_t offset, idx_t budget) const {
	const auto remaining = count - offset;
	if (layout.AllConstant()) {
		if (fixed_row_bytes == 0) {
			return remaining;
		}
		const auto fits = budget / fixed_row_bytes + (budget % fixed_row_bytes != 0);
		return MaxValue<idx_t>(MinValue(remaining, fits), 1);
	}
	auto row_sizes = row_bytes_state->heap_sizes.Values<idx_t>();
	idx_t total = 0;
	for (idx_t i = 0; i < remaining; i++) {
		total += row_sizes[offset + i].GetValue();
		if (total >= budget) {
			return i + 1;
		}
	}
	return remaining;
}

DataChunk &CopyBatchSlicer::Slice(DataChunk &chunk, idx_t &offset, const ColumnDataCollection &batch) {
	D_ASSERT(offset < chunk.size());
	if (!batch_size_bytes.IsValid()) {
		offset = chunk.size();
		return chunk;
	}
	if (!layout.AllConstant() && offset == 0) {
		ComputeRowSizes(chunk);
	}
	// InitializeAppend allocates a chunk, so an empty batch reports its capacity
	const auto current_batch_bytes = batch.Count() == 0 ? 0 : batch.SizeInBytes();
	const auto limit = batch_size_bytes.GetIndex();
	// Avoid slicing chunks that only marginally cross the byte limit.
	const auto slack_limit = limit + limit / BATCH_SIZE_BYTES_SLACK_DIVISOR;
	auto append_count =
	    RowsThatFit(chunk.size(), offset, slack_limit > current_batch_bytes ? slack_limit - current_batch_bytes : 0);
	if (append_count < chunk.size() - offset) {
		append_count = RowsThatFit(chunk.size(), offset, limit > current_batch_bytes ? limit - current_batch_bytes : 0);
	}
	if (offset == 0 && append_count == chunk.size()) {
		offset = chunk.size();
		return chunk;
	}
	partial.Slice(chunk, offset, offset + append_count);
	offset += append_count;
	return partial;
}

} // namespace duckdb
