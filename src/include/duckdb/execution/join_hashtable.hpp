//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/join_hashtable.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/types/column/column_data_consumer.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/row/partitioned_tuple_data.hpp"
#include "duckdb/common/types/row/tuple_data_iterator.hpp"
#include "duckdb/common/types/row/tuple_data_layout.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/joinside.hpp"

namespace duckdb {

class BufferManager;
class BufferHandle;
class ColumnDataCollection;
struct ColumnDataAppendState;
struct ClientConfig;
struct ResidualPredicateInfo;
class PhysicalHashJoin;

struct JoinHTScanState {
public:
	JoinHTScanState(TupleDataCollection &collection, idx_t chunk_idx_from, idx_t chunk_idx_to,
	                TupleDataPinProperties properties = TupleDataPinProperties::ALREADY_PINNED)
	    : iterator(collection, properties, chunk_idx_from, chunk_idx_to, false), offset_in_chunk(0), chunks_done(0) {
	}

	TupleDataChunkIterator iterator;
	idx_t offset_in_chunk;
	//! The number of chunks that have been scanned entirely
	idx_t chunks_done;

private:
	//! Implicit copying is not allowed
	JoinHTScanState(const JoinHTScanState &) = delete;
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

   Two-phase lifecycle: the constructor populates only layout-INDEPENDENT state; all layout-DEPENDENT state
   (layout_ptr, row matchers, tuple_size/pointer_offset/entry_size, data_collection, sink_collection, dead_end,
   dict_info) is published by FinishInitWithLayout on the first build chunk, so slot widths (and the dictionary
   a narrowed slot indexes into) can be chosen from the data's actual runtime encoding. Until then the JHT is
   unusable except for the null-safe Count() / SizeInBytes() accessors; the layout-dependent accessors assert
   IsLayoutFinalized(). ResetForNewIterationSinglePartition returns the JHT to the unpublished state so a recursive
   CTE iteration publishes afresh.
*/
//! One build payload column of the dict-surviving optimisation: the row store carries a narrow index into `entry`
//! instead of the native value.
struct DictSurvivingColumn {
	//! Byte width of the narrowed dict-index slot (1/2/4), 0 when the column is stored at native width
	uint8_t index_width = 0;
	//! Self-owned copy of the upstream dictionary plus one trailing NULL slot; set iff narrowed
	buffer_ptr<DictionaryEntry> entry;
	//! Id of the upstream dictionary the column was narrowed against; later chunks must carry the same id
	string upstream_id;

	//! The reserved trailing slot that encodes a NULL-only chunk
	idx_t NullIndex() const {
		return entry->data.size() - 1;
	}
};

//! Descriptor of the dict-surviving optimisation for one hash join. Chosen by the layout publisher from the first
//! build chunk together with the layout and shared read-only by every per-thread JHT. Re-chosen whenever the layout
//! is (e.g. per recursive CTE iteration), since a rerun build may deliver a different dictionary or none at all.
struct DictSurvivingInfo {
	//! One record per build payload column (parallel to build_types); index_width 0 = stored at native width
	vector<DictSurvivingColumn> columns;

	//! True iff any column is narrowed
	bool Any() const {
		for (const auto &column : columns) {
			if (column.index_width != 0) {
				return true;
			}
		}
		return false;
	}
	//! The record of build payload column build_col_idx, or nullptr when it is stored at native width
	optional_ptr<const DictSurvivingColumn> Get(idx_t build_col_idx) const {
		if (build_col_idx >= columns.size() || columns[build_col_idx].index_width == 0) {
			return nullptr;
		}
		return &columns[build_col_idx];
	}
	//! Row-store slot type of a narrowed column of the given index width
	static LogicalType IndexType(uint8_t index_width);
};

class JoinHashTable {
public:
	using ValidityBytes = TemplatedValidityMask<uint8_t>;

	struct ResidualPredicateProbeState {
		//! Evaluation chunk
		DataChunk eval_chunk;
		SelectionVector selected_sel;
		SelectionVector remaining_sel;

		ResidualPredicateProbeState() : selected_sel(STANDARD_VECTOR_SIZE), remaining_sel(STANDARD_VECTOR_SIZE) {
		}

