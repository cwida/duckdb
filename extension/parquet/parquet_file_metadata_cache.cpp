#include "parquet_file_metadata_cache.hpp"

#include <memory>
#include <unordered_map>
#include <utility>

#include "duckdb/common/enums/cache_validation_mode.hpp"
#include "duckdb/common/encryption_state.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache.hpp"
#include "duckdb/storage/external_file_cache/external_file_cache_util.hpp"
#include "duckdb/storage/external_file_cache/caching_file_system.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/client_context.hpp"
#include "parquet_crypto.hpp"

namespace duckdb {

ParquetFileMetadataCache::ParquetFileMetadataCache(unique_ptr<ParquetMetadata> metadata_p, CachingFileHandle &handle,
                                                   unique_ptr<GeoParquetFileMetadata> geo_metadata,
                                                   unique_ptr<FileCryptoMetaData> crypto_metadata,
                                                   string encryption_key_hash_p, idx_t footer_size)
    : metadata(std::move(metadata_p)), geo_metadata(std::move(geo_metadata)),
      crypto_metadata(std::move(crypto_metadata)), encryption_key_hash(std::move(encryption_key_hash_p)),
      footer_size(footer_size), last_modified(handle.GetLastModifiedTime()), version_tag(handle.GetVersionTag()) {
}

string ParquetFileMetadataCache::ObjectType() {
	return "parquet_metadata";
}

string ParquetFileMetadataCache::GetObjectType() {
	return ObjectType();
}

optional_idx ParquetFileMetadataCache::GetEstimatedCacheMemory() const {
	// Base memory consumption
	idx_t memory = sizeof(*this);

	if (metadata) {
		memory += sizeof(duckdb_parquet::FileMetaData) + metadata->arena.AllocationSize();
	}
	if (geo_metadata) {
		memory +=
		    sizeof(GeoParquetFileMetadata) + geo_metadata->GetColumnMeta().size() * sizeof(GeoParquetColumnMetadata);
	}
	if (crypto_metadata) {
		memory += sizeof(FileCryptoMetaData);
	}

	memory += footer_size;
	memory += encryption_key_hash.size();
	memory += version_tag.size();

	return memory;
}

bool ParquetFileMetadataCache::IsEncrypted() const {
	return crypto_metadata != nullptr;
}

bool ParquetFileMetadataCache::CanUseMetadataStatistics(const shared_ptr<ParquetEncryptionConfig> &encryption_config,
                                                        optional_ptr<const string> provided_key_hash) const {
	if (!IsEncrypted()) {
		// This can run from planner paths before ParquetReader validates the encryption configuration.
		return !encryption_config;
	}
	if (!encryption_config || !provided_key_hash) {
		return false;
	}
	return encryption_key_hash == *provided_key_hash;
}

string ParquetFileMetadataCache::CreateEncryptionKeyHash(const ParquetEncryptionConfig &encryption_config,
                                                         const EncryptionUtil &encryption_util) {
	string result(CryptoHash::GetHexDigestSize(CryptoHashFunction::SHA256), '\0');
	const auto &footer_key = encryption_config.GetFooterKey();
	encryption_util.HashHex(CryptoHashFunction::SHA256, const_data_ptr_cast(footer_key.data()), footer_key.size(),
	                        &result[0]);
	return result;
}

bool ParquetFileMetadataCache::IsValid(CachingFileHandle &new_handle) const {
	return ExternalFileCache::IsValid(new_handle.Validate(), version_tag, last_modified, new_handle.GetVersionTag(),
	                                  new_handle.GetLastModifiedTime());
}

ParquetCacheValidity ParquetFileMetadataCache::IsValid(const OpenFileInfo &info, ClientContext &context) const {
	CacheValidationMode validation_mode = ExternalFileCacheUtil::GetCacheValidationMode(info, &context, *context.db);
	if (validation_mode == CacheValidationMode::NO_VALIDATION) {
		return ParquetCacheValidity::VALID;
	}
	if (validation_mode == CacheValidationMode::VALIDATE_REMOTE && !FileSystem::IsRemoteFile(info.path)) {
		return ParquetCacheValidity::VALID;
	}
	if (info.extended_info == nullptr) {
		return ParquetCacheValidity::UNKNOWN;
	}

	const auto &extended_info = *info.extended_info;
	timestamp_t new_last_modified;
	if (!extended_info.TryGetOption("last_modified", new_last_modified)) {
		return ParquetCacheValidity::UNKNOWN;
	}
	string new_etag;
	extended_info.TryGetOption("etag", new_etag);

	if (ExternalFileCache::IsValid(/*validate=*/true, version_tag, last_modified, new_etag, new_last_modified)) {
		return ParquetCacheValidity::VALID;
	}
	return ParquetCacheValidity::INVALID;
}

} // namespace duckdb
