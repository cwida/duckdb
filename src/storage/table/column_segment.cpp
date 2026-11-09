#include "duckdb/storage/table/column_segment.hpp"

#include "duckdb/common/types/vector.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/table_filter.hpp"
#include "duckdb/storage/data_pointer.hpp"
#include "duckdb/storage/table/append_state.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/planner/table_filter_state.hpp"

#include <cstring>

namespace duckdb {

//===--------------------------------------------------------------------===//
// Create
//===--------------------------------------------------------------------===//

unique_ptr<ColumnSegment> ColumnSegment::CreatePersistentSegment(DatabaseInstance &db, BlockManager &block_manager,
                                                                 DataPointer &data_pointer) {
	auto &block_id = data_pointer.block_pointer.block_id;
	auto &offset = data_pointer.block_pointer.offset;
	auto &count = data_pointer.tuple_count;
	auto &compression_type = data_pointer.compression_type;
	auto &statistics = data_pointer.statistics;
	auto &segment_state = data_pointer.segment_state;
	auto &byte_size = data_pointer.byte_size;

	auto &config = DBConfig::GetConfig(db);
	shared_ptr<BlockHandle> block;

	auto &type = statistics.GetType();
	auto function = config.GetCompressionFunction(compression_type, type.InternalType());
	if (block_id == INVALID_BLOCK && !function.get().supports_null_block) {
		throw DataCorruptionException("Persistent %s segment is missing its data block",
		                              CompressionTypeToString(compression_type));
	}
	if (block_id != INVALID_BLOCK) {
		block = block_manager.RegisterBlock(block_id);
	}

	auto segment_size = block_manager.GetBlockSize();
	return make_uniq<ColumnSegment>(db, std::move(block), ColumnSegmentType::PERSISTENT, count, function,
	                                std::move(statistics), block_id, offset, segment_size, std::move(segment_state),
	                                byte_size);
}

unique_ptr<ColumnSegment> ColumnSegment::CreateTransientSegment(DatabaseInstance &db,
                                                                const CompressionFunction &function,
                                                                const LogicalType &type, const idx_t segment_size,
                                                                BlockManager &block_manager) {
	// Allocate a buffer for the uncompressed segment.
	auto &buffer_manager = BufferManager::GetBufferManager(db);
	D_ASSERT(&buffer_manager == &block_manager.buffer_manager);
	auto block = buffer_manager.RegisterTransientMemory(segment_size, block_manager);

	return make_uniq<ColumnSegment>(db, std::move(block), ColumnSegmentType::TRANSIENT, 0U, function,
	                                BaseStatistics::CreateEmpty(type), INVALID_BLOCK, 0U, segment_size);
}

unique_ptr<ColumnSegment> SuballocationBlock::CreateTransientSegment(DatabaseInstance &db,
                                                                     const CompressionFunction &function,
                                                                     const LogicalType &type, const idx_t segment_size,
                                                                     BlockManager &block_manager) {
	auto &buffer_manager = BufferManager::GetBufferManager(db);
	D_ASSERT(&buffer_manager == &block_manager.buffer_manager);

	//	Do we have enough room in the block?
	if (!block || block->GetBlockSize() < allocated + segment_size) {
		auto &temp_block_manger = buffer_manager.GetTemporaryBlockManager();
		block = buffer_manager.RegisterTransientMemory(temp_block_manger.GetBlockSize(), temp_block_manger);
		allocated = 0;
		block_id = block->BlockId();
	}

	const auto offset = allocated;
	allocated += segment_size;
	return make_uniq<ColumnSegment>(db, block, ColumnSegmentType::TRANSIENT, 0U, function,
	                                BaseStatistics::CreateEmpty(type), INVALID_BLOCK, offset, segment_size);
}

//===--------------------------------------------------------------------===//
// Construct/Destruct
//===--------------------------------------------------------------------===//
ColumnSegment::ColumnSegment(DatabaseInstance &db, shared_ptr<BlockHandle> block_p,
                             const ColumnSegmentType segment_type, const idx_t count,
                             const CompressionFunction &function_p, BaseStatistics statistics,
                             const block_id_t block_id_p, const idx_t offset, const idx_t segment_size_p,
                             const unique_ptr<ColumnSegmentState> segment_state_p,
                             optional<uint32_t> data_size_in_bytes_p)

