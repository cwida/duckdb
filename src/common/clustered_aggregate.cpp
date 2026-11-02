#include "duckdb/common/clustered_aggregate.hpp"

#include "duckdb/common/common.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"

#include <cstring>

namespace duckdb {

// Why does clustering help performance so much in TPC-H q01? Yes, "in-register" sum (for example) is
// faster than load-add-store and avoids setting "bool is_set" every time - but this is the sub-plot only.
// The main reason is cache-consistency latency because multiple load-add-store patterns hit the
// same aggregate state in short succession. To illustrate that with TPC-H Q01, change SELECT
// l_returnflag,l_shipmode into SELECT CAST(l_quantity * 1.0 AS INT) .. GROUP BY ALL ORDER BY ALL.
// With the 1.0 constant there are 50 groups, but by decreasing it, one can scale flexibly down to
// fewer groups. Negative performance effects start around 25 and grow strongest at <= 5 groups.
//
// Is this only affecting aggregations with <25 groups? No, there are two more cases:
// - repeating keys: rolling up an ordered dataframe, or aggregating back to the PK of a FK-PK join.
//   e.g. change SELECT l_returnflag, l_shipmode into SELECT l_orderkey>>2, .. GROUP BY ALL
//   (orderkey repeats 4 times avg -- this is not enough, I put the threshold at 6, hence >>2)
// - (very) heavy hittng keys in a GROUP BY with potentially many groups, e.g. when 75% is 0:
//   e.g. change SELECT l_returnflag, l_shipmode into SELECT ((l_orderkey>>1)&l_orderkey&1)*l_orderkey,
//
// We also can deploy clustering on top of the dictionary-lookup optimization (mostly the few keys case)
// e.g. change SELECT l_returnflag, l_shipmode into SELECT l_shipinstruct, .. GROUP BY ALL
//
// Strategy:
// - A short dual-cursor sample inserts keys via hash_store. miss counters per cursor say whether
//   the input looks (near-)sequential.
// - Sequential -> process the rest (most) using a merge_loop (no hash lookups).
// - Otherwise -> continue dual hash_store until the total hot-key budget is exhausted, then
//   mixed_loop per cursor (hot via HT, cold via seq region).
// - At the end, memmove st2's group_run slice down next to st1's so consumers see one contiguous range.
//   (we construct separate group_run[] arrays for the two cursors but concatenate in them eventually)
//
// Eventually we declare success iff >75% of the inputs ends up in a large run (>= 6).

// Slot bitfield widths come from ClusteredAggr (header). Pull them into file scope for brevity.
constexpr idx_t SLOT_GRP_BITS = ClusteredAggr::SLOT_GRP_BITS;
constexpr idx_t SLOT_CURSOR_BITS = ClusteredAggr::SLOT_CURSOR_BITS;
constexpr idx_t SLOT_GID_BITS = ClusteredAggr::SLOT_GID_BITS;
constexpr uint64_t GID_MASK = ClusteredAggr::MAX_GID_COUNT - 1;

static_assert((1 << SLOT_GRP_BITS) >= STANDARD_VECTOR_SIZE, "Group IDs must be able to address vectorsize");

struct Slot { // really just a 64-bits integer
	union {
		struct {
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
			uint64_t cursor : SLOT_CURSOR_BITS, idx : SLOT_GRP_BITS, gid : SLOT_GID_BITS;
#else // reverse order
			uint64_t gid : SLOT_GID_BITS, idx : SLOT_GRP_BITS, cursor : SLOT_CURSOR_BITS;
#endif
		} bitfields;
		uint64_t i64; // allows to increment cursor without bit extraction logic
	} val;
	Slot() {
		val.i64 = ClusteredAggr::FREE_SLOT;
	}
	Slot(uint64_t c, uint64_t i, uint64_t g) {
		val.bitfields.cursor = c;
		val.bitfields.idx = i;
		val.bitfields.gid = g;
	}
	//! Sentinel value meaning "no slot allocated yet" (all bitfields are 1s).
	static const Slot UNINITIALIZED;
};
const Slot Slot::UNINITIALIZED {};

// Each cursor uses an independent slice of the shared arena and its own slice of group_runs[].
// Each slice has a hot region first (MAX_HOT_PER_CURSOR lists of HALF_VEC slots) followed by a
// HALF_VEC-sized seq bump region. We keep a non-restricted slice base for offset math, plus
// restricted pointers to the two disjoint write regions.
struct SlotTab {
	Slot *const tab;               // [HASHTAB_SZ] hash table
	sel_t *const slice;            // this cursor's full arena slice base
	sel_t *__restrict const arena; // hot region base inside this slice
	sel_t *__restrict seq;         // bump pointer in the disjoint sequential region
	const idx_t group_base;        // base index into group_runs[] for this cursor (0 or HALF_VEC)
	idx_t n_runs;                  // total runs created so far in this cursor's slice
	idx_t hot_n_runs;              // frozen at the end of the hot-insertion phase
};

static inline uint64_t slot_hash(uint64_t gid) {
	return ((gid * 3217161767) ^ (gid >> ClusteredAggr::HASHTAB_LOG2)) & (ClusteredAggr::HASHTAB_SZ - 1);
}

bool ClusteredAggr::TryClustered(const uint64_t *group_ids, sel_t count, sel_t *arena, uint64_t *slots,
                                 GroupRun *runs) {
	group_runs = runs;

	constexpr sel_t HALF_VEC = STANDARD_VECTOR_SIZE / 2;
	constexpr idx_t MAX_HOT_PER_CURSOR = MAX_HOTKEYS / 2;
	idx_t tuples_in_large = 0;

	// Per-cursor arena slices: each has MAX_HOT_PER_CURSOR hot regions followed by 1 seq bump
	// region, all HALF_VEC slots wide. Total per slice = (MAX_HOT_PER_CURSOR + 1) * HALF_VEC.
	constexpr idx_t SLICE_HALF_VECS = MAX_HOT_PER_CURSOR + 1;
	sel_t *const slice1 = arena;
	sel_t *const slice2 = arena + SLICE_HALF_VECS * HALF_VEC;
	SlotTab st1 {reinterpret_cast<Slot *>(slots), slice1, slice1, slice1 + MAX_HOT_PER_CURSOR * HALF_VEC, 0, 0, 0};
	SlotTab st2 {reinterpret_cast<Slot *>(slots + HASHTAB_SZ),
	             slice2,
	             slice2,
	             slice2 + MAX_HOT_PER_CURSOR * HALF_VEC,
	             HALF_VEC,
	             0,
	             0};

	auto finish_run = [&](SlotTab &st, const Slot s) -> uint64_t {
		const idx_t cnt = static_cast<idx_t>((st.slice + s.val.bitfields.cursor) - group_runs[s.val.bitfields.idx].sel);
		group_runs[s.val.bitfields.idx].count = cnt;
		if (s.val.bitfields.idx >= st.group_base + st.hot_n_runs) {
			st.seq += cnt; // sequential runs sequentially allocate from this cursor's seq region
		}
		if (cnt >= RUNLENGTH_THRESHOLD) {
			tuples_in_large += cnt;
		}
		return s.val.bitfields.cursor;
	};

	auto free_slot = [&](SlotTab &st, uint64_t hash) {
		if (st.tab[hash].val.i64 != FREE_SLOT) {
			finish_run(st, st.tab[hash]);
			st.tab[hash].val.i64 = FREE_SLOT;
		}
	};

	auto new_run = [&](SlotTab &st, uint64_t gid, uint64_t list_start) -> Slot {
		uint64_t idx = st.group_base + st.n_runs++;
		group_runs[idx].sel = st.slice + list_start;
		group_runs[idx].gid = gid;
		return Slot(list_start, idx, gid);
	};

	auto flush = [&](SlotTab &st, Slot s) {
		if (s.val.bitfields.idx < st.group_base + st.hot_n_runs) { // small groupnrs are hot keys
			st.tab[slot_hash(s.val.bitfields.gid)] = s;            // store back hot slot in HT
		} else {
			finish_run(st, s); // finish non-hot (sequential) run
		}
	};

	auto hash_store = [&](SlotTab &st, uint64_t gid, sel_t pos) -> Slot {
		uint64_t hash = slot_hash(gid) & GID_MASK;
		Slot s = st.tab[hash];
		if (DUCKDB_UNLIKELY(s.val.i64 == FREE_SLOT)) { // first insert
			s = new_run(st, gid, st.n_runs * HALF_VEC);
		} else if (DUCKDB_UNLIKELY(s.val.bitfields.gid != gid)) { // collision
			s = new_run(st, gid, finish_run(st, s));              // close old, new run continues in same slot
		}
		st.arena[s.val.bitfields.cursor] = pos;
		s.val.i64++; // val.bitfields.cursor++ without bit-extraction overhead
		return st.tab[hash] = s;
	};

	auto mixed_loop = [&](SlotTab &st, Slot cur, sel_t lo, sel_t hi) {
		for (sel_t pos = lo; pos < hi; pos++) {
			uint64_t gid = group_ids[pos] & GID_MASK;
			if (DUCKDB_UNLIKELY(cur.val.bitfields.gid != gid)) { // no sequential match?
				flush(st, cur);                                  // flush previous list
				Slot s = st.tab[slot_hash(gid)];                 // hot key lookup
				cur = (s.val.i64 != FREE_SLOT && s.val.bitfields.gid == gid)
				          ? s // hot key found
				          : new_run(st, gid, static_cast<uint64_t>(st.seq - st.slice));
			}
			st.slice[cur.val.bitfields.cursor] = pos;
			cur.val.i64++;
		}
		if (lo < hi) {
			flush(st, cur);
		}
	};

	// merge_loop: seeded from sample's last cur per cursor; no HT use. Each gid change creates a
	// new sequential run. We avoid an unconditional initial new_run so that the worst-case run
	// count fits in the per-cursor HALF_VEC budget even when every position is a transition.
	auto merge_loop = [&](SlotTab &st, Slot cur, sel_t lo, sel_t hi) {
		for (sel_t pos = lo; pos < hi; pos++) {
			uint64_t gid = group_ids[pos] & GID_MASK;
			if (DUCKDB_UNLIKELY(cur.val.bitfields.gid != gid)) { // just check sequentially
				flush(st, cur);                                  // flush previous list
				cur = new_run(st, gid, static_cast<uint64_t>(st.seq - st.slice));
			}
			st.slice[cur.val.bitfields.cursor] = pos;
			cur.val.i64++;
		}
		if (lo < hi) {
			flush(st, cur);
		}
	};

	auto free_table = [&](SlotTab &st) {
		for (idx_t i = 0; i < st.hot_n_runs; i++) {
			free_slot(st, slot_hash(group_runs[st.group_base + i].gid));
		}
	};

	const sel_t sample = SAMPLE_SIZE / 2; // dual cursor: half as many iterations as positions
	const sel_t half = count / 2;
	Slot cur1 = Slot::UNINITIALIZED;
	Slot cur2 = Slot::UNINITIALIZED;
	sel_t miss1 = 0, miss2 = 0, pos;
	for (pos = 0; pos < sample && st1.n_runs < MAX_HOT_PER_CURSOR && st2.n_runs < MAX_HOT_PER_CURSOR; pos++) {
		uint64_t gid1 = group_ids[pos] & GID_MASK;
		uint64_t gid2 = group_ids[pos + half] & GID_MASK;
		miss1 += (cur1.val.bitfields.gid != gid1);
		miss2 += (cur2.val.bitfields.gid != gid2);
		cur1 = hash_store(st1, gid1, pos);
		cur2 = hash_store(st2, gid2, pos + half);
	}
	bool fully_clustered = (miss1 + miss2 <= (st1.n_runs + st2.n_runs) + 2); // +2 because at pos==0 both miss

	if (!fully_clustered) { // use hash strategy until we find too many distinct hot keys
		for (; pos < half && st1.n_runs < MAX_HOT_PER_CURSOR && st2.n_runs < MAX_HOT_PER_CURSOR; pos++) {
			cur1 = hash_store(st1, group_ids[pos] & GID_MASK, pos);
			cur2 = hash_store(st2, group_ids[pos + half] & GID_MASK, pos + half);
		}
	}
	// All key insertions are done; new runs created beyond this point are sequential.
	st1.hot_n_runs = st1.n_runs;
	st2.hot_n_runs = st2.n_runs;

	if (fully_clustered) { // Merge strategy: No HT lookups; just detect transitions.
		merge_loop(st1, cur1, pos, half);
		merge_loop(st2, cur2, pos + half, count);
	} else { // Mixed strategy: do HT lookup to find hot keys, cold gids are just matched sequentially
		mixed_loop(st1, cur1, pos, half);
		mixed_loop(st2, cur2, pos + half, count);
	}
	free_table(st1);
	free_table(st2);

	if (st2.n_runs > 0 && st1.n_runs < HALF_VEC) { // make st2 groups consecutive with st1
		std::memmove(&group_runs[st1.n_runs], &group_runs[HALF_VEC], st2.n_runs * sizeof(GroupRun));
	}
	n_group_runs = st1.n_runs + st2.n_runs;
	cached_dict_sel = nullptr;
	return (5 * tuples_in_large >= 4 * static_cast<idx_t>(count)); // require >=80% of tuples in long-enough runs
}

void ClusteredAggr::SetSingleRun(data_ptr_t state_ptr, idx_t count) {
	D_ASSERT(group_runs == &single_run);
	n_group_runs = 1;
	group_runs[0].state = state_ptr;
	group_runs[0].sel = nullptr;
	group_runs[0].gid = 0;
	group_runs[0].count = count;
	cached_dict_sel = nullptr;
}

void ClusteredAggr::AdvanceStates(idx_t payload_size) {
	for (idx_t r = 0; r < n_group_runs; r++) {
		group_runs[r].state += payload_size;
	}
}

const sel_t *ClusteredAggr::ClusterIter(const Vector &input) const {
	if (input.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return nullptr;
	}
	auto &child = DictionaryVector::Child(input);
	if (child.GetVectorType() != VectorType::FLAT_VECTOR) {
		return nullptr;
	}
	const sel_t *dict_data = DictionaryVector::SelVector(input).data();
	if (dict_data == nullptr) {
		return nullptr;
	}
	auto run_range = runs();
	if (run_range.size() == 1 && run_range[0].sel == nullptr) {
		return dict_data;
	}
	D_ASSERT(state);
	if (cached_dict_sel == dict_data) {
		return state->composed_sel_data.get();
	}
	if (!state->composed_sel_data) {
		state->composed_sel_data = make_unsafe_uniq_array_uninitialized<sel_t>(STANDARD_VECTOR_SIZE);
	}
	auto composed_sel_data = state->composed_sel_data.get();
	idx_t pos = 0;
	for (auto &run : run_range) {
		for (idx_t k = 0; k < run.count; k++) {
			auto idx = run.sel ? run.sel[k] : k;
			composed_sel_data[pos + k] = dict_data[idx];
		}
		pos += run.count;
	}
	cached_dict_sel = dict_data;
	return composed_sel_data;
}

void ClusteredAggrState::Initialize() {
	// Two per-cursor slices: each has MAX_HOTKEYS/2 hot regions + 1 seq region of HALF_VEC each.
	// Total = 2 * (MAX_HOTKEYS/2 + 1) * HALF_VEC = (MAX_HOTKEYS/2 + 1) * STANDARD_VECTOR_SIZE.
	arena = make_unsafe_uniq_array_uninitialized<sel_t>((ClusteredAggr::MAX_HOTKEYS / 2 + 1) * STANDARD_VECTOR_SIZE);
	slots = make_unsafe_uniq_array_uninitialized<uint64_t>(2 * ClusteredAggr::HASHTAB_SZ);
	group_runs = make_unsafe_uniq_array_uninitialized<ClusteredAggr::GroupRun>(ClusteredAggr::MAX_RUNS);
	std::fill_n(slots.get(), 2 * ClusteredAggr::HASHTAB_SZ, ClusteredAggr::FREE_SLOT);
	skipped_opportunities = 0;
	retry_backoff = 1;
}

bool ClusteredAggrState::TryBuild(ClusteredAggr &clustered, const uint64_t *group_ids, idx_t count) {
	if (!arena || !group_runs) {
		return false;
	}
	if (skipped_opportunities > 0) {
		skipped_opportunities--;
		return false;
	}
	if (count >= ClusteredAggr::SAMPLE_SIZE &&
	    clustered.TryClustered(group_ids, static_cast<sel_t>(count), arena.get(), slots.get(), group_runs.get())) {
		clustered.state = this;
		skipped_opportunities = 0;
		retry_backoff = 1;
		return true;
	}
	skipped_opportunities = retry_backoff;
	retry_backoff = MinValue<idx_t>(NumericLimits<idx_t>::Maximum() / 2, retry_backoff) * 2;
	return false;
}

optional_ptr<const DictProps> ClusteredAggrState::GetDictProps(const Vector &input) const {
	if (input.GetVectorType() != VectorType::DICTIONARY_VECTOR) {
		return nullptr;
	}
	const auto &dict_id = DictionaryVector::DictionaryId(input);
	if (dict_id.empty()) {
		return nullptr;
	}
	auto it = dict_props.find(dict_id);
	if (it != dict_props.end()) {
		return &it->second;
	}

	auto &slot = dict_props[dict_id];
	const auto dict_size_opt = DictionaryVector::DictionarySize(input);
	if (!dict_size_opt.IsValid()) {
		return &slot;
	}
	const idx_t dict_size = dict_size_opt.GetIndex();
	auto &child = DictionaryVector::Child(input);
	if (child.GetVectorType() != VectorType::FLAT_VECTOR) {
		return &slot;
	}

	auto &mask = FlatVector::Validity(child);
	auto buf = make_unsafe_uniq_array_uninitialized<int64_t>(dict_size);
	auto try_copy = [&](auto src) {
		for (idx_t i = 0; i < dict_size; i++) {
			if (!mask.RowIsValid(i)) {
				buf[i] = 0;
				continue;
			}
			const int64_t v = static_cast<int64_t>(src[i]);
			if (!I64VectorSumSafe(v)) {
				return false;
			}
			buf[i] = v;
		}
		return true;
	};
	auto type = child.GetType().InternalType();
	if (type == PhysicalType::UINT32 || type == PhysicalType::INT32) {
		if (try_copy(FlatVector::GetData<int32_t>(child))) {
			slot = std::move(buf);
		}
	} else if (type == PhysicalType::UINT64 || type == PhysicalType::INT64) {
		if (try_copy(FlatVector::GetData<int64_t>(child))) {
			slot = std::move(buf);
		}
	}
	return &slot;
}

} // namespace duckdb
