//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/hive_partitioning.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/types/column/partitioned_column_data.hpp"
#include "duckdb/common/open_file_info.hpp"
#include "duckdb/original/std/sstream.hpp"

#include <iostream>

namespace duckdb {
struct MultiFilePushdownInfo;

struct HivePartitioningFilterInfo {
	identifier_map_t<column_t> column_map;
	Identifier filename_column;
	bool hive_enabled;
	bool filename_enabled;
};

class HivePartitioning {
public:
	//! The directory name that is written for a NULL partition value
	static constexpr const char *DEFAULT_PARTITION_NAME = "__HIVE_DEFAULT_PARTITION__";

public:
	//! Parse a filename that follows the hive partitioning scheme
	DUCKDB_API static std::map<string, string> Parse(const string &filename);
	//! Prunes a list of filenames based on a set of filters, can be used by TableFunctions in the
	//! pushdown_complex_filter function to skip files with filename-based filters. Also removes the filters that always
	//! evaluate to true.
	DUCKDB_API static void ApplyFiltersToFileList(ClientContext &context, vector<OpenFileInfo> &files,
	                                              vector<unique_ptr<Expression>> &filters,
	                                              const HivePartitioningFilterInfo &filter_info,
	                                              MultiFilePushdownInfo &info);

	DUCKDB_API static Value GetValue(ClientContext &context, const string &key, const string &value,
	                                 const LogicalType &type);
	//! Escape a hive partition key or value using URL encoding
	DUCKDB_API static string Escape(const string &input);
	//! Escape a non-NULL hive partition value, avoiding a collision with the NULL partition directory
	DUCKDB_API static string EscapeValue(const string &input);
	//! Unescape a hive partition key or value encoded using URL encoding
	DUCKDB_API static string Unescape(const string &input);
	//! Whether the value is a null marker when detecting Hive partition types
	DUCKDB_API static bool IsNull(const string &input);
};

struct HivePartitionKey {
	//! Columns by which we want to partition
	vector<Value> values;
	//! Precomputed hash of values
	hash_t hash;

	struct Hash {
		std::size_t operator()(const HivePartitionKey &k) const {
			return k.hash;
		}
	};

	struct Equality {
		bool operator()(const HivePartitionKey &a, const HivePartitionKey &b) const {
			if (a.values.size() != b.values.size()) {
				return false;
			}
			for (idx_t i = 0; i < a.values.size(); i++) {
				if (!Value::NotDistinctFrom(a.values[i], b.values[i])) {
					return false;
				}
			}
			return true;
		}
	};
};

//! Maps hive partitions to partition_ids
typedef unordered_map<HivePartitionKey, idx_t, HivePartitionKey::Hash, HivePartitionKey::Equality> hive_partition_map_t;

//! class shared between HivePartitionColumnData classes that synchronizes partition discovery between threads.
//! each HivePartitionedColumnData will hold a local copy of the key->partition map
class GlobalHivePartitionState {
public:
	mutex lock;
	hive_partition_map_t partition_map;
};

class HivePartitionedColumnData : public PartitionedColumnData {
public:
	HivePartitionedColumnData(ClientContext &context, vector<LogicalType> types, vector<idx_t> partition_by_cols,
	                          shared_ptr<GlobalHivePartitionState> global_state = nullptr);
	void ComputePartitionIndices(PartitionedColumnDataAppendState &state, DataChunk &input) override;

	//! Reverse lookup map to reconstruct keys from a partition id
	std::map<idx_t, const HivePartitionKey *> GetReverseMap();

protected:
	//! Register a newly discovered partition
	idx_t RegisterNewPartition(HivePartitionKey key, PartitionedColumnDataAppendState &state);
	//! Add a new partition with the given partition id
	void AddNewPartition(HivePartitionKey key, idx_t partition_id, PartitionedColumnDataAppendState &state);

private:
	void InitializeKeys();

protected:
	//! Shared HivePartitionedColumnData should always have a global state to allow parallel key discovery
	shared_ptr<GlobalHivePartitionState> global_state;
	//! Thread-local copy of the partition map
	hive_partition_map_t local_partition_map;
	//! The columns that make up the key
	vector<idx_t> group_by_columns;
	//! Thread-local pre-allocated vector for hashes
	Vector hashes_v;
	//! Thread-local pre-allocated HivePartitionKeys
	vector<HivePartitionKey> keys;
};

} // namespace duckdb
