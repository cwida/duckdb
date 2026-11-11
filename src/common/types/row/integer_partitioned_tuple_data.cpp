#include "duckdb/common/types/row/integer_partitioned_tuple_data.hpp"

#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"

#include <type_traits>

namespace duckdb {

IntegerPartitionedTupleData::IntegerPartitionedTupleData(BufferManager &buffer_manager,
                                                         shared_ptr<TupleDataLayout> layout_ptr, const MemoryTag tag,
                                                         const idx_t key_col_idx_p, const Value &min_value_p,
                                                         const idx_t value_count_p, QueryContext context)
    : PartitionedTupleData(PartitionedTupleDataType::INTEGER, buffer_manager, layout_ptr, tag, context),
      key_col_idx(key_col_idx_p), min_value(min_value_p), value_count(value_count_p) {
	D_ASSERT(key_col_idx < layout.GetTypes().size());
	D_ASSERT(SupportsType(layout.GetTypes()[key_col_idx]));
	D_ASSERT(min_value.type() == layout.GetTypes()[key_col_idx]);
	D_ASSERT(value_count > 0);
	Initialize();
}

IntegerPartitionedTupleData::IntegerPartitionedTupleData(IntegerPartitionedTupleData &other)
    : PartitionedTupleData(other), key_col_idx(other.key_col_idx), min_value(other.min_value),
      value_count(other.value_count) {
	Initialize();
}

IntegerPartitionedTupleData::~IntegerPartitionedTupleData() {
}

bool IntegerPartitionedTupleData::SupportsType(const LogicalType &type) {
	if (!type.IsIntegral()) {
		return false;
	}
	switch (type.InternalType()) {
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
		return true;
	default:
		return false;
	}
}

//! The largest key range that is partitioned on directly. Beyond a few hundred keys an input chunk touches almost
//! every partition with a row or two, so the per-partition build and pin work of the append dominates, and every
//! thread keeps a row block and a heap block per partition: on 8M narrow rows at 4 threads the direct path wins up to
//! 256 keys and loses from about 1000 keys (1.65x slower at 3000). Larger ranges take the radix path.
static constexpr idx_t MAX_INTEGER_PARTITION_KEYS = 256;

//! The largest key range the memory budget allows: every partition keeps a partially filled row block and heap block
//! pinned per thread while appending (zero if not even the NULL and overflow partitions fit)
static idx_t MaxIntegerPartitionKeys(ClientContext &context) {
	auto &buffer_manager = BufferManager::GetBufferManager(context);
	const auto memory_per_thread = PhysicalOperator::GetMaxThreadMemory(context);
	const auto partition_memory = 2 * buffer_manager.GetBlockAllocSize();
	const auto memory_partitions = memory_per_thread / partition_memory;
	const auto extra_partitions = IntegerPartitionedTupleData::PartitionCountFor(0);
	if (memory_partitions <= extra_partitions) {
		return 0;
	}
	return MinValue<idx_t>(MAX_INTEGER_PARTITION_KEYS, memory_partitions - extra_partitions);
}

bool IntegerPartitionedTupleData::TryBoundedKey(ClientContext &context, const LogicalType &key_type,
                                                optional_ptr<const BaseStatistics> stats, Value &min_value,
                                                idx_t &value_count) {
	if (!stats || !SupportsType(key_type) || stats->GetType() != key_type) {
		return false;
	}
	if (!NumericStats::HasMinMax(*stats)) {
		return false;
	}
	auto min = NumericStats::Min(*stats);
	auto max = NumericStats::Max(*stats);
	if (min.IsNull() || max.IsNull()) {
		return false;
	}
	const auto lo = min.GetValue<hugeint_t>();
	const auto hi = max.GetValue<hugeint_t>();
	if (hi < lo) {
		return false;
	}
	const auto range = hi - lo + 1;
	const auto max_keys = MaxIntegerPartitionKeys(context);
	if (max_keys == 0 || range > hugeint_t(static_cast<int64_t>(max_keys))) {
		return false;
	}
	value_count = range.lower;
	min_value = std::move(min);
	return true;
}

void IntegerPartitionedTupleData::Initialize() {
	const auto num_partitions = PartitionCountFor(value_count);
	for (idx_t i = 0; i < num_partitions; i++) {
		partitions.emplace_back(CreatePartitionCollection());
		partitions.back()->SetPartitionIndex(i);
	}
}

namespace {

//! Unsigned subtraction wraps, so keys below the minimum end up far above value_count and land in the overflow
//! partition without any signed overflow
template <class T>
void TemplatedComputePartitionIndices(const Vector &keys, const SelectionVector &append_sel, const idx_t append_count,
                                      const T min, const idx_t value_count, const idx_t null_partition,
                                      const idx_t overflow_partition, Vector &partition_indices) {
	using UNSIGNED = typename std::make_unsigned<T>::type;
	const auto unsigned_min = static_cast<UNSIGNED>(min);
	VectorIterator<T> values(keys);
	partition_indices.SetVectorType(VectorType::FLAT_VECTOR);
	auto writer = FlatVector::Writer<idx_t>(partition_indices, append_count);
	for (idx_t i = 0; i < append_count; i++) {
		const auto entry = values[append_sel.get_index(i)];
		if (!entry.IsValid()) {
			writer.WriteValue(null_partition);
			continue;
		}
		const auto offset =
		    static_cast<idx_t>(static_cast<UNSIGNED>(static_cast<UNSIGNED>(entry.GetValue()) - unsigned_min));
		writer.WriteValue(offset < value_count ? offset : overflow_partition);
	}
}

} // namespace

void IntegerPartitionedTupleData::ComputePartitionIndices(const Vector &keys, const SelectionVector &append_sel,
                                                          const idx_t append_count, Vector &partition_indices) const {
	const auto null_partition = NullPartition();
	const auto overflow_partition = OverflowPartition();
	switch (keys.GetType().InternalType()) {
	case PhysicalType::INT8:
		TemplatedComputePartitionIndices<int8_t>(keys, append_sel, append_count, min_value.GetValue<int8_t>(),
		                                         value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::INT16:
		TemplatedComputePartitionIndices<int16_t>(keys, append_sel, append_count, min_value.GetValue<int16_t>(),
		                                          value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::INT32:
		TemplatedComputePartitionIndices<int32_t>(keys, append_sel, append_count, min_value.GetValue<int32_t>(),
		                                          value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::INT64:
		TemplatedComputePartitionIndices<int64_t>(keys, append_sel, append_count, min_value.GetValue<int64_t>(),
		                                          value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::UINT8:
		TemplatedComputePartitionIndices<uint8_t>(keys, append_sel, append_count, min_value.GetValue<uint8_t>(),
		                                          value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::UINT16:
		TemplatedComputePartitionIndices<uint16_t>(keys, append_sel, append_count, min_value.GetValue<uint16_t>(),
		                                           value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::UINT32:
		TemplatedComputePartitionIndices<uint32_t>(keys, append_sel, append_count, min_value.GetValue<uint32_t>(),
		                                           value_count, null_partition, overflow_partition, partition_indices);
		break;
	case PhysicalType::UINT64:
		TemplatedComputePartitionIndices<uint64_t>(keys, append_sel, append_count, min_value.GetValue<uint64_t>(),
		                                           value_count, null_partition, overflow_partition, partition_indices);
		break;
	default:
		throw InternalException("Unsupported key type for IntegerPartitionedTupleData");
	}
}

void IntegerPartitionedTupleData::ComputePartitionIndices(PartitionedTupleDataAppendState &state, DataChunk &input,
                                                          const SelectionVector &append_sel, const idx_t append_count) {
	D_ASSERT(partitions.size() == PartitionCountFor(value_count));
	ComputePartitionIndices(input.data[key_col_idx], append_sel, append_count, state.partition_indices);
}

void IntegerPartitionedTupleData::ComputePartitionIndices(Vector &row_locations, idx_t count, Vector &partition_indices,
                                                          unique_ptr<Vector> &utility_vector,
                                                          const SelectionVector &append_sel) const {
	const auto &key_type = layout.GetTypes()[key_col_idx];
	if (!utility_vector || utility_vector->GetType() != key_type) {
		utility_vector = make_uniq<Vector>(key_type);
	}
	auto &keys = *utility_vector;
	keys.SetVectorType(VectorType::FLAT_VECTOR);
	partitions[0]->Gather(row_locations, append_sel, count, key_col_idx, keys,
	                      *FlatVector::IncrementalSelectionVector(), nullptr);
	FlatVector::SetSize(keys, count);
	ComputePartitionIndices(keys, *FlatVector::IncrementalSelectionVector(), count, partition_indices);
}

} // namespace duckdb
