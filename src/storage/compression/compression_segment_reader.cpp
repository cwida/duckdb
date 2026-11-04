#include "duckdb/storage/compression/compression_segment_reader.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/storage/buffer/buffer_handle.hpp"
#include "duckdb/storage/table/column_segment.hpp"

namespace duckdb {

CompressionSegmentReader CompressionSegmentReader::FromSegment(const BufferHandle &handle, const ColumnSegment &segment,
                                                               const char *context) {
	auto block_offset = segment.GetBlockOffset();
	auto block_size = segment.GetBlockSize();
	if (DUCKDB_UNLIKELY(block_offset > block_size)) {
		ThrowOffsetExceedsBlockSize(context);
	}
	auto reader_size = block_size - block_offset;
	auto &data_size = segment.GetDataSize();
	if (data_size) {
		if (DUCKDB_UNLIKELY(*data_size > reader_size)) {
			ThrowByteSizeExceedsBlockSize(context);
		}
		reader_size = *data_size;
	}
	return CompressionSegmentReader(handle.Ptr() + block_offset, reader_size, context);
}

void CompressionSegmentReader::ThrowOffsetExceedsBlockSize(const char *context) {
	throw DataCorruptionException("Corrupted %s: block offset exceeds the block size", context);
}

void CompressionSegmentReader::ThrowByteSizeExceedsBlockSize(const char *context) {
	throw DataCorruptionException("Corrupted %s: segment byte size exceeds the remaining block size", context);
}

void CompressionSegmentReader::ThrowForwardReadOutOfBounds() const {
	throw DataCorruptionException("Corrupted %s: attempted to read past the end of the segment", context);
}

void CompressionSegmentReader::ThrowBackwardReadOutOfBounds() const {
	throw DataCorruptionException("Corrupted %s: attempted to read before the start of the segment", context);
}

void CompressionSegmentReader::ThrowOffsetOutOfBounds() const {
	throw DataCorruptionException("Corrupted %s: offset is outside the segment", context);
}

void CompressionSegmentReader::ThrowRangeOutOfBounds() const {
	throw DataCorruptionException("Corrupted %s: requested range extends past the end of the segment", context);
}

void CompressionSegmentReader::ThrowArrayMisaligned() const {
	throw DataCorruptionException("Corrupted %s: array offset is misaligned", context);
}

void CompressionSegmentReader::ThrowArrayOutOfBounds() const {
	throw DataCorruptionException("Corrupted %s: array count extends past the end of the segment", context);
}

void CompressionSegmentReader::ThrowDestinationTooSmall() const {
	throw DataCorruptionException("Corrupted %s: read does not fit the destination buffer", context);
}

void CompressionSegmentReader::Align(idx_t alignment) {
	if (alignment == 0) {
		throw InternalException("CompressionSegmentReader::Align called with a zero alignment");
	}
	idx_t remainder = position % alignment;
	if (remainder == 0) {
		return;
	}
	idx_t padding = alignment - remainder;
	CheckForwardRead(padding);
	position += padding;
}

void CompressionSegmentReader::AlignBackward(idx_t alignment) {
	if (alignment == 0) {
		throw InternalException("CompressionSegmentReader::AlignBackward called with a zero alignment");
	}
	auto padding = position % alignment;
	position -= padding;
}

} // namespace duckdb
