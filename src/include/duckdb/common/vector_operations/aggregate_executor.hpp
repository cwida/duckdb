//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/aggregate_executor.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/bitset.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/aggregate_state.hpp"
#include <cstring>
#include <type_traits>
#include <utility>

namespace duckdb {

// structs
struct AggregateInputData;

// The bounds of a window frame
struct FrameBounds {
	FrameBounds() : start(0), end(0) {};
	FrameBounds(idx_t start, idx_t end) : start(start), end(end) {};
	idx_t start = 0;
	idx_t end = 0;
};

// A set of window subframes for windowed EXCLUDE
using SubFrames = vector<FrameBounds>;

class AggregateExecutor {
private:
#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
	template <class STATE_TYPE, class OP>
	static inline void NullaryFlatLoop(STATE_TYPE **__restrict states, AggregateInputData &aggr_input_data,
	                                   idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			OP::template Operation<STATE_TYPE, OP>(*states[i], aggr_input_data, i);
		}
	}
#endif

	template <class STATE_TYPE, class OP>
	static inline void NullaryScatterLoop(STATE_TYPE *__restrict const *__restrict const states,
	                                      AggregateInputData &aggr_input_data, const SelectionVector &ssel,
	                                      const idx_t count) {
		if (ssel.IsSet()) {
			for (idx_t i = 0; i < count; i++) {
				auto sidx = ssel.get_index_unsafe(i);
				OP::template Operation<STATE_TYPE, OP>(*states[sidx], aggr_input_data, sidx);
			}
		} else {
			for (idx_t i = 0; i < count; i++) {
				OP::template Operation<STATE_TYPE, OP>(*states[i], aggr_input_data, i);
			}
		}
	}

#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnaryFlatLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                 STATE_TYPE **__restrict states, const ValidityMask &mask, idx_t count) {
		if (OP::IgnoreNull() && mask.CanHaveNull()) {
			AggregateUnaryInput input(aggr_input_data, mask);
			auto &base_idx = input.input_idx;
			base_idx = 0;
			auto entry_count = ValidityMask::EntryCount(count);
			for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
				auto validity_entry = mask.GetValidityEntry(entry_idx);
				idx_t next = MinValue<idx_t>(base_idx + ValidityMask::BITS_PER_VALUE, count);
				if (ValidityMask::AllValid(validity_entry)) {
					// all valid: perform operation
					for (; base_idx < next; base_idx++) {
						OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*states[base_idx], idata[base_idx], input);
					}
				} else if (ValidityMask::NoneValid(validity_entry)) {
					// nothing valid: skip all
					base_idx = next;
					continue;
				} else {
					// partially valid: need to check individual elements for validity
					idx_t start = base_idx;
					for (; base_idx < next; base_idx++) {
						if (ValidityMask::RowIsValid(validity_entry, base_idx - start)) {
							OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*states[base_idx], idata[base_idx],
							                                                   input);
						}
					}
				}
			}
		} else {
			AggregateUnaryInput input(aggr_input_data, mask);
			auto &i = input.input_idx;
			for (i = 0; i < count; i++) {
				OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*states[i], idata[i], input);
			}
		}
	}
#endif

#if !DUCKDB_SMALLER_BINARY(aggregate_executor_sel_flags)
	template <class STATE_TYPE, class INPUT_TYPE, class OP, bool HAS_ISEL, bool HAS_SSEL>
#else
	template <class STATE_TYPE, class INPUT_TYPE, class OP>
#endif
	static inline void UnaryScatterLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                    STATE_TYPE **__restrict states, const SelectionVector &isel,
	                                    const SelectionVector &ssel, const ValidityMask &mask, idx_t count) {
#if DUCKDB_SMALLER_BINARY(aggregate_executor_sel_flags)
		const auto HAS_ISEL = isel.IsSet();
		const auto HAS_SSEL = ssel.IsSet();
#endif
		if (OP::IgnoreNull() && mask.CanHaveNull()) {
			// potential NULL values and NULL values are ignored
			AggregateUnaryInput input(aggr_input_data, mask);
			for (idx_t i = 0; i < count; i++) {
				input.input_idx = HAS_ISEL ? isel.get_index_unsafe(i) : i;
				auto sidx = HAS_SSEL ? ssel.get_index_unsafe(i) : i;
				if (mask.RowIsValidUnsafe(input.input_idx)) {
					OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*states[sidx], idata[input.input_idx], input);
				}
			}
		} else {
			// quick path: no NULL values or NULL values are not ignored
			AggregateUnaryInput input(aggr_input_data, mask);
			for (idx_t i = 0; i < count; i++) {
				input.input_idx = HAS_ISEL ? isel.get_index_unsafe(i) : i;
				auto sidx = HAS_SSEL ? ssel.get_index_unsafe(i) : i;
				OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*states[sidx], idata[input.input_idx], input);
			}
		}
	}

#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnaryFlatUpdateLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                       STATE_TYPE *__restrict state, idx_t count, const ValidityMask &mask) {
		AggregateUnaryInput input(aggr_input_data, mask);
		auto &base_idx = input.input_idx;
		base_idx = 0;
		auto entry_count = ValidityMask::EntryCount(count);
		for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
			auto validity_entry = mask.GetValidityEntry(entry_idx);
			idx_t next = MinValue<idx_t>(base_idx + ValidityMask::BITS_PER_VALUE, count);
			if (!OP::IgnoreNull() || ValidityMask::AllValid(validity_entry)) {
				// all valid: perform operation
				for (; base_idx < next; base_idx++) {
					OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[base_idx], input);
				}
			} else if (ValidityMask::NoneValid(validity_entry)) {
				// nothing valid: skip all
				base_idx = next;
				continue;
			} else {
				// partially valid: need to check individual elements for validity
				idx_t start = base_idx;
				for (; base_idx < next; base_idx++) {
					if (ValidityMask::RowIsValid(validity_entry, base_idx - start)) {
						OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[base_idx], input);
					}
				}
			}
		}
	}
