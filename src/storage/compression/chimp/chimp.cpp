#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/chimp/chimp_analyze.hpp"
#include "duckdb/storage/compression/chimp/chimp_compress.hpp"
#include "duckdb/storage/compression/chimp/chimp_scan.hpp"

namespace duckdb {

void ThrowChimpMetadataBeforeHeader() {
	throw DataCorruptionException("Corrupted Chimp segment: metadata ends before the segment header");
}

void ThrowChimpLeadingZeroBlockCountOutOfBounds(uint8_t block_count) {
	throw DataCorruptionException("Corrupted Chimp segment: leading-zero block count %d exceeds %d", block_count,
	                              ChimpPrimitives::CHIMP_SEQUENCE_SIZE / 8);
}

void ThrowChimpLeadingZeroCountMismatch(idx_t stored_count, idx_t required_count) {
	throw DataCorruptionException(
	    "Corrupted Chimp segment: leading-zero metadata contains %d values but %d are required", stored_count,
	    required_count);
}

void ThrowChimpPackedDataExceedsType(uint8_t leading_zero, uint8_t significant_bits, idx_t bit_width) {
	throw DataCorruptionException(
	    "Corrupted Chimp segment: packed data uses %d leading-zero and %d significant bits for a %d-bit value",
	    leading_zero, significant_bits, bit_width);
}

void ThrowChimpLeadingZeroStateMissing() {
	throw DataCorruptionException("Corrupted Chimp segment: leading-zero equality has no preceding leading-zero value");
}

template <class T>
CompressionFunction GetChimpFunction(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_CHIMP, data_type, ChimpInitAnalyze<T>, ChimpAnalyze<T>,
	                           ChimpFinalAnalyze<T>, ChimpInitCompression<T>, ChimpCompress<T>,
	                           ChimpFinalizeCompress<T>, ChimpInitScan<T>, ChimpScan<T>, ChimpScanPartial<T>,
	                           ChimpSkip<T>);
}

CompressionFunction ChimpCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::FLOAT:
		return GetChimpFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetChimpFunction<double>(type);
	default:
		throw InternalException("Unsupported type for Chimp");
	}
}

bool ChimpCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
