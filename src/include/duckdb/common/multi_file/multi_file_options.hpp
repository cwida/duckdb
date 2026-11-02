//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/multi_file/multi_file_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/hive_partitioning.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"

namespace duckdb {
class BaseStatistics;
struct BindInfo;
class MultiFileList;

enum class MultiFileColumnMappingMode : uint8_t { BY_NAME, BY_FIELD_ID };

struct MultiFileOptions {
	bool filename = false;
	bool hive_partitioning = false;
	bool auto_detect_hive_partitioning = true;
	bool union_by_name = false;
	bool hive_types_autocast = true;
	bool allow_empty = false;
	//! The maximum number of files that are opened to determine the schema - the schemas of the sampled files are
	//! combined into one. NumericLimits<idx_t>::Maximum() samples every file
	idx_t maximum_sample_files = 1;
	//! Whether the schemas of the sampled files are combined into a union of their columns - files are then allowed
	//! to be missing columns of the combined schema, the way they are with union_by_name. This is how the schemas of
	//! sampled files are combined unless the reader combines them itself
	bool sampled_schema_is_union = true;
	MultiFileColumnMappingMode mapping = MultiFileColumnMappingMode::BY_NAME;
	//! (Optional) The schema of the scan, given by the "schema" option. Every file is mapped onto it - by field id
	//! when its columns are identified by INTEGER, by name otherwise - instead of the schema being read from the files
	vector<MultiFileColumnDefinition> schema;
	//! (Optional) Complete ordered scan schema, mapped by name before conversion. Unlike schema, no defaults or
	//! field identifiers are supplied. Physical reader columns remain independent of these output columns.
	vector<MultiFileColumnDefinition> bound_schema;
	//! Whether to add a "file_row_number" column with the row number of each row within its file - it is read from the
	//! row number virtual column of the reader
	bool file_row_number = false;
	//! (Optional) Column statistics given by the "column_statistics" option: a MAP of column key to a MAP of statistic
	//! name to its value as text - "min", "max", "count", "null_count" and "distinct_count" - the shape
	//! COPY ... (RETURN_STATS) reports them in, keys included: a quoted column path, "s"."x" for a struct field,
	//! "l"."element" for list elements, "m"."key" / "m"."value" for a map (an unquoted key is a plain column name).
	//! The keys name the columns OF THE SCAN - what it produces after union_by_name, schema mapping or renames
	//! across its files - not the columns of any one file. They stand in for the statistics the files would be
	//! opened for, which lets a caller that knows the data (a catalog, a query coordinator) hand them to the
	//! optimizer without any file being read for them - and lets a scan of many files have statistics at all.
	//! NULL when not given.
	//! They never set the scan's cardinality: that is the reader's estimate or, when given, explicit_cardinality.
	//! A column's "count" only tells, against its "null_count", whether the column has any non-NULL values.
	Value column_statistics;

	case_insensitive_map_t<LogicalType> hive_types_schema;

	// Default/configurable name of the column containing the file names
	static constexpr const char *DEFAULT_FILENAME_COLUMN = "filename";
	string filename_column = DEFAULT_FILENAME_COLUMN;
	// These are used to pass options through custom multifilereaders
	case_insensitive_map_t<Value> custom_options;

	DUCKDB_API void Serialize(Serializer &serializer) const;
	DUCKDB_API static MultiFileOptions Deserialize(Deserializer &source);
	DUCKDB_API void AddBatchInfo(BindInfo &bind_info) const;
	//! Settle the hive partitioning options against each other and against the files: hive_types implies
	//! hive_partitioning, and the type of a partition key the caller did not give is taken from the values
	//! in the paths. With detect_partitioning, WHETHER the files are partitioned is taken from the paths
	//! too; a caller that already knows the scan's schema passes false and asks for partitioning itself
	DUCKDB_API void AutoDetectHivePartitioning(MultiFileList &files, ClientContext &context,
	                                           bool detect_partitioning = true);
	DUCKDB_API static bool AutoDetectHivePartitioningInternal(MultiFileList &files, ClientContext &context);
	DUCKDB_API void AutoDetectHiveTypesInternal(MultiFileList &files, ClientContext &context);
	DUCKDB_API void VerifyHiveTypesArePartitions(const std::map<string, string> &partitions) const;
	DUCKDB_API LogicalType GetHiveLogicalType(const string &hive_partition_column) const;
	DUCKDB_API Value GetHivePartitionValue(const string &base, const string &entry, ClientContext &context) const;
	DUCKDB_API bool AnySet() const;
	DUCKDB_API void SetBoundSchema(const Value &value, ClientContext &context);
	//! Validate owned declarations before binding or restoring even an empty file list.
	DUCKDB_API void ValidateBoundSchema() const;
	//! Set "column_statistics" from an option value - see column_statistics for its shape
	DUCKDB_API void SetColumnStatistics(const Identifier &key, const Value &val);
	//! Check "column_statistics" against the scan's columns once they are bound: every key must name a column, or a
	//! part of one, and every value must parse for it. Errors here, at bind, rather than when the optimizer asks.
	DUCKDB_API void VerifyColumnStatistics(const vector<Identifier> &names, const vector<LogicalType> &types) const;
	//! The statistics the "column_statistics" option gives for a column of the given type, or nullptr when it says
	//! nothing about it. The bounds are cast to the column's type; a value that does not cast is an error.
	DUCKDB_API unique_ptr<BaseStatistics> GetColumnStatistics(const Identifier &column_name,
	                                                          const LogicalType &type) const;
	//! Set "maximum_sample_files" from an option value - a positive count, or -1 for all files
	DUCKDB_API void SetMaximumSampleFiles(const Identifier &key, const Value &val);
	//! Set "maximum_sample_files" from an option value, returning false if it is not a valid count
	DUCKDB_API bool TrySetMaximumSampleFiles(const Value &val);
	//! Whether the global schema is a union of the schemas of several files - individual files are then allowed to
	//! be missing columns that are present in the global schema
	bool SchemaIsUnion() const {
		return union_by_name || (maximum_sample_files > 1 && sampled_schema_is_union);
	}
};

} // namespace duckdb
