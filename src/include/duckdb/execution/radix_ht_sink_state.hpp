//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/radix_ht_sink_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/radix_partitioned_hashtable.hpp"
#include "duckdb/execution/aggregate_hashtable.hpp"
#include "duckdb/execution/aggregate_state_spilling.hpp"
#include "duckdb/storage/temporary_memory_manager.hpp"

namespace duckdb {

class RadixHTGlobalSinkState;

//! How aggregate state spilling and native row combining are allowed to interleave
enum class SpillPhase {
	//! Rows may still be combined natively, and the exported width may still grow beyond it
	NATIVE_ALLOWED,
	//! The exported width grew beyond the native width: every combine must export its rows
	EXPORTED_ONLY,
	//! Rows were combined natively: the exported width can no longer grow beyond the native width
	NATIVE_COMBINE_STARTED
};

struct RadixHTConfig {
public:
	explicit RadixHTConfig(RadixHTGlobalSinkState &sink);

	void ResetForNewIteration(idx_t scheduled_workers);
	void SetRadixBits(const idx_t &radix_bits_p);
	bool SetRadixBitsToExternal();
	idx_t GetRadixBits() const;
	idx_t GetMaximumSinkRadixBits() const;

private:
	void SetRadixBitsInternal(idx_t radix_bits_p, bool external);
	idx_t InitialSinkRadixBits(idx_t num_threads) const;
	idx_t ExternalRadixBits(bool dynamic) const;
	idx_t MaximumSinkRadixBits() const;
	idx_t SinkCapacity() const;

private:
	//! The global sink state
	RadixHTGlobalSinkState &sink;

public:
	//! Width of tuples
	const idx_t row_width;
	//! Capacity of HTs during the Sink
	const idx_t sink_capacity;

private:
	//! Sink radix bits to initialize with
	static constexpr idx_t MAXIMUM_INITIAL_SINK_RADIX_BITS = 4;

public:
	//! Maximum Sink radix bits (independent of threads)
	static constexpr idx_t MAXIMUM_FINAL_SINK_RADIX_BITS = 8;

private:
	//! Current thread-global sink radix bits
	atomic<idx_t> sink_radix_bits;
	//! Maximum Sink radix bits (set based on number of threads, if not external)
	const idx_t maximum_sink_radix_bits;

	//! Thresholds at which we reduce the sink radix bits
	//! This needed to reduce cache misses when we have very wide rows
	static constexpr idx_t ROW_WIDTH_THRESHOLD_ONE = 32;
	static constexpr idx_t ROW_WIDTH_THRESHOLD_TWO = 64;

public:
	//! If we have this many or less threads, we grow the HT, otherwise we abandon
	static constexpr idx_t GROW_STRATEGY_THREAD_THRESHOLD = 2;
	//! If we fill this many blocks per partition, we trigger a repartition
	static constexpr double BLOCK_FILL_FACTOR = 0.5;
	//! By how many bits to repartition if a repartition is triggered
	static constexpr idx_t REPARTITION_RADIX_BITS = 2;
	//! Thread-limit divisor for state export and exported partition sizing
	static constexpr idx_t AGGREGATE_STATE_SPILL_DIVISOR = 8;
	//! Arena-only pressure that forces external aggregation
	static constexpr idx_t AGGREGATE_STATE_PRESSURE_DIVISOR = 2;
	//! Estimated memory amplification when importing exported states
	static constexpr idx_t EXPORTED_STATE_MEMORY_MULTIPLIER = 2;
};

class RadixHTGlobalSinkState : public GlobalSinkState {
public:
	RadixHTGlobalSinkState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);

