//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/window/window_collection.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/once_flag.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

#include <exception>

namespace duckdb {

// A wrapper for building ColumnDataCollections in parallel
class WindowCollection {
public:
	using ColumnDataCollectionPtr = unique_ptr<ColumnDataCollection>;
	using ColumnDataCollectionSpec = pair<idx_t, optional_ptr<ColumnDataCollection>>;
	using ColumnSet = unordered_set<column_t>;

	WindowCollection(BufferManager &buffer_manager, idx_t count, const vector<LogicalType> &types);

	idx_t ColumnCount() const {
		return types.size();
	}

	idx_t size() const { // NOLINT
		return count;
	}

	const vector<LogicalType> &GetTypes() const {
		return types;
	}

	//! Update a thread-local collection for appending data to a given row
	void GetCollection(idx_t row_idx, ColumnDataCollectionSpec &spec);
	//! Combine once in order after all appends have finished.
	void Combine(const ColumnSet &build_validity);

	//! The collection data. May be null if the column count is 0.
	ColumnDataCollectionPtr inputs;
	//! Global validity mask
	vector<atomic<bool>> all_valids;
	//! Optional validity mask for the entire collection
	vector<ValidityMask> validities;

	//! The collection columns
	const vector<LogicalType> types;
	//! The collection rows
	const idx_t count;

	//! Guard for range updates
	mutex lock;
	//! The paging buffer manager to use
	BufferManager &buffer_manager;
	//! The component column data collections
	vector<ColumnDataCollectionPtr> collections;
	//! The (sorted) collection ranges
	using Range = pair<idx_t, idx_t>;
	vector<Range> ranges;

private:
	void CombineInternal(const ColumnSet &validity_cols);

private:
	OnceFlag combine_once;
	//! Combining consumes its inputs, so failures must be shared rather than retried.
	std::exception_ptr combine_error;
};

class WindowBuilder {
public:
	explicit WindowBuilder(WindowCollection &collection);

	//! Add a new chunk at the given index
	void Sink(DataChunk &chunk, idx_t input_idx);

	//! The collection we are helping to build
	WindowCollection &collection;
	//! The thread's current input collection
	using ColumnDataCollectionSpec = WindowCollection::ColumnDataCollectionSpec;
	ColumnDataCollectionSpec sink;
	//! The state used for appending to the collection
	ColumnDataAppendState appender;
	//! Are all the sunk rows valid?
	bool all_valid = true;
};

class WindowCursor {
public:
	WindowCursor(const WindowCollection &paged, column_t col_idx);
	WindowCursor(const WindowCollection &paged, vector<column_t> column_ids);

	//! The row count of the paged collection
	idx_t Count() const {
		return paged.size();
	}
	//! Is the scan in range?
	inline bool RowIsVisible(idx_t row_idx) const {
		return (row_idx < state.next_row_index && state.current_row_index <= row_idx);
	}
	//! The offset of the row in the given state
	inline sel_t RowOffset(idx_t row_idx) const {
		D_ASSERT(RowIsVisible(row_idx));
		return UnsafeNumericCast<sel_t>(row_idx - state.current_row_index);
	}
	//! Scan the next chunk
	inline bool Scan() {
		if (!paged.inputs->Scan(state, chunk)) {
			return false;
		}
		// compact pages are flattened once per load: the cell accessors are too hot for a representation branch
		chunk.Flatten();
		return true;
	}
	//! Seek to the given row
	inline idx_t Seek(idx_t row_idx) {
		if (!RowIsVisible(row_idx)) {
			D_ASSERT(paged.inputs.get());
			paged.inputs->Seek(row_idx, state, chunk);
			chunk.Flatten();
		}
		return RowOffset(row_idx);
	}
	//! Check a collection cell for nullity
	bool CellIsNull(idx_t col_idx, idx_t row_idx) {
		D_ASSERT(chunk.ColumnCount() > col_idx);
		auto index = Seek(row_idx);
		auto &source = chunk.data[col_idx];
		return FlatVector::IsNull(source, index);
	}
	//! Read a typed cell
	template <typename T>
	T GetCell(idx_t col_idx, idx_t row_idx) {
		D_ASSERT(chunk.ColumnCount() > col_idx);
		auto index = Seek(row_idx);
		auto &source = chunk.data[col_idx];
		const auto data = FlatVector::GetData<T>(source);
		return data[index];
	}
	//! Copy a single value
	void CopyCell(idx_t col_idx, idx_t row_idx, Vector &target, idx_t target_offset) {
		D_ASSERT(chunk.ColumnCount() > col_idx);
		auto index = Seek(row_idx);
		auto &source = chunk.data[col_idx];
		VectorOperations::Copy(source, target, index + 1, index, target_offset);
	}

	unique_ptr<WindowCursor> Copy() const {
		return make_uniq<WindowCursor>(paged, state.column_ids);
	}

