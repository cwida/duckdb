//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_bloom_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/execution/ht_entry.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/buffer_manager.hpp"

#include <bitset>
#include <cmath>

namespace duckdb {

static constexpr idx_t MAX_NUM_SECTORS = (1ULL << 26);
static constexpr idx_t MIN_NUM_BITS_PER_KEY = 12;
static constexpr idx_t MIN_NUM_BITS = 512;
static constexpr idx_t LOG_SECTOR_SIZE = 6;             // a sector is 64 bits, log2(64) = 6
static constexpr idx_t SHIFT_MASK = 0x3F3F3F3F3F3F3F3F; // 6 bits for 64 positions
static constexpr idx_t N_BITS = 4;                      // the number of bits to set per hash
static constexpr idx_t BITS_PER_SECTOR = 1ULL << LOG_SECTOR_SIZE;

//! A sector is one entry of the bit array, so the two have to agree on how wide it is
static_assert(BITS_PER_SECTOR == sizeof(uint64_t) * 8, "a bloom filter sector must be exactly one array entry");

//! A filter is sized from a cardinality estimate, so never let it claim more than this share of the memory budget
static constexpr idx_t MAX_MEMORY_FRACTION = 64;

void BloomFilter::Initialize(ClientContext &context_p, idx_t number_of_rows) {
	// a published filter is shared with probe-side consumers and must stay immutable
	D_ASSERT(!published);
	BufferManager &buffer_manager = BufferManager::GetBufferManager(context_p);

	// a capped filter stays complete - fewer sectors only raise the false positive rate, and Seal drops a
	// filter that ended up too dense to reject anything
	const idx_t min_sectors = MIN_NUM_BITS >> LOG_SECTOR_SIZE;
	const idx_t budget_bytes = buffer_manager.GetMaxMemory() / MAX_MEMORY_FRACTION;
	const idx_t budget_sectors = MaxValue(PreviousPowerOfTwo(budget_bytes / sizeof(uint64_t)), min_sectors);
	num_sectors = MinValue(GetNumberOfSectors(number_of_rows), budget_sectors);
	bitmask = num_sectors - 1;

	if (allocated_sectors < num_sectors) {
		buf_ = buffer_manager.GetBufferAllocator().Allocate(64 + num_sectors * sizeof(uint64_t));
		// make sure blocks is a 64-byte aligned pointer, i.e., cache-line aligned
		bf = reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(buf_.get())) & ~63ULL);
		allocated_sectors = num_sectors;
	}
	std::fill_n(bf, num_sectors, 0);

	initialized = true;
	sealed = false;
	useful = false;
}

void BloomFilter::Fold(idx_t new_num_sectors) {
	D_ASSERT(initialized);
	D_ASSERT(!sealed);
	D_ASSERT(IsPowerOfTwo(new_num_sectors));
	D_ASSERT(new_num_sectors <= num_sectors);
	const uint64_t new_bitmask = new_num_sectors - 1;
	for (idx_t i = new_num_sectors; i < num_sectors; i++) {
		bf[i & new_bitmask] |= bf[i];
	}
	num_sectors = new_num_sectors;
	bitmask = new_bitmask;
}

double BloomFilter::Density() const {
	D_ASSERT(initialized);
	idx_t bits_set = 0;
	for (idx_t i = 0; i < num_sectors; i++) {
		bits_set += static_cast<idx_t>(std::bitset<BITS_PER_SECTOR>(bf[i]).count());
	}
	return static_cast<double>(bits_set) / static_cast<double>(num_sectors * BITS_PER_SECTOR);
}

//! False positive rate the sizing in GetNumberOfSectors aims for. With m bits, n keys and k probes per key a
//! given bit stays zero with probability (1 - 1/m)^(n*k) ~ e^(-k*n/m), so all k probed bits are set - a false
//! positive - with probability (1 - e^(-k*n/m))^k. Here m/n is MIN_NUM_BITS_PER_KEY and k is N_BITS.
static double TargetFalsePositiveRate() {
	const double load = static_cast<double>(N_BITS) / static_cast<double>(MIN_NUM_BITS_PER_KEY);
	return std::pow(1.0 - std::exp(-load), static_cast<double>(N_BITS));
}