	//! Destroys aggregate states (if multi-scan)
	~RadixHTGlobalSinkState() override;
	void Destroy();
	//! Minimum reservation to sink into HTs partitioned by the given radix bits
	idx_t CalculateMinimumReservation(idx_t radix_bits) const;
	//! Recomputes and publishes the reservation floor for the given radix bits
	void SetReservationFloor(ClientContext &context, idx_t radix_bits);
	//! Restarts the sink configuration for a recursive iteration and resets the floor to match
	void ResetConfigForNewIteration(ClientContext &context, idx_t scheduled_workers);

public:
	idx_t GetThreadLimit() const {
		return temporary_memory_state->GetReservation() / number_of_threads / 10 * 8;
	}
	//! Whether the aggregate arena alone approaches the thread memory limit
	bool StatePressureExceeded(GroupedAggregateHashTable &ht) const;

public:
	ClientContext &context;
	//! Temporary memory state for managing this hash table's memory usage
	unique_ptr<TemporaryMemoryState> temporary_memory_state;
	atomic<idx_t> minimum_reservation;

	//! Whether we've called Finalize
	bool finalized;
	//! Whether we are doing an external aggregation
	atomic<bool> external;
	//! Whether the recursive runtime reset this sink for reuse across iterations
	atomic<bool> iteration_reused;
	//! Threads that have called Sink
	atomic<idx_t> active_threads;
	//! Number of threads (from TaskScheduler)
	const idx_t number_of_threads;
	//! Memory limit (from BufferManager)
	const idx_t memory_limit;
	//! Block size (from BufferManager)
	const idx_t block_alloc_size;
	//! If any thread has called combine
	atomic<bool> any_combined;
	//! If any thread has called ht.Abandon() during Sink (meaning uncombined_data may have duplicates)
	atomic<bool> any_abandoned;

	//! The radix HT
	const RadixPartitionedHashTable &radix_ht;
	//! The spill metadata of the aggregate layout, set if the states can spill
	unique_ptr<AggregateStateSpillPlan> spill_plan;
	//! Config for partitioning
	RadixHTConfig config;

	//! Uncombined partitioned data that will be put into the AggregatePartitions
	unique_ptr<PartitionedTupleData> uncombined_data;
	//! Synchronizes the transition to exported-only aggregation with concurrent combines
	SpillPhase spill_phase DUCKDB_GUARDED_BY(lock);
	//! Uncombined exported data, aligned one-to-one with the partitions of uncombined_data
	vector<unique_ptr<ColumnDataCollection>> uncombined_exported_data;
	//! Allocators used during the Sink/Finalize
	vector<shared_ptr<ArenaAllocator>> stored_allocators;
	idx_t stored_allocators_size;

	//! Partitions that are finalized during GetData
	vector<unique_ptr<AggregatePartition>> partitions;
	//! For keeping track of progress
	atomic<idx_t> finalize_done;

	//! Pin properties when scanning
	TupleDataPinProperties scan_pin_properties;
	//! Total count before combining
	idx_t count_before_combining;
	//! Maximum partition size if all unique
	idx_t max_partition_size;
};

class RadixHTLocalSinkState : public LocalSinkState {
public:
	RadixHTLocalSinkState(ClientContext &context, const RadixPartitionedHashTable &radix_ht);
	void ResetForReuse(const RadixPartitionedHashTable &radix_ht, RadixHTGlobalSinkState &gstate);
	void PrepareForSpill(RadixHTGlobalSinkState &gstate);

public:
	//! Thread-local HT that is re-used after abandoning
	unique_ptr<GroupedAggregateHashTable> ht;
	//! Chunk with group columns
	DataChunk group_chunk;

	//! Whether this local state has already registered itself as active for the current iteration
	bool registered;
	//! Whether this local table has entered spilling or state export
	bool spilling = false;
	//! Sink capacity for this thread
	idx_t local_sink_capacity;
	//! Data that is abandoned ends up here (only if we're doing external aggregation)
	unique_ptr<PartitionedTupleData> abandoned_data;
	//! Exported abandoned aggregate states, aligned one-to-one with the partitions of abandoned_data
	vector<unique_ptr<ColumnDataCollection>> abandoned_exported_data;
};

} // namespace duckdb
