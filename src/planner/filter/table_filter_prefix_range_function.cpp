//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_prefix_range_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include "duckdb/common/allocator.hpp"
#include "duckdb/common/array.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/enums/filter_propagate_result.hpp"
#include "duckdb/common/enums/vector_type.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/uhugeint.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {

AllocatedData AllocateBitmap(ClientContext &context, const idx_t word_count, uint64_t *&bitmap_begin) {
	const idx_t size = word_count * sizeof(uint64_t);
	BufferManager &buffer_manager = BufferManager::GetBufferManager(context);
	auto buffer = buffer_manager.GetBufferAllocator().Allocate(64ULL + size);
	bitmap_begin = reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(buffer.get())) & ~63ULL);
	std::fill_n(bitmap_begin, word_count, 0);
	return buffer;
}

struct PrefixRangeBitmapBuildState : public PrefixRangeFilter::BuildState {
	explicit PrefixRangeBitmapBuildState(AllocatedData data_p, uint64_t *bitmap_p)
	    : data(std::move(data_p)), bitmap(bitmap_p) {
	}

	AllocatedData data;
	uint64_t *bitmap;
};

template <typename U>
class PrefixRangeBitmap {
public:
	void Initialize(ClientContext &context, U min_p, U span_p, idx_t max_bits) {
		min = min_p;
		span = span_p;
		shift = 0;

		while ((span >> shift) >= max_bits) {
			shift++;
		}

		const idx_t buckets = UnsafeNumericCast<idx_t>((span >> shift) + 1);
		word_count = buckets == 0 ? 1 : (buckets + 63) >> WORD_SHIFT;

		buf_ = AllocateBitmap(context, word_count, bitmap);

		// Only mark initialized as true when local bitmaps are merged.
		initialized = false;
		dense = false;
	}

	unique_ptr<PrefixRangeBitmapBuildState> InitializeBuildState(ClientContext &context) const {
		D_ASSERT(bitmap);
		uint64_t *state_bitmap;
		auto state_data = AllocateBitmap(context, word_count, state_bitmap);
		return make_uniq<PrefixRangeBitmapBuildState>(std::move(state_data), state_bitmap);
	}

	template <bool PARALLEL>
	static void SetBit(uint64_t *state_bitmap, idx_t bit_idx) {
		const auto word_idx = bit_idx >> WORD_SHIFT;
		const auto mask = 1ULL << (bit_idx & WORD_MASK);
		if (PARALLEL) {
			// Shared build lanes are published only after every finalize task completes.
			auto &slot = *reinterpret_cast<atomic<uint64_t> *>(&state_bitmap[word_idx]);
			slot.fetch_or(mask, std::memory_order_relaxed);
		} else {
			state_bitmap[word_idx] |= mask;
		}
	}

	template <typename T, typename CONVERTER, bool PARALLEL>
	void InsertKeys(Vector &keys, uint64_t *state_bitmap) const {
		for (const auto &entry : keys.template ValidValues<T>()) {
			const U y = CONVERTER::Convert(entry.GetValue()) - min;
			// All keys are in-range by construction, so the range check can be omitted here.
			const U idx = y >> shift;
			SetBit<PARALLEL>(state_bitmap, UnsafeNumericCast<idx_t>(idx));
		}
	}

	void MergeBuildState(PrefixRangeBitmapBuildState &state) {
		const auto last_word_mask = ~uint64_t(0) >> (WORD_MASK - UnsafeNumericCast<idx_t>((span >> shift) & WORD_MASK));
		dense = true;
		for (idx_t word_idx = 0; word_idx < word_count; word_idx++) {
			bitmap[word_idx] |= state.bitmap[word_idx];
			const auto expected = word_idx + 1 == word_count ? last_word_mask : ~uint64_t(0);
			dense &= bitmap[word_idx] == expected;
		}
		initialized = true;
	}

	idx_t GetBuildStateSize() const {
		return word_count * sizeof(uint64_t) + 64;
	}

