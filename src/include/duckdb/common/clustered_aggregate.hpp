//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/clustered_aggregate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector_size.hpp"

namespace duckdb {

class Vector;
struct ClusteredAggrState;

using DictProps = unsafe_unique_array<int64_t>;

// log2 is constexpr only from C++26.
// Only powers of two are supported
static_assert((STANDARD_VECTOR_SIZE & (STANDARD_VECTOR_SIZE - 1)) == 0);
template <unsigned int Arg>
constexpr uint64_t c_log2() {
	if constexpr (Arg < 2) {
		return 0;
	} else {
		return 1 + c_log2<Arg / 2>();
	}
}

// Users of I64VectorSumSafe accumulate sum into int64_t. One bit is for sign,
// and there are log2(vector size) bits occupied by Vector values, so we can
// guarantee no overflow if at most 64 - 1 - log2(size) bits are set.
static constexpr uint64_t VECTOR_SIZE_LOG2 = c_log2<STANDARD_VECTOR_SIZE>();
static constexpr uint64_t SUM_OVERFLOW_MAX_BITS = 64 - 1 - VECTOR_SIZE_LOG2;
static constexpr uint64_t SUM_OVERFLOW_MASK = ~((uint64_t(1) << SUM_OVERFLOW_MAX_BITS) - 1);
static inline bool I64VectorSumSafe(int64_t v) {
	return ((static_cast<uint64_t>(v) ^ static_cast<uint64_t>(v >> 63)) & SUM_OVERFLOW_MASK) == 0;
}

//! Per-chunk tuple clustering by group. Built once and passed to aggregate kernels.
//! Clustered-aware kernels can use per-run accumulation; everyone else keeps the
//! regular scatter path over input-order addresses.
struct ClusteredAggr {
	static constexpr idx_t RUNLENGTH_THRESHOLD = 6;
	static constexpr idx_t MAX_RUNS = STANDARD_VECTOR_SIZE;
	static constexpr idx_t HOTKEYS_LOG2 = 5;
	static constexpr idx_t MAX_HOTKEYS = idx_t(1) << HOTKEYS_LOG2;
	static constexpr idx_t SAMPLE_SIZE = 128;
	static constexpr idx_t HASHTAB_LOG2 = 11;
	static constexpr idx_t HASHTAB_SZ = idx_t(1) << HASHTAB_LOG2;
	static constexpr idx_t SLOT_GRP_BITS = 13;
	static constexpr idx_t SLOT_CURSOR_BITS = (HOTKEYS_LOG2 + 1) + SLOT_GRP_BITS;
	static constexpr idx_t SLOT_GID_BITS = 64 - (SLOT_GRP_BITS + SLOT_CURSOR_BITS);
	static constexpr idx_t MAX_GID_COUNT = idx_t(1) << SLOT_GID_BITS;
	static constexpr uint64_t FREE_SLOT = ~uint64_t(0);

	struct GroupRun {
		data_ptr_t state; //! caller fills this after TryClustered; advanced between aggregates
		const sel_t *sel; //! strictly increasing input row positions, or nullptr for 0..count-1
		uint64_t gid;     //! raw group id for this run
		idx_t count;      //! number of tuples in this group
	};

	ClusteredAggr() : group_runs(&single_run) {
	}

	struct RunRange {
		const GroupRun *begin_ptr;
		const GroupRun *end_ptr;
		const GroupRun *begin() const {
			return begin_ptr;
		}
		const GroupRun *end() const {
			return end_ptr;
		}
		idx_t size() const {
			return static_cast<idx_t>(end_ptr - begin_ptr);
		}
		const GroupRun &operator[](idx_t i) const {
			return begin_ptr[i];
		}
	};
	RunRange runs() const {
		return RunRange {group_runs, group_runs + n_group_runs};
	}

	const ClusteredAggrState *state = nullptr;

	//! Initialize a single run covering 0..count-1 for one aggregate state.
	void SetSingleRun(data_ptr_t state_ptr, idx_t count);

	//! Advance all run state pointers by payload_size.
	void AdvanceStates(idx_t payload_size);

	template <class GET_STATE>
	void InitializeStates(GET_STATE &&get_state) {
		for (idx_t r = 0; r < n_group_runs; r++) {
			group_runs[r].state = get_state(group_runs[r].gid);
		}
	}

	//! Returns a composed dict sel for simple dictionary input, or nullptr.
	const sel_t *ClusterIter(const Vector &input) const;

private:
	friend struct ClusteredAggrState;

	//! Build a clustered permutation of 0..count-1 from group ids into runs.
	//! On success fills runs[].sel/gid/count.
	bool TryClustered(const uint64_t *group_ids, sel_t count, sel_t *arena, uint64_t *slots, GroupRun *runs);

	idx_t n_group_runs = 0;
	GroupRun single_run;
	GroupRun *group_runs;

	mutable const sel_t *cached_dict_sel = nullptr;
};

//! Scratch state shared by GroupedAggregateHashTable and PerfectAggregateHashTable.
struct ClusteredAggrState {
	unsafe_unique_array<sel_t> arena;
	unsafe_unique_array<uint64_t> slots;
	unsafe_unique_array<ClusteredAggr::GroupRun> group_runs;
	mutable unsafe_unique_array<sel_t> composed_sel_data;
	bool all_clustered = false;
	idx_t n_clustered = 0;
	idx_t skipped_opportunities = 0;
	idx_t retry_backoff = 1;

	mutable unordered_map<string, DictProps> dict_props;

	void Initialize();
	bool TryBuild(ClusteredAggr &clustered, const uint64_t *group_ids, idx_t count);
	optional_ptr<const DictProps> GetDictProps(const Vector &input) const;
};

} // namespace duckdb
