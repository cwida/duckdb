//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/ie_join_union.hpp
//
//===----------------------------------------------------------------------===//
#pragma once
#include "duckdb/execution/operator/join/physical_range_join.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

template <typename T, typename VECTOR_TYPE = T>
class IEJoinCursor {
public:
	explicit IEJoinCursor(ColumnDataCollection &collection) : collection(collection) {
		collection.InitializeScan(state);
		collection.InitializeScanChunk(state, chunk);
	}

	//! The row count of the paged collection
	idx_t size() const { //	NOLINT
		return collection.Count();
	}

	//! Read a typed cell
	const T &operator[](idx_t row_idx) {
		auto index = Seek(row_idx);
		return reinterpret_cast<const T &>((*values)[index].GetValueUnsafe());
	}

private:
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
		if (!collection.Scan(state, chunk)) {
			return false;
		}
		values = make_uniq<VectorIterator<VECTOR_TYPE>>(chunk.data[0]);
		return true;
	}
	//! Seek to the given row
	inline idx_t Seek(idx_t row_idx) {
		if (!RowIsVisible(row_idx)) {
			collection.Seek(row_idx, state, chunk);
			values = make_uniq<VectorIterator<VECTOR_TYPE>>(chunk.data[0]);
		}
		return RowOffset(row_idx);
	}

	//! The pageable data
	const ColumnDataCollection &collection;
	//! The state used for reading the collection
	ColumnDataScanState state;
	//! The data chunk read into
	DataChunk chunk;
	//! Cell access into the page, rebuilt per page load - scans can return constant and dictionary vectors
	unique_ptr<VectorIterator<VECTOR_TYPE>> values;
};

struct IEJoinUnion {
	using SortedTable = PhysicalRangeJoin::GlobalSortedTable;
	using ChunkRange = std::pair<idx_t, idx_t>;

	//	Comparison utilities
	static bool IsStrictComparison(ExpressionType comparison) {
		switch (comparison) {
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_GREATERTHAN:
			return true;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			return false;
		default:
			throw InternalException("Unimplemented comparison type for IEJoin!");
		}
	}

	template <typename T>
	static inline bool Compare(const T &lhs, const T &rhs, const bool strict) {
		const bool less_than = lhs < rhs;
		if (!less_than && !strict) {
			return !(rhs < lhs);
		}
		return less_than;
	}

	static idx_t AppendKey(ExecutionContext &context, InterruptState &interrupt, SortedTable &table,
	                       ExpressionExecutor &executor, SortedTable &marked, int64_t increment, int64_t rid,
	                       const ChunkRange &range);

	static unique_ptr<ColumnDataCollection> ExtractColumn(SortedTable &table, idx_t col_idx,
	                                                      BufferManager &buffer_manager) {
		auto &collection = *table.sorted->payload_data;
		vector<column_t> scan_ids(1, col_idx);
		TupleDataScanState scan_state;
		collection.InitializeScan(scan_state, scan_ids);

		DataChunk payload;
		collection.InitializeScanChunk(scan_state, payload);

		auto result = make_uniq<ColumnDataCollection>(buffer_manager, payload.GetTypes());
		ColumnDataAppendState append_state;
		result->InitializeAppend(append_state);
		while (collection.Scan(scan_state, payload)) {
			result->Append(append_state, payload);
		}

		return result;
	}

	class UnionIterator {
	public:
		UnionIterator(SortedTable &table, bool strict) : state(table.CreateIteratorState()), strict(strict) {
		}

		inline idx_t GetIndex() const {
			return index;
		}

		inline void SetIndex(idx_t i) {
			index = i;
		}

		inline idx_t GetChunkIndex() const {
			idx_t chunk_idx;
			idx_t tuple_idx;
			state->RandomAccess(chunk_idx, tuple_idx, index);
			return chunk_idx;
		}

		inline void SetChunkIndex(idx_t chunk_idx) {
			index = state->GetDivisor() * chunk_idx;
		}

		UnionIterator &operator++() {
			++index;
			return *this;
		}

		void Repin() {
			state->SetKeepPinned(true);
			state->SetPinPayload(true);
		}

		unique_ptr<ExternalBlockIteratorState> state;
		idx_t index = 0;
		const bool strict;
	};

	IEJoinUnion(SortedTable &l2, ColumnDataCollection &li, ColumnDataCollection &p,
	            const vector<JoinCondition> &conditions, const ChunkRange &chunks);

	static void InitializeTables(ClientContext &client, const PhysicalComparisonJoin &op,
	                             const vector<JoinCondition> &conditions, unique_ptr<SortedTable> &l1,
	                             unique_ptr<SortedTable> &l2);

	//! Start the current row.
	//! Returns false if there are no more rows to process
	template <SortKeyType SORT_KEY_TYPE>
	bool NextRow();

	//! NextRow pointer to member for the sort key type.
	using next_row_t = bool (duckdb::IEJoinUnion::*)();
	next_row_t next_row_func;

	//! Finish this row and move to the next one.
	//! Returns false if there are no more rows to process
	bool FinishRow() {
		++i;
		return (this->*next_row_func)();
	}

	//! Inverted loop
	idx_t JoinBlocks(unsafe_vector<idx_t> &lsel, unsafe_vector<idx_t> &rsel);

	//! B
	vector<validity_t> bit_array;
	ValidityMask bit_mask;
	//! Bloom Filter
	static constexpr idx_t BLOOM_CHUNK_BITS = 1024;
	idx_t bloom_count;
	vector<validity_t> bloom_array;
	ValidityMask bloom_filter;

	//! Iteration state
	idx_t n;
	idx_t i;
	idx_t n_j;
	idx_t j;
	unique_ptr<UnionIterator> op2;
	unique_ptr<UnionIterator> off2;
	int64_t lrid = std::numeric_limits<int64_t>::max();

	//! ANTI JOIN bookmark
	idx_t anti_i;

	//! Li
	IEJoinCursor<int64_t> li;
	//! P
	IEJoinCursor<idx_t, int64_t> p;
};

} // namespace duckdb
