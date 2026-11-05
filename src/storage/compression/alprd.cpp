#include "duckdb/common/limits.hpp"
#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/alprd/alprd_analyze.hpp"
#include "duckdb/storage/compression/alprd/alprd_compress.hpp"
#include "duckdb/storage/compression/alprd/alprd_scan.hpp"

namespace duckdb {

void ThrowAlpRDMetadataBeforeHeader() {
	throw DataCorruptionException("Corrupted ALPRD segment: metadata ends before the segment header");
}

void ThrowAlpRDRightBitWidthOutOfRange(AlpRDConstants::BIT_WIDTH_TYPE bit_width,
                                       AlpRDConstants::BIT_WIDTH_TYPE max_bit_width) {
	throw DataCorruptionException("Corrupted ALPRD segment: right bit width %d exceeds %d", bit_width, max_bit_width);
}

void ThrowAlpRDLeftBitWidthOutOfRange(AlpRDConstants::BIT_WIDTH_TYPE bit_width) {
	throw DataCorruptionException("Corrupted ALPRD segment: left bit width %d exceeds %d", bit_width,
	                              AlpRDConstants::MAX_DICTIONARY_BIT_WIDTH);
}

void ThrowAlpRDDictionarySizeExceedsMaximum() {
	throw DataCorruptionException("Corrupt database file: ALPRD dictionary size exceeds maximum");
}

void ThrowAlpRDMetadataTableOutOfBounds() {
	throw DataCorruptionException("Corrupted ALPRD segment: metadata offset table exceeds the segment");
}

void ThrowAlpRDVectorOffsetOutOfBounds() {
	throw DataCorruptionException("Corrupted ALPRD segment: vector offset is outside the data region");
}

void ThrowAlpRDVectorOffsetsInvalid() {
	throw DataCorruptionException("Corrupted ALPRD segment: vector offsets do not describe a data range");
}

void ThrowAlpRDExceptionCountOutOfRange(AlpRDConstants::EXCEPTIONS_COUNT_TYPE exception_count, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALPRD segment: exception count %d exceeds vector size %d", exception_count,
	                              vector_size);
}

void ThrowAlpRDExceptionPositionOutOfRange(AlpRDConstants::EXCEPTION_POSITION_TYPE position, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALPRD segment: exception position %d is outside vector size %d", position,
	                              vector_size);
}

template <class T>
CompressionFunction GetAlpRDFunction(PhysicalType data_type) {
	throw NotImplementedException("GetAlpFunction not implemented for the given datatype");
}

template <>
CompressionFunction GetAlpRDFunction<float>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_ALPRD, data_type, AlpRDInitAnalyze<float>,
	                           AlpRDAnalyze<float>, AlpRDFinalAnalyze<float>, AlpRDInitCompression<float>,
	                           AlpRDCompress<float>, AlpRDFinalizeCompress<float>, AlpRDInitScan<float>,
	                           AlpRDScan<float>, AlpRDScanPartial<float>, AlpRDSkip<float>);
}

template <>
CompressionFunction GetAlpRDFunction<double>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_ALPRD, data_type, AlpRDInitAnalyze<double>,
	                           AlpRDAnalyze<double>, AlpRDFinalAnalyze<double>, AlpRDInitCompression<double>,
	                           AlpRDCompress<double>, AlpRDFinalizeCompress<double>, AlpRDInitScan<double>,
	                           AlpRDScan<double>, AlpRDScanPartial<double>, AlpRDSkip<double>);
}

CompressionFunction AlpRDCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::FLOAT:
		return GetAlpRDFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetAlpRDFunction<double>(type);
	default:
		throw InternalException("Unsupported type for Alp");
	}
}

bool AlpRDCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