	template <typename T, typename CONVERTER>
	inline bool LookupOne(const Value &value) const {
		if (value.IsNull()) {
			return false;
		}

		const U comparable = CONVERTER::Convert(value.GetValueUnsafe<T>());
		const U y = comparable - min;
		if (dense) {
			return y <= span;
		}
		const U bit_idx = y >> shift;
		const uint8_t in_range = y <= span;
		const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
		const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;
		return bit & in_range;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const {
		D_ASSERT(count <= keys.size());
		auto key_entries = keys.Values<T>();
		if (dense) {
			return LookupDense<T, CONVERTER>(key_entries, SelectionVector(), result_sel, count);
		}
		if (count >= PREFETCH_MIN_COUNT && ShouldPrefetch<T, CONVERTER>(key_entries, SelectionVector(), count)) {
			return LookupPrefetched<T, CONVERTER>(key_entries, SelectionVector(), result_sel, count);
		}
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto key_entry = key_entries[i];
			if (!key_entry.IsValid()) {
				continue;
			}
			const U comparable = CONVERTER::Convert(key_entry.GetValue());
			const U y = comparable - min;
			const U bit_idx = y >> shift;
			const uint8_t in_range = y <= span;
			const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
			const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;

			result_sel.set_index(found_count, i);
			found_count += bit & in_range;
		}
		return found_count;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel, idx_t count) const {
		auto key_entries = keys.Values<T>();
		if (dense) {
			return LookupDense<T, CONVERTER>(key_entries, sel, result_sel, count);
		}
		if (count >= PREFETCH_MIN_COUNT && ShouldPrefetch<T, CONVERTER>(key_entries, sel, count)) {
			return LookupPrefetched<T, CONVERTER>(key_entries, sel, result_sel, count);
		}
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto key_entry = key_entries[sel.get_index_unsafe(i)];
			if (!key_entry.IsValid()) {
				continue;
			}
			const U comparable = CONVERTER::Convert(key_entry.GetValue());
			const U y = comparable - min;
			const U bit_idx = y >> shift;
			const uint8_t in_range = y <= span;
			const uint32_t word_idx = (bit_idx >> WORD_SHIFT) & (0U - in_range);
			const uint8_t bit = (bitmap[word_idx] >> (bit_idx & WORD_MASK)) & 1ULL;

			result_sel.set_index(found_count, i);
			found_count += bit & in_range;
		}
		return found_count;
	}

	template <typename T, typename CONVERTER>
	bool ShouldPrefetch(const VectorIterator<T> &keys, const SelectionVector &sel, idx_t count) const {
#if defined(__GNUC__) || defined(__clang__)
		if (word_count * sizeof(uint64_t) < PREFETCH_MIN_BYTES) {
			return false;
		}
		const auto step = MaxValue<idx_t>(count / 16, 1);
		idx_t min_word = word_count;
		idx_t max_word = 0;
		for (idx_t i = 0; i < count; i += step) {
			const auto entry = keys[sel.get_index(i)];
			if (!entry.IsValid()) {
				continue;
			}
			const U y = CONVERTER::Convert(entry.GetValue()) - min;
			if (y > span) {
				continue;
			}
			const auto word_idx = UnsafeNumericCast<idx_t>((y >> shift) >> WORD_SHIFT);
			min_word = MinValue(min_word, word_idx);
			max_word = MaxValue(max_word, word_idx);
			if (max_word - min_word >= PREFETCH_MIN_BYTES / sizeof(uint64_t)) {
				return true;
			}
		}
#endif
		return false;
	}

	template <bool HAS_NULL, typename T, typename CONVERTER>
	void PreparePositions(const VectorIterator<T> &keys, const SelectionVector &sel, idx_t offset, idx_t count,
	                      array<uint8_t, STANDARD_VECTOR_SIZE> &bit_offsets,
	                      array<uint32_t, STANDARD_VECTOR_SIZE> &word_indices,
	                      array<uint8_t, STANDARD_VECTOR_SIZE> &in_range) const {
		for (idx_t i = 0; i < count; i++) {
			const auto entry = keys[sel.get_index(offset + i)];
			if (HAS_NULL && !entry.IsValid()) {
				bit_offsets[i] = 0;
				word_indices[i] = 0;
				in_range[i] = 0;
				continue;
			}
			const U y = CONVERTER::Convert(entry.GetValueUnsafe()) - min;
			const U bit_idx = y >> shift;
			const uint8_t valid = y <= span;
			bit_offsets[i] = bit_idx & WORD_MASK;
			word_indices[i] = (bit_idx >> WORD_SHIFT) & (0U - valid);
			in_range[i] = valid;
		}
	}

	template <typename T, typename CONVERTER>
	idx_t LookupPrefetched(const VectorIterator<T> &key_entries, const SelectionVector &sel,
	                       SelectionVector &result_sel, idx_t count) const {
		array<uint8_t, STANDARD_VECTOR_SIZE> bit_offsets;
		array<uint32_t, STANDARD_VECTOR_SIZE> word_indices;
		array<uint8_t, STANDARD_VECTOR_SIZE> in_range;
		idx_t found_count = 0;
		for (idx_t offset = 0; offset < count;) {
			const auto batch_count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, count - offset);
			if (key_entries.CanHaveNull()) {
				PreparePositions<true, T, CONVERTER>(key_entries, sel, offset, batch_count, bit_offsets, word_indices,
				                                     in_range);
			} else {
				PreparePositions<false, T, CONVERTER>(key_entries, sel, offset, batch_count, bit_offsets, word_indices,
				                                      in_range);
			}
			for (idx_t i = 0; i < batch_count; i++) {
				const uint8_t bit = (bitmap[word_indices[i]] >> bit_offsets[i]) & 1ULL;
				result_sel.set_index(found_count, offset + i);
				found_count += bit & in_range[i];
				PrefetchHashTableEntry(i, batch_count, [&](idx_t next_idx) { return bitmap + word_indices[next_idx]; });
			}
			offset += batch_count;
		}
		return found_count;
	}

	template <typename T, typename CONVERTER>
	idx_t LookupDense(const VectorIterator<T> &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                  idx_t count) const {
		// A bitmap with every bucket set is equivalent to its bounds.
		idx_t found_count = 0;
		for (idx_t i = 0; i < count; i++) {
			const auto entry = keys[sel.get_index(i)];
			if (!entry.IsValid()) {
				continue;
			}
			const U y = CONVERTER::Convert(entry.GetValue()) - min;
			result_sel.set_index(found_count, i);
			found_count += y <= span;
		}
		return found_count;
	}

	FilterPropagateResult LookupRange(U lower_bound, U upper_bound) const {
		const U lb_y = lower_bound - min;
		const U lb_bit_idx = lb_y >> shift;
		const auto lb_word_idx = lb_bit_idx >> WORD_SHIFT;

		const U ub_y = upper_bound - min;
		const U ub_bit_idx = ub_y >> shift;
		const auto ub_word_idx = ub_bit_idx >> WORD_SHIFT;

		const idx_t lb_bit_off = UnsafeNumericCast<idx_t>(lb_bit_idx & UnsafeNumericCast<U>(WORD_MASK));
		const idx_t ub_bit_off = UnsafeNumericCast<idx_t>(ub_bit_idx & UnsafeNumericCast<U>(WORD_MASK));

		// TODO: Count the amount of 1's in the range, compare to a threshold, and make a decision if we want to use the
		// per-row filter for this row group.
		if (lb_word_idx == ub_word_idx) {
			const auto range_mask = ((~0ULL << lb_bit_off) & (~0ULL >> (WORD_MASK - ub_bit_off)));
			if (bitmap[lb_word_idx] & range_mask) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		const auto lb_word_mask = (~0ULL << lb_bit_off);
		if (bitmap[lb_word_idx] & lb_word_mask) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		for (idx_t i = UnsafeNumericCast<idx_t>(lb_word_idx) + 1; i < UnsafeNumericCast<idx_t>(ub_word_idx); i++) {
			if (bitmap[i]) {
				return FilterPropagateResult::NO_PRUNING_POSSIBLE;
			}
		}

		const auto ub_word_mask = ~0ULL >> (WORD_MASK - ub_bit_off);
		if (bitmap[ub_word_idx] & ub_word_mask) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	bool IsInitialized() const {
		return initialized;
	}

	bool IsDense() const {
		return dense;
	}

	U Min() const {
		return min;
	}

	U Span() const {
		return span;
	}

private:
	// Staging only pays off once bitmap loads are expensive enough.
	static constexpr idx_t PREFETCH_MIN_BYTES = 1024ULL * 1024ULL;
	static constexpr idx_t PREFETCH_MIN_COUNT = 128;
	static constexpr idx_t WORD_SHIFT = 6;
	static constexpr idx_t WORD_MASK = 63;

	bool initialized = false;
	//! All buckets within span are set, excluding padding bits in the final word.
	bool dense = false;
	U min;
	U span;
	idx_t shift;
	idx_t word_count;
	AllocatedData buf_;
	uint64_t *bitmap;
};

template <typename T>
struct NumericConverter {
	using comparable_type = typename MakeUnsigned<T>::type;

	static inline comparable_type Convert(T value) {
		// Overflow is explicitly allowed for unsigned to signed cast
		return static_cast<comparable_type>(value);
	}
};

struct StringPrefixConverter {
	static inline uint32_t Convert(const string_t &value) {
		return value.GetPrefixIntegerComparable();
	}
};

uint32_t StringMinComparable(const Value &value) {
	return StringPrefixConverter::Convert(value.GetValueUnsafe<string_t>());
}

uint32_t StringMaxComparable(const Value &value) {
	const auto max_string = value.GetValueUnsafe<string_t>();
	if (max_string.GetSize() >= string_t::PREFIX_BYTES) {
		return max_string.GetPrefixIntegerComparable();
	}

	// Pad string prefix with 0xFF to keep correctness if max is truncated at \0 char, e.g., ab\0c -> ab
	array<char, string_t::PREFIX_BYTES> padded_prefix;
	padded_prefix.fill(char(0xFF));
	for (idx_t i = 0; i < max_string.GetSize(); i++) {
		padded_prefix[i] = max_string.GetData()[i];
	}
	return string_t(padded_prefix.data(), string_t::PREFIX_BYTES).GetPrefixIntegerComparable();
}

template <typename T>
class NumericPrefixRangeFilter : public PrefixRangeFilter {
private:
	using Comparable = typename MakeUnsigned<T>::type;

public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                idx_t max_bits) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = NumericConverter<T>::Convert(min_val.GetValueUnsafe<T>());
		const auto max = NumericConverter<T>::Convert(max_val.GetValueUnsafe<T>());
		bitmap.Initialize(context, min, max - min, max_bits);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>, false>(keys, bitmap_state.bitmap);
	}

	void InsertKeysParallel(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<T, NumericConverter<T>, true>(keys, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t GetBuildStateSize() const override {
		return bitmap.GetBuildStateSize();
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<T, NumericConverter<T>>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<T, NumericConverter<T>>(keys, result_sel, count);
	}

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<T, NumericConverter<T>>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<T, NumericConverter<T>>(keys, sel, result_sel, count);
	}

	FilterPropagateResult LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		const auto lb = lower_bound.GetValueUnsafe<T>();
		const auto ub = upper_bound.GetValueUnsafe<T>();

		const auto bitmap_min = static_cast<T>(bitmap.Min());
		const auto bitmap_max = static_cast<T>(bitmap.Min() + bitmap.Span());
		if (ub < bitmap_min || lb > bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}
		if (bitmap.IsDense() && lb >= bitmap_min && ub <= bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_TRUE;
		}

		const auto adjusted_lb = NumericConverter<T>::Convert(MaxValue<T>(lb, bitmap_min));
		const auto adjusted_ub = NumericConverter<T>::Convert(MinValue<T>(ub, bitmap_max));
		return bitmap.LookupRange(adjusted_lb, adjusted_ub);
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

private:
	PrefixRangeBitmap<Comparable> bitmap;
};