		void Initialize(Allocator &allocator, const vector<LogicalType> &eval_types,
		                const vector<bool> &initialize_columns) {
			eval_chunk.Initialize(allocator, eval_types, initialize_columns, STANDARD_VECTOR_SIZE);
		}
	};

#ifdef DUCKDB_HASH_ZERO
	//! Verify salt when all hashes are 0
	static constexpr const idx_t USE_SALT_THRESHOLD = 0;
#else
	//! only compare salts with the ht entries if the capacity is larger than 8192 so
	//! that it does not fit into the CPU cache
	static constexpr const idx_t USE_SALT_THRESHOLD = 8192;
#endif

	//! Scan structure that can be used to resume scans, as a single probe can
	//! return 1024*N values (where N is the size of the HT). This is
	//! returned by the JoinHashTable::Scan function and can be used to resume a
	//! probe.
	struct ScanStructure {
		TupleDataChunkState &key_state;
		//! Directly point to the entry in the hash table
		Vector pointers;
		idx_t count;
		SelectionVector sel_vector;
		SelectionVector chain_match_sel_vector;
		SelectionVector chain_no_match_sel_vector;

		// whether or not the given tuple has found a match
		unsafe_unique_array<bool> found_match;
		JoinHashTable &ht;
		bool finished;
		bool is_null;
		bool has_null_value_filter = false;

		// it records the RHS pointers for the result chunk
		Vector rhs_pointers;
		// it records the LHS sel vector for the result chunk
		SelectionVector lhs_sel_vector;
		// these two variable records the last match results
		idx_t last_match_count;
		SelectionVector last_sel_vector;

		// Thread-local probe match count, flushed when the current chunk's pointer scan is exhausted.
		idx_t local_probe_matches = 0;

		explicit ScanStructure(JoinHashTable &ht, TupleDataChunkState &key_state);
		void Reset();
		//! Get the next batch of data from the scan structure
		void Next(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		//! Are pointer chains all pointing to NULL?
		bool PointersExhausted() const;
		//! Flush the thread-local probe match count into the global counter (idempotent)
		void FlushProbeMatches();

	private:
		//! Next operator for the inner join
		void NextInnerJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		void NextSemiJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		void NextAntiJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		void NextRightSemiOrAntiJoin(DataChunk &keys, DataChunk &probe_data);
		void NextLeftJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		void NextMarkJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		void NextSingleJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);
		//! Next operator for left join when RHS keys are unique (single-pass, no state machine)
		void NextUniqueLeftJoin(DataChunk &keys, DataChunk &probe_data, DataChunk &result);

		//! Scan the hashtable for matches of the specified keys, setting the found_match[] array to true or false
		//! for every tuple
		void ScanKeyMatches(DataChunk &keys, DataChunk &probe_data);
		template <bool MATCH>
		void NextSemiOrAntiJoin(DataChunk &keys, DataChunk &left, DataChunk &result);
		void ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &child, DataChunk &result);

		idx_t ScanInnerJoin(DataChunk &keys, DataChunk &probe_data, SelectionVector &result_vector);

		//! Update the data chunk compaction buffer
		void UpdateCompactionBuffer(idx_t base_count, SelectionVector &result_vector, idx_t result_count);

		//! Apply residual predicate filtering
		idx_t ApplyResidualPredicate(DataChunk &probe_data, SelectionVector &match_sel, idx_t match_count,
		                             optional_ptr<SelectionVector> no_match_sel, idx_t no_match_offset = 0);

	private:
		unique_ptr<ExpressionExecutor> residual_executor;
		unique_ptr<ResidualPredicateProbeState> residual_state;

	public:
		void AdvancePointers();
		void AdvancePointers(const SelectionVector &sel, idx_t sel_count);
		void GatherResult(Vector &result, const SelectionVector &result_vector, const SelectionVector &sel_vector,
		                  const idx_t count, const idx_t col_idx);
		void GatherResult(Vector &result, const SelectionVector &sel_vector, const idx_t count, const idx_t col_idx);
		void GatherResult(Vector &result, const idx_t count, const idx_t col_idx);
		idx_t ResolvePredicates(DataChunk &keys, DataChunk &probe_data, SelectionVector &match_sel,
		                        optional_ptr<SelectionVector> no_match_sel);
	};

public:
	struct SharedState {
		SharedState();

