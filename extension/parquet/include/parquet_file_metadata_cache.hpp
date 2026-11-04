//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_file_metadata_cache.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include <functional>
#include <string>

#include "duckdb.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/storage/object_cache.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "parquet_column_schema.hpp"
#include "parquet_geometry.hpp"
#include "parquet_types.h"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {
struct CachingFileHandle;
class ClientContext;
class EncryptionUtil;
class ParquetEncryptionConfig;

using duckdb_parquet::FileCryptoMetaData;

enum class ParquetCacheValidity { VALID, INVALID, UNKNOWN };

//! The Thrift file metadata together with the arena that backs all of its strings and vectors
struct ParquetMetadata {
	explicit ParquetMetadata(Allocator &allocator) : arena(allocator), file_metadata(arena) {
	}

	//! Backs all strings and vectors of the Thrift structures - declared first as it must outlive them
	ArenaAllocator arena;
	//! Parquet file metadata
	duckdb_parquet::FileMetaData file_metadata;
};

class ParquetFileMetadataCache : public ObjectCacheEntry {
public:
	ParquetFileMetadataCache(unique_ptr<ParquetMetadata> metadata, CachingFileHandle &handle,
	                         unique_ptr<GeoParquetFileMetadata> geo_metadata,
	                         unique_ptr<FileCryptoMetaData> crypto_metadata, string encryption_key_hash,
	                         idx_t footer_size);
	~ParquetFileMetadataCache() override = default;

	//! Parquet file metadata - declared before the other metadata as that is allocated from its arena
	unique_ptr<const ParquetMetadata> metadata;

	//! GeoParquet metadata
	unique_ptr<GeoParquetFileMetadata> geo_metadata;

	//! Crypto metadata
	unique_ptr<FileCryptoMetaData> crypto_metadata;
	//! SHA256 fingerprint of the footer encryption key used to decrypt the cached metadata
	//! Here we store the hash of the key to avoid key leak
	string encryption_key_hash;

	//! Parquet footer size
	idx_t footer_size;

public:
	static string ObjectType();
	string GetObjectType() override;
	optional_idx GetEstimatedCacheMemory() const override;

	bool IsEncrypted() const;
	bool CanUseMetadataStatistics(const shared_ptr<ParquetEncryptionConfig> &encryption_config,
	                              optional_ptr<const string> encryption_key_hash = nullptr) const;
	static string CreateEncryptionKeyHash(const ParquetEncryptionConfig &encryption_config,
	                                      const EncryptionUtil &encryption_util);
	bool IsValid(CachingFileHandle &new_handle) const;
	//! Return if a cache entry is valid.
	ParquetCacheValidity IsValid(const OpenFileInfo &info, ClientContext &context) const;
	//! The statistics of a column merged over all row groups, computed by 'compute' on first use
	unique_ptr<BaseStatistics> GetColumnStatistics(const ParquetColumnSchema &schema, bool can_have_nan,
	                                               const std::function<unique_ptr<BaseStatistics>()> &compute);

private:
	struct ColumnStatisticsKey {
		reference<const ParquetColumnSchema> schema;
		bool can_have_nan;
	};
	struct ColumnStatisticsKeyHash {
		hash_t operator()(const ColumnStatisticsKey &key) const;
	};
	struct ColumnStatisticsKeyEquality {
		bool operator()(const ColumnStatisticsKey &a, const ColumnStatisticsKey &b) const;
	};
	struct CachedColumnStatistics {
		//! Owns the schema referenced by the key of this entry
		unique_ptr<const ParquetColumnSchema> schema;
		//! nullptr if the column has no statistics
		unique_ptr<BaseStatistics> statistics;
	};

	timestamp_t last_modified;
	string version_tag;
	annotated_mutex column_statistics_lock;
	// Maps from a column schema and the options to the statistics of that column merged over all row groups.
	unordered_map<ColumnStatisticsKey, CachedColumnStatistics, ColumnStatisticsKeyHash, ColumnStatisticsKeyEquality>
	    column_statistics;
};

} // namespace duckdb