class StringPrefixRangeFilter : public PrefixRangeFilter {
public:
	void Initialize(ClientContext &context, idx_t number_of_rows, Value min_val, Value max_val,
	                idx_t max_bits) override {
		D_ASSERT(min_val <= max_val);
		D_ASSERT(number_of_rows > 0);
		const auto min = StringPrefixConverter::Convert(min_val.GetValueUnsafe<string_t>());
		const auto max = StringPrefixConverter::Convert(max_val.GetValueUnsafe<string_t>());
		D_ASSERT(min <= max);
		bitmap.Initialize(context, min, max - min, max_bits);
	}

	unique_ptr<BuildState> InitializeBuildState(ClientContext &context) const override {
		return bitmap.InitializeBuildState(context);
	}

	void InsertKeys(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter, false>(keys, bitmap_state.bitmap);
	}

	void InsertKeysParallel(Vector &keys, BuildState &state) const override {
		auto &bitmap_state = state.Cast<PrefixRangeBitmapBuildState>();
		bitmap.template InsertKeys<string_t, StringPrefixConverter, true>(keys, bitmap_state.bitmap);
	}

	void MergeBuildState(BuildState &state) override {
		bitmap.MergeBuildState(state.Cast<PrefixRangeBitmapBuildState>());
	}

