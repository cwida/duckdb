//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/sorted_run.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/row/tuple_data_states.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

class Sort;
class SortedRun;
class BufferManager;
class DataChunk;
class TupleDataCollection;
class TupleDataLayout;

class SortedRunScanState {
public:
	SortedRunScanState(ClientContext &context, const Sort &sort);

public:
	void Scan(const SortedRun &sorted_run, const Vector &sort_key_pointers, DataChunk &chunk);
	void Clear();

private:
	template <SortKeyType sort_key_type>
	void TemplatedScan(const SortedRun &sorted_run, const Vector &sort_key_pointers, DataChunk &chunk);

private:
	const Sort &sort;
	ExpressionExecutor key_executor;
	DataChunk key;
	DataChunk decoded_key;
	TupleDataScanState payload_state;
	vector<char> key_buffer;
};

class SortedRun {
public:
	SortedRun(ClientContext &context, const Sort &sort, bool is_index_sort);
	unique_ptr<SortedRun> CreateRunForMaterialization() const;
	~SortedRun();

public:
	//! Appends data to key/data collections
	void Sink(DataChunk &key, DataChunk &payload);
	//! Sorts the data (physically reorder data if external)
	void Finalize(bool external);
	//! Physically reorders the data of a run that was finalized for an in-memory sort, as an external sort needs
	void MakeExternal();
	//! Destroy keys/payload between these tuple indices
	void DestroyKeyData(idx_t tuple_idx_begin, idx_t tuple_idx_end);
	void DestroyPayloadData(idx_t tuple_idx_begin, idx_t tuple_idx_end);
	//! Number of tuples
	idx_t Count() const;
	//! Size of this sorted run
	idx_t SizeInBytes() const;
	//! Size of this sorted run plus the merge buffer that Finalize allocates to sort it
	idx_t SizeInBytesForSorting() const;

private:
	mutex merger_global_state_lock;
	unique_ptr<GlobalSourceState> merge_global_state;

public:
	ClientContext &context;
	const Sort &sort;

	//! Key and payload collections (and associated append states)
	unique_ptr<TupleDataCollection> key_data;
	unique_ptr<TupleDataCollection> payload_data;
	TupleDataAppendState key_append_state;
	TupleDataAppendState payload_append_state;

	//! Whether this is an (approximate) index sort
	const bool is_index_sort;

	//! Whether this run has been finalized
	bool finalized;
	//! Whether its data has been physically reordered for an external sort
	bool external = false;

private:
	//! Reorder the variable-size portion of the sorted keys and the payload, and unpin the data
	void ReorderForExternal();
};

} // namespace duckdb
