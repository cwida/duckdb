//===--------------------------------------------------------------------===//
// row_gather.cpp
// Description: This file contains the implementation of the gather operators
//===--------------------------------------------------------------------===//

#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/constant_operators.hpp"
#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/types/row_data_collection.hpp"
#include "duckdb/common/types/row_layout.hpp"

namespace duckdb {

using ValidityBytes = RowLayout::ValidityBytes;

template <class T>
static void TemplatedGatherLoop(Vector &rows, const SelectionVector &row_sel, Vector &col,
                                const SelectionVector &col_sel, idx_t count, idx_t col_offset, idx_t col_no) {
	// Precompute mask indexes
	idx_t entry_idx;
	idx_t idx_in_entry;
	ValidityBytes::GetEntryIndex(col_no, entry_idx, idx_in_entry);

	auto ptrs = FlatVector::GetData<data_ptr_t>(rows);
	auto data = FlatVector::GetData<T>(col);
	auto &col_mask = FlatVector::Validity(col);

	for (idx_t i = 0; i < count; i++) {
		auto row_idx = row_sel.get_index(i);
		auto row = ptrs[row_idx];
		auto col_idx = col_sel.get_index(i);
		data[col_idx] = Load<T>(row + col_offset);
		ValidityBytes row_mask(row);
		if (!row_mask.RowIsValid(row_mask.GetValidityEntry(entry_idx), idx_in_entry)) {
			col_mask.SetInvalid(col_idx);
		}
	}
}

static void GatherNestedVector(Vector &rows, const SelectionVector &row_sel, Vector &col,
                               const SelectionVector &col_sel, idx_t count, idx_t col_offset, idx_t col_no) {
	auto ptrs = FlatVector::GetData<data_ptr_t>(rows);

	// Build the gather locations
	data_ptr_t data_locations[STANDARD_VECTOR_SIZE];
	data_ptr_t mask_locations[STANDARD_VECTOR_SIZE];
	for (idx_t i = 0; i < count; i++) {
		auto row_idx = row_sel.get_index(i);
		mask_locations[i] = ptrs[row_idx];
		data_locations[i] = Load<data_ptr_t>(ptrs[row_idx] + col_offset);
	}

	// Deserialise into the selected locations
	RowDataCollection::DeserializeIntoVector(col, count, col_sel, col_no, data_locations, mask_locations);
}

void RowOperations::Gather(Vector &rows, const SelectionVector &row_sel, Vector &col, const SelectionVector &col_sel,
                           const idx_t count, const idx_t col_offset, const idx_t col_no) {
	D_ASSERT(rows.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(rows.GetType().id() == LogicalTypeId::POINTER); // "Cannot gather from non-pointer type!"

	col.SetVectorType(VectorType::FLAT_VECTOR);
	switch (col.GetType().InternalType()) {
	case PhysicalType::UINT8:
		TemplatedGatherLoop<uint8_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::UINT16:
		TemplatedGatherLoop<uint16_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::UINT32:
		TemplatedGatherLoop<uint32_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::UINT64:
		TemplatedGatherLoop<uint64_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedGatherLoop<int8_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::INT16:
		TemplatedGatherLoop<int16_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::INT32:
		TemplatedGatherLoop<int32_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::INT64:
		TemplatedGatherLoop<int64_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::INT128:
		TemplatedGatherLoop<hugeint_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::FLOAT:
		TemplatedGatherLoop<float>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::DOUBLE:
		TemplatedGatherLoop<double>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::POINTER:
		TemplatedGatherLoop<uintptr_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::INTERVAL:
		TemplatedGatherLoop<interval_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::HASH:
		TemplatedGatherLoop<hash_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::VARCHAR:
		TemplatedGatherLoop<string_t>(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	case PhysicalType::LIST:
	case PhysicalType::MAP:
	case PhysicalType::STRUCT:
		GatherNestedVector(rows, row_sel, col, col_sel, count, col_offset, col_no);
		break;
	default:
		throw InternalException("Unimplemented type for RowOperations::Gather");
	}
}

} // namespace duckdb