	idx_t GetBuildStateSize() const override {
		return bitmap.GetBuildStateSize();
	}

	idx_t LookupKeys(Vector &keys, SelectionVector &result_sel, idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<string_t, StringPrefixConverter>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<string_t, StringPrefixConverter>(keys, result_sel, count);
	}

	idx_t LookupKeys(Vector &keys, const SelectionVector &sel, SelectionVector &result_sel,
	                 idx_t count) const override {
		if (keys.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			return bitmap.template LookupOne<string_t, StringPrefixConverter>(keys.GetValue(0)) ? count : 0;
		}
		return bitmap.template LookupKeys<string_t, StringPrefixConverter>(keys, sel, result_sel, count);
	}

	FilterPropagateResult LookupRange(const Value &lower_bound, const Value &upper_bound) const override {
		auto lower_bound_comparable = StringMinComparable(lower_bound);
		auto upper_bound_comparable = StringMaxComparable(upper_bound);
		if (lower_bound_comparable > upper_bound_comparable) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}

		const auto bitmap_min = bitmap.Min();
		const auto bitmap_max = bitmap.Min() + bitmap.Span();
		if (upper_bound_comparable < bitmap_min || lower_bound_comparable > bitmap_max) {
			return FilterPropagateResult::FILTER_ALWAYS_FALSE;
		}