    : SegmentBase<ColumnSegment>(count), db(db), segment_type(segment_type), block(std::move(block_p)),
      function(function_p), block_id(block_id_p), offset(offset), segment_size(segment_size_p),
      data_size_in_bytes(data_size_in_bytes_p), stats(std::move(statistics)) {
	if (function.get().init_segment) {
		segment_state = function.get().init_segment(*this, block_id, segment_state_p.get());
	}

	// For constant segments (CompressionType::COMPRESSION_CONSTANT) the block is a nullptr.
	D_ASSERT(!block || segment_size <= GetBlockSize());
}

ColumnSegment::ColumnSegment(ColumnSegment &other)
    : SegmentBase<ColumnSegment>(other.count.load()), db(other.db), segment_type(other.segment_type),
      block(std::move(other.block)), function(other.function), block_id(other.block_id), offset(other.offset),
      segment_size(other.segment_size), data_size_in_bytes(other.data_size_in_bytes),
      segment_state(std::move(other.segment_state)), stats(std::move(other.stats)) {
	// For constant segments (CompressionType::COMPRESSION_CONSTANT) the block is a nullptr.
	D_ASSERT(!block || segment_size <= GetBlockSize());
}

ColumnSegment::~ColumnSegment() {
}

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//
void ColumnSegment::InitializePrefetch(PrefetchState &prefetch_state, ColumnScanState &) {
	if (!block || block->BlockId() >= MAXIMUM_BLOCK) {
		// not an on-disk block
		return;
	}
	if (function.get().init_prefetch) {
		function.get().init_prefetch(*this, prefetch_state);
	} else {
		prefetch_state.AddBlock(block);
	}
}

void ColumnSegment::InitializeScan(ColumnScanState &state) {
	state.scan_state = function.get().init_scan(state.context, *this);
}

void ColumnSegment::Scan(ColumnScanState &state, idx_t scan_count, Vector &result, idx_t result_offset,
                         ScanVectorType scan_type) {
	if (scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR) {
		D_ASSERT(result_offset == 0);
		Scan(state, scan_count, result);
	} else {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
		ScanPartial(state, scan_count, result, result_offset);
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	}
}

void ColumnSegment::Select(ColumnScanState &state, idx_t scan_count, Vector &result, const SelectionVector &sel,
                           idx_t sel_count, idx_t result_offset, ScanVectorType scan_type) {
	if (!function.get().select) {
		throw InternalException("ColumnSegment::Select not implemented for this compression method");
	}
	if (scan_type == ScanVectorType::SCAN_ENTIRE_VECTOR) {
		D_ASSERT(result_offset == 0);
	} else {
		D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	}
	function.get().select(*this, state, scan_count, result, sel, sel_count, result_offset, scan_type);
}

void ColumnSegment::Filter(ColumnScanState &state, idx_t scan_count, Vector &result, SelectionVector &sel,
                           idx_t &sel_count, const TableFilter &filter, TableFilterState &filter_state) {
	if (!function.get().filter) {
		throw InternalException("ColumnSegment::Filter not implemented for this compression method");
	}
	function.get().filter(*this, state, scan_count, result, sel, sel_count, filter, filter_state);
}

void ColumnSegment::Skip(ColumnScanState &state) {
	function.get().skip(*this, state, state.offset_in_column - state.internal_index);
	state.internal_index = state.offset_in_column;
}

void ColumnSegment::Scan(ColumnScanState &state, idx_t scan_count, Vector &result) {
	function.get().scan_vector(*this, state, scan_count, result);
}

void ColumnSegment::ScanPartial(ColumnScanState &state, idx_t scan_count, Vector &result, idx_t result_offset) {
	function.get().scan_partial(*this, state, scan_count, result, result_offset);
}

//===--------------------------------------------------------------------===//
// Append
//===--------------------------------------------------------------------===//
idx_t ColumnSegment::SegmentSize() const {
	return segment_size;
}

void ColumnSegment::Resize(idx_t new_size) {
	D_ASSERT(new_size > segment_size);
	D_ASSERT(offset == 0);
	D_ASSERT(block && new_size <= GetBlockSize());

	auto &buffer_manager = BufferManager::GetBufferManager(db);
	auto old_handle = buffer_manager.Pin(block);
	auto new_handle = buffer_manager.Allocate(MemoryTag::IN_MEMORY_TABLE, new_size);
	auto new_block = new_handle.GetBlockHandle();
	memcpy(new_handle.GetDataMutable(), old_handle.Ptr(), segment_size);

	this->block_id = new_block->BlockId();
	this->block = std::move(new_block);
	this->segment_size = new_size;
}

void ColumnSegment::InitializeAppend(ColumnAppendState &state) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	if (!function.get().init_append) {
		throw InternalException("Attempting to init append to a segment without init_append method");
	}
	state.append_state = function.get().init_append(*this);
}