void BloomFilter::Seal(ClientContext &context) {
	if (!initialized || sealed) {
		sealed = true;
		return;
	}
	const idx_t min_sectors = MIN_NUM_BITS >> LOG_SECTOR_SIZE;
	double density = Density();
	// a density of exactly zero means no key was ever inserted, so there is nothing to fold down
	if (density > 0.0) {
		// each fold ORs two sectors together, so the expected density goes from p to 1 - (1 - p)^2
		const double target = TargetFalsePositiveRate();
		// target_sectors is only a candidate size - nothing is folded until the loop has settled on one
		idx_t target_sectors = num_sectors;
		while (target_sectors > min_sectors) {
			const double folded_density = 1.0 - (1.0 - density) * (1.0 - density);
			if (std::pow(folded_density, static_cast<double>(N_BITS)) > target) {
				break;
			}
			density = folded_density;
			target_sectors >>= 1;
		}
		if (target_sectors < num_sectors) {
			Fold(target_sectors);
			// the filter was sized from an estimate, so hand the memory folding freed up back to the allocator
			auto &allocator = BufferManager::GetBufferManager(context).GetBufferAllocator();
			auto compacted_buf = allocator.Allocate(64 + num_sectors * sizeof(uint64_t));
			auto compacted_bf =
			    reinterpret_cast<uint64_t *>((64ULL + reinterpret_cast<uint64_t>(compacted_buf.get())) & ~63ULL);
			std::copy_n(bf, num_sectors, compacted_bf);
			buf_ = std::move(compacted_buf);
			bf = compacted_bf;
			allocated_sectors = num_sectors;
		}
	}
	sealed = true;
	// a filter that accepts more than the scan-time policy tolerates gets paused on its first vectors anyway,
	// so it is not worth the memory it holds or the hashing it costs
	float selectivity_threshold;
	idx_t n_vectors_to_check;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::BF, selectivity_threshold, n_vectors_to_check);
	useful = std::pow(density, static_cast<double>(N_BITS)) < static_cast<double>(selectivity_threshold);
}

idx_t BloomFilter::GetNumberOfSectors(idx_t number_of_rows) {
	const idx_t min_bits = MaxValue(MIN_NUM_BITS, number_of_rows * MIN_NUM_BITS_PER_KEY);
	return MinValue(NextPowerOfTwo(min_bits) >> LOG_SECTOR_SIZE, MAX_NUM_SECTORS);
}

inline uint64_t GetMask(const hash_t hash) {
	const uint64_t shifts = hash & SHIFT_MASK;
	const auto shifts_8 = reinterpret_cast<const uint8_t *>(&shifts);

	uint64_t mask = 0;

	for (idx_t bit_idx = 8 - N_BITS; bit_idx < 8; bit_idx++) {
		const uint8_t bit_pos = shifts_8[bit_idx];
		mask |= (1ULL << bit_pos);
	}

	return mask;
}

void BloomFilter::InsertHashes(const Vector &hashes_v, const SelectionVector &sel, const idx_t count) {
	D_ASSERT(hashes_v.GetType() == LogicalType::HASH);
	if (count == 0) {
		return;
	}
	// VectorOperations::Hash produces a constant vector for a constant input, and a flat one otherwise
	if (hashes_v.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		InsertOne(*ConstantVector::GetData<hash_t>(hashes_v));
		return;
	}
	D_ASSERT(hashes_v.GetVectorType() == VectorType::FLAT_VECTOR);
	const auto hashes = FlatVector::GetData<uint64_t>(hashes_v);
	for (idx_t i = 0; i < count; i++) {
		// sel can be the incremental selection vector, so this cannot use get_index_unsafe
		InsertOne(hashes[sel.get_index(i)]);
	}
}

void BloomFilter::InsertHashes(const Vector &hashes_v) {
	for (auto hash : hashes_v.Values<uint64_t>()) {
		InsertOne(hash.GetValue());
	}
}