		lower_bound_comparable = MaxValue<uint32_t>(lower_bound_comparable, bitmap_min);
		upper_bound_comparable = MinValue<uint32_t>(upper_bound_comparable, bitmap_max);
		return bitmap.LookupRange(lower_bound_comparable, upper_bound_comparable);
	}

	bool IsInitialized() const override {
		return bitmap.IsInitialized();
	}

private:
	PrefixRangeBitmap<uint32_t> bitmap;
};

template <typename T>
bool ComputeSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
	T lb_value = lower_bound.GetValueUnsafe<T>();
	T ub_value = upper_bound.GetValueUnsafe<T>();
	T res;
	if (TrySubtractOperator::Operation(ub_value, lb_value, res)) {
		result = Uhugeint::Convert(res);
		return true;
	} else {
		return false;
	}
}

bool ComputeStringPrefixSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
#ifdef DUCKDB_DEBUG_NO_INLINE
	return false;
#else
	auto lb_value = lower_bound.GetValueUnsafe<string_t>().GetPrefixIntegerComparable();
	auto ub_value = upper_bound.GetValueUnsafe<string_t>().GetPrefixIntegerComparable();
	uint32_t res;
	if (TrySubtractOperator::Operation(ub_value, lb_value, res)) {
		result = Uhugeint::Convert(res);
		return true;
	} else {
		return false;
	}
#endif
}

