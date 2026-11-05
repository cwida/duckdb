//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/read_csv.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/common/csv_writer.hpp"

namespace duckdb {
class BaseScanner;
class StringValueScanner;

class ReadCSV {
public:
	static unique_ptr<CSVFileHandle> OpenCSV(const OpenFileInfo &file, const CSVReaderOptions &options,
	                                         ClientContext &context);
};

struct BaseCSVData : public TableFunctionData {
	//! The CSV reader options
	CSVReaderOptions options;
	//! Offsets for generated columns
	idx_t filename_col_idx {};
	idx_t hive_partition_col_idx {};

	void Finalize();
};

struct WriteCSVData : public BaseCSVData {
	explicit WriteCSVData(const vector<Identifier> &names) {
		options.name_list = names;
		if (options.dialect_options.state_machine_options.escape == '\0') {
			options.dialect_options.state_machine_options.escape = options.dialect_options.state_machine_options.quote;
		}
	}
	//! The size of the CSV file (in bytes) that we buffer before we flush it to disk
	idx_t flush_size = 4096ULL * 8ULL;
	//! Expressions used to convert the input into strings
	vector<unique_ptr<Expression>> cast_expressions;
};

struct ColumnInfo {
	ColumnInfo() {
	}
	ColumnInfo(vector<std::string> names_p, vector<LogicalType> types_p) {
		names = std::move(names_p);
		types = std::move(types_p);
	}
	void Serialize(Serializer &serializer) const;
	static ColumnInfo Deserialize(Deserializer &deserializer);

	vector<std::string> names;
	vector<LogicalType> types;
};

struct ReadCSVData : public BaseCSVData {
	ReadCSVData();
	//! If the sql types from the file were manually set
	vector<bool> manually_set;
	//! The buffer manager (if any): this is used when automatic detection is used during binding.
	//! In this case, some CSV buffers have already been read and can be reused.
	shared_ptr<CSVBufferManager> buffer_manager;
	//! The CSV schema, in case there is a unified schema that all files must read
	CSVSchema csv_schema;

	void FinalizeRead(ClientContext &context);
};

struct SerializedCSVReaderOptions {
	SerializedCSVReaderOptions() = default;
	SerializedCSVReaderOptions(CSVReaderOptions options, MultiFileOptions file_options);
	SerializedCSVReaderOptions(CSVOption<char> single_byte_delimiter, const CSVOption<string> &multi_byte_delimiter);

	CSVReaderOptions options;
	MultiFileOptions file_options;

	void Serialize(Serializer &serializer) const;
	static SerializedCSVReaderOptions Deserialize(Deserializer &deserializer);
};

struct SerializedReadCSVData {
	vector<string> files;
	vector<LogicalType> csv_types;
	vector<string> csv_names;
	vector<LogicalType> return_types;
	vector<string> return_names;
	idx_t filename_col_idx;
	SerializedCSVReaderOptions options;
	MultiFileReaderBindData reader_bind;
	vector<ColumnInfo> column_info;

	void Serialize(Serializer &serializer) const;
	static SerializedReadCSVData Deserialize(Deserializer &deserializer);
};

struct CSVCopyFunction {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct ReadCSVTableFunction {
	static TableFunction GetFunction();
	static TableFunction GetAutoFunction();
	//! The CSV reader that reads a single file - this is what the multi-file CSV reader wraps
	static TableFunction GetSingleFileFunction();
	//! The single-file CSV reader wrapped into a multi-file function
	static TableFunction GetMultiFileFunction(Identifier name);
	static void ReadCSVAddNamedParameters(TableFunction &table_function);
	static void RegisterFunction(BuiltinFunctions &set);
};

} // namespace duckdb
