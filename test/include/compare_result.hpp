//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// compare_result.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string_util.hpp"
#include "duckdb.hpp"

namespace duckdb {

bool CHECK_COLUMN(QueryResult &result, size_t column_number, vector<duckdb::Value> values);
bool CHECK_COLUMN(duckdb::unique_ptr<duckdb::QueryResult> &result, size_t column_number, vector<duckdb::Value> values);

string compare_csv(duckdb::QueryResult &result, string csv, bool header = false);
string compare_csv_collection(duckdb::ColumnDataCollection &collection, string csv, bool header = false);

bool parse_datachunk(string csv, DataChunk &result, vector<LogicalType> sql_types, bool has_header);

//! Compares the result of a pipe-delimited CSV with the given DataChunk
//! Returns true if they are equal, and stores an error_message otherwise
bool compare_result(string csv, ColumnDataCollection &collection, vector<LogicalType> sql_types, bool has_header,
                    string &error_message);

} // namespace duckdb