	//! The pageable data
	const WindowCollection &paged;
	//! The state used for reading the collection
	ColumnDataScanState state;
	//! The data chunk read into
	DataChunk chunk;
};

class WindowCollectionChunkScanner {
public:
	WindowCollectionChunkScanner(ColumnDataCollection &collection, const vector<column_t> &scan_ids,
	                             const idx_t begin_idx)
	    : collection(collection), curr_idx(0) {
		collection.InitializeScan(state, scan_ids);
		collection.InitializeScanChunk(state, chunk);

		Seek(begin_idx);
	}

	void Seek(idx_t begin_idx) {
		idx_t chunk_idx;
		idx_t seg_idx;
		idx_t row_idx;
		for (; curr_idx > begin_idx; --curr_idx) {
			collection.PrevScanIndex(state, chunk_idx, seg_idx, row_idx);
		}
		for (; curr_idx < begin_idx; ++curr_idx) {
			collection.NextScanIndex(state, chunk_idx, seg_idx, row_idx);
		}
	}

	bool Scan() {
		const auto result = collection.Scan(state, chunk);
		++curr_idx;
		return result;
	}

	idx_t Scanned() const {
		return state.next_row_index;
	}

	//! Return a struct type for comparing keys
	LogicalType PrefixStructType(column_t end, column_t begin = 0);
	//! Reference the chunk into a struct vector matching the keys
	static void ReferenceStructColumns(DataChunk &chunk, Vector &vec, column_t end, column_t begin = 0);

	ColumnDataCollection &collection;
	ColumnDataScanState state;
	DataChunk chunk;
	idx_t curr_idx;
};

template <typename OP>
static void WindowDeltaScanner(ColumnDataCollection &collection, idx_t block_begin, idx_t block_end,
                               const vector<column_t> &scan_cols, const idx_t key_count, OP operation) {
	//	Stop if there is no work to do
	if (!collection.Count()) {
		return;
	}

	//	Start back one to get the overlap
	idx_t block_curr = block_begin ? block_begin - 1 : 0;

	//	Scan the sort columns
	WindowCollectionChunkScanner scanner(collection, scan_cols, block_curr);
	auto &scanned = scanner.chunk;

	//	Shifted buffer for the next values
	DataChunk next;
	collection.InitializeScanChunk(scanner.state, next);

	//	Delay buffer for the previous row
	DataChunk delayed;
	collection.InitializeScanChunk(scanner.state, delayed);

	//	Only compare the key arguments.
	const auto key_type = scanner.PrefixStructType(key_count);
	Vector compare_curr(key_type);
	Vector compare_prev(key_type);

	bool boundary_compare = (block_begin > 0);
	idx_t row_idx = 1;
	if (!scanner.Scan()) {
		return;
	}

	//	Process chunks offset by 1
	SelectionVector next_sel(1, STANDARD_VECTOR_SIZE);
	SelectionVector distinct(STANDARD_VECTOR_SIZE);
	SelectionVector matching(STANDARD_VECTOR_SIZE);

	// In order to reuse the verbose `distinct from` logic for both the main vector comparisons
	// and single element boundary comparisons, we alternate between single element compares
	// and count-1 compares.
	while (block_curr < block_end) {
		//	Compare the current to the previous;
		DataChunk *curr = nullptr;
		DataChunk *prev = nullptr;

		idx_t count = 0;
		if (boundary_compare) {
			//	Save the last row of the scanned chunk
			count = 1;
			sel_t last = UnsafeNumericCast<sel_t>(scanned.size() - 1);
			SelectionVector sel(&last, 1ULL);
			delayed.Reset();
			scanned.Copy(delayed, sel, count);
			prev = &delayed;

			// Try to read the next chunk
			++block_curr;
			row_idx = scanner.Scanned();
			if (block_curr >= block_end || !scanner.Scan()) {
				break;
			}
			curr = &scanned;
		} else {
			//	Compare the [1..size) values with the [0..size-1) values
			count = scanned.size() - 1;
			if (!count) {
				//	1 row scanned, so just skip the rest of the loop.
				boundary_compare = true;
				continue;
			}
			prev = &scanned;

			// Slice the current back one into the previous
			next.Slice(scanned, next_sel, count);
			curr = &next;
		}

		//	Reference the comparison prefix as a struct to simplify the compares.
		scanner.ReferenceStructColumns(*prev, compare_prev, key_count);
		scanner.ReferenceStructColumns(*curr, compare_curr, key_count);

		const auto ndistinct =
		    VectorOperations::DistinctFrom(compare_curr, compare_prev, nullptr, count, &distinct, &matching);

		//	If n is 0, neither SV has been filled in?
		auto match_sel = ndistinct ? &matching : FlatVector::IncrementalSelectionVector();

		operation(row_idx, *prev, *curr, ndistinct, distinct, *match_sel);

		//	Transition between comparison ranges.
		boundary_compare = !boundary_compare;
		row_idx += count;
	}
}

} // namespace duckdb
