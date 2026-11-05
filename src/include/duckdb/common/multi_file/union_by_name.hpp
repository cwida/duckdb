//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/multi_file/union_by_name.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_options.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {
struct MultiFileReader;
struct MultiFileReaderInterface;

class UnionByName {
public:
	static void CombineUnionTypes(const vector<string> &new_names, const vector<LogicalType> &new_types,
	                              vector<LogicalType> &union_col_types, vector<Identifier> &union_col_names,
	                              identifier_map_t<idx_t> &union_names_map);

	//! Union all files(readers) by their col names
	static vector<shared_ptr<BaseUnionData>>
	UnionCols(ClientContext &context, const vector<OpenFileInfo> &files, vector<LogicalType> &union_col_types,
	          vector<Identifier> &union_col_names, BaseFileReaderOptions &options, MultiFileOptions &file_options,
	          MultiFileReader &multi_file_reader, MultiFileReaderInterface &interface);

	//! Read the union data (schema, cardinality, statistics access) of every file in parallel, without combining the
	//! schemas. With ignore_errors, a file that cannot be read yields a null entry instead of failing
	DUCKDB_API static vector<shared_ptr<BaseUnionData>>
	ReadUnionData(ClientContext &context, const vector<OpenFileInfo> &files, BaseFileReaderOptions &options,
	              MultiFileOptions &file_options, MultiFileReader &multi_file_reader,
	              MultiFileReaderInterface &interface, bool ignore_errors = false,
	              TaskSchedulerType scheduler_type = TaskSchedulerType::REGULAR);
};

} // namespace duckdb
