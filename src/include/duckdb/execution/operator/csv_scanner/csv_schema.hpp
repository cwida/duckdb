//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/csv_scanner/csv_schema.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/sniff_result.hpp"

namespace duckdb {
//! Basic CSV Column Info
struct CSVColumnInfo {
	CSVColumnInfo(const Identifier &name_p, const LogicalType &type_p) : name(name_p), type(type_p) {
	}
	Identifier name;
	LogicalType type;
};

//! Basic CSV Schema
struct CSVSchema {
	explicit CSVSchema(const bool empty = false) : empty(empty) {
	}

	CSVSchema(const vector<Identifier> &names, const vector<LogicalType> &types, const string &file_path,
	          idx_t rows_read, const bool empty = false);

	//! Initializes the schema based on names and types
	void Initialize(const vector<Identifier> &names, const vector<LogicalType> &types, const string &file_path);

	//! If the schema is empty
	bool Empty() const;

	//! Whether this schema was determined on a file without any data
	bool IsEmptyFile() const {
		return empty;
	}

	//! If the columns of the schema match
	bool MatchColumns(const CSVSchema &other) const;

	//! We merge two schemas by ensuring that the column types are compatible between both
	void MergeSchemas(CSVSchema &other, bool null_padding);

	//! What's the file path for the file that generated this schema
	string GetPath() const;

	//! How many columns we have in this schema
	idx_t GetColumnCount() const;

	//! Check if two schemas match.
	bool SchemasMatch(string &error_message, SnifferResult &sniffer_result, const string &cur_file_path,
	                  bool is_minimal_sniffer) const;

	//! How many rows were read when generating this schema, this is only used for sniffing during the binder
	idx_t GetRowsRead() const;

	//! Get a vector with names
	vector<string> GetNames() const;

	//! Get a vector with types
	vector<LogicalType> GetTypes() const;

	//! Replace any SQLNull types with Varchar
	void ReplaceNullWithVarchar();

private:
	//! If a type can be cast to another
	static bool CanWeCastIt(LogicalTypeId source, LogicalTypeId destination);
	vector<CSVColumnInfo> columns;
	identifier_map_t<idx_t> name_idx_map;
	string file_path;
	idx_t rows_read = 0;
	bool empty = false;
};
} // namespace duckdb
