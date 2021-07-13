//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/join_hashtable.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/types/row_layout.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/storage/storage_info.hpp"

namespace duckdb {
class BufferManager;
class BufferHandle;
class RowDataCollection;

struct JoinHTScanState {
	JoinHTScanState() : position(0), block_position(0) {
	}

	idx_t position;
	idx_t block_position;
	mutex lock;
};

//! JoinHashTable is a linear probing HT that is used for computing joins
/*!
   The JoinHashTable concatenates incoming chunks inside a linked list of
   data ptrs. The storage looks like this internally.
   [SERIALIZED ROW][NEXT POINTER]
   [SERIALIZED ROW][NEXT POINTER]
   There is a separate hash map of pointers that point into this table.
   This is what is used to resolve the hashes.
   [POINTER]
   [POINTER]
   [POINTER]
   The pointers are either NULL
*/
class JoinHashTable {
public:
	using ValidityBytes = TemplatedValidityMask<uint8_t>;

	//! Scan structure that can be used to resume scans, as a single probe can
	//! return 1024*N values (where N is the size of the HT). This is
	//! returned by the JoinHashTable::Scan function and can be used to resume a
	//! probe.
	struct ScanStructure {
		unique_ptr<VectorData[]> key_data;
		Vector pointers;
		idx_t count;
		SelectionVector sel_vector;
		// whether or not the given tuple has found a match
		unique_ptr<bool[]> found_match;
		JoinHashTable &ht;
		bool finished;

		explicit ScanStructure(JoinHashTable &ht);
		//! Get the next batch of data from the scan structure
		void Next(DataChunk &keys, DataChunk &left, DataChunk &result);

	private:
		//! Next operator for the inner join
		void NextInnerJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		//! Next operator for the semi join
		void NextSemiJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		//! Next operator for the anti join
		void NextAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		//! Next operator for the left outer join
		void NextLeftJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		//! Next operator for the mark join
		void NextMarkJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		//! Next operator for the single join
		void NextSingleJoin(DataChunk &keys, DataChunk &left, DataChunk &result);

		//! Scan the hashtable for matches of the specified keys, setting the found_match[] array to true or false
		//! for every tuple
		void ScanKeyMatches(DataChunk &keys);
		template <bool MATCH>
		void NextSemiOrAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result);

		void ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &child, DataChunk &result);

		idx_t ScanInnerJoin(DataChunk &keys, SelectionVector &result_vector);

		idx_t ResolvePredicates(DataChunk &keys, SelectionVector &match_sel);
		idx_t ResolvePredicates(DataChunk &keys, SelectionVector &match_sel, SelectionVector &no_match_sel);

	public:
		void AdvancePointers();
		void AdvancePointers(const SelectionVector &sel, idx_t sel_count);
		void GatherResult(Vector &result, const SelectionVector &result_vector, const SelectionVector &sel_vector,
		                  const idx_t count, const idx_t col_idx);
		void GatherResult(Vector &result, const SelectionVector &sel_vector, const idx_t count, const idx_t col_idx);
		idx_t ResolvePredicates(DataChunk &keys, SelectionVector &match_sel, SelectionVector *no_match_sel);
	};

private:
	mutex ht_lock;

	//! Nodes store the actual data of the tuples inside the HT as a linked list
	struct HTDataBlock {
		idx_t count;
		idx_t capacity;
		shared_ptr<BlockHandle> block;
	};

	struct BlockAppendEntry {
		BlockAppendEntry(data_ptr_t baseptr_, idx_t count_) : baseptr(baseptr_), count(count_) {
		}

		data_ptr_t baseptr;
		idx_t count;
	};

	idx_t AppendToBlock(HTDataBlock &block, BufferHandle &handle, vector<BlockAppendEntry> &append_entries,
	                    idx_t remaining);