unique_ptr<PrefixRangeFilter> PrefixRangeFilter::CreatePrefixRangeFilter(const LogicalType &key_type) {
	switch (key_type.InternalType()) {
	case PhysicalType::UINT8:
		return make_uniq<NumericPrefixRangeFilter<uint8_t>>();
	case PhysicalType::UINT16:
		return make_uniq<NumericPrefixRangeFilter<uint16_t>>();
	case PhysicalType::UINT32:
		return make_uniq<NumericPrefixRangeFilter<uint32_t>>();
	case PhysicalType::UINT64:
		return make_uniq<NumericPrefixRangeFilter<uint64_t>>();
	case PhysicalType::INT8:
		return make_uniq<NumericPrefixRangeFilter<int8_t>>();
	case PhysicalType::INT16:
		return make_uniq<NumericPrefixRangeFilter<int16_t>>();
	case PhysicalType::INT32:
		return make_uniq<NumericPrefixRangeFilter<int32_t>>();
	case PhysicalType::INT64:
		return make_uniq<NumericPrefixRangeFilter<int64_t>>();
	case PhysicalType::VARCHAR:
#ifdef DUCKDB_DEBUG_NO_INLINE
		throw NotImplementedException("Prefix range filter is not implemented for type %s", key_type.ToString());
#else
		return make_uniq<StringPrefixRangeFilter>();
#endif
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		throw NotImplementedException("Prefix range filter is not implemented for type %s", key_type.ToString());
	}
}

bool PrefixRangeFilter::TryComputeSpan(const Value &lower_bound, const Value &upper_bound, uhugeint_t &result) {
	if (lower_bound.type().InternalType() != upper_bound.type().InternalType()) {
		return false;
	}

	switch (lower_bound.type().InternalType()) {
	case PhysicalType::UINT8:
		return ComputeSpan<uint8_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT16:
		return ComputeSpan<uint16_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT32:
		return ComputeSpan<uint32_t>(lower_bound, upper_bound, result);
	case PhysicalType::UINT64:
		return ComputeSpan<uint64_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT8:
		return ComputeSpan<int8_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT16:
		return ComputeSpan<int16_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT32:
		return ComputeSpan<int32_t>(lower_bound, upper_bound, result);
	case PhysicalType::INT64:
		return ComputeSpan<int64_t>(lower_bound, upper_bound, result);
	case PhysicalType::VARCHAR:
		return ComputeStringPrefixSpan(lower_bound, upper_bound, result);
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		return false;
	}
}

bool PrefixRangeFilter::SupportedType(const LogicalType &type) {
	switch (type.InternalType()) {
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
		return true;
	case PhysicalType::VARCHAR:
#ifdef DUCKDB_DEBUG_NO_INLINE
		return false;
#else
		return true;
#endif
	case PhysicalType::INT128:
	case PhysicalType::UINT128:
	default:
		return false;
	}
}

PrefixRangeFunctionData::PrefixRangeFunctionData(optional_ptr<PrefixRangeFilter> filter_p, bool filters_null_values_p,
                                                 const string &key_column_name_p, const LogicalType &key_type_p,
                                                 float selectivity_threshold_p, idx_t n_vectors_to_check_p)
    : filter(filter_p), filters_null_values(filters_null_values_p), key_column_name(key_column_name_p),
      key_type(key_type_p), selectivity_threshold(selectivity_threshold_p), n_vectors_to_check(n_vectors_to_check_p) {
}

unique_ptr<FunctionData> PrefixRangeFunctionData::Copy() const {
	return make_uniq<PrefixRangeFunctionData>(filter, filters_null_values, key_column_name, key_type,
	                                          selectivity_threshold, n_vectors_to_check);
}

bool PrefixRangeFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<PrefixRangeFunctionData>();
	return filter.get() == other.filter.get() && filters_null_values == other.filters_null_values &&
	       key_column_name == other.key_column_name && key_type == other.key_type;
}