		Vector salt_v;

		SelectionVector keys_to_compare_sel;
		SelectionVector keys_no_match_sel;
	};

	//! Mirrors GroupedAggregateHashTable::AggregateDictionaryState for the join probe path
	struct ProbeDictionaryState {
		ProbeDictionaryState();

		//! The current dictionary vector id (if any)
		string dictionary_id;
		DataChunk unique_values;
		TupleDataChunkState unique_key_state;
		Vector hashes;
		Vector new_dictionary_pointers;
		SelectionVector unique_entries;
		//! Per-slot head-of-chain pointer cache; nullptr marks a miss
		unique_ptr<Vector> dictionary_pointers;
		unsafe_unique_array<bool> found_entry;
		idx_t capacity = 0;
		SelectionVector match_sel;
		//! Number of dict slots already resolved; the unique-entries walk is skipped once it reaches dict_size
		idx_t resolved_count = 0;
	};

	struct ProbeState : SharedState {
		ProbeState();

		Vector ht_offsets_and_salts_v;
		Vector hashes_dense_v;
		SelectionVector non_empty_sel;
		//! Allocated only when the operator gates the compressed-probe paths on; null otherwise
		unique_ptr<ProbeDictionaryState> dict_state;
	};

	struct InsertState : SharedState {
		explicit InsertState(const JoinHashTable &ht);
		/// Because of the index hick up
		SelectionVector remaining_sel;
		SelectionVector key_match_sel;

		// The ptrs to the row to which a key should be inserted into during building
		// or matched against during probing
		Vector rhs_row_locations;

		DataChunk lhs_data;
		TupleDataChunkState chunk_state;
	};

	JoinHashTable(ClientContext &context, const PhysicalOperator &op, const vector<JoinCondition> &conditions,
	              vector<LogicalType> build_types, JoinType type, idx_t initial_radix_bits,
	              const vector<idx_t> &output_columns, unique_ptr<ResidualPredicateInfo> residual_p,
	              optional_ptr<Expression> predicate_ptr = nullptr, const vector<idx_t> &output_in_probe = {});
	~JoinHashTable();

	//! Initialize layout-dependent state from a layout (and dict-surviving descriptor, nullptr = no narrowed column)
	//! shared across all per-thread JHTs (deferred ctor body)
	void FinishInitWithLayout(shared_ptr<TupleDataLayout> published_layout,
	                          shared_ptr<const DictSurvivingInfo> dict_info_p = nullptr);
	//! True iff FinishInitWithLayout has populated layout-dependent state
	bool IsLayoutFinalized() const {
		return layout_ptr.get() != nullptr;
	}

	//! Per-column index-width decision for the dict-surviving optimisation, consulted by the layout publisher on
	//! the first build chunk. Returns the narrowed index byte width (1/2/4), or 0 to keep native width. The width
	//! leaves room for one extra slot: the reserved NULL index that encodes NULL-only chunks.
	uint8_t GetDictSurvivingIndexWidth(idx_t build_col_idx, const Vector &incoming) const;
	//! Create the record of a narrowed column: a self-owned, global copy of the incoming global dictionary with one
	//! trailing NULL slot (index = upstream size) and a fresh id, plus the width and the upstream id to check later
	//! chunks against.
	static DictSurvivingColumn CreateDictSurvivingColumn(const Vector &incoming, uint8_t index_width);
	//! True iff any build payload column is stored as a narrow dictionary index
	bool HasDictSurvivingColumns() const {
		return dict_info && dict_info->Any();
	}
	//! The dict-surviving record of build payload column build_col_idx, or nullptr when it is stored natively
	optional_ptr<const DictSurvivingColumn> GetDictSurvivingColumn(idx_t build_col_idx) const {
		return dict_info ? dict_info->Get(build_col_idx) : nullptr;
	}