public:
	void Hash(DataChunk &keys, const SelectionVector &sel, idx_t count, Vector &hashes);

	JoinHashTable(BufferManager &buffer_manager, vector<JoinCondition> &conditions, vector<LogicalType> build_types,
	              JoinType type);
	~JoinHashTable();

	//! Add the given data to the HT
	void Build(DataChunk &keys, DataChunk &input);
	//! Finalize the build of the HT, constructing the actual hash table and making the HT ready for probing.
	//! Finalize must be called before any call to Probe, and after Finalize is called Build should no longer be
	//! ever called.
	void Finalize();
	//! Probe the HT with the given input chunk, resulting in the given result
	unique_ptr<ScanStructure> Probe(DataChunk &keys);
	//! Scan the HT to construct the final full outer join result after
	void ScanFullOuter(DataChunk &result, JoinHTScanState &state);
	//! Fill the pointer with all the addresses from the hashtable
	idx_t FillWithHTOffsets(data_ptr_t *key_locations, JoinHTScanState &state);
	idx_t size() {
		return count;
	}

	//! The stringheap of the JoinHashTable
	unique_ptr<RowDataCollection> string_heap;
	//! BufferManager
	BufferManager &buffer_manager;
	//! The types of the keys used in equality comparison
	vector<LogicalType> equality_types;
	//! The types of the keys
	vector<LogicalType> condition_types;
	//! The types of all conditions
	vector<LogicalType> build_types;
	//! The comparison predicates
	vector<ExpressionType> predicates;
	//! Data column layout
	RowLayout layout;
	//! Size of the validity vector for each tuple.
	idx_t validity_size;
	//! The size of an entry as stored in the HashTable
	idx_t entry_size;
	//! The total tuple size
	idx_t tuple_size;
	//! Next pointer offset in tuple
	idx_t pointer_offset;
	//! A constant false column for initialising right outer joins
	Vector vfound;
	//! The join type of the HT
	JoinType join_type;
	//! Whether or not the HT has been finalized
	bool finalized {false};
	//! Whether or not any of the key elements contain NULL
	bool has_null {false};
	//! Bitmask for getting relevant bits from the hashes to determine the position
	uint64_t bitmask;
	//! The amount of entries stored per block
	idx_t block_capacity;

	struct {
		mutex mj_lock;
		//! The types of the duplicate eliminated columns, only used in correlated MARK JOIN for flattening
		//! ANY()/ALL() expressions
		vector<LogicalType> correlated_types;
		//! The aggregate expression nodes used by the HT
		vector<unique_ptr<Expression>> correlated_aggregates;
		//! The HT that holds the group counts for every correlated column
		unique_ptr<GroupedAggregateHashTable> correlated_counts;
		//! Group chunk used for aggregating into correlated_counts
		DataChunk group_chunk;
		//! Payload chunk used for aggregating into correlated_counts
		DataChunk correlated_payload;
		//! Result chunk used for aggregating into correlated_counts
		DataChunk result_chunk;
	} correlated_mark_join_info;
	//! Apply a bitmask to the hashes
	void ApplyBitmask(Vector &hashes, idx_t count);
	void ApplyBitmask(Vector &hashes, const SelectionVector &sel, idx_t count, Vector &pointers);

private:
	//! Insert the given set of locations into the HT with the given set of
	//! hashes. Caller should hold lock in parallel HT.
	void InsertHashes(Vector &hashes, idx_t count, data_ptr_t key_locations[]);

	idx_t PrepareKeys(DataChunk &keys, unique_ptr<VectorData[]> &key_data, const SelectionVector *&current_sel,
	                  SelectionVector &sel, bool build_side);

	//! The amount of entries stored in the HT currently
	idx_t count;
	//! The blocks holding the main data of the hash table
	vector<HTDataBlock> blocks;
	//! Pinned handles, these are pinned during finalization only
	vector<unique_ptr<BufferHandle>> pinned_handles;
	//! The hash map of the HT, created after finalization
	unique_ptr<BufferHandle> hash_map;
	//! Whether or not NULL values are considered equal in each of the comparisons
	vector<bool> null_values_are_equal;

	//! Copying not allowed
	JoinHashTable(const JoinHashTable &) = delete;
};

} // namespace duckdb