idx_t ColumnSegment::Append(ColumnAppendState &state, UnifiedVectorFormat &append_data, idx_t offset, idx_t count) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	if (!function.get().append) {
		throw InternalException("Attempting to append to a segment without append method");
	}
	return function.get().append(*state.append_state, *this, *state.append_stats, append_data, offset, count);
}

idx_t ColumnSegment::FinalizeAppend(ColumnAppendState &state) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	if (!function.get().finalize_append) {
		throw InternalException("Attempting to call FinalizeAppend on a segment without a finalize_append method");
	}
	auto result_count = function.get().finalize_append(*this, *state.append_stats);
	state.append_state.reset();
	return result_count;
}

void ColumnSegment::RevertAppend(idx_t new_count) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	if (function.get().revert_append) {
		function.get().revert_append(*this, new_count);
	}
	this->count = new_count;
}

//===--------------------------------------------------------------------===//
// Convert To Persistent
//===--------------------------------------------------------------------===//
void ColumnSegment::ConvertToPersistentConstant() {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	D_ASSERT(stats.statistics.IsConstant());

	segment_type = ColumnSegmentType::PERSISTENT;
	block_id = INVALID_BLOCK;
	offset = 0;

	auto &config = DBConfig::GetConfig(db);
	function = config.GetCompressionFunction(CompressionType::COMPRESSION_CONSTANT, GetType().InternalType());
	block.reset();
}

void ColumnSegment::MarkAsPersistent(shared_ptr<BlockHandle> block_p, uint32_t offset_p) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	block_id = block_p->BlockId();
	SetBlock(std::move(block_p), offset_p);
}

void ColumnSegment::UsePackedCopy(const shared_ptr<BlockHandle> &block_p, uint32_t offset_p) {
	D_ASSERT(segment_type == ColumnSegmentType::TRANSIENT);
	offset = offset_p;
	block = block_p;
}

void ColumnSegment::SetBlock(shared_ptr<BlockHandle> block_p, uint32_t offset_p) {
	segment_type = ColumnSegmentType::PERSISTENT;
	offset = offset_p;
	block = std::move(block_p);
}

DataPointer ColumnSegment::GetDataPointer(idx_t row_start) {
	if (segment_type != ColumnSegmentType::PERSISTENT) {
		throw InternalException("Attempting to call ColumnSegment::GetDataPointer on a transient segment");
	}
	// set up the data pointer directly using the data from the persistent segment
	DataPointer pointer(stats.statistics.Copy());
	pointer.block_pointer.block_id = GetBlockId();
	pointer.block_pointer.offset = NumericCast<uint32_t>(GetBlockOffset());
	pointer.row_start = row_start;
	pointer.tuple_count = count;
	pointer.compression_type = function.get().type;
	pointer.byte_size = data_size_in_bytes;
	if (function.get().serialize_state) {
		pointer.segment_state = function.get().serialize_state(*this);
	}
	return pointer;
}

//===--------------------------------------------------------------------===//
// Drop Segment
//===--------------------------------------------------------------------===//
void ColumnSegment::VisitBlockIds(BlockIdVisitor &visitor) const {
	if (block_id != INVALID_BLOCK) {
		visitor.Visit(block_id);
	}
	if (function.get().visit_block_ids) {
		function.get().visit_block_ids(*this, visitor);
	}
}