static idx_t SelectPrefixRange(Vector &input, const PrefixRangeFunctionData &func_data, SelectionVector &result_sel,
                               idx_t count) {
	D_ASSERT(func_data.filter);
	const auto filter_count = func_data.filter->LookupKeys(input, result_sel, count);
	if (func_data.filters_null_values || filter_count == count) {
		return filter_count;
	}

	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	if (input_data.validity.CannotHaveNull()) {
		return filter_count;
	}

	SelectionVector filter_sel(filter_count);
	for (idx_t i = 0; i < filter_count; i++) {
		filter_sel.set_index(i, result_sel.get_index_unsafe(i));
	}
	idx_t result_count = 0;
	idx_t filter_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto matched = filter_idx < filter_count && filter_sel.get_index_unsafe(filter_idx) == i;
		filter_idx += matched;
		const auto input_idx = input_data.sel->get_index(i);
		if (matched || !input_data.validity.RowIsValid(input_idx)) {
			result_sel.set_index(result_count++, i);
		}
	}
	return result_count;
}

static unique_ptr<FunctionLocalState>
PrefixRangeInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &data = bind_data->Cast<PrefixRangeFunctionData>();
	if (!data.filter) {
		return nullptr;
	}
	return InitSelectivityTrackingLocalState(data.n_vectors_to_check, data.selectivity_threshold);
}

static idx_t PrefixRangeSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                               optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &func_data = func_expr.BindInfo()->Cast<PrefixRangeFunctionData>();
	auto local_state_ptr = ExecuteFunctionState::GetFunctionState(state);
	auto tracking_state = local_state_ptr ? &local_state_ptr->Cast<SelectivityTrackingLocalState>() : nullptr;

	auto count = args.size();
	if (!func_data.filter || !func_data.filter->IsInitialized()) {
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}
	if (tracking_state && !tracking_state->IsActive()) {
		tracking_state->Update(0, 0);
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}

	SelectionVector temp_true;
	auto &result_true_sel = GetFilterResultSelection(count, sel, true_sel, temp_true);
	auto approved_count = SelectPrefixRange(args.data[0], func_data, result_true_sel, count);
	approved_count = TranslateSelection(count, sel, result_true_sel, approved_count, true_sel, false_sel);
	if (tracking_state) {
		tracking_state->Update(approved_count, count);
	}
	return approved_count;
}

ScalarFunction PrefixRangeScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, nullptr, TableFilterFunctions::Bind);
	func.GetSignature().GetParameter(0).SetName("col");
	func.SetInitStateCallback(PrefixRangeInitLocalState);
	func.SetSelectCallback(PrefixRangeSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(PrefixRangeScalarFun::FilterPrune);
	func.SetSerializeCallback(TableFilterFunctionSerialize);
	func.SetDeserializeCallback(TableFilterFunctionDeserialize);
	return func;
}

string PrefixRangeScalarFun::ToString(const string &column_name, const string &key_column_name) {
	return column_name + " IN PRF(" + key_column_name + ")";
}

FilterPropagateResult PrefixRangeScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<PrefixRangeFunctionData>();
	if (!data.filter || !data.filter->IsInitialized()) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!data.filters_null_values) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &stats = *column_stats;
	if (!stats.CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	switch (stats.GetStatsType()) {
	case StatisticsType::NUMERIC_STATS: {
		if (!NumericStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto min = NumericStats::Min(stats);
		const auto max = NumericStats::Max(stats);
		if (min > max) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		const auto result = data.filter->LookupRange(min, max);
		if (result == FilterPropagateResult::FILTER_ALWAYS_TRUE && stats.CanHaveNull()) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return result;
	}
	case StatisticsType::STRING_STATS: {
		if (!StringStats::HasMinMax(stats)) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		// String stats may contain raw parquet bytes that are not valid UTF-8. Reconstruct them as BLOBs so the
		// prefix-range comparable logic can inspect the raw bytes without value-construction validation.
		return data.filter->LookupRange(Value::BLOB_RAW(StringStats::Min(stats)),
		                                Value::BLOB_RAW(StringStats::Max(stats)));
	}
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

ScalarFunction TableFilterPrefixRangeFun::GetFunction() {
	return PrefixRangeScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