	//! Add the given data to the HT
	void Build(PartitionedTupleDataAppendState &append_state, DataChunk &keys, DataChunk &input);
	//! Merge another HT into this one
	void Merge(JoinHashTable &other);
	//! Combines the partitions in sink_collection into data_collection, as if it were not partitioned
	void Unpartition();
	//! Allocate the pointer table for the probe
	void AllocatePointerTable(bool use_radix_layout = false);
	idx_t GetBucketShift() const {
		return bucket_shift;
	}
	//! Initialize the pointer table for the probe
	void InitializePointerTable(idx_t entry_idx_from, idx_t entry_idx_to);
	//! Finalize the build of the HT, constructing the actual hash table and making the HT ready for probing.
	//! Finalize must be called before any call to Probe, and after Finalize is called Build should no longer be
	//! ever called.
	void Finalize(idx_t chunk_idx_from, idx_t chunk_idx_to, bool parallel,
	              optional_ptr<PrefixRangeFilter::BuildState> prefix_range_state = nullptr,
	              bool prefix_range_parallel = false);
	//! Probe the HT with the given input chunk, resulting in the given result
	void Probe(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state, ProbeState &probe_state,
	           optional_ptr<Vector> precomputed_hashes = nullptr);
	//! Enable selective NULL refinement for an uncorrelated multi-column MARK join
	void InitializeUncorrelatedMarkJoin();
	bool HasUncorrelatedMarkJoin() const;
	//! Construct a MARK result, including selective UNKNOWN refinement when enabled
	void ConstructMarkJoinResult(DataChunk &join_keys, DataChunk &probe_data, DataChunk &result,
	                             optional_ptr<const bool> found_match = nullptr);
	//! Scan the HT to construct the full outer join result
	void ScanFullOuter(JoinHTScanState &state, Vector &addresses, DataChunk &result) const;

	//! Fill the pointer with all the addresses from the hashtable for full scan
	static idx_t FillWithHTOffsets(JoinHTScanState &state, Vector &addresses);

	//! Pre-materialize RHS output columns into dict_arrays and embed the dict index into NEXT_PTR;
	//! called once from HashJoinFinalizeEvent::FinishEvent, after all finalize tasks have completed.
	void BuildDictionaryArrays(const PhysicalHashJoin &op);
	//! Emit dictionary vectors for row_ptrs[ptr_sel[0..count)]; pass
	//! *FlatVector::IncrementalSelectionVector() when row_ptrs is already compacted
	void EmitDictVectors(const data_ptr_t *row_ptrs, const SelectionVector &ptr_sel, idx_t count, DataChunk &result,
	                     idx_t rhs_col_offset) const;
	//! Emit RHS output columns for matched rows row_ptrs[ptr_sel[0..count)] into result starting at
	//! result column rhs_col_offset; routes through EmitDictVectors when use_dict_emission is active.
	void GatherRHS(Vector &row_ptrs, const SelectionVector &ptr_sel, const idx_t count, DataChunk &result,
	               idx_t rhs_col_offset) const;
	//! Follow the chain pointer; when USE_DICT_EMISSION, resolves via aux_next_ptrs
	template <bool USE_DICT_EMISSION>
	inline data_ptr_t GetNextPointer(data_ptr_t row_ptr) const {
		if (USE_DICT_EMISSION) {
			if (!chains_longer_than_one.load(std::memory_order_relaxed)) {
				// aux_next_ptrs is unallocated in this case
				return nullptr;
			}
			return aux_next_ptrs_data[Load<uint32_t>(row_ptr + pointer_offset)];
		}
		return cast_uint64_to_pointer(Load<uint64_t>(row_ptr + pointer_offset));
	}

	idx_t Count() const {
		return data_collection ? data_collection->Count() : 0;
	}
	idx_t SizeInBytes() const {
		idx_t size = data_collection ? data_collection->SizeInBytes() : 0;
		if (mark_join_info.uncorrelated_condition_rows) {
			size += mark_join_info.uncorrelated_condition_rows->SizeInBytes();
		}
		return size;
	}

	PartitionedTupleData &GetSinkCollection() {
		// Only valid after FinishInitWithLayout; assert so a premature access fails loudly, not as a null-deref.
		D_ASSERT(IsLayoutFinalized());
		return *sink_collection;
	}