#endif

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnaryUpdateLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                   STATE_TYPE *__restrict state, idx_t count, const ValidityMask &mask,
	                                   const SelectionVector &__restrict sel_vector) {
		AggregateUnaryInput input(aggr_input_data, mask);
		if (OP::IgnoreNull() && mask.CanHaveNull()) {
			// potential NULL values and NULL values are ignored
			for (idx_t i = 0; i < count; i++) {
				input.input_idx = sel_vector.get_index(i);
				if (mask.RowIsValid(input.input_idx)) {
					OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[input.input_idx], input);
				}
			}
		} else {
			// quick path: no NULL values or NULL values are not ignored
			for (idx_t i = 0; i < count; i++) {
				input.input_idx = sel_vector.get_index(i);
				OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[input.input_idx], input);
			}
		}
	}

	template <bool CHECK_VALIDITY, class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnarySelectedUpdateLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                           STATE_TYPE *__restrict state, idx_t count, const ValidityMask &mask,
	                                           const sel_t *__restrict sel) {
		AggregateUnaryInput input(aggr_input_data, mask);
		for (idx_t i = 0; i < count; i++) {
			input.input_idx = sel[i];
			if (!CHECK_VALIDITY || mask.RowIsValidUnsafe(input.input_idx)) {
				OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[input.input_idx], input);
			}
		}
	}

	template <bool CHECK_VALIDITY, class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnarySelectedUpdateLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                           STATE_TYPE *__restrict state, idx_t count, const ValidityMask &mask,
	                                           const SelectionVector &isel, const sel_t *__restrict sel) {
		AggregateUnaryInput input(aggr_input_data, mask);
		for (idx_t i = 0; i < count; i++) {
			input.input_idx = isel.get_index(sel[i]);
			if (!CHECK_VALIDITY || mask.RowIsValidUnsafe(input.input_idx)) {
				OP::template Operation<INPUT_TYPE, STATE_TYPE, OP>(*state, idata[input.input_idx], input);
			}
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void UnaryUpdateLoop(const INPUT_TYPE *__restrict idata, AggregateInputData &aggr_input_data,
	                                   STATE_TYPE *__restrict state, idx_t count, const ValidityMask &mask,
	                                   const sel_t *__restrict sel) {
		if (OP::IgnoreNull() && mask.CanHaveNull()) {
			UnarySelectedUpdateLoop<true, STATE_TYPE, INPUT_TYPE, OP>(idata, aggr_input_data, state, count, mask, sel);
		} else {
			UnarySelectedUpdateLoop<false, STATE_TYPE, INPUT_TYPE, OP>(idata, aggr_input_data, state, count, mask, sel);
		}
	}

#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
	template <class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static inline void BinaryFlatLoop(const A_TYPE *__restrict adata, AggregateInputData &aggr_input_data,
	                                  const B_TYPE *__restrict bdata, STATE_TYPE **__restrict states, idx_t count,
	                                  const ValidityMask &avalidity, const ValidityMask &bvalidity) {
		AggregateBinaryInput input(aggr_input_data, avalidity, bvalidity);
		if (OP::IgnoreNull() && (avalidity.CanHaveNull() || bvalidity.CanHaveNull())) {
			const auto entry_count = ValidityMask::EntryCount(count);
			for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
				auto validity_entry = avalidity.GetValidityEntry(entry_idx) & bvalidity.GetValidityEntry(entry_idx);
				const auto start = entry_idx * ValidityMask::BITS_PER_VALUE;
				const auto next = MinValue<idx_t>(start + ValidityMask::BITS_PER_VALUE, count);
				if (ValidityMask::AllValid(validity_entry)) {
					for (idx_t i = start; i < next; i++) {
						input.lidx = input.ridx = i;
						OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*states[i], adata[i], bdata[i], input);
					}
				} else {
					validity_entry &= ValidityMask::EntryWithValidBits(next - start);
					while (validity_entry) {
						const auto i = start + CountZeros<validity_t>::Trailing(validity_entry);
						input.lidx = input.ridx = i;
						OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*states[i], adata[i], bdata[i], input);
						validity_entry &= validity_entry - 1;
					}
				}
			}
		} else {
			for (idx_t i = 0; i < count; i++) {
				input.lidx = input.ridx = i;
				OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*states[i], adata[i], bdata[i], input);
			}
		}
	}
#endif

	template <class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static inline void BinaryScatterLoop(const A_TYPE *__restrict adata, AggregateInputData &aggr_input_data,
	                                     const B_TYPE *__restrict bdata, STATE_TYPE **__restrict states, idx_t count,
	                                     const SelectionVector &asel, const SelectionVector &bsel,
	                                     const SelectionVector &ssel, const ValidityMask &avalidity,
	                                     const ValidityMask &bvalidity) {
		AggregateBinaryInput input(aggr_input_data, avalidity, bvalidity);
		if (OP::IgnoreNull() && (avalidity.CanHaveNull() || bvalidity.CanHaveNull())) {
			// potential NULL values and NULL values are ignored
			for (idx_t i = 0; i < count; i++) {
				input.lidx = asel.get_index(i);
				input.ridx = bsel.get_index(i);
				auto sidx = ssel.get_index(i);
				if (avalidity.RowIsValid(input.lidx) && bvalidity.RowIsValid(input.ridx)) {
					OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*states[sidx], adata[input.lidx],
					                                                       bdata[input.ridx], input);
				}
			}
		} else {
			// quick path: no NULL values or NULL values are not ignored
			for (idx_t i = 0; i < count; i++) {
				input.lidx = asel.get_index(i);
				input.ridx = bsel.get_index(i);
				auto sidx = ssel.get_index(i);
				OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*states[sidx], adata[input.lidx],
				                                                       bdata[input.ridx], input);
			}
		}
	}

	template <class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static inline void BinaryUpdateLoop(const A_TYPE *__restrict adata, AggregateInputData &aggr_input_data,
	                                    const B_TYPE *__restrict bdata, STATE_TYPE *__restrict state, idx_t count,
	                                    const SelectionVector &asel, const SelectionVector &bsel,
	                                    const ValidityMask &avalidity, const ValidityMask &bvalidity) {
		AggregateBinaryInput input(aggr_input_data, avalidity, bvalidity);
		if (OP::IgnoreNull() && (avalidity.CanHaveNull() || bvalidity.CanHaveNull())) {
			// potential NULL values and NULL values are ignored
			for (idx_t i = 0; i < count; i++) {
				input.lidx = asel.get_index(i);
				input.ridx = bsel.get_index(i);
				if (avalidity.RowIsValid(input.lidx) && bvalidity.RowIsValid(input.ridx)) {
					OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*state, adata[input.lidx], bdata[input.ridx],
					                                                       input);
				}
			}
		} else {
			// quick path: no NULL values or NULL values are not ignored
			for (idx_t i = 0; i < count; i++) {
				input.lidx = asel.get_index(i);
				input.ridx = bsel.get_index(i);
				OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*state, adata[input.lidx], bdata[input.ridx],
				                                                       input);
			}
		}
	}

	template <bool CHECK_VALIDITY, class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static inline void BinarySelectedUpdateLoop(const A_TYPE *__restrict adata, AggregateInputData &aggr_input_data,
	                                            const B_TYPE *__restrict bdata, STATE_TYPE *__restrict state,
	                                            idx_t count, const SelectionVector &asel, const SelectionVector &bsel,
	                                            ValidityMask &avalidity, ValidityMask &bvalidity,
	                                            const sel_t *__restrict sel) {
		AggregateBinaryInput input(aggr_input_data, avalidity, bvalidity);
		for (idx_t i = 0; i < count; i++) {
			const auto idx = sel[i];
			input.lidx = asel.get_index(idx);
			input.ridx = bsel.get_index(idx);
			if (!CHECK_VALIDITY || (avalidity.RowIsValidUnsafe(input.lidx) && bvalidity.RowIsValidUnsafe(input.ridx))) {
				OP::template Operation<A_TYPE, B_TYPE, STATE_TYPE, OP>(*state, adata[input.lidx], bdata[input.ridx],
				                                                       input);
			}
		}
	}

