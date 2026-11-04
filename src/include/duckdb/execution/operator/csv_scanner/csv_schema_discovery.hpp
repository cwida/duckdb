//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema.hpp"

namespace duckdb {

struct CSVSchemaDiscovery {
	//! "replace_null_with_varchar" is false when the caller combines this schema with the schemas of other files -
	//! columns without any value must stay SQLNULL until every file has been seen, or they widen the other schemas
	static CSVSchema SchemaDiscovery(ClientContext &context, shared_ptr<CSVBufferManager> &buffer_manager,
	                                 CSVReaderOptions &options, const MultiFileOptions &file_options,
	                                 vector<LogicalType> &return_types, vector<Identifier> &names,
	                                 MultiFileList &multi_file_list, bool replace_null_with_varchar = true);
};

} // namespace duckdb