	TupleDataCollection &GetDataCollection() {
		// Only valid after FinishInitWithLayout (see GetSinkCollection).
		D_ASSERT(IsLayoutFinalized());
		return *data_collection;
	}
	//! Perform a full scan of a build column, filling the provided addresses vector and result vector.
	//! Returns the number of tuples found (can be smaller than the vector capacity).
	idx_t ScanKeyColumn(Vector &addresses, Vector &result, idx_t column_index) const;

	bool NullValuesAreEqual(idx_t col_idx) const {
		return null_values_are_equal[col_idx];
	}

	ClientContext &context;
	const PhysicalOperator &op;
	//! BufferManager
	BufferManager &buffer_manager;
	//! The join conditions
	const vector<JoinCondition> &conditions;
	//! The types of the keys used in equality comparison
	vector<LogicalType> equality_types;
	//! The types of the keys
	vector<LogicalType> condition_types;
	//! The types of all conditions
	vector<LogicalType> build_types;
	//! Positions of the columns that need to output
	const vector<idx_t> &output_columns;
	//! The comparison predicates that only contain equality predicates
	vector<ExpressionType> equality_predicates;
	//! The comparison predicates that contain non-equality predicates
	vector<ExpressionType> non_equality_predicates;

	//! The column indices of the equality predicates to be used to compare the rows
	vector<column_t> equality_predicate_columns;
	//! The column indices of the non-equality predicates to be used to compare the rows
	vector<column_t> non_equality_predicate_columns;
	//! Data column layout
	shared_ptr<TupleDataLayout> layout_ptr;
	//! Matches the equal condition rows during the build phase of the hash join to prevent
	//! duplicates in a list because of hash-collisions
	RowMatcher row_matcher_build;
	//! Efficiently matches the non-equi rows during the probing phase, only there if non_equality_predicates is not
	//! empty
	unique_ptr<RowMatcher> row_matcher_probe;
	//! Matches the same rows as the row_matcher, but also returns a vector for no matches
	unique_ptr<RowMatcher> row_matcher_probe_no_match_sel;
	//! Is true if there are predicates that are not equality predicates and we need to use the matchers during probing
	bool needs_chain_matcher;

	//! If there is more than one element in the chain, we need to scan the next elements of the chain
	atomic<bool> chains_longer_than_one {false};

	//! The capacity of the HT. Is the same as hash_map.GetSize() / sizeof(ht_entry_t)
	idx_t capacity = DConstants::INVALID_INDEX;
	//! The size of an entry as stored in the HashTable
	idx_t entry_size;
	//! The total tuple size
	idx_t tuple_size;
	//! Next pointer offset in tuple, also used for the position of the hash, which then gets overwritten by the pointer
	idx_t pointer_offset;
	//! A constant false column for initialising right outer joins
	Vector vfound;
	//! The join type of the HT
	JoinType join_type;
	//! Whether or not the HT has been finalized
	bool finalized;
	//! Whether or not any of the key elements contain NULL
	bool has_null;
	//! Bitmask for getting relevant bits from the hashes to determine the position
	uint64_t bitmask = DConstants::INVALID_INDEX;
	//! Whether or not we error on multiple rows found per match in a SINGLE join
	bool single_join_error_on_multiple_rows = true;
	//! Whether or not to perform deduplication based on join_keys when building ht
	bool insert_duplicate_keys = true;
	//! Number of probe matches, isolated from immutable probe metadata.
	PaddedAtomic<idx_t> total_probe_matches {0};
	//! Residual predicate to evaluate during probing
	optional_ptr<Expression> residual_predicate;
	//! Residual predicate mapping info
	unique_ptr<ResidualPredicateInfo> residual_info;
	//! Mapping from lhs_output_columns positions to lhs_probe_data positions
	vector<idx_t> lhs_output_in_probe;

	//! True once BuildDictionaryArrays has embedded dictionary indices into NEXT_PTR
	bool use_dict_emission = false;
	//! Pre-materialized columnar data, one entry per RHS output column
	vector<buffer_ptr<DictionaryEntry>> dict_arrays;
	//! Dict-surviving descriptor shared with every other per-thread JHT of the same join; set by
	//! FinishInitWithLayout, nullptr when no build payload column is narrowed
	shared_ptr<const DictSurvivingInfo> dict_info;
	//! Saved NEXT_PTR values, indexed by dict index; only allocated when chains_longer_than_one
	AllocatedData aux_next_ptrs;
	//! Typed pointer into aux_next_ptrs; set by BuildDictionaryArrays alongside the allocation
	data_ptr_t *aux_next_ptrs_data = nullptr;

