#include "duckdb/function/compression/compression.hpp"
#include "duckdb/function/compression_function.hpp"
#include "duckdb/storage/compression/patas/patas_analyze.hpp"
#include "duckdb/storage/compression/patas/patas_compress.hpp"
#include "duckdb/storage/compression/patas/patas_scan.hpp"

namespace duckdb {

void ThrowPatasInvalidBackwardReference() {
	throw DataCorruptionException("Corrupted Patas segment: invalid backward reference");
}

void ThrowPatasInvalidPackedValueMetadata() {
	throw DataCorruptionException("Corrupted Patas segment: invalid packed value metadata");
}

void ThrowPatasMetadataBeforeHeader() {
	throw DataCorruptionException("Corrupted Patas segment: metadata ends before the segment header");
}

void ThrowPatasMetadataTableOutOfBounds() {
	throw DataCorruptionException("Corrupted Patas segment: metadata table exceeds the segment");
}

void ThrowPatasGroupDataOutOfBounds() {
	throw DataCorruptionException("Corrupted Patas segment: group data is outside the data region");
}

template <class T>
CompressionFunction GetPatasFunction(PhysicalType data_type) {
	throw NotImplementedException("GetPatasFunction not implemented for the given datatype");
}

template <>
CompressionFunction GetPatasFunction<float>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_PATAS, data_type, PatasInitAnalyze<float>,
	                           PatasAnalyze<float>, PatasFinalAnalyze<float>, PatasInitCompression<float>,
	                           PatasCompress<float>, PatasFinalizeCompress<float>, PatasInitScan<float>,
	                           PatasScan<float>, PatasScanPartial<float>, PatasSkip<float>);
}

template <>
CompressionFunction GetPatasFunction<double>(PhysicalType data_type) {
	return CompressionFunction(CompressionType::COMPRESSION_PATAS, data_type, PatasInitAnalyze<double>,
	                           PatasAnalyze<double>, PatasFinalAnalyze<double>, PatasInitCompression<double>,
	                           PatasCompress<double>, PatasFinalizeCompress<double>, PatasInitScan<double>,
	                           PatasScan<double>, PatasScanPartial<double>, PatasSkip<double>);
}

CompressionFunction PatasCompressionFun::GetFunction(PhysicalType type) {
	switch (type) {
	case PhysicalType::FLOAT:
		return GetPatasFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetPatasFunction<double>(type);
	default:
		throw InternalException("Unsupported type for Patas");
	}
}

bool PatasCompressionFun::TypeIsSupported(const PhysicalType physical_type) {
	switch (physical_type) {
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return true;
	default:
		return false;
	}
}

} // namespace duckdb
