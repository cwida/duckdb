//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/aggregate_ht_adaptivity_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/hyperloglog.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"

namespace duckdb {

//! Observations and lookup mode owned by a grouped aggregate HT.
class AggregateHTAdaptivityState {
public:
	idx_t GetSinkCount() const {
		return sink_count;
	}
	//! Logical input since the current pointer table was cleared
	idx_t GetCycleInputCount() const {
		return sink_count - sink_count_at_abandon;
	}
	bool LookupsSkipped() const {
		return skip_lookups;
	}
	//! Logical input since lookups were skipped
	idx_t GetSkippedInputCount() const {
		return skip_lookups ? sink_count - sink_count_at_skip : 0;
	}
	bool HLLEnabled() const {
		return enable_hll;
	}
	idx_t GetHLLUpperBound() const {
		D_ASSERT(enable_hll);
		return LossyNumericCast<idx_t>((1 + HyperLogLogP<8>::GetErrorRate()) * static_cast<double>(hll.Count()));
	}

private:
	friend class GroupedAggregateHashTable;

	void AddInput(idx_t count) {
		sink_count += count;
	}
	void BeginCycle() {
		sink_count_at_abandon = sink_count;
	}
	void SkipLookups() {
		skip_lookups = true;
		sink_count_at_skip = sink_count;
	}
	void ResumeLookups() {
		skip_lookups = false;
	}
	void EnableHLL(bool enable) {
		enable_hll = enable;
	}
	void ObserveHashes(const Vector &hashes) {
		D_ASSERT(enable_hll);
		hll.Update(hashes);
	}
	void ObserveHashes(const VectorIterator<hash_t> &hashes, const SelectionVector &new_groups, idx_t count) {
		D_ASSERT(enable_hll);
		// Lookup hits were already observed when their groups were inserted.
		for (idx_t i = 0; i < count; i++) {
			hll.InsertElement(hashes[new_groups.get_index(i)].GetValue());
		}
	}
	void Reset() {
		*this = AggregateHTAdaptivityState();
	}

private:
	//! How many tuples went into this HT (before de-duplication)
	idx_t sink_count = 0;
	//! Start of the current pointer-table fill cycle
	idx_t sink_count_at_abandon = 0;
	//! If true, we just append, skipping HT lookups
	bool skip_lookups = false;
	//! Start of the current append-only interval
	idx_t sink_count_at_skip = 0;
	//! Whether to enable HLL counting the hashes
	bool enable_hll = false;
	HyperLogLogP<8> hll;
};

} // namespace duckdb