	//! Total bytes the dict_arrays allocation would cost for the given RHS output types
	idx_t ComputeBuildPayloadBytes(const vector<LogicalType> &rhs_output_types) const;
	//! Returns true iff small-build-side dictionary emission should activate
	bool CanUseDictionaryEmission(const PhysicalHashJoin &op, bool external, idx_t probe_cardinality) const;

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
		//! Whether an RHS condition can produce UNKNOWN during equality comparison
		bool uncorrelated_has_null = false;
		//! All RHS condition rows, used only for uncorrelated row equality NULL refinement
		unique_ptr<ColumnDataCollection> uncorrelated_condition_rows;
	} mark_join_info;

private:
	void InitializeScanStructure(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
	                             optional_ptr<const SelectionVector> &current_sel);
	void Hash(DataChunk &keys, const SelectionVector &sel, idx_t count, Vector &hashes);

	//! Dictionary-aware variant of Probe. Returns false if the LHS keys are not dictionary-eligible.
	bool TryProbeDictionary(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
	                        ProbeState &probe_state);
	//! Constant-vector variant of Probe. Returns false if the LHS keys are not a constant vector.
	bool TryProbeConstant(ScanStructure &scan_structure, DataChunk &keys, TupleDataChunkState &key_state,
	                      ProbeState &probe_state);

	bool UseSalt() const;

	//! Gets a pointer to the entry in the HT for each of the hashes_v using linear probing. Will update the
	//! key_match_sel vector and the count argument to the number and position of the matches
	void GetRowPointers(DataChunk &keys, TupleDataChunkState &key_state, ProbeState &state, Vector &hashes_v,
	                    optional_ptr<const SelectionVector> sel, idx_t &count, Vector &pointers_result_v,
	                    SelectionVector &match_sel, bool has_sel);

private:
	//! Insert the given set of locations into the HT with the given set of hashes_v
	void InsertHashes(Vector &hashes_v, TupleDataChunkState &chunk_state, InsertState &insert_state, bool parallel);
	//! Prepares keys by filtering NULLs
	idx_t PrepareKeys(DataChunk &keys, vector<TupleDataVectorFormat> &vector_data,
	                  optional_ptr<const SelectionVector> &current_sel, SelectionVector &sel, bool build_side);

	unsafe_optional_ptr<ht_entry_t> GetEntries() {
		D_ASSERT(hash_map.get());
		return reinterpret_cast<ht_entry_t *>(hash_map.get());
	}
	unsafe_optional_ptr<atomic<ht_entry_t>> GetAtomicEntries() {
		D_ASSERT(hash_map.get());
		return reinterpret_cast<atomic<ht_entry_t> *>(hash_map.get());
	}

	//! Lock for combining data_collection when merging HTs
	mutex data_lock;
	//! Partitioned data collection that the data is sunk into when building
	unique_ptr<PartitionedTupleData> sink_collection;
	//! The DataCollection holding the main data of the hash table
	unique_ptr<TupleDataCollection> data_collection;

	//! The hash map of the HT, created after finalization
	HugeAllocation hash_map;
	//! Shift to align fully resident buckets with radix partitions; zero selects the external/low-bit layout.
	idx_t bucket_shift = 0;
	//! Whether or not NULL values are considered equal in each of the comparisons
	vector<bool> null_values_are_equal;
	//! An empty tuple that's a "dead end", can be used to stop chains early
	unsafe_unique_array<data_t> dead_end;

	//! Bloom filter over the join keys. An external join fills it during Sink, an in-memory join during
	//! finalize. Shared by every thread-local hash table of this operator, and shared again with any scan it
	//! gets published to, so it outlives a reset of this table
	shared_ptr<BloomFilter> bloom_filter;
	//! Unpublished filter kept from a previous recursive iteration so finalize can reuse its allocation.
	//! Only ever set on the global hash table, never on the thread-local ones that share bloom_filter
	shared_ptr<BloomFilter> recycled_bloom_filter;
	//! Whether finalize should build the bloom filter from the materialized build side
	bool should_build_bloom_filter = false;