public:
	template <class STATE_TYPE, class OP>
	static void NullaryScatter(Vector &states, AggregateInputData &aggr_input_data, idx_t count) {
		// COUNT(*) can add run lengths directly.
		if (aggr_input_data.clustered) {
			auto &cs = *aggr_input_data.clustered;
			for (auto &run : cs.runs()) {
				OP::template ConstantOperation<STATE_TYPE, OP>(*reinterpret_cast<STATE_TYPE *>(run.state),
				                                               aggr_input_data, run.count);
			}
			return;
		}
		if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			auto sdata = ConstantVector::GetData<STATE_TYPE *>(states);
			OP::template ConstantOperation<STATE_TYPE, OP>(**sdata, aggr_input_data, count);
#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
		} else if (states.GetVectorType() == VectorType::FLAT_VECTOR) {
			auto sdata = FlatVector::GetDataMutable<STATE_TYPE *>(states);
			NullaryFlatLoop<STATE_TYPE, OP>(sdata, aggr_input_data, count);
#endif
		} else {
			UnifiedVectorFormat sdata;
			states.ToUnifiedFormat(sdata);
			NullaryScatterLoop<STATE_TYPE, OP>((STATE_TYPE **)sdata.data, aggr_input_data, *sdata.sel, count);
		}
	}

	template <class STATE_TYPE, class OP>
	static void NullaryClustUpdate(AggregateInputData &aggr_input_data, const ClusteredAggr &clustered, idx_t count) {
		for (auto &run : clustered.runs()) {
			OP::template ConstantOperation<STATE_TYPE, OP>(*reinterpret_cast<STATE_TYPE *>(run.state), aggr_input_data,
			                                               run.count);
		}
	}

	template <class STATE_TYPE, class OP>
	static void NullaryUpdate(data_ptr_t state, AggregateInputData &aggr_input_data, idx_t count) {
		OP::template ConstantOperation<STATE_TYPE, OP>(*reinterpret_cast<STATE_TYPE *>(state), aggr_input_data, count);
	}

	template <class...>
	using void_t_helper = void;

	template <class OP, class STATE, class = void>
	struct HasClusteredLocalState : std::false_type {};
	template <class OP, class STATE>
	struct HasClusteredLocalState<OP, STATE, void_t_helper<typename OP::template ClusteredLocalState<STATE>::Type>>
	    : std::true_type {};
	template <class OP, class STATE>
	using clustered_local_state_t = typename OP::template ClusteredLocalState<STATE>::Type;

	template <class OP, class INPUT, class LOCAL, class = void>
	struct HasClusteredLocalRepeatCount : std::false_type {};
	template <class OP, class INPUT, class LOCAL>
	struct HasClusteredLocalRepeatCount<
	    OP, INPUT, LOCAL,
	    void_t_helper<decltype(OP::template UpdateClusteredLocal<INPUT, LOCAL>(
	        std::declval<LOCAL &>(), std::declval<const INPUT &>(), std::declval<idx_t>()))>> : std::true_type {};

	template <bool CHECK_VALIDITY, class LOCAL_TYPE, class INPUT_TYPE, class OP>
	static inline bool UpdateUnaryClusteredOpt(const INPUT_TYPE *__restrict vals, LOCAL_TYPE &local, idx_t count,
	                                           const ValidityMask &mask, const sel_t *sel = nullptr,
	                                           const SelectionVector *isel = nullptr) {
		bool saw_value = !CHECK_VALIDITY && count != 0;
		if (isel) {
			if (sel) {
				for (idx_t i = 0; i < count; i++) {
					auto idx = isel->get_index(sel[i]);
					if constexpr (CHECK_VALIDITY) {
						if (!mask.RowIsValidUnsafe(idx)) {
							continue;
						}
						saw_value = true;
					}
					OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[idx]);
				}
			} else {
				for (idx_t i = 0; i < count; i++) {
					auto idx = isel->get_index(i);
					if constexpr (CHECK_VALIDITY) {
						if (!mask.RowIsValidUnsafe(idx)) {
							continue;
						}
						saw_value = true;
					}
					OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[idx]);
				}
			}
		} else if (sel) {
			for (idx_t i = 0; i < count; i++) {
				auto idx = sel[i];
				if constexpr (CHECK_VALIDITY) {
					if (!mask.RowIsValidUnsafe(idx)) {
						continue;
					}
					saw_value = true;
				}
				OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[idx]);
			}
		} else {
			for (idx_t i = 0; i < count; i++) {
				if constexpr (CHECK_VALIDITY) {
					if (!mask.RowIsValidUnsafe(i)) {
						continue;
					}
					saw_value = true;
				}
				OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[i]);
			}
		}
		return saw_value;
	}

	template <class LOCAL_TYPE, class INPUT_TYPE, class OP>
	static inline bool UpdateUnaryClusteredDispatch(const INPUT_TYPE *__restrict vals, LOCAL_TYPE &local, idx_t count,
	                                                const ValidityMask &mask, const sel_t *sel = nullptr,
	                                                const SelectionVector *isel = nullptr) {
		if (OP::IgnoreNull() && mask.CanHaveNull()) {
			return UpdateUnaryClusteredOpt<true, LOCAL_TYPE, INPUT_TYPE, OP>(vals, local, count, mask, sel, isel);
		}
		return UpdateUnaryClusteredOpt<false, LOCAL_TYPE, INPUT_TYPE, OP>(vals, local, count, mask, sel, isel);
	}

	template <class LOCAL_TYPE, class INPUT_TYPE, class OP>
	static inline void UpdateUnaryClusteredLocalRepeat(LOCAL_TYPE &local, const INPUT_TYPE &input, idx_t count,
	                                                   std::true_type) {
		OP::template UpdateClusteredLocal<INPUT_TYPE, LOCAL_TYPE>(local, input, count);
	}

	template <class LOCAL_TYPE, class INPUT_TYPE, class OP>
	static inline void UpdateUnaryClusteredLocalRepeat(LOCAL_TYPE &local, const INPUT_TYPE &input, idx_t count,
	                                                   std::false_type) {
		for (idx_t i = 0; i < count; i++) {
			OP::template UpdateClusteredLocal<INPUT_TYPE>(local, input);
		}
	}

	template <class LOCAL_TYPE, class INPUT_TYPE, class OP>
	static inline bool UpdateUnaryClusteredRange(const INPUT_TYPE *__restrict vals, LOCAL_TYPE &local, idx_t start,
	                                             idx_t count, const ValidityMask &mask) {
		bool saw_value = false;
		const auto end = start + count;
		while (start < end) {
			const auto offset = start % ValidityMask::BITS_PER_VALUE;
			const auto n = MinValue<idx_t>(end - start, ValidityMask::BITS_PER_VALUE - offset);
			const auto range_mask = ValidityMask::EntryWithValidBits(n);
			auto bits = (mask.GetValidityEntry(start / ValidityMask::BITS_PER_VALUE) >> offset) & range_mask;
			if (bits == range_mask) {
				saw_value = true;
				for (idx_t i = start; i < start + n; i++) {
					OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[i]);
				}
			} else {
				while (bits) {
					const auto idx = start + CountZeros<validity_t>::Trailing(bits);
					OP::template UpdateClusteredLocal<INPUT_TYPE>(local, vals[idx]);
					bits &= bits - 1;
					saw_value = true;
				}
			}
			start += n;
		}
		return saw_value;
	}

	template <bool CHECK_VALIDITY, class STATE_TYPE, class INPUT_TYPE, class OP, bool SPARSE = false>
	static void ExecuteUnaryClusteredOpt(const INPUT_TYPE *vals, const ClusteredAggr &clustered,
	                                     const ValidityMask &validity, const SelectionVector *isel = nullptr,
	                                     const sel_t *cluster_iter = nullptr) {
		idx_t pos = 0;
		using local_type = clustered_local_state_t<OP, STATE_TYPE>;
		if (cluster_iter) {
			for (auto &run : clustered.runs()) {
				auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
				local_type local;
				OP::template InitializeClusteredLocal<STATE_TYPE>(local, state);
				auto run_count = run.count;
				auto saw_value = UpdateUnaryClusteredOpt<CHECK_VALIDITY, local_type, INPUT_TYPE, OP>(
				    vals, local, run_count, validity, cluster_iter + pos);
				OP::template FlushClusteredLocal<STATE_TYPE>(state, local, saw_value);
				pos += run_count;
			}
		} else if (isel) {
			for (auto &run : clustered.runs()) {
				auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
				local_type local;
				OP::template InitializeClusteredLocal<STATE_TYPE>(local, state);
				auto run_count = run.count;
				auto saw_value = UpdateUnaryClusteredOpt<CHECK_VALIDITY, local_type, INPUT_TYPE, OP>(
				    vals, local, run_count, validity, run.sel, isel);
				OP::template FlushClusteredLocal<STATE_TYPE>(state, local, saw_value);
			}
		} else {
			for (auto &run : clustered.runs()) {
				auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
				local_type local;
				OP::template InitializeClusteredLocal<STATE_TYPE>(local, state);
				auto run_count = run.count;
				if constexpr (SPARSE) {
					if (run_count && (!run.sel || run.sel[run_count - 1] - run.sel[0] == run_count - 1)) {
						const auto start = run.sel ? run.sel[0] : 0;
						auto saw_value = UpdateUnaryClusteredRange<local_type, INPUT_TYPE, OP>(vals, local, start,
						                                                                       run_count, validity);
						OP::template FlushClusteredLocal<STATE_TYPE>(state, local, saw_value);
						continue;
					}
				}
				auto saw_value = UpdateUnaryClusteredOpt<CHECK_VALIDITY, local_type, INPUT_TYPE, OP>(
				    vals, local, run_count, validity, run.sel);
				OP::template FlushClusteredLocal<STATE_TYPE>(state, local, saw_value);
			}
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static inline void
	ExecuteUnaryClusteredDispatch(const INPUT_TYPE *vals, const ClusteredAggr &clustered, const ValidityMask &validity,
	                              const SelectionVector *isel = nullptr, const sel_t *cluster_iter = nullptr) {
		if constexpr (HasI64HugeintSumFastPath<INPUT_TYPE, OP>::value) {
			if (!validity.CanHaveNull()) {
				OP::template ExecuteFlatI64HugeintSum<STATE_TYPE, INPUT_TYPE>(vals, clustered, isel, cluster_iter);
				return;
			}
		}
		if (OP::IgnoreNull() && validity.CanHaveNull()) {
			ExecuteUnaryClusteredOpt<true, STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, validity, isel, cluster_iter);
		} else {
			ExecuteUnaryClusteredOpt<false, STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, validity, isel, cluster_iter);
		}
	}

	static inline bool IsDenseSingleRun(const ClusteredAggr &clustered, idx_t count) {
		auto runs = clustered.runs();
		return runs.size() == 1 && runs[0].count == count && runs[0].sel == nullptr;
	}

	template <bool SIMPLE_DICT, class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClusteredDictOpt(Vector &input, const ClusteredAggr &clustered, idx_t count,
	                                         const sel_t *cluster_iter = nullptr) {
		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);
		auto vals = UnifiedVectorFormat::GetData<INPUT_TYPE>(idata);
		if constexpr (SIMPLE_DICT) {
			ExecuteUnaryClusteredDispatch<STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, idata.validity, nullptr,
			                                                          cluster_iter);
		} else {
			ExecuteUnaryClusteredDispatch<STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, idata.validity, idata.sel);
		}
	}

	static inline bool IsSparseClusteredInput(const ValidityMask &mask, idx_t count) {
		const auto max_valid = count / 4;
		idx_t valid_count = 0;
		const auto full_entries = count / ValidityMask::BITS_PER_VALUE;
		for (idx_t entry_idx = 0; entry_idx < full_entries; entry_idx++) {
			valid_count += bitset<ValidityMask::BITS_PER_VALUE>(mask.GetValidityEntry(entry_idx)).count();
			if (valid_count > max_valid) {
				return false;
			}
		}
		const auto tail_count = count % ValidityMask::BITS_PER_VALUE;
		if (tail_count) {
			const auto entry = mask.GetValidityEntry(full_entries) & ValidityMask::EntryWithValidBits(tail_count);
			valid_count += bitset<ValidityMask::BITS_PER_VALUE>(entry).count();
		}
		return valid_count <= max_valid;
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClusteredOpt(Vector &input, const ClusteredAggr &clustered, idx_t count) {
		auto vals = FlatVector::GetData<INPUT_TYPE>(input);
		auto &validity = FlatVector::Validity(input);
		if (IsDenseSingleRun(clustered, count)) {
			auto runs = clustered.runs();
			auto &state = *reinterpret_cast<STATE_TYPE *>(runs[0].state);
			using local_type = clustered_local_state_t<OP, STATE_TYPE>;
			local_type local;
			OP::template InitializeClusteredLocal<STATE_TYPE>(local, state);
			auto saw_value = UpdateUnaryClusteredDispatch<local_type, INPUT_TYPE, OP>(vals, local, count, validity);
			OP::template FlushClusteredLocal<STATE_TYPE>(state, local, saw_value);
			return;
		}
		if (OP::IgnoreNull() && validity.CanHaveNull() && IsSparseClusteredInput(validity, count)) {
			ExecuteUnaryClusteredOpt<true, STATE_TYPE, INPUT_TYPE, OP, true>(vals, clustered, validity);
			return;
		}
		ExecuteUnaryClusteredDispatch<STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, validity);
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClusteredConstantOpt(Vector &input, const ClusteredAggr &clustered) {
		using local_type = clustered_local_state_t<OP, STATE_TYPE>;
		if (OP::IgnoreNull() && ConstantVector::IsNull(input)) {
			return;
		}
		const auto &constant_input = *ConstantVector::GetData<INPUT_TYPE>(input);
		using has_repeat_count = HasClusteredLocalRepeatCount<OP, INPUT_TYPE, local_type>;
		for (auto &run : clustered.runs()) {
			auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
			local_type local;
			OP::template InitializeClusteredLocal<STATE_TYPE>(local, state);
			auto run_count = run.count;
			if (run_count != 0) {
				UpdateUnaryClusteredLocalRepeat<local_type, INPUT_TYPE, OP>(local, constant_input, run_count,
				                                                            has_repeat_count {});
				OP::template FlushClusteredLocal<STATE_TYPE>(state, local, true);
			} else {
				OP::template FlushClusteredLocal<STATE_TYPE>(state, local, false);
			}
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClusteredUnifiedOpt(Vector &input, const ClusteredAggr &clustered, idx_t count) {
		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);
		auto vals = UnifiedVectorFormat::GetData<INPUT_TYPE>(idata);
		ExecuteUnaryClusteredDispatch<STATE_TYPE, INPUT_TYPE, OP>(vals, clustered, idata.validity, idata.sel);
	}

	// true if OP defines ClusteredOp (early-exit optimization for first/last/any_value).
	template <class OP, class INPUT_TYPE, class STATE_TYPE, class = void>
	struct HasClusteredOperation : std::false_type {};
	template <class OP, class INPUT_TYPE, class STATE_TYPE>
	struct HasClusteredOperation<OP, INPUT_TYPE, STATE_TYPE,
	                             void_t_helper<decltype(&OP::template ClusteredOp<INPUT_TYPE, STATE_TYPE, OP>)>>
	    : std::true_type {};

	template <class INPUT_TYPE, class OP, class = void>
	struct HasI64HugeintSumFastPath : std::false_type {};
	template <class INPUT_TYPE, class OP>
	struct HasI64HugeintSumFastPath<INPUT_TYPE, OP, void_t_helper<decltype(OP::ClusteredI64HugeintSum)>>
	    : std::integral_constant<bool, std::is_integral<INPUT_TYPE>::value && OP::ClusteredI64HugeintSum> {};

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClustConstantCustomState(Vector &input, AggregateInputData &aggr_input_data,
	                                                 const ClusteredAggr &clustered) {
		if (OP::IgnoreNull() && ConstantVector::IsNull(input)) {
			return;
		}
		auto idata = ConstantVector::GetData<INPUT_TYPE>(input);
		AggregateUnaryInput unary_input(aggr_input_data, ConstantVector::Validity(input));
		for (auto &run : clustered.runs()) {
			auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
			OP::template ConstantOperation<INPUT_TYPE, STATE_TYPE, OP>(state, *idata, unary_input, run.count);
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryNoClusteredOpt(Vector &input, AggregateInputData &aggr_input_data,
	                                       const ClusteredAggr &clustered, idx_t count) {
		static_assert(HasClusteredOperation<OP, INPUT_TYPE, STATE_TYPE>::value, "Expected custom clustered operation");
		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			ExecuteUnaryClustConstantCustomState<STATE_TYPE, INPUT_TYPE, OP>(input, aggr_input_data, clustered);
			return;
		}
		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);
		auto vals = UnifiedVectorFormat::GetData<INPUT_TYPE>(idata);
		AggregateUnaryInput unary_input(aggr_input_data, idata.validity);
		for (auto &run : clustered.runs()) {
			auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
			OP::template ClusteredOp<INPUT_TYPE, STATE_TYPE, OP>(state, vals, unary_input, run.sel, *idata.sel,
			                                                     idata.validity, 0, run.count);
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void ExecuteUnaryClustered(Vector &input, AggregateInputData &aggr_input_data,
	                                  const ClusteredAggr &clustered, idx_t count) {
		if constexpr (HasClusteredLocalState<OP, STATE_TYPE>::value) {
			switch (input.GetVectorType()) {
			case VectorType::FLAT_VECTOR:
				ExecuteUnaryClusteredOpt<STATE_TYPE, INPUT_TYPE, OP>(input, clustered, count);
				return;
			case VectorType::CONSTANT_VECTOR:
				ExecuteUnaryClusteredConstantOpt<STATE_TYPE, INPUT_TYPE, OP>(input, clustered);
				return;
			case VectorType::DICTIONARY_VECTOR: {
				if constexpr (HasI64HugeintSumFastPath<INPUT_TYPE, OP>::value) {
					if (clustered.state) {
						auto props = clustered.state->GetDictProps(input);
						if (props && *props) {
							OP::template ExecuteDictI64HugeintSum<STATE_TYPE>(input, clustered, props->get());
							return;
						}
					}
				}
				auto *cluster_iter = clustered.ClusterIter(input);
				if (cluster_iter) {
					ExecuteUnaryClusteredDictOpt<true, STATE_TYPE, INPUT_TYPE, OP>(input, clustered, count,
					                                                               cluster_iter);
				} else {
					ExecuteUnaryClusteredDictOpt<false, STATE_TYPE, INPUT_TYPE, OP>(input, clustered, count);
				}
				return;
			}
			default:
				ExecuteUnaryClusteredUnifiedOpt<STATE_TYPE, INPUT_TYPE, OP>(input, clustered, count);
				return;
			}
		}
		if constexpr (HasClusteredOperation<OP, INPUT_TYPE, STATE_TYPE>::value) {
			ExecuteUnaryNoClusteredOpt<STATE_TYPE, INPUT_TYPE, OP>(input, aggr_input_data, clustered, count);
		} else {
			D_ASSERT(false);
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void UnaryScatter(Vector &input, Vector &states, AggregateInputData &aggr_input_data, idx_t count) {
		if (aggr_input_data.clustered) {
			ExecuteUnaryClustered<STATE_TYPE, INPUT_TYPE, OP>(input, aggr_input_data, *aggr_input_data.clustered,
			                                                  count);
			return;
		}
		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR &&
		    states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (OP::IgnoreNull() && ConstantVector::IsNull(input)) {
				// constant NULL input in function that ignores NULL values
				return;
			}
			// regular constant: get first state
			auto idata = ConstantVector::GetData<INPUT_TYPE>(input);
			auto sdata = ConstantVector::GetData<STATE_TYPE *>(states);
			AggregateUnaryInput input_data(aggr_input_data, ConstantVector::Validity(input));
			OP::template ConstantOperation<INPUT_TYPE, STATE_TYPE, OP>(**sdata, *idata, input_data, count);
#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
		} else if (input.GetVectorType() == VectorType::FLAT_VECTOR &&
		           states.GetVectorType() == VectorType::FLAT_VECTOR) {
			auto idata = FlatVector::GetData<INPUT_TYPE>(input);
			auto sdata = FlatVector::GetDataMutable<STATE_TYPE *>(states);
			UnaryFlatLoop<STATE_TYPE, INPUT_TYPE, OP>(idata, aggr_input_data, sdata, FlatVector::ValidityMutable(input),
			                                          count);
#endif
		} else {
			UnifiedVectorFormat idata, sdata;
			input.ToUnifiedFormat(idata);
			states.ToUnifiedFormat(sdata);
#if DUCKDB_SMALLER_BINARY(aggregate_executor_sel_flags)
			UnaryScatterLoop<STATE_TYPE, INPUT_TYPE, OP>(UnifiedVectorFormat::GetData<INPUT_TYPE>(idata),
			                                             aggr_input_data, (STATE_TYPE **)sdata.data, *idata.sel,
			                                             *sdata.sel, idata.validity, count);
#else
			if (idata.sel->IsSet()) {
				if (sdata.sel->IsSet()) {
					UnaryScatterLoop<STATE_TYPE, INPUT_TYPE, OP, true, true>(
					    UnifiedVectorFormat::GetData<INPUT_TYPE>(idata), aggr_input_data, (STATE_TYPE **)sdata.data,
					    *idata.sel, *sdata.sel, idata.validity, count);
				} else {
					UnaryScatterLoop<STATE_TYPE, INPUT_TYPE, OP, true, false>(
					    UnifiedVectorFormat::GetData<INPUT_TYPE>(idata), aggr_input_data, (STATE_TYPE **)sdata.data,
					    *idata.sel, *sdata.sel, idata.validity, count);
				}
			} else {
				if (sdata.sel->IsSet()) {
					UnaryScatterLoop<STATE_TYPE, INPUT_TYPE, OP, false, true>(
					    UnifiedVectorFormat::GetData<INPUT_TYPE>(idata), aggr_input_data, (STATE_TYPE **)sdata.data,
					    *idata.sel, *sdata.sel, idata.validity, count);
				} else {
					UnaryScatterLoop<STATE_TYPE, INPUT_TYPE, OP, false, false>(
					    UnifiedVectorFormat::GetData<INPUT_TYPE>(idata), aggr_input_data, (STATE_TYPE **)sdata.data,
					    *idata.sel, *sdata.sel, idata.validity, count);
				}
			}

#endif
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE, class OP>
	static void UnaryUpdate(Vector &input, AggregateInputData &aggr_input_data, data_ptr_t state, idx_t count) {
		switch (input.GetVectorType()) {
		case VectorType::CONSTANT_VECTOR: {
			if (OP::IgnoreNull() && ConstantVector::IsNull(input)) {
				return;
			}
			auto idata = ConstantVector::GetData<INPUT_TYPE>(input);
			AggregateUnaryInput input_data(aggr_input_data, ConstantVector::Validity(input));
			OP::template ConstantOperation<INPUT_TYPE, STATE_TYPE, OP>(*reinterpret_cast<STATE_TYPE *>(state), *idata,
			                                                           input_data, count);
			break;
		}
#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
		case VectorType::FLAT_VECTOR: {
			auto idata = FlatVector::GetData<INPUT_TYPE>(input);
			UnaryFlatUpdateLoop<STATE_TYPE, INPUT_TYPE, OP>(idata, aggr_input_data, (STATE_TYPE *)state, count,
			                                                FlatVector::ValidityMutable(input));
			break;
		}
#endif
		default: {
			UnifiedVectorFormat idata;
			input.ToUnifiedFormat(idata);
			UnaryUpdateLoop<STATE_TYPE, INPUT_TYPE, OP>(UnifiedVectorFormat::GetData<INPUT_TYPE>(idata),
			                                            aggr_input_data, (STATE_TYPE *)state, count, idata.validity,
			                                            *idata.sel);
			break;
		}
		}
	}

	template <class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static void BinaryScatter(AggregateInputData &aggr_input_data, Vector &a, Vector &b, Vector &states, idx_t count) {
#if !DUCKDB_SMALLER_BINARY(aggregate_executor_flat)
		if (OP::IgnoreNull() && a.GetVectorType() == VectorType::FLAT_VECTOR &&
		    b.GetVectorType() == VectorType::FLAT_VECTOR && states.GetVectorType() == VectorType::FLAT_VECTOR) {
			BinaryFlatLoop<STATE_TYPE, A_TYPE, B_TYPE, OP>(FlatVector::GetData<A_TYPE>(a), aggr_input_data,
			                                               FlatVector::GetData<B_TYPE>(b),
			                                               FlatVector::GetDataMutable<STATE_TYPE *>(states), count,
			                                               FlatVector::Validity(a), FlatVector::Validity(b));
			return;
		}
#endif
		UnifiedVectorFormat adata, bdata, sdata;

		a.ToUnifiedFormat(adata);
		b.ToUnifiedFormat(bdata);
		states.ToUnifiedFormat(sdata);

		BinaryScatterLoop<STATE_TYPE, A_TYPE, B_TYPE, OP>(
		    UnifiedVectorFormat::GetData<A_TYPE>(adata), aggr_input_data, UnifiedVectorFormat::GetData<B_TYPE>(bdata),
		    (STATE_TYPE **)sdata.data, count, *adata.sel, *bdata.sel, *sdata.sel, adata.validity, bdata.validity);
	}

	template <class STATE_TYPE, class A_TYPE, class B_TYPE, class OP>
	static void BinaryUpdate(AggregateInputData &aggr_input_data, Vector &a, Vector &b, data_ptr_t state, idx_t count) {
		UnifiedVectorFormat adata, bdata;

		a.ToUnifiedFormat(adata);
		b.ToUnifiedFormat(bdata);

		BinaryUpdateLoop<STATE_TYPE, A_TYPE, B_TYPE, OP>(
		    UnifiedVectorFormat::GetData<A_TYPE>(adata), aggr_input_data, UnifiedVectorFormat::GetData<B_TYPE>(bdata),
		    (STATE_TYPE *)state, count, *adata.sel, *bdata.sel, adata.validity, bdata.validity);
	}

	template <class STATE_TYPE, class OP, class = void>
	struct HasRepeatedCombine : std::false_type {};
	template <class STATE_TYPE, class OP>
	struct HasRepeatedCombine<STATE_TYPE, OP,
	                          void_t_helper<decltype(OP::template RepeatedCombine<STATE_TYPE, OP>(
	                              std::declval<const STATE_TYPE &>(), std::declval<STATE_TYPE &>(),
	                              std::declval<AggregateInputData &>(), std::declval<idx_t>()))>> : std::true_type {};

	template <class STATE_TYPE, class OP, class = void>
	struct HasInitialize : std::false_type {};
	template <class STATE_TYPE, class OP>
	struct HasInitialize<STATE_TYPE, OP, void_t_helper<decltype(OP::Initialize(std::declval<STATE_TYPE &>()))>>
	    : std::true_type {};

	template <class STATE_TYPE, class OP, class = void>
	struct HasDestroy : std::false_type {};
	template <class STATE_TYPE, class OP>
	struct HasDestroy<STATE_TYPE, OP,
	                  void_t_helper<decltype(OP::template Destroy<STATE_TYPE>(
	                      std::declval<STATE_TYPE &>(), std::declval<AggregateInputData &>()))>> : std::true_type {};

	template <class STATE_TYPE, class OP>
	static void InitializeState(STATE_TYPE &state) {
		if constexpr (HasInitialize<STATE_TYPE, OP>::value) {
			OP::Initialize(state);
		} else {
			memset(&state, 0, sizeof(STATE_TYPE));
		}
	}

	template <class STATE_TYPE, class OP>
	static void DestroyState(STATE_TYPE &state, AggregateInputData &aggr_input_data) {
		if constexpr (HasDestroy<STATE_TYPE, OP>::value) {
			OP::template Destroy<STATE_TYPE>(state, aggr_input_data);
		}
	}

	template <class STATE_TYPE, class OP>
	class AggregateStateGuard {
	public:
		explicit AggregateStateGuard(AggregateInputData &aggr_input_data) : aggr_input_data(aggr_input_data) {
		}

		AggregateStateGuard(const AggregateStateGuard &) = delete;
		AggregateStateGuard &operator=(const AggregateStateGuard &) = delete;

		~AggregateStateGuard() {
			Destroy();
		}

		STATE_TYPE &Get() {
			return *reinterpret_cast<STATE_TYPE *>(storage);
		}

		void Initialize() {
			D_ASSERT(!initialized);
			InitializeState<STATE_TYPE, OP>(Get());
			initialized = true;
		}

		void Destroy() {
			if (!initialized) {
				return;
			}
			DestroyState<STATE_TYPE, OP>(Get(), aggr_input_data);
			initialized = false;
		}

	private:
		alignas(STATE_TYPE) data_t storage[sizeof(STATE_TYPE)];
		AggregateInputData &aggr_input_data;
		bool initialized = false;
	};

	template <class STATE_TYPE, class OP>
	static void GenericRepeatedCombine(const STATE_TYPE &source, STATE_TYPE &target,
	                                   AggregateInputData &aggr_input_data, idx_t multiplicity) {
		AggregateInputData combine_input(aggr_input_data.function, aggr_input_data.bind_data, aggr_input_data.allocator,
		                                 AggregateCombineType::PRESERVE_INPUT);

		AggregateStateGuard<STATE_TYPE, OP> result(combine_input);
		AggregateStateGuard<STATE_TYPE, OP> power_a(combine_input);
		AggregateStateGuard<STATE_TYPE, OP> power_b(combine_input);

		result.Initialize();
		power_a.Initialize();
		OP::template Combine<STATE_TYPE, OP>(source, power_a.Get(), combine_input);

		auto power = &power_a;
		auto next_power = &power_b;
		while (multiplicity > 0) {
			if (multiplicity & 1) {
				OP::template Combine<STATE_TYPE, OP>(power->Get(), result.Get(), combine_input);
			}
			multiplicity >>= 1;
			if (multiplicity == 0) {
				break;
			}
			next_power->Initialize();
			OP::template Combine<STATE_TYPE, OP>(power->Get(), next_power->Get(), combine_input);
			OP::template Combine<STATE_TYPE, OP>(power->Get(), next_power->Get(), combine_input);
			power->Destroy();
			std::swap(power, next_power);
		}
		OP::template Combine<STATE_TYPE, OP>(result.Get(), target, combine_input);
	}

	template <class STATE_TYPE, class OP>
	static void RepeatedCombine(const STATE_TYPE &source, STATE_TYPE &target, AggregateInputData &aggr_input_data,
	                            idx_t multiplicity) {
		if constexpr (HasRepeatedCombine<STATE_TYPE, OP>::value) {
			AggregateInputData combine_input(aggr_input_data.function, aggr_input_data.bind_data,
			                                 aggr_input_data.allocator, aggr_input_data.combine_type);
			OP::template RepeatedCombine<STATE_TYPE, OP>(source, target, combine_input, multiplicity);
		} else {
			GenericRepeatedCombine<STATE_TYPE, OP>(source, target, aggr_input_data, multiplicity);
		}
	}

	template <class STATE_TYPE, class OP>
	static void CombineWithoutMultiplicities(Vector &source, Vector &target, AggregateInputData &aggr_input_data,
	                                         idx_t count) {
		auto sdata = source.Values<const STATE_TYPE *>();
		auto tdata = target.Values<STATE_TYPE *>();
		for (idx_t i = 0; i < count; i++) {
			OP::template Combine<STATE_TYPE, OP>(*sdata[i].GetValueUnsafe(), *tdata[i].GetValueUnsafe(),
			                                     aggr_input_data);
		}
	}

	template <class STATE_TYPE, class OP>
	static void CombineWithMultiplicities(Vector &source, Vector &target, AggregateInputData &aggr_input_data,
	                                      idx_t count) {
		auto sdata = source.Values<const STATE_TYPE *>();
		auto tdata = target.Values<STATE_TYPE *>();
		UnifiedVectorFormat multiplicities;
		aggr_input_data.combine_multiplicities->ToUnifiedFormat(multiplicities);
		auto multiplicity_data = UnifiedVectorFormat::GetData<int64_t>(multiplicities);
		for (idx_t i = 0; i < count; i++) {
			auto multiplicity_idx = multiplicities.sel->get_index(i);
			if (!multiplicities.validity.RowIsValid(multiplicity_idx)) {
				continue;
			}
			const auto multiplicity = multiplicity_data[multiplicity_idx];
			if (multiplicity < 0) {
				InvalidInputException::Throw("combine_aggr multiplicity must be non-negative");
			}
			if (multiplicity == 0) {
				continue;
			}
			auto &source_state = *sdata[i].GetValueUnsafe();
			auto &target_state = *tdata[i].GetValueUnsafe();
			if (multiplicity == 1) {
				OP::template Combine<STATE_TYPE, OP>(source_state, target_state, aggr_input_data);
				continue;
			}
			RepeatedCombine<STATE_TYPE, OP>(source_state, target_state, aggr_input_data,
			                                static_cast<idx_t>(multiplicity));
		}
	}

	template <class STATE_TYPE, class OP>
	static void Combine(Vector &source, Vector &target, AggregateInputData &aggr_input_data, idx_t count) {
		D_ASSERT(source.GetType().id() == LogicalTypeId::POINTER && target.GetType().id() == LogicalTypeId::POINTER);
		if (aggr_input_data.combine_multiplicities) {
			CombineWithMultiplicities<STATE_TYPE, OP>(source, target, aggr_input_data, count);
		} else {
			CombineWithoutMultiplicities<STATE_TYPE, OP>(source, target, aggr_input_data, count);
		}
	}

	template <class STATE_TYPE, class RESULT_TYPE, class OP>
	static void Finalize(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result, idx_t count,
	                     idx_t offset) {
		if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			FlatVector::SetSize(result, count);

			auto sdata = ConstantVector::GetData<STATE_TYPE *>(states);
			auto rdata = ConstantVector::GetData<RESULT_TYPE>(result);
			AggregateFinalizeData finalize_data(result, finalize_input_data, count);
			OP::template Finalize<RESULT_TYPE, STATE_TYPE>(**sdata, *rdata, finalize_data);
		} else {
			D_ASSERT(states.GetVectorType() == VectorType::FLAT_VECTOR);
			result.SetVectorType(VectorType::FLAT_VECTOR);

			auto sdata = FlatVector::GetData<STATE_TYPE *>(states);
			auto rdata = FlatVector::GetDataMutable<RESULT_TYPE>(result);
			AggregateFinalizeData finalize_data(result, finalize_input_data, count);
			for (idx_t i = 0; i < count; i++) {
				finalize_data.result_idx = i + offset;
				OP::template Finalize<RESULT_TYPE, STATE_TYPE>(*sdata[i], rdata[finalize_data.result_idx],
				                                               finalize_data);
			}
		}
	}

	template <class STATE_TYPE, class OP>
	static void VoidFinalize(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result,
	                         idx_t count, idx_t offset) {
		if (states.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			FlatVector::SetSize(result, count);

			auto sdata = ConstantVector::GetData<STATE_TYPE *>(states);
			AggregateFinalizeData finalize_data(result, finalize_input_data, count);
			OP::template Finalize<STATE_TYPE>(**sdata, finalize_data);
		} else {
			D_ASSERT(states.GetVectorType() == VectorType::FLAT_VECTOR);
			result.SetVectorType(VectorType::FLAT_VECTOR);

			auto sdata = FlatVector::GetData<STATE_TYPE *>(states);
			AggregateFinalizeData finalize_data(result, finalize_input_data);
			for (idx_t i = 0; i < count; i++) {
				finalize_data.result_idx = i + offset;
				OP::template Finalize<STATE_TYPE>(*sdata[i], finalize_data);
			}
		}
	}

	template <typename OP>
	static void IntersectFrames(const SubFrames &lefts, const SubFrames &rights, OP &op) {
		const auto cover_start = MinValue(rights[0].start, lefts[0].start);
		const auto cover_end = MaxValue(rights.back().end, lefts.back().end);
		const FrameBounds last(cover_end, cover_end);

		//	Subframe indices
		idx_t l = 0;
		idx_t r = 0;
		for (auto i = cover_start; i < cover_end;) {
			uint8_t overlap = 0;

			// Are we in the previous frame?
			auto left = &last;
			if (l < lefts.size()) {
				left = &lefts[l];
				overlap |= uint8_t(left->start <= i && i < left->end) << 0;
			}

			// Are we in the current frame?
			auto right = &last;
			if (r < rights.size()) {
				right = &rights[r];
				overlap |= uint8_t(right->start <= i && i < right->end) << 1;
			}

			auto limit = i;
			switch (overlap) {
			case 0x00:
				// i ∉ F U P
				limit = MinValue(right->start, left->start);
				op.Neither(i, limit);
				break;
			case 0x01:
				// i ∈ P \ F
				limit = MinValue(left->end, right->start);
				op.Left(i, limit);
				break;
			case 0x02:
				// i ∈ F \ P
				limit = MinValue(right->end, left->start);
				op.Right(i, limit);
				break;
			case 0x03:
			default:
				D_ASSERT(overlap == 0x03);
				// i ∈ F ∩ P
				limit = MinValue(right->end, left->end);
				op.Both(i, limit);
				break;
			}

			// Advance  the subframe indices
			i = limit;
			l += (i == left->end);
			r += (i == right->end);
		}
	}

	template <class STATE_TYPE, class OP>
	static void Destroy(Vector &states, AggregateInputData &aggr_input_data, idx_t count) {
		auto sdata = states.Values<STATE_TYPE *>();
		;
		for (idx_t i = 0; i < count; i++) {
			OP::template Destroy<STATE_TYPE>(*sdata[i].GetValueUnsafe(), aggr_input_data);
		}
	}
};

} // namespace duckdb
