#include "duckdb/common/enums/compression_type.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/storage/storage_info.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

// LCOV_EXCL_START

vector<string> ListCompressionTypes(void) {
	vector<string> compression_types;
	uint8_t amount_of_compression_options = (uint8_t)CompressionType::ENUM_SIZE;
	compression_types.reserve(amount_of_compression_options);
	for (uint8_t i = 0; i < amount_of_compression_options; i++) {
		compression_types.push_back(CompressionTypeToString((CompressionType)i));
	}
	return compression_types;
}

namespace {
struct CompressionMethodRequirements {
	CompressionType type;
	StorageVersion minimum_storage_version;
	StorageVersion maximum_storage_version;
};
} // namespace

CompressionAvailabilityResult CompressionTypeIsAvailable(CompressionType compression_type,
                                                         optional_ptr<StorageManager> storage_manager) {
	//! Max storage compatibility
	// if minimum version is StorageVersion::INVALID, this is an indication that the
	// compression method is deprecated (e.g. chimp and patas)
	vector<CompressionMethodRequirements> candidates(
	    {{CompressionType::COMPRESSION_PATAS, StorageVersion::INVALID, StorageVersion::V0_10_2}, // phased out
	     {CompressionType::COMPRESSION_CHIMP, StorageVersion::INVALID, StorageVersion::V0_10_2}, // phased out
	     {CompressionType::COMPRESSION_DICTIONARY, StorageVersion::V0_10_2, StorageVersion::V1_2_0},
	     {CompressionType::COMPRESSION_FSST, StorageVersion::V0_10_2, StorageVersion::V1_2_0},
	     {CompressionType::COMPRESSION_ROARING, StorageVersion::V1_2_0, StorageVersion::LATEST},
	     {CompressionType::COMPRESSION_ZSTD, StorageVersion::V1_2_0, StorageVersion::LATEST},
	     {CompressionType::COMPRESSION_DICT_FSST, StorageVersion::V1_3_0, StorageVersion::LATEST},
	     // Not implemented yet
	     {CompressionType::COMPRESSION_PFOR_DELTA, (StorageVersion)((int)StorageVersion::LATEST + 1),
	      StorageVersion::INVALID}});

	StorageVersion current_storage_version = StorageVersion::INVALID;
	if (storage_manager && storage_manager->HasStorageVersion()) {
		current_storage_version = storage_manager->GetStorageVersion();
	}
	for (auto &candidate : candidates) {
		auto &type = candidate.type;
		if (type != compression_type) {
			continue;
		}
		auto &min = candidate.minimum_storage_version;
		auto &max = candidate.maximum_storage_version;

		if (min == StorageVersion::INVALID) {
			//! Used to signal: always deprecated
			return CompressionAvailabilityResult::Deprecated();
		}

		if (current_storage_version == StorageVersion::INVALID) {
			//! Can't determine in this call whether it's available or not, default to available
			return CompressionAvailabilityResult();
		}

		auto current_version = current_storage_version;
		D_ASSERT(min != StorageVersion::INVALID);
		if (min > current_version) {
			//! Minimum required storage version is higher than the current storage version, this method isn't available
			//! yet
			return CompressionAvailabilityResult::NotAvailableYet();
		}
		if (max != StorageVersion::INVALID && max < current_version) {
			//! Maximum supported storage version is lower than the current storage version, this method is no longer
			//! available
			return CompressionAvailabilityResult::Deprecated();
		}
		return CompressionAvailabilityResult();
	}
	return CompressionAvailabilityResult();
}

string CompressionTypeToString(CompressionType type) {
	switch (type) {
	case CompressionType::COMPRESSION_AUTO:
		return "Auto";
	case CompressionType::COMPRESSION_UNCOMPRESSED:
		return "Uncompressed";
	case CompressionType::COMPRESSION_CONSTANT:
		return "Constant";
	case CompressionType::COMPRESSION_RLE:
		return "RLE";
	case CompressionType::COMPRESSION_DICTIONARY:
		return "Dictionary";
	case CompressionType::COMPRESSION_PFOR_DELTA:
		return "PFOR";
	case CompressionType::COMPRESSION_BITPACKING:
		return "BitPacking";
	case CompressionType::COMPRESSION_FSST:
		return "FSST";
	case CompressionType::COMPRESSION_CHIMP:
		return "Chimp";
	case CompressionType::COMPRESSION_PATAS:
		return "Patas";
	case CompressionType::COMPRESSION_ZSTD:
		return "ZSTD";
	case CompressionType::COMPRESSION_ALP:
		return "ALP";
	case CompressionType::COMPRESSION_ALPRD:
		return "ALPRD";
	case CompressionType::COMPRESSION_ROARING:
		return "Roaring";
	case CompressionType::COMPRESSION_DICT_FSST:
		return "DICT_FSST";
	case CompressionType::COMPRESSION_EMPTY:
		return "Empty Validity";
	default:
		throw InternalException("Unrecognized compression type!");
	}
}
// LCOV_EXCL_STOP

} // namespace duckdb
