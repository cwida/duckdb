//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_opencsv_record.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {

//! Splits a record into fields as opencsv 2.3's CSVParser::parseLine splits a line, which is how Hive/Athena's
//! OpenCSVSerde reads a record (dialect = 'opencsv'). The record is a single line: the scanner's line machine ends
//! every record at a line break, as Hive's line reader does.
class OpenCSVRecordParser {
public:
	//! The separator is one character (possibly several bytes); the quote and escape are single bytes
	OpenCSVRecordParser(string separator_p, char quote_p, char escape_p);

	//! Splits a record, keeping at most max_fields fields, and returns how many were kept. A field in progress when
	//! the record ends inside an open quote is dropped, as opencsv's reader drops the parser's pending field.
	idx_t Parse(const char *record, idx_t size, idx_t max_fields);
	const string &GetField(idx_t index) const {
		return fields[index];
	}

private:
	bool IsSeparatorAt(const char *record, idx_t size, idx_t position) const;

private:
	const string separator;
	const char quote;
	const char escape;
	//! Reused between records
	vector<string> fields;
};

} // namespace duckdb