idx_t BloomFilter::LookupHashes(const Vector &hashes_v, SelectionVector &result_sel, const idx_t count) const {
	D_ASSERT(hashes_v.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(hashes_v.GetType() == LogicalType::HASH);

	const auto hashes = FlatVector::GetData<uint64_t>(hashes_v);
	const bool prefetch = count >= 128 && num_sectors * sizeof(uint64_t) >= 1024ULL * 1024ULL;
	idx_t found_count = 0;
	for (idx_t i = 0; i < count; i++) {
		result_sel.set_index(found_count, i);
		found_count += LookupOne(hashes[i]);
		if (prefetch) {
			PrefetchHashTableEntry(i, count, [&](idx_t next_idx) { return &bf[hashes[next_idx] & bitmask]; });
		}
	}
	return found_count;
}

idx_t BloomFilter::LookupHashes(const Vector &hashes_v, const SelectionVector &sel, SelectionVector &result_sel,
                                const idx_t count) const {
	D_ASSERT(hashes_v.GetVectorType() == VectorType::FLAT_VECTOR);
	D_ASSERT(hashes_v.GetType() == LogicalType::HASH);

	const auto hashes = FlatVector::GetData<uint64_t>(hashes_v);
	const bool prefetch = count >= 128 && num_sectors * sizeof(uint64_t) >= 1024ULL * 1024ULL;
	idx_t found_count = 0;
	for (idx_t i = 0; i < count; i++) {
		result_sel.set_index(found_count, i);
		found_count += LookupOne(hashes[sel.get_index_unsafe(i)]);
		if (prefetch) {
			PrefetchHashTableEntry(
			    i, count, [&](idx_t next_idx) { return &bf[hashes[sel.get_index_unsafe(next_idx)] & bitmask]; });
		}
	}
	return found_count;
}

inline void BloomFilter::InsertOne(const hash_t hash) {
	D_ASSERT(initialized);
	D_ASSERT(!sealed);
	const uint64_t bf_offset = hash & bitmask;
	const uint64_t mask = GetMask(hash);
	atomic<uint64_t> &slot = *reinterpret_cast<atomic<uint64_t> *>(&bf[bf_offset]);

	// duplicate keys find their bits already set - checking first keeps the shared sector read-only then,
	// instead of a contended read-modify-write
	if ((slot.load(std::memory_order_relaxed) & mask) != mask) {
		slot.fetch_or(mask, std::memory_order_relaxed);
	}
}

bool BloomFilter::LookupOne(const uint64_t hash) const {
	D_ASSERT(initialized);
	D_ASSERT(sealed);
	const uint64_t bf_offset = hash & bitmask;
	const uint64_t mask = GetMask(hash);
	atomic<uint64_t> &slot = *reinterpret_cast<atomic<uint64_t> *>(&bf[bf_offset]);
	auto bf_entry = slot.load(std::memory_order_relaxed);

	return (bf_entry & mask) == mask;
}

BloomFilterFunctionData::BloomFilterFunctionData(shared_ptr<const BloomFilter> filter_p, bool filters_null_values_p,
                                                 const string &key_column_name_p, const LogicalType &key_type_p,
                                                 float selectivity_threshold_p, idx_t n_vectors_to_check_p)
    : filter(std::move(filter_p)), filters_null_values(filters_null_values_p), key_column_name(key_column_name_p),
      key_type(key_type_p), selectivity_threshold(selectivity_threshold_p), n_vectors_to_check(n_vectors_to_check_p) {
}

unique_ptr<FunctionData> BloomFilterFunctionData::Copy() const {
	return make_uniq<BloomFilterFunctionData>(filter, filters_null_values, key_column_name, key_type,
	                                          selectivity_threshold, n_vectors_to_check);
}

bool BloomFilterFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<BloomFilterFunctionData>();
	return filter.get() == other.filter.get() && filters_null_values == other.filters_null_values &&
	       key_column_name == other.key_column_name && key_type == other.key_type;
}

struct BloomFilterLocalState : public SelectivityTrackingLocalState {
	BloomFilterLocalState(idx_t n_vectors_to_check, float selectivity_threshold)
	    : SelectivityTrackingLocalState(n_vectors_to_check, selectivity_threshold), hashes(LogicalType::HASH) {
	}

	void Prepare(idx_t count) {
		if (count <= capacity) {
			return;
		}
		hashes.Initialize(VectorDataInitialization::UNINITIALIZED, count);
		bloom_sel.Initialize(count);
		capacity = count;
	}

	Vector hashes;
	SelectionVector bloom_sel;
	SelectionVector result_sel;
	idx_t capacity = 0;
};

static idx_t SelectBloomFilter(Vector &input, const BloomFilterFunctionData &func_data, BloomFilterLocalState &state,
                               SelectionVector &result_sel, idx_t count) {
	D_ASSERT(func_data.filter);
	state.Prepare(count);
	auto &hashes = state.hashes;
	VectorOperations::Hash(input, hashes, count);
	auto validity = input.Validity();
	auto &bloom_sel = validity.CannotHaveNull() ? result_sel : state.bloom_sel;
	idx_t bloom_count;
	if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		bloom_count = func_data.filter->LookupOne(*ConstantVector::GetData<hash_t>(hashes)) ? count : 0;
		for (idx_t i = 0; i < bloom_count; i++) {
			bloom_sel.set_index(i, i);
		}
	} else {
		bloom_count = func_data.filter->LookupHashes(hashes, bloom_sel, count);
	}

	if (validity.CannotHaveNull()) {
		return bloom_count;
	}

	if (func_data.filters_null_values) {
		idx_t result_count = 0;
		for (idx_t i = 0; i < bloom_count; i++) {
			const auto row_idx = bloom_sel.get_index(i);
			result_sel.set_index(result_count, row_idx);
			result_count += validity.IsValid(row_idx);
		}
		return result_count;
	}

	idx_t result_count = 0;
	idx_t bloom_idx = 0;
	for (idx_t i = 0; i < count; i++) {
		const auto matched = bloom_idx < bloom_count && bloom_sel.get_index_unsafe(bloom_idx) == i;
		if (matched) {
			bloom_idx++;
		}
		bool passed;
		if (!validity.IsValid(i)) {
			passed = !func_data.filters_null_values;
		} else {
			passed = matched;
		}
		if (passed) {
			result_sel.set_index(result_count++, i);
		}
	}
	return result_count;
}

