#include "duckdb/storage/compression/roaring/roaring.hpp"

#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/storage/table/column_segment.hpp"
#include "duckdb/storage/segment/uncompressed.hpp"

namespace duckdb {

namespace roaring {

//===--------------------------------------------------------------------===//
// Scan
//===--------------------------------------------------------------------===//

ContainerSegmentScan::ContainerSegmentScan(unsafe_array_ptr<const uint8_t> data) : segments(data), index(0), count(0) {
}

// Returns the base of the current segment, forwarding the index if the segment is depleted of values
uint16_t ContainerSegmentScan::operator++(int) {
	while (index < COMPRESSED_SEGMENT_COUNT && count >= segments[index]) {
		count = 0;
		index++;
	}
	count++;

	// index == COMPRESSED_SEGMENT_COUNT is allowed for runs, as the last run could end at ROARING_CONTAINER_SIZE
	D_ASSERT(index <= COMPRESSED_SEGMENT_COUNT);
	if (index < COMPRESSED_SEGMENT_COUNT) {
		D_ASSERT(segments[index] != 0);
	}
	uint16_t base = static_cast<uint16_t>(index) * COMPRESSED_SEGMENT_SIZE;
	return base;
}

//===--------------------------------------------------------------------===//
// ContainerScanState
//===--------------------------------------------------------------------===//

//! RunContainer

[[noreturn]] static void ThrowRoaringInvalidRun() {
	throw DataCorruptionException("Corrupted Roaring segment: invalid run bounds or ordering");
}

template <class READER>
RunContainerScanState<READER>::RunContainerScanState(idx_t container_index, idx_t container_size, idx_t count)
    : ContainerScanState(container_index, container_size), count(count) {
}

UncompressedRunContainerScanState::UncompressedRunContainerScanState(idx_t container_index, idx_t container_size,
                                                                     idx_t count,
                                                                     unsafe_array_ptr<const RunContainerRLEPair> data)
    : RunContainerScanState(container_index, container_size, count), data(data) {
}

template <class READER>
void RunContainerScanState<READER>::ScanPartial(ValidityMask &result_mask, idx_t result_offset, idx_t to_scan) {
	// This method assumes that the validity mask starts off as having all bits set for the entries that are being
	// scanned.

	idx_t result_idx = 0;
	if (!run_index) {
		LoadNextRun();
	}
	while (!finished && result_idx < to_scan) {
		// Either we are already inside a run, then 'start_of_run' will be scanned_count
		// or we're skipping values until the run begins
		auto start_of_run =
		    MaxValue<idx_t>(MinValue<idx_t>(run.start, scanned_count + to_scan), scanned_count + result_idx);
		result_idx = start_of_run - scanned_count;

		// How much of the run are we covering?
		idx_t run_end = run.start + 1 + run.length;
		auto run_or_scan_end = MinValue<idx_t>(run_end, scanned_count + to_scan);

		// Process the run
		D_ASSERT(run_or_scan_end >= start_of_run);
		if (run_or_scan_end > start_of_run) {
			idx_t amount = run_or_scan_end - start_of_run;
			idx_t start = result_offset + result_idx;
			idx_t end = start + amount;
			SetInvalidRange(result_mask, start, end);
		}

		result_idx += run_or_scan_end - start_of_run;
		if (scanned_count + result_idx == run_end) {
			// Fully processed the current run
			LoadNextRun();
		}
	}
	scanned_count += to_scan;
}

template <class READER>
void RunContainerScanState<READER>::Skip(idx_t to_skip) {
	idx_t end = scanned_count + to_skip;
	if (!run_index) {
		LoadNextRun();
	}
	while (scanned_count < end && !finished) {
		idx_t run_end = run.start + 1 + run.length;
		scanned_count = MinValue<idx_t>(run_end, end);
		if (scanned_count == run_end) {
			LoadNextRun();
		}
	}
	// In case run_index has already reached count
	scanned_count = end;
}

void UncompressedRunContainerScanState::VerifyWrittenData() const {
#ifdef DEBUG
	uint16_t index = 0;
	for (idx_t i = 0; i < count; i++) {
		auto run = data[i];
		D_ASSERT(run.start >= index);
		index = run.start + 1 + run.length;
	}
#endif
}

void UncompressedRunContainerScanState::LoadNextRunInternal() {
	if (run_index >= count) {
		finished = true;
		return;
	}
	const idx_t previous_end = run_index ? run.start + 1 + run.length : 0;
	run = data[run_index];
	const idx_t end = run.start + 1 + run.length;

	// Finalization can make the last run end at container_size + 1.
	const auto max_end = container_size + (run_index + 1 == count);
	if (run.start < previous_end || run.start >= container_size || end > max_end) {
		ThrowRoaringInvalidRun();
	}
	run_index++;
}

CompressedRunContainerScanState::CompressedRunContainerScanState(idx_t container_index, idx_t container_size,
                                                                 idx_t count, unsafe_array_ptr<const uint8_t> segments,
                                                                 unsafe_array_ptr<const uint8_t> data)
    : RunContainerScanState(container_index, container_size, count), data(data), segments(segments), segment(segments) {
	D_ASSERT(count >= COMPRESSED_RUN_THRESHOLD);
	//! Used by VerifyWrittenData, have to use it to avoid a compiler warning/error
	(void)this->segments;
}

void CompressedRunContainerScanState::LoadNextRunInternal() {
	if (run_index >= count) {
		finished = true;
		return;
	}
	uint16_t start = segment++;
	start += data[(run_index * 2) + 0];

	uint16_t end = segment++;
	end += data[(run_index * 2) + 1];

	const idx_t previous_end = run_index ? run.start + 1 + run.length : 0;
	if (end <= start || end > container_size || start < previous_end) {
		ThrowRoaringInvalidRun();
	}
	run = RunContainerRLEPair {start, static_cast<uint16_t>(end - 1 - start)};
	run_index++;
}

void CompressedRunContainerScanState::VerifyWrittenData() const {
#ifdef DEBUG
	uint16_t index = 0;
	ContainerSegmentScan verify_segment(segments);
	for (idx_t i = 0; i < count; i++) {
		// Get the start index of the run
		uint16_t start = verify_segment++;
		start += data[(i * 2) + 0];

		// Get the end index of the run
		uint16_t end = verify_segment++;
		end += data[(i * 2) + 1];

		D_ASSERT(!i || start >= index);
		D_ASSERT(end > start);
		index = end;
	}
#endif
}

template struct RunContainerScanState<UncompressedRunContainerScanState>;
template struct RunContainerScanState<CompressedRunContainerScanState>;

//! ArrayContainer

[[noreturn]] void ThrowRoaringInvalidArray() {
	throw DataCorruptionException("Corrupted Roaring segment: invalid array position or ordering");
}

//! BitsetContainer

BitsetContainerScanState::BitsetContainerScanState(idx_t container_index, idx_t container_size,
                                                   unsafe_array_ptr<const validity_t> bitset)
    : ContainerScanState(container_index, container_size), bitset(bitset) {
}

void BitsetContainerScanState::ScanPartial(ValidityMask &result_mask, idx_t result_offset, idx_t to_scan) {
	if (!result_offset && (to_scan % ValidityMask::BITS_PER_VALUE) == 0 &&
	    (scanned_count % ValidityMask::BITS_PER_VALUE) == 0) {
		ValidityUncompressed::AlignedScan(bitset, scanned_count, result_mask, to_scan);
	} else {
		ValidityUncompressed::UnalignedScan(bitset, container_size, scanned_count, result_mask, result_offset, to_scan);
	}
	scanned_count += to_scan;
}

void BitsetContainerScanState::Skip(idx_t to_skip) {
	// NO OP: we only need to forward scanned_count
	scanned_count += to_skip;
}

void BitsetContainerScanState::VerifyWrittenData() const {
	// uncompressed, nothing to verify
	return;
}

RoaringScanState::SegmentLayout RoaringScanState::ReadLayout(const BufferHandle &handle, const ColumnSegment &segment) {
	auto reader = CompressionSegmentReader::FromSegment(handle, segment, "Roaring segment");
	const auto metadata_offset = reader.Read<idx_t>();
	// The metadata offset is relative to the end of the header.
	auto body = reader.ReadSubReader(reader.Remaining(), "Roaring segment body");
	body.SetPosition(metadata_offset);
	auto metadata = body.ReadSubReader(body.Remaining(), "Roaring metadata");
	auto data = body.GetSubReader(0, metadata_offset, "Roaring container data");
	return {data, metadata};
}

RoaringScanState::RoaringScanState(const QueryContext &context, ColumnSegment &segment)
    : handle(BufferManager::GetBufferManager(segment.GetDatabase()).Pin(context, segment.GetBlockHandle())),
      segment(segment), layout(ReadLayout(handle, segment)) {
	// Deserialize the container metadata for this segment
	auto segment_count = segment.count.load();
	auto container_count = segment_count / ROARING_CONTAINER_SIZE;
	if (segment_count % ROARING_CONTAINER_SIZE != 0) {
		container_count++;
	}
	metadata_collection.Deserialize(layout.metadata, container_count);
	ContainerMetadataCollectionScanner scanner(metadata_collection);
	container_metadata.reserve(container_count);
	data_start_position.reserve(container_count);
	auto data = layout.data;
	for (idx_t i = 0; i < container_count; i++) {
		auto metadata = scanner.GetNext();
		container_metadata.push_back(metadata);
		if (metadata.IsUncompressed()) {
			data.Align(sizeof(idx_t));
		} else if (metadata.IsArray() && metadata.Cardinality() < COMPRESSED_ARRAY_THRESHOLD) {
			data.Align(sizeof(uint16_t));
		} else if (metadata.IsRun() && metadata.NumberOfRuns() < COMPRESSED_RUN_THRESHOLD) {
			data.Align(sizeof(RunContainerRLEPair));
		}
		data_start_position.push_back(data.Position());
		auto container_size = MinValue<idx_t>(segment_count - i * ROARING_CONTAINER_SIZE, ROARING_CONTAINER_SIZE);
		// A partial bitset still occupies complete validity words.
		auto stored_size = AlignValue<idx_t, ValidityMask::BITS_PER_VALUE>(container_size);
		data.Skip(metadata.GetDataSizeInBytes(stored_size));
	}
}

bool RoaringScanState::UseContainerStateCache(idx_t container_index, idx_t internal_offset) {
	if (!current_container) {
		// No container loaded yet
		return false;
	}
	if (current_container->container_index != container_index) {
		// Not the same container
		return false;
	}
	if (current_container->scanned_count != internal_offset) {
		// Not the same scan offset
		return false;
	}
	return true;
}

ContainerMetadata RoaringScanState::GetContainerMetadata(idx_t container_index) {
	return container_metadata[container_index];
}

CompressionSegmentReader RoaringScanState::GetContainerReader(idx_t container_index, idx_t container_size) {
	auto metadata = GetContainerMetadata(container_index);
	auto stored_size = AlignValue<idx_t, ValidityMask::BITS_PER_VALUE>(container_size);
	return layout.data.GetSubReader(data_start_position[container_index], metadata.GetDataSizeInBytes(stored_size),
	                                "Roaring container");
}

ContainerScanState &RoaringScanState::LoadContainer(idx_t container_index, idx_t internal_offset) {
	if (UseContainerStateCache(container_index, internal_offset)) {
		return *current_container;
	}
	auto metadata = GetContainerMetadata(container_index);

	auto segment_count = segment.count.load();
	auto start_of_container = container_index * ROARING_CONTAINER_SIZE;
	auto container_size = MinValue<idx_t>(segment_count - start_of_container, ROARING_CONTAINER_SIZE);
	auto reader = GetContainerReader(container_index, container_size);
	if (metadata.IsUncompressed()) {
		// Include the final validity word even when the container only uses part of it.
		auto word_count =
		    container_size / ValidityMask::BITS_PER_VALUE + (container_size % ValidityMask::BITS_PER_VALUE != 0);
		auto bitset = reader.GetArray<validity_t>(0, word_count);
		current_container = make_uniq<BitsetContainerScanState>(container_index, container_size, bitset);
	} else if (metadata.IsRun()) {
		D_ASSERT(metadata.IsInverted());
		auto number_of_runs = metadata.NumberOfRuns();
		if (number_of_runs >= COMPRESSED_RUN_THRESHOLD) {
			auto segments = reader.ReadBytes(COMPRESSED_SEGMENT_COUNT);
			auto data = reader.ReadBytes(number_of_runs * 2);
			current_container = make_uniq<CompressedRunContainerScanState>(container_index, container_size,
			                                                               number_of_runs, segments, data);
		} else {
			auto data = reader.GetArray<RunContainerRLEPair>(0, number_of_runs);
			current_container =
			    make_uniq<UncompressedRunContainerScanState>(container_index, container_size, number_of_runs, data);
		}
	} else {
		auto cardinality = metadata.Cardinality();
		if (cardinality >= COMPRESSED_ARRAY_THRESHOLD) {
			auto segments = reader.ReadBytes(COMPRESSED_SEGMENT_COUNT);
			auto data = reader.ReadBytes(cardinality);
			if (metadata.IsInverted()) {
				current_container = make_uniq<CompressedArrayContainerScanState<NULLS>>(container_index, container_size,
				                                                                        cardinality, segments, data);
			} else {
				current_container = make_uniq<CompressedArrayContainerScanState<NON_NULLS>>(
				    container_index, container_size, cardinality, segments, data);
			}
		} else {
			auto data = reader.GetArray<uint16_t>(0, cardinality);
			if (metadata.IsInverted()) {
				current_container = make_uniq<UncompressedArrayContainerScanState<NULLS>>(
				    container_index, container_size, cardinality, data);
			} else {
				current_container = make_uniq<UncompressedArrayContainerScanState<NON_NULLS>>(
				    container_index, container_size, cardinality, data);
			}
		}
	}

	auto &scan_state = *current_container;
	if (internal_offset) {
		Skip(scan_state, internal_offset);
	}
	return *current_container;
}

void RoaringScanState::ScanInternal(ContainerScanState &scan_state, idx_t to_scan, ValidityMask &result, idx_t offset) {
	scan_state.ScanPartial(result, offset, to_scan);
}

idx_t RoaringScanState::GetContainerIndex(idx_t start_index, idx_t &offset) {
	idx_t container_index = start_index / ROARING_CONTAINER_SIZE;
	offset = start_index % ROARING_CONTAINER_SIZE;
	return container_index;
}

void RoaringScanState::ScanPartial(idx_t start_idx, ValidityMask &result, idx_t offset, idx_t count) {
	idx_t remaining = count;
	idx_t scanned = 0;
	while (remaining) {
		idx_t internal_offset;
		idx_t container_idx = GetContainerIndex(start_idx + scanned, internal_offset);
		auto &scan_state = LoadContainer(container_idx, internal_offset);
		idx_t remaining_in_container = scan_state.container_size - scan_state.scanned_count;
		idx_t to_scan = MinValue<idx_t>(remaining, remaining_in_container);
		ScanInternal(scan_state, to_scan, result, offset + scanned);
		remaining -= to_scan;
		scanned += to_scan;
	}
	D_ASSERT(scanned == count);
}

void RoaringScanState::Skip(ContainerScanState &scan_state, idx_t skip_count) {
	D_ASSERT(scan_state.scanned_count + skip_count <= scan_state.container_size);
	if (scan_state.scanned_count + skip_count == scan_state.container_size) {
		scan_state.scanned_count = scan_state.container_size;
		// This skips all remaining values covered by this container
		return;
	}
	scan_state.Skip(skip_count);
}

} // namespace roaring

} // namespace duckdb
