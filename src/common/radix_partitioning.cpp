#include "duckdb/common/radix_partitioning.hpp"

#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"

namespace duckdb {

//! Templated radix partitioning constants, can be templated to the number of radix bits
template <idx_t radix_bits>
struct RadixPartitioningConstants {
	static hash_t ApplyMask(const hash_t hash) {
		return RadixPartitioning::ApplyMask(hash, radix_bits);
	}
};

template <class OP, class RETURN_TYPE, typename... ARGS>
RETURN_TYPE RadixBitsSwitch(const idx_t radix_bits, ARGS &&... args) {
	D_ASSERT(radix_bits <= RadixPartitioning::MAX_RADIX_BITS);
	switch (radix_bits) {
	case 0:
		return OP::template Operation<0>(std::forward<ARGS>(args)...);
	case 1:
		return OP::template Operation<1>(std::forward<ARGS>(args)...);
	case 2:
		return OP::template Operation<2>(std::forward<ARGS>(args)...);
	case 3:
		return OP::template Operation<3>(std::forward<ARGS>(args)...);
	case 4:
		return OP::template Operation<4>(std::forward<ARGS>(args)...);
	case 5: // LCOV_EXCL_START
		return OP::template Operation<5>(std::forward<ARGS>(args)...);
	case 6:
		return OP::template Operation<6>(std::forward<ARGS>(args)...);
	case 7:
		return OP::template Operation<7>(std::forward<ARGS>(args)...);
	case 8:
		return OP::template Operation<8>(std::forward<ARGS>(args)...);
	case 9:
		return OP::template Operation<9>(std::forward<ARGS>(args)...);
	case 10:
		return OP::template Operation<10>(std::forward<ARGS>(args)...);
	case 11:
		return OP::template Operation<11>(std::forward<ARGS>(args)...);
	case 12:
		return OP::template Operation<12>(std::forward<ARGS>(args)...);
	default:
		throw InternalException(
		    "radix_bits higher than RadixPartitioning::MAX_RADIX_BITS encountered in RadixBitsSwitch");
	} // LCOV_EXCL_STOP
}

struct SelectFunctor {
	template <idx_t radix_bits>
	static idx_t Operation(const Vector &hashes, const SelectionVector *sel, const idx_t count,
	                       const ValidityMask &partition_mask, SelectionVector *true_sel, SelectionVector *false_sel) {
		using CONSTANTS = RadixPartitioningConstants<radix_bits>;
		return UnaryExecutor::Select<hash_t>(
		    hashes, sel, count,
		    [&](const hash_t hash) {
			    const auto partition_idx = CONSTANTS::ApplyMask(hash);
			    return partition_mask.RowIsValidUnsafe(partition_idx);
		    },
		    true_sel, false_sel);
	}
};

idx_t RadixPartitioning::Select(const Vector &hashes, const SelectionVector *sel, const idx_t count,
                                const idx_t radix_bits, const ValidityMask &partition_mask, SelectionVector *true_sel,
                                SelectionVector *false_sel) {
	return RadixBitsSwitch<SelectFunctor, idx_t>(radix_bits, hashes, sel, count, partition_mask, true_sel, false_sel);
}

struct ComputePartitionIndicesFunctor {
	template <idx_t radix_bits>
	static void Operation(const Vector &hashes, Vector &partition_indices, const idx_t original_count,
	                      const SelectionVector &append_sel, const idx_t append_count) {
		using CONSTANTS = RadixPartitioningConstants<radix_bits>;
		if (!append_sel.IsSet() || hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			UnaryExecutor::Execute<hash_t, hash_t>(hashes, partition_indices, append_count,
			                                       [&](hash_t hash) { return CONSTANTS::ApplyMask(hash); });
		} else {
			partition_indices.SetVectorType(VectorType::FLAT_VECTOR);
			// We could just slice the "hashes" vector and use the UnaryExecutor
			// But slicing a dictionary vector causes SelectionData to be allocated
			// Instead, we just directly compute the partition indices using the selection vectors
			const auto source_data = hashes.Values<hash_t>();
			partition_indices.SetVectorType(VectorType::FLAT_VECTOR);
			auto target = FlatVector::Writer<hash_t>(partition_indices, append_count);
			for (idx_t i = 0; i < append_count; i++) {
				const auto source_idx = append_sel[i];
				target.WriteValue(CONSTANTS::ApplyMask(source_data[source_idx].GetValue()));
			}
		}
	}
};

//===--------------------------------------------------------------------===//
// Column Data Partitioning
//===--------------------------------------------------------------------===//
RadixPartitionedColumnData::RadixPartitionedColumnData(ClientContext &context_p, vector<LogicalType> types_p,
                                                       idx_t radix_bits_p, idx_t hash_col_idx_p)
    : PartitionedColumnData(PartitionedColumnDataType::RADIX, context_p, std::move(types_p)), radix_bits(radix_bits_p),
      hash_col_idx(hash_col_idx_p) {
	D_ASSERT(radix_bits <= RadixPartitioning::MAX_RADIX_BITS);
	D_ASSERT(hash_col_idx < types.size());
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	allocators->allocators.reserve(num_partitions);
	for (idx_t i = 0; i < num_partitions; i++) {
		CreateAllocator();
		allocators->allocators.back()->SetPartitionIndex(i);
	}
	D_ASSERT(allocators->allocators.size() == num_partitions);
}

RadixPartitionedColumnData::RadixPartitionedColumnData(const RadixPartitionedColumnData &other)
    : PartitionedColumnData(other), radix_bits(other.radix_bits), hash_col_idx(other.hash_col_idx) {
	for (idx_t i = 0; i < RadixPartitioning::NumberOfPartitions(radix_bits); i++) {
		partitions.emplace_back(CreatePartitionCollection(i));
	}
}

RadixPartitionedColumnData::~RadixPartitionedColumnData() {
}

void RadixPartitionedColumnData::InitializeAppendStateInternal(PartitionedColumnDataAppendState &state) const {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	state.partition_append_states.reserve(num_partitions);
	state.partition_buffers.reserve(num_partitions);
	for (idx_t i = 0; i < num_partitions; i++) {
		state.partition_append_states.emplace_back(make_uniq<ColumnDataAppendState>());
		partitions[i]->InitializeAppend(*state.partition_append_states[i]);
		state.partition_buffers.emplace_back(CreatePartitionBuffer());
	}

	// Initialize fixed-size map
	state.fixed_partition_entries.resize(RadixPartitioning::NumberOfPartitions(radix_bits));
}

void RadixPartitionedColumnData::ComputePartitionIndices(PartitionedColumnDataAppendState &state, DataChunk &input) {
	D_ASSERT(partitions.size() == RadixPartitioning::NumberOfPartitions(radix_bits));
	D_ASSERT(state.partition_buffers.size() == RadixPartitioning::NumberOfPartitions(radix_bits));
	RadixBitsSwitch<ComputePartitionIndicesFunctor, void>(radix_bits, input.data[hash_col_idx], state.partition_indices,
	                                                      input.size(), *FlatVector::IncrementalSelectionVector(),
	                                                      input.size());
}

//===--------------------------------------------------------------------===//
// Tuple Data Partitioning
//===--------------------------------------------------------------------===//
RadixPartitionedTupleData::RadixPartitionedTupleData(BufferManager &buffer_manager,
                                                     shared_ptr<TupleDataLayout> layout_ptr, const MemoryTag tag,
                                                     const idx_t radix_bits_p, const idx_t hash_col_idx_p,
                                                     QueryContext context)
    : PartitionedTupleData(PartitionedTupleDataType::RADIX, buffer_manager, layout_ptr, tag, context),
      radix_bits(radix_bits_p), hash_col_idx(hash_col_idx_p) {
	D_ASSERT(radix_bits <= RadixPartitioning::MAX_RADIX_BITS);
	D_ASSERT(hash_col_idx < layout.GetTypes().size());
	Initialize();
}

RadixPartitionedTupleData::RadixPartitionedTupleData(RadixPartitionedTupleData &other)
    : PartitionedTupleData(other), radix_bits(other.radix_bits), hash_col_idx(other.hash_col_idx) {
	Initialize();
}

RadixPartitionedTupleData::~RadixPartitionedTupleData() {
}

void RadixPartitionedTupleData::Initialize() {
	const auto num_partitions = RadixPartitioning::NumberOfPartitions(radix_bits);
	for (idx_t i = 0; i < num_partitions; i++) {
		partitions.emplace_back(CreatePartitionCollection());
		partitions.back()->SetPartitionIndex(i);
	}
}

void RadixPartitionedTupleData::ComputePartitionIndices(PartitionedTupleDataAppendState &state, DataChunk &input,
                                                        const SelectionVector &append_sel, const idx_t append_count) {
	D_ASSERT(partitions.size() == RadixPartitioning::NumberOfPartitions(radix_bits));
	RadixBitsSwitch<ComputePartitionIndicesFunctor, void>(radix_bits, input.data[hash_col_idx], state.partition_indices,
	                                                      input.size(), append_sel, append_count);
}

void RadixPartitionedTupleData::ComputePartitionIndices(Vector &row_locations, idx_t count, Vector &partition_indices,
                                                        unique_ptr<Vector> &utility_vector,
                                                        const SelectionVector &append_sel) const {
	if (radix_bits == 0) {
		// Keep the buffer capacity for subsequent appends from vectors.
		partition_indices.SetVectorType(VectorType::CONSTANT_VECTOR);
		partition_indices.SetValue(0, Value::UBIGINT(0));
		FlatVector::SetSize(partition_indices, count_t(count));
		return;
	}
	if (!utility_vector) {
		utility_vector = make_uniq<Vector>(LogicalType::HASH);
	}
	Vector &intermediate = *utility_vector;
	intermediate.SetVectorType(VectorType::FLAT_VECTOR);
	partitions[0]->Gather(row_locations, append_sel, count, hash_col_idx, intermediate,
	                      *FlatVector::IncrementalSelectionVector(), nullptr);
	RadixBitsSwitch<ComputePartitionIndicesFunctor, void>(radix_bits, intermediate, partition_indices, count,
	                                                      *FlatVector::IncrementalSelectionVector(), count);
}

void RadixPartitionedTupleData::RepartitionFinalizeStates(PartitionedTupleData &old_partitioned_data,
                                                          PartitionedTupleData &new_partitioned_data,
                                                          PartitionedTupleDataAppendState &state,
                                                          idx_t finished_partition_idx) const {
	D_ASSERT(old_partitioned_data.GetType() == PartitionedTupleDataType::RADIX &&
	         new_partitioned_data.GetType() == PartitionedTupleDataType::RADIX);
	const auto &old_radix_partitions = old_partitioned_data.Cast<RadixPartitionedTupleData>();
	const auto &new_radix_partitions = new_partitioned_data.Cast<RadixPartitionedTupleData>();
	const auto old_radix_bits = old_radix_partitions.GetRadixBits();
	const auto new_radix_bits = new_radix_partitions.GetRadixBits();
	D_ASSERT(new_radix_bits > old_radix_bits);

	// We take the most significant digits as the partition index
	// When repartitioning, e.g., partition 0 from "old" goes into the first N partitions in "new"
	// When partition 0 is done, we can already finalize the append states, unpinning blocks
	const auto multiplier = RadixPartitioning::NumberOfPartitions(new_radix_bits - old_radix_bits);
	const auto from_idx = finished_partition_idx * multiplier;
	const auto to_idx = from_idx + multiplier;
	auto &partitions = new_partitioned_data.GetPartitions();
	for (idx_t partition_index = from_idx; partition_index < to_idx; partition_index++) {
		auto &partition = *partitions[partition_index];
		auto &partition_pin_state = state.partition_pin_states[partition_index];
		partition.FinalizePinState(partition_pin_state);
	}
}

} // namespace duckdb
