#include "duckdb/storage/compression/roaring/roaring.hpp"

#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/bitpacking.hpp"

namespace duckdb {

namespace roaring {

ContainerMetadata ContainerMetadata::CreateMetadata(uint16_t count, uint16_t array_null, uint16_t array_non_null,
                                                    uint16_t runs) {
	const bool can_use_null_array = array_null < MAX_ARRAY_IDX;
	const bool can_use_non_null_array = array_non_null < MAX_ARRAY_IDX;

	const bool can_use_run = runs < MAX_RUN_IDX;

	const bool can_use_array = can_use_null_array || can_use_non_null_array;
	if (!can_use_array && !can_use_run) {
		// Can not efficiently encode at all, write it as bitset
		return ContainerMetadata::BitsetContainer(count);
	}
	uint16_t null_array_cost = array_null < COMPRESSED_ARRAY_THRESHOLD
	                               ? array_null * sizeof(uint16_t)
	                               : COMPRESSED_SEGMENT_COUNT + (array_null * sizeof(uint8_t));
	uint16_t non_null_array_cost = array_non_null < COMPRESSED_ARRAY_THRESHOLD
	                                   ? array_non_null * sizeof(uint16_t)
	                                   : COMPRESSED_SEGMENT_COUNT + (array_non_null * sizeof(uint8_t));

	uint16_t lowest_array_cost = MinValue<uint16_t>(null_array_cost, non_null_array_cost);
	uint16_t lowest_run_cost = runs < COMPRESSED_RUN_THRESHOLD ? runs * sizeof(uint32_t)
	                                                           : COMPRESSED_SEGMENT_COUNT + (runs * sizeof(uint16_t));
	uint16_t bitset_cost =
	    (AlignValue<uint16_t, ValidityMask::BITS_PER_VALUE>(count) / ValidityMask::BITS_PER_VALUE) * sizeof(validity_t);
	if (MinValue<uint16_t>(lowest_array_cost, lowest_run_cost) > bitset_cost) {
		// The amount of values is too small, better off using bitset
		// we can detect this at decompression because we know how many values are left
		return ContainerMetadata::BitsetContainer(count);
	}

	if (lowest_array_cost <= lowest_run_cost) {
		if (array_null <= array_non_null) {
			return ContainerMetadata::ArrayContainer(array_null, NULLS);
		} else {
			return ContainerMetadata::ArrayContainer(array_non_null, NON_NULLS);
		}
	} else {
		return ContainerMetadata::RunContainer(runs);
	}
}

idx_t ContainerMetadata::GetDataSizeInBytes(idx_t container_size) const {
	if (IsUncompressed()) {
		return (container_size / ValidityMask::BITS_PER_VALUE) * sizeof(validity_t);
	}
	if (IsRun()) {
		auto number_of_runs = NumberOfRuns();
		if (number_of_runs >= COMPRESSED_RUN_THRESHOLD) {
			return COMPRESSED_SEGMENT_COUNT + (sizeof(uint8_t) * number_of_runs * 2);
		} else {
			return sizeof(RunContainerRLEPair) * number_of_runs;
		}
	} else {
		auto cardinality = Cardinality();
		if (cardinality >= COMPRESSED_ARRAY_THRESHOLD) {
			return COMPRESSED_SEGMENT_COUNT + (sizeof(uint8_t) * cardinality);
		} else {
			return sizeof(uint16_t) * cardinality;
		}
	}
}

ContainerMetadataCollection::ContainerMetadataCollection() {
}

void ContainerMetadataCollection::AddMetadata(ContainerMetadata metadata) {
	if (metadata.IsRun()) {
		AddRunContainer(metadata.NumberOfRuns(), metadata.IsInverted());
	} else if (metadata.IsUncompressed()) {
		AddBitsetContainer();
	} else {
		AddArrayContainer(metadata.Cardinality(), metadata.IsInverted());
	}
}

idx_t ContainerMetadataCollection::GetMetadataSizeForSegment() const {
	idx_t runs_count = GetRunContainerCount();
	idx_t arrays_count = GetArrayAndBitsetContainerCount();
	return GetMetadataSize(runs_count + arrays_count, runs_count, arrays_count);
}

idx_t ContainerMetadataCollection::GetMetadataSize(idx_t container_count, idx_t run_containers,
                                                   idx_t array_containers) const {
	idx_t types_size = BitpackingPrimitives::GetRequiredSize(container_count, CONTAINER_TYPE_BITWIDTH);
	idx_t runs_size = BitpackingPrimitives::GetRequiredSize(run_containers, RUN_CONTAINER_SIZE_BITWIDTH);
	idx_t arrays_size = sizeof(uint8_t) * array_containers;
	return types_size + runs_size + arrays_size;
}

idx_t ContainerMetadataCollection::GetRunContainerCount() const {
	return runs_in_segment;
}
idx_t ContainerMetadataCollection::GetArrayAndBitsetContainerCount() const {
	return arrays_in_segment;
}

void ContainerMetadataCollection::FlushSegment() {
	runs_in_segment = 0;
	count_in_segment = 0;
	arrays_in_segment = 0;
}

void ContainerMetadataCollection::Reset() {
	FlushSegment();
	container_type.clear();
	number_of_runs.clear();
	cardinality.clear();
}

// Write the metadata for the current segment
idx_t ContainerMetadataCollection::Serialize(data_ptr_t dest) const {
	// Element sizes (in bits) for written metadata
	// +======================================+
	// |mmmmmm|rrrrrr|aaaaaaa|                |
	// +======================================+
	//
	// m: 2: (1: is_run, 1: is_inverted)
	// r: 7: number_of_runs
	// a: 8: cardinality

	idx_t types_size = BitpackingPrimitives::GetRequiredSize(count_in_segment, CONTAINER_TYPE_BITWIDTH);
	idx_t runs_size = BitpackingPrimitives::GetRequiredSize(runs_in_segment, RUN_CONTAINER_SIZE_BITWIDTH);
	idx_t arrays_size = sizeof(uint8_t) * arrays_in_segment;

	idx_t types_offset = container_type.size() - count_in_segment;
	data_ptr_t types_data = (data_ptr_t)(container_type.data()); // NOLINT: c-style cast (for const)
	BitpackingPrimitives::PackBuffer<uint8_t>(dest, types_data + types_offset, count_in_segment,
	                                          CONTAINER_TYPE_BITWIDTH);
	dest += types_size;

	if (!number_of_runs.empty()) {
		idx_t runs_offset = number_of_runs.size() - runs_in_segment;
		data_ptr_t run_data = (data_ptr_t)(number_of_runs.data()); // NOLINT: c-style cast (for const)
		BitpackingPrimitives::PackBuffer<uint8_t>(dest, run_data + runs_offset, runs_in_segment,
		                                          RUN_CONTAINER_SIZE_BITWIDTH);
		dest += runs_size;
	}

	if (!cardinality.empty()) {
		idx_t arrays_offset = cardinality.size() - arrays_in_segment;
		data_ptr_t arrays_data = (data_ptr_t)(cardinality.data()); // NOLINT: c-style cast (for const)
		memcpy(dest, arrays_data + arrays_offset, sizeof(uint8_t) * arrays_in_segment);
	}
	return types_size + runs_size + arrays_size;
}

[[noreturn]] static void ThrowRoaringContainerCountOutOfBounds() {
	throw DataCorruptionException("Corrupted Roaring segment: container count exceeds available metadata space");
}

void ContainerMetadataCollection::Deserialize(CompressionSegmentReader reader, idx_t container_count) {
	const auto group_size = BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE;
	const auto group_count = container_count / group_size + (container_count % group_size != 0);
	const auto type_group_size = group_size * CONTAINER_TYPE_BITWIDTH / 8;
	if (group_count > reader.Remaining() / type_group_size) {
		ThrowRoaringContainerCountOutOfBounds();
	}

	// Metadata unpacking reads and writes complete groups, including the final padded group.
	container_type.resize(AlignValue<idx_t, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE>(container_count));
	count_in_segment = container_count;

	// Load the types of the containers
	idx_t types_size = BitpackingPrimitives::GetRequiredSize(container_type.size(), 2);
	auto types = reader.ReadBytes(types_size);
	BitpackingPrimitives::UnPackBuffer<uint8_t>(container_type.data(), types.data(), container_count, 2, true);

	// Figure out how many are run containers
	idx_t runs_count = 0;
	for (idx_t i = 0; i < container_count; i++) {
		auto type = container_type[i];
		runs_count += ((type >> 1) & 1) == 1;
	}
	runs_in_segment = runs_count;
	number_of_runs.resize(AlignValue<idx_t, BitpackingPrimitives::BITPACKING_ALGORITHM_GROUP_SIZE>(runs_count));
	cardinality.resize(container_count - runs_count);

	// Load the run containers
	if (runs_count) {
		idx_t runs_size = BitpackingPrimitives::GetRequiredSize(runs_count, RUN_CONTAINER_SIZE_BITWIDTH);
		auto runs = reader.ReadBytes(runs_size);
		BitpackingPrimitives::UnPackBuffer<uint8_t>(number_of_runs.data(), runs.data(), runs_count,
		                                            RUN_CONTAINER_SIZE_BITWIDTH, true);
	}

	// Load the array/bitset containers
	if (!cardinality.empty()) {
		idx_t arrays_size = sizeof(uint8_t) * cardinality.size();
		arrays_in_segment = arrays_size;
		reader.ReadBytesInto(cardinality.data(), arrays_size);
	}
}

void ContainerMetadataCollection::AddBitsetContainer() {
	AddContainerType(false, false);
	cardinality.push_back(BITSET_CONTAINER_SENTINEL_VALUE);
	arrays_in_segment++;
	count_in_segment++;
}

void ContainerMetadataCollection::AddArrayContainer(idx_t amount, bool is_inverted) {
	AddContainerType(false, is_inverted);
	D_ASSERT(amount < MAX_ARRAY_IDX);
	cardinality.push_back(NumericCast<uint8_t>(amount));
	arrays_in_segment++;
	count_in_segment++;
}

void ContainerMetadataCollection::AddRunContainer(idx_t amount, bool is_inverted) {
	AddContainerType(true, is_inverted);
	D_ASSERT(amount < MAX_RUN_IDX);
	number_of_runs.push_back(NumericCast<uint8_t>(amount));
	runs_in_segment++;
	count_in_segment++;
}

void ContainerMetadataCollection::AddContainerType(bool is_run, bool is_inverted) {
	uint8_t type = 0;
	if (is_run) {
		type |= IS_RUN_FLAG;
	}
	if (is_inverted) {
		type |= IS_INVERTED_FLAG;
	}
	container_type.push_back(type);
}

ContainerMetadataCollectionScanner::ContainerMetadataCollectionScanner(ContainerMetadataCollection &collection)
    : collection(collection) {
}

ContainerMetadata ContainerMetadataCollectionScanner::GetNext() {
	D_ASSERT(idx < collection.count_in_segment);
	auto type = collection.container_type[idx++];
	const bool is_inverted = (type & 1) == 1;
	const bool is_run = ((type >> 1) & 1) == 1;
	uint8_t amount;
	if (is_run) {
		amount = collection.number_of_runs[run_idx++];
	} else {
		amount = collection.cardinality[array_idx++];
	}
	if (is_run) {
		return ContainerMetadata::RunContainer(amount);
	}
	if (amount == BITSET_CONTAINER_SENTINEL_VALUE) {
		return ContainerMetadata::BitsetContainer(amount);
	}
	return ContainerMetadata::ArrayContainer(amount, is_inverted);
}

} // namespace roaring

} // namespace duckdb