	unique_ptr<PrefixRangeFilter> prefix_range_filter;
	bool should_build_prefix_range_filter = false;

	//! Copying not allowed
	JoinHashTable(const JoinHashTable &) = delete;

public:
	//===--------------------------------------------------------------------===//
	// External Join
	//===--------------------------------------------------------------------===//
	struct ProbeSpillLocalAppendState {
		ProbeSpillLocalAppendState() {
		}
		//! Local partition and append state (if partitioned)
		optional_ptr<PartitionedColumnData> local_partition;
		optional_ptr<PartitionedColumnDataAppendState> local_partition_append_state;
	};
	//! ProbeSpill represents materialized probe-side data that could not be probed during PhysicalHashJoin::Execute
	//! because the HashTable did not fit in memory. The ProbeSpill is not partitioned if the remaining data can be
	//! dealt with in just 1 more round of probing, otherwise it is radix partitioned in the same way as the HashTable
	struct ProbeSpill {
	public:
		ProbeSpill(JoinHashTable &ht, ClientContext &context, const vector<LogicalType> &probe_types);

	public:
		//! Create a state for a new thread
		ProbeSpillLocalAppendState RegisterThread();
		//! Append a chunk to this ProbeSpill
		void Append(DataChunk &chunk, ProbeSpillLocalAppendState &local_state);
		//! Finalize by merging the thread-local accumulated data
		void Finalize();

	public:
		//! Prepare the next probe round
		void PrepareNextProbe();
		//! Scans and consumes the ColumnDataCollection
		unique_ptr<ColumnDataConsumer> consumer;

	private:
		JoinHashTable &ht;
		mutex lock;
		ClientContext &context;

		//! The types of the probe DataChunks
		const vector<LogicalType> &probe_types;
		//! The column ids
		vector<column_t> column_ids;

		//! The partitioned probe data and append states
		unique_ptr<PartitionedColumnData> global_partitions;
		vector<unique_ptr<PartitionedColumnData>> local_partitions;
		vector<unique_ptr<PartitionedColumnDataAppendState>> local_partition_append_states;

		//! The active probe data
		unique_ptr<ColumnDataCollection> global_spill_collection;
	};

	idx_t GetRadixBits() const {
		return radix_bits;
	}

	//! For a LOAD_FACTOR of 2.0, the HT is between 25% and 50% full
	static constexpr double DEFAULT_LOAD_FACTOR = 2.0;
	//! For a LOAD_FACTOR of 1.5, the HT is between 33% and 67% full
	static constexpr double EXTERNAL_LOAD_FACTOR = 1.5;
	//! Minimum capacity of the pointer table
	static constexpr idx_t MINIMUM_CAPACITY = 16384;

	double load_factor = DEFAULT_LOAD_FACTOR;

	//! Capacity of the pointer table given the ht count
	idx_t PointerTableCapacity(idx_t count) const {
		const auto capacity = NextPowerOfTwo(LossyNumericCast<idx_t>(static_cast<double>(count) * load_factor));
		return MaxValue<idx_t>(capacity, MINIMUM_CAPACITY);
	}
	//! Size of the pointer table (in bytes)
	idx_t PointerTableSize(idx_t count) const {
		return PointerTableCapacity(count) * sizeof(data_ptr_t);
	}

	void SetBloomFilter(shared_ptr<BloomFilter> filter) {
		bloom_filter = std::move(filter);
	}
	//! Requests a filter built during finalize, from the materialized build side (in-memory joins only)
	void SetBuildBloomFilter(const bool should_build) {
		this->should_build_bloom_filter = should_build;
	}

	shared_ptr<BloomFilter> GetBloomFilter() {
		return bloom_filter;
	}
	//! Hands an unpublished filter from a previous recursive iteration to the finalize-built filter path
	void SetRecycledBloomFilter(shared_ptr<BloomFilter> filter) {
		// a null argument would drop a parked filter; callers must only hand over live, unpublished filters
		D_ASSERT(filter && !filter->IsPublished());
		recycled_bloom_filter = std::move(filter);
	}
	//! Marks the bloom filter complete. The reference stays, because the thread-local tables hold it too
	void SealBloomFilter() {
		if (bloom_filter) {
			bloom_filter->Seal(context);
		}
	}
	//! Whether the filter is complete and rejects enough for pushing it into a scan to pay off
	bool HasPublishableBloomFilter() const {
		return bloom_filter && bloom_filter->IsSealed() && bloom_filter->IsUseful();
	}