//===--------------------------------------------------------------------===//
// Filter Selection
//===--------------------------------------------------------------------===//
template <class T, class OP, bool HAS_NULL>
static idx_t TemplatedFilterSelection(const UnifiedVectorFormat &vdata, T predicate, const SelectionVector &sel,
                                      const idx_t approved_tuple_count, SelectionVector &result_sel) {
	auto &mask = vdata.validity;
	const auto vec = UnifiedVectorFormat::GetData<const T>(vdata);
	idx_t result_count = 0;
	for (idx_t i = 0; i < approved_tuple_count; i++) {
		const auto idx = sel.get_index(i);
		auto vector_idx = vdata.sel->get_index(idx);
		bool comparison_result =
		    (!HAS_NULL || mask.RowIsValidUnsafe(vector_idx)) && OP::Operation(vec[vector_idx], predicate);
		result_sel.set_index(result_count, idx);
		result_count += comparison_result;
	}
	return result_count;
}

template <class T>
static void FilterSelectionSwitch(UnifiedVectorFormat &vdata, T predicate, SelectionVector &sel,
                                  idx_t &approved_tuple_count, ExpressionType comparison_type) {
	SelectionVector new_sel(approved_tuple_count);
	auto &mask = vdata.validity;
	// the inplace loops take the result as the last parameter
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count =
			    TemplatedFilterSelection<T, Equals, false>(vdata, predicate, sel, approved_tuple_count, new_sel);
		} else {
			approved_tuple_count =
			    TemplatedFilterSelection<T, Equals, true>(vdata, predicate, sel, approved_tuple_count, new_sel);
		}
		break;
	}
	case ExpressionType::COMPARE_NOTEQUAL: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count =
			    TemplatedFilterSelection<T, NotEquals, false>(vdata, predicate, sel, approved_tuple_count, new_sel);
		} else {
			approved_tuple_count =
			    TemplatedFilterSelection<T, NotEquals, true>(vdata, predicate, sel, approved_tuple_count, new_sel);
		}
		break;
	}
	case ExpressionType::COMPARE_LESSTHAN: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count =
			    TemplatedFilterSelection<T, LessThan, false>(vdata, predicate, sel, approved_tuple_count, new_sel);
		} else {
			approved_tuple_count =
			    TemplatedFilterSelection<T, LessThan, true>(vdata, predicate, sel, approved_tuple_count, new_sel);
		}
		break;
	}
	case ExpressionType::COMPARE_GREATERTHAN: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count =
			    TemplatedFilterSelection<T, GreaterThan, false>(vdata, predicate, sel, approved_tuple_count, new_sel);
		} else {
			approved_tuple_count =
			    TemplatedFilterSelection<T, GreaterThan, true>(vdata, predicate, sel, approved_tuple_count, new_sel);
		}
		break;
	}
	case ExpressionType::COMPARE_LESSTHANOREQUALTO: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count = TemplatedFilterSelection<T, LessThanEquals, false>(vdata, predicate, sel,
			                                                                          approved_tuple_count, new_sel);
		} else {
			approved_tuple_count =
			    TemplatedFilterSelection<T, LessThanEquals, true>(vdata, predicate, sel, approved_tuple_count, new_sel);
		}
		break;
	}
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO: {
		if (mask.CannotHaveNull()) {
			approved_tuple_count = TemplatedFilterSelection<T, GreaterThanEquals, false>(vdata, predicate, sel,
			                                                                             approved_tuple_count, new_sel);
		} else {
			approved_tuple_count = TemplatedFilterSelection<T, GreaterThanEquals, true>(vdata, predicate, sel,
			                                                                            approved_tuple_count, new_sel);
		}
		break;
	}
	default:
		throw NotImplementedException("Unknown comparison type for filter pushed down to table!");
	}
	sel.Initialize(new_sel);
}

template <bool IS_NULL>
static idx_t TemplatedNullSelection(UnifiedVectorFormat &vdata, SelectionVector &sel, idx_t &approved_tuple_count) {
	auto &mask = vdata.validity;
	if (mask.CannotHaveNull()) {
		// no NULL values
		if (IS_NULL) {
			approved_tuple_count = 0;
			return 0;
		} else {
			return approved_tuple_count;
		}
	} else {
		SelectionVector result_sel(approved_tuple_count);
		idx_t result_count = 0;
		for (idx_t i = 0; i < approved_tuple_count; i++) {
			auto idx = sel.get_index(i);
			auto vector_idx = vdata.sel->get_index(idx);
			if (mask.RowIsValid(vector_idx) != IS_NULL) {
				result_sel.set_index(result_count++, idx);
			}
		}
		sel.Initialize(result_sel);
		approved_tuple_count = result_count;
		return result_count;
	}
}