static unique_ptr<FunctionLocalState>
BloomFilterInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data) {
	auto &data = bind_data->Cast<BloomFilterFunctionData>();
	if (!data.filter) {
		return nullptr;
	}
	return make_uniq<BloomFilterLocalState>(data.n_vectors_to_check, data.selectivity_threshold);
}

static idx_t BloomFilterSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                               optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &func_data = func_expr.BindInfo()->Cast<BloomFilterFunctionData>();
	auto local_state_ptr = ExecuteFunctionState::GetFunctionState(state);
	auto count = args.size();
	if (!func_data.filter) {
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}
	D_ASSERT(local_state_ptr);
	auto &local_state = local_state_ptr->Cast<BloomFilterLocalState>();
	if (func_data.n_vectors_to_check != 0 && !local_state.IsActive()) {
		local_state.Update(0, 0);
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}

	auto &result_true_sel = GetFilterResultSelection(count, sel, true_sel, local_state.result_sel);
	auto approved_count = SelectBloomFilter(args.data[0], func_data, local_state, result_true_sel, count);
	approved_count = TranslateSelection(count, sel, result_true_sel, approved_count, true_sel, false_sel);
	if (func_data.n_vectors_to_check != 0) {
		local_state.Update(approved_count, count);
	}
	return approved_count;
}

template <class T>
static FilterPropagateResult TemplatedBloomFilterPrune(const BloomFilter &bf, const BaseStatistics &stats) {
	if (!NumericStats::HasMinMax(stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	const auto min = NumericStats::GetMin<T>(stats);
	const auto max = NumericStats::GetMax<T>(stats);
	if (min > max) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	T range_typed;
	idx_t range;
	if (!TrySubtractOperator::Operation(max, min, range_typed) || !TryCast::Operation(range_typed, range) ||
	    range >= DEFAULT_STANDARD_VECTOR_SIZE) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	T val = min;
	idx_t hits = 0;
	for (idx_t i = 0; i <= range; i++) {
		hits += bf.LookupOne(Hash(val));
		val += i < range;
	}

	if (hits == 0) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	if (hits == range + 1) {
		return FilterPropagateResult::FILTER_ALWAYS_TRUE;
	}
	return FilterPropagateResult::NO_PRUNING_POSSIBLE;
}

ScalarFunction BloomFilterScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, nullptr, TableFilterFunctions::Bind);
	func.GetSignature().GetParameter(0).SetName("col");
	func.SetInitStateCallback(BloomFilterInitLocalState);
	func.SetSelectCallback(BloomFilterSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(BloomFilterScalarFun::FilterPrune);
	func.SetSerializeCallback(TableFilterFunctionSerialize);
	func.SetDeserializeCallback(TableFilterFunctionDeserialize);
	return func;
}

string BloomFilterScalarFun::ToString(const string &column_name, const string &key_column_name) {
	return column_name + " IN BF(" + key_column_name + ")";
}

FilterPropagateResult BloomFilterScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<BloomFilterFunctionData>();
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

	switch (data.key_type.InternalType()) {
	case PhysicalType::UINT8:
		return TemplatedBloomFilterPrune<uint8_t>(*data.filter, stats);
	case PhysicalType::UINT16:
		return TemplatedBloomFilterPrune<uint16_t>(*data.filter, stats);
	case PhysicalType::UINT32:
		return TemplatedBloomFilterPrune<uint32_t>(*data.filter, stats);
	case PhysicalType::UINT64:
		return TemplatedBloomFilterPrune<uint64_t>(*data.filter, stats);
	case PhysicalType::UINT128:
		return TemplatedBloomFilterPrune<uhugeint_t>(*data.filter, stats);
	case PhysicalType::INT8:
		return TemplatedBloomFilterPrune<int8_t>(*data.filter, stats);
	case PhysicalType::INT16:
		return TemplatedBloomFilterPrune<int16_t>(*data.filter, stats);
	case PhysicalType::INT32:
		return TemplatedBloomFilterPrune<int32_t>(*data.filter, stats);
	case PhysicalType::INT64:
		return TemplatedBloomFilterPrune<int64_t>(*data.filter, stats);
	case PhysicalType::INT128:
		return TemplatedBloomFilterPrune<hugeint_t>(*data.filter, stats);
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

ScalarFunction TableFilterBloomFilterFun::GetFunction() {
	return BloomFilterScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