	void SetPrefixRangeFilter(unique_ptr<PrefixRangeFilter> filter) {
		prefix_range_filter = std::move(filter);
	}

	void SetBuildPrefixRangeFilter() {
		should_build_prefix_range_filter = true;
	}

	optional_ptr<PrefixRangeFilter> GetPrefixRangeFilter() {
		return prefix_range_filter;
	}

	bool ShouldBuildPrefixRangeFilter() const {
		return should_build_prefix_range_filter && prefix_range_filter;
	}

	void BuildPrefixRangeFilter();
	unique_ptr<PrefixRangeFilter::BuildState> InitializePrefixRangeBuildState();
	idx_t GetPrefixRangeBuildStateSize() const;
	void InsertPrefixRangeChunk(TupleDataChunkState &chunk_state, idx_t count, PrefixRangeFilter::BuildState &state,
	                            bool parallel = false);
	void MergePrefixRangeBuildState(PrefixRangeFilter::BuildState &state);

	//! Get total size of HT if all partitions would be built
	idx_t GetTotalSize(const vector<reference<JoinHashTable>> &local_hts, idx_t &max_partition_size,
	                   idx_t &max_partition_count) const;
	idx_t GetTotalSize(const vector<idx_t> &partition_sizes, const vector<idx_t> &partition_counts,
	                   idx_t &max_partition_size, idx_t &max_partition_count) const;
	//! Get the remaining size of the unbuilt partitions
	idx_t GetRemainingSize() const;
	//! Sets number of radix bits according to the max ht size
	void SetRepartitionRadixBits(const idx_t max_ht_size, const idx_t max_partition_size,
	                             const idx_t max_partition_count);
	//! Initialized "current_partitions" and "completed_partitions"
	void InitializePartitionMasks();
	//! How many partitions are currently active
	idx_t CurrentPartitionCount() const;
	//! Get the current partitions validity mask
	const ValidityMask &GetCurrentPartitions() const;
	//! How many partitions are fully done
	idx_t FinishedPartitionCount() const;
	//! Partition this HT
	void Repartition(JoinHashTable &global_ht);

	//! Delete blocks that belong to the current partitioned HT
	void Reset();
	//! Collapses the sink collection to a single partition.
	//! Used by recursive CTEs to avoid per-iteration overhead of managing many radix partitions
	//! when only one thread is building the hash table.
	void ResetForNewIterationSinglePartition();
	//! Build HT for the next partitioned probe round
	bool PrepareExternalFinalize(const idx_t max_ht_size);
	//! Probe whatever we can, sink the rest into a thread-local HT
	void ProbeAndSpill(ScanStructure &scan_structure, DataChunk &probe_keys, TupleDataChunkState &key_state,
	                   ProbeState &probe_state, DataChunk &probe_chunk, ProbeSpill &probe_spill,
	                   ProbeSpillLocalAppendState &spill_state, DataChunk &spill_chunk);

private:
	//! True iff the residual predicate (if any) reads build payload column build_col_idx from its row slot
	bool ColumnReferencedByResidual(idx_t build_col_idx) const;
	//! Materialise the narrow index vector for a narrowed column: the dictionary indices of a chunk that wraps the
	//! pinned upstream dictionary, or the reserved NULL index for a NULL-only chunk (e.g. the unmatched-build scan of
	//! a FULL/RIGHT join upstream). Any other shape means the build pipeline is not single-source and throws.
	void EncodeDictSurvivingColumn(const DictSurvivingColumn &column, idx_t build_col_idx, const Vector &incoming,
	                               idx_t count, vector<AllocatedData> &buffers, vector<Vector> &vectors);

private:
	//! The current number of radix bits used to partition
	idx_t radix_bits;

	//! Bits set to 1 for currently active partitions
	ValidityMask current_partitions;
	//! Bits set to 1 for completed partitions
	ValidityMask completed_partitions;
};

} // namespace duckdb