static idx_t ExecuteExpressionFilterSelection(SelectionVector &sel, Vector &vector, ExpressionFilterState &state,
                                              idx_t scan_count, idx_t &approved_tuple_count) {
	if (approved_tuple_count == 0) {
		return 0;
	}
	D_ASSERT(state.executor);
	SelectionVector result_sel(approved_tuple_count);
	if (scan_count > STANDARD_VECTOR_SIZE) {
		// scan count is > vector size - split up the vector into multiple chunks
		idx_t offset = 0;
		idx_t result_offset = 0;
		idx_t current_sel_offset = 0;
		SelectionVector current_sel(approved_tuple_count);
		while (offset < scan_count) {
			idx_t chunk_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, scan_count - offset);
			idx_t chunk_end = offset + chunk_count;
			DataChunk chunk;
			chunk.data.emplace_back(vector, offset, chunk_end);

			// construct the relevant selection vector for the current chunk (offset ... offset + chunk_count)
			idx_t current_count = 0;
			for (; current_sel_offset < approved_tuple_count; current_sel_offset++) {
				auto sel_index = sel.get_index(current_sel_offset);
				if (sel_index >= chunk_end) {
					// exhausted the chunk
					break;
				}
				if (sel_index < offset) {
					throw InternalException("sel_index < offset in expression filter");
				}
				current_sel.set_index(current_count++, sel_index - offset);
			}
			if (current_count == 0) {
				// no matching tuples in this chunk
				offset += chunk_count;
				continue;
			}
			auto current_result_data = result_sel.data() + result_offset;
			SelectionVector current_result_sel(current_result_data, result_sel.Capacity() - result_offset);
			idx_t new_matches = state.executor->SelectExpression(chunk, current_result_sel, current_sel, current_count);
			// increment all matches by the offset
			for (idx_t i = 0; i < new_matches; i++) {
				current_result_data[i] += offset;
			}
			result_offset += new_matches;
			offset += chunk_count;
		}
		approved_tuple_count = result_offset;
	} else {
		// standard case: we can handle everything at once - run the expression once
		DataChunk chunk;
		chunk.data.emplace_back(Vector::Ref(vector));
		chunk.SetChildCardinality(scan_count);
		SelectionVector identity_sel;
		optional_ptr<SelectionVector> current_sel = sel.IsSet() ? &sel : nullptr;
		if (!sel.IsSet() && (vector.GetType().IsNested() || approved_tuple_count != scan_count)) {
			identity_sel = SelectionVector::Incremental(approved_tuple_count);
			current_sel = &identity_sel;
		}
		approved_tuple_count = state.executor->SelectExpression(chunk, result_sel, current_sel, approved_tuple_count);
	}
	sel.Initialize(result_sel);
	return approved_tuple_count;
}

idx_t ColumnSegment::FilterSelection(SelectionVector &sel, Vector &vector, UnifiedVectorFormat &vdata,
                                     const TableFilter &filter, TableFilterState &filter_state, idx_t scan_count,
                                     idx_t &approved_tuple_count) {
	(void)vdata;
	return FilterSelection(sel, vector, filter_state, scan_count, approved_tuple_count);
}

idx_t ColumnSegment::FilterSelection(SelectionVector &sel, Vector &vector, TableFilterState &filter_state,
                                     idx_t scan_count, idx_t &approved_tuple_count) {
	auto &state = filter_state.Cast<ExpressionFilterState>();
	if (state.fast_executor && scan_count <= STANDARD_VECTOR_SIZE) {
		return state.fast_executor->FilterSelection(sel, vector, scan_count, approved_tuple_count);
	}
	return ExecuteExpressionFilterSelection(sel, vector, state, scan_count, approved_tuple_count);
}

const CompressionFunction &ColumnSegment::GetCompressionFunction() {
	return function.get();
}

} // namespace duckdb
