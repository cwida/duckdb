#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/alp/alp_analyze.hpp"
#include "duckdb/storage/compression/alp/alp_compress.hpp"
#include "duckdb/storage/compression/alp/alp_scan.hpp"

namespace duckdb {

void ThrowAlpMetadataBeforeHeader() {
	throw DataCorruptionException("Corrupted ALP segment: metadata ends before the segment header");
}

void ThrowAlpMetadataTableOutOfBounds() {
	throw DataCorruptionException("Corrupted ALP segment: metadata offset table exceeds the segment");
}

void ThrowAlpVectorOffsetOutOfBounds() {
	throw DataCorruptionException("Corrupted ALP segment: vector offset is outside the data region");
}

void ThrowAlpVectorOffsetsInvalid() {
	throw DataCorruptionException("Corrupted ALP segment: vector offsets do not describe a data range");
}

void ThrowAlpExponentOutOfRange(AlpConstants::EXPONENT_TYPE exponent, AlpConstants::EXPONENT_TYPE max_exponent) {
	throw DataCorruptionException("Corrupted ALP segment: exponent %d exceeds the maximum %d", exponent, max_exponent);
}

void ThrowAlpFactorOutOfRange(AlpConstants::FACTOR_TYPE factor, AlpConstants::EXPONENT_TYPE exponent) {
	throw DataCorruptionException("Corrupted ALP segment: factor %d exceeds exponent %d", factor, exponent);
}

void ThrowAlpExceptionCountOutOfRange(AlpConstants::EXCEPTIONS_COUNT_TYPE exception_count, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALP segment: exception count %d exceeds vector size %d", exception_count,
	                              vector_size);
}

void ThrowAlpBitWidthOutOfRange(AlpConstants::BIT_WIDTH_TYPE bit_width) {
	throw DataCorruptionException("Corrupted ALP segment: bit width %d exceeds %d", bit_width,
	                              AlpConstants::MAX_BIT_WIDTH);
}

void ThrowAlpExceptionPositionOutOfRange(AlpConstants::EXCEPTION_POSITION_TYPE position, idx_t vector_size) {
	throw DataCorruptionException("Corrupted ALP segment: exception position %d is outside vector size %d", position,
	                              vector_size);
}

template <class T>
CompressionFunction GetAlpFunction(PhysicalType data_type) {
	throw NotImplementedException("GetAlpFunction not implemented for the given datatype");
}

template <>
CompressionFunction GetAlpFunction<float>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_ALP, data_type, AlpInitAnalyze<float>, AlpAnalyze<float>,
	                           AlpFinalAnalyze<float>, AlpInitCompression<float>, AlpCompress<float>,
	                           AlpFinalizeCompress<float>, AlpInitScan<float>, AlpScan<float>, AlpScanPartial<float>,
	                           AlpSkip<float>);
}

template <>
CompressionFunction GetAlpFunction<double>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_ALP, data_type, AlpInitAnalyze<double>, AlpAnalyze<double>,
	                           AlpFinalAnalyze<double>, AlpInitCompression<double>, AlpCompress<double>,
	                           AlpFinalizeCompress<double>, AlpInitScan<double>, AlpScan<double>,
	                           AlpScanPartial<double>, AlpSkip<double>);
}

CompressionFunction AlpCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::FLOAT:
		return GetAlpFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetAlpFunction<double>(type);
	default:
		throw InternalException("Unsupported type for Alp");
	}
}

bool AlpCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
