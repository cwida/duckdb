#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/table/read_duckdb.hpp"

#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/multi_file/union_by_name.hpp"
#include "duckdb/execution/operator/csv_scanner/global_csv_state.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_error.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/base_scanner.hpp"

#include "duckdb/execution/operator/csv_scanner/string_value_scanner.hpp"

#include <limits>
#include "duckdb/execution/operator/csv_scanner/csv_schema.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"

namespace duckdb {

SerializedCSVReaderOptions::SerializedCSVReaderOptions(CSVReaderOptions options_p, MultiFileOptions file_options_p)
    : options(std::move(options_p)), file_options(std::move(file_options_p)) {
}

SerializedCSVReaderOptions::SerializedCSVReaderOptions(CSVOption<char> single_byte_delimiter,
                                                       const CSVOption<string> &multi_byte_delimiter)
    : options(single_byte_delimiter, multi_byte_delimiter) {
}

unique_ptr<CSVFileHandle> ReadCSV::OpenCSV(const OpenFileInfo &file, const CSVReaderOptions &options,
                                           ClientContext &context) {
	return CSVFileHandle::OpenFile(context, file, options);
}

ReadCSVData::ReadCSVData() {
}

void ReadCSVData::FinalizeRead(ClientContext &context) {
	BaseCSVData::Finalize();
}

void ReadCSVTableFunction::ReadCSVAddNamedParameters(TableFunction &table_function) {
	// the reader keeps the default of every option in CSVReaderOptions - an option the call leaves out is not passed
	table_function.GetSignature().WithTypedKwargs("options", [](TypedKwargs &options) {
		options.Add("sep", LogicalType::VARCHAR)
		    .Add("delim", LogicalType::VARCHAR)
		    .Add("separator", LogicalType::VARCHAR)
		    .Add("delimiter", LogicalType::VARCHAR)
		    .Add("null", LogicalType::ANY)
		    .Add("date_format", LogicalType::VARCHAR)
		    .Add("timestamp_format", LogicalType::VARCHAR)
		    .Add("quote", LogicalType::VARCHAR)
		    .Add("new_line", LogicalType::VARCHAR)
		    .Add("escape", LogicalType::VARCHAR)
		    .Add("nullstr", LogicalType::ANY)
		    .Add("columns", LogicalType::ANY)
		    .Add("auto_type_candidates", LogicalType::ANY)
		    .Add("header", LogicalType::BOOLEAN)
		    .Add("auto_detect", LogicalType::BOOLEAN)
		    .Add("sample_size", LogicalType::BIGINT)
		    .Add("all_varchar", LogicalType::BOOLEAN)
		    .Add("dateformat", LogicalType::VARCHAR)
		    .Add("timestampformat", LogicalType::VARCHAR)
		    .Add("normalize_names", LogicalType::BOOLEAN)
		    .Add("compression", LogicalType::VARCHAR)
		    .Add("skip", LogicalType::BIGINT)
		    .Add("max_line_size", LogicalType::BIGINT)
		    .Add("maximum_line_size", LogicalType::BIGINT)
		    .Add("ignore_errors", LogicalType::BOOLEAN)
		    .Add("store_rejects", LogicalType::BOOLEAN)
		    .Add("rejects_table", LogicalType::VARCHAR)
		    .Add("rejects_scan", LogicalType::VARCHAR)
		    .Add("rejects_limit", LogicalType::BIGINT)
		    .Add("rejects_line_size_limit", LogicalType::BIGINT)
		    .Add("force_not_null", LogicalType::LIST(LogicalType::VARCHAR))
		    .Add("buffer_size", LogicalType::BIGINT)
		    .Add("decimal_separator", LogicalType::VARCHAR)
		    .Add("parallel", LogicalType::BOOLEAN)
		    .Add("null_padding", LogicalType::BOOLEAN)
		    .Add("allow_quoted_nulls", LogicalType::BOOLEAN)
		    .Add("column_types", LogicalType::ANY)
		    .Add("dtypes", LogicalType::ANY)
		    .Add("types", LogicalType::ANY)
		    .Add("names", LogicalType::LIST(LogicalType::VARCHAR))
		    .Add("column_names", LogicalType::LIST(LogicalType::VARCHAR))
		    .Add("comment", LogicalType::VARCHAR)
		    .Add("encoding", LogicalType::VARCHAR)
		    .Add("strict_mode", LogicalType::BOOLEAN)
		    .Add("thousands", LogicalType::VARCHAR)
		    .Add("files_to_sniff", LogicalType::BIGINT);
	});
}

static void CSVReaderSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const BoundTableFunction &function) {
	throw NotImplementedException("CSVReaderSerialize not implemented");
	// auto &bind_data = bind_data_p->Cast<MultiFileBindData>();
	// auto &csv_data = bind_data.bind_data->Cast<ReadCSVData>();
	// serializer.WriteProperty(100, "extra_info", function.extra_info);
	// // generate the serialized data
	// SerializedReadCSVData serialized_data;
	// serialized_data.return_types = serialized_data.csv_types = bind_data.types;
	// serialized_data.return_names = serialized_data.csv_names = bind_data.names;
	// serialized_data.files = bind_data.file_list->GetAllFiles();
	// serialized_data.filename_col_idx = csv_data.filename_col_idx;
	// serialized_data.options.options = csv_data.options;
	// serialized_data.options.file_options = bind_data.file_options;
	// serialized_data.reader_bind = bind_data.reader_bind;
	// serializer.WriteProperty(101, "csv_data", serialized_data);
}

static unique_ptr<FunctionData> CSVReaderDeserialize(Deserializer &deserializer, BoundTableFunction &function) {
	throw NotImplementedException("CSVReaderDeserialize not implemented");
	// auto &context = deserializer.Get<ClientContext &>();
	// SerializedReadCSVData serialized_data;
	// deserializer.ReadProperty(100, "extra_info", function.extra_info);
	// deserializer.ReadProperty(101, "csv_data", serialized_data);
	//
	// vector<Value> file_path;
	// for (auto &path : serialized_data.files) {
	// 	file_path.emplace_back(path);
	// }
	// auto multi_file_reader = MultiFileReader::Create(function);
	// auto file_list = multi_file_reader->CreateFileList(context, Value::LIST(LogicalType::VARCHAR, file_path),
	//                                                    FileGlobOptions::ALLOW_EMPTY);
	// auto csv_options = make_uniq<CSVFileReaderOptions>();
	// csv_options->options = std::move(serialized_data.options.options);
	//
	// auto bind_data = MultiFileFunction<CSVMultiFileInfo>::MultiFileBindInternal(
	//     context, std::move(multi_file_reader), std::move(file_list), serialized_data.return_types,
	//     serialized_data.return_names, std::move(serialized_data.options.file_options), std::move(csv_options));
	// return bind_data;
}

static bool PushdownProjectionExpression(ClientContext &context, const TableFunctionProjectionExpressionInput &input) {
	if (!BoundCastExpression::IsCast(input.expr)) {
		return false;
	}
	const auto &cast = input.expr.Cast<BoundFunctionExpression>();
	const auto &target_type = cast.GetReturnType();
	auto &bind_data = input.get.bind_data->Cast<MultiFileBindData>();
	// With a custom schema (the 'schema' option, or a MultiFileReader that binds its own schema) the files are mapped
	// onto that schema, not onto bind_data.columns, so retyping the columns would not reach the column mapping.
	// See test/optimizer/pushdown/csv_type_pushdown_schema.test
	if (!bind_data.reader_bind.schema.empty()) {
		return false;
	}
	const idx_t idx = input.get.GetColumnIds()[input.column_index].GetPrimaryIndex();
	// Hive-partition and filename columns are produced from the file path by
	// separate finalize expressions, not parsed from the file. Retyping them
	// here would desync those expressions, so leave the cast in place.
	// See test/sql/copy/csv/csv_hive.test
	for (const auto &partition : bind_data.reader_bind.hive_partitioning_indexes) {
		if (partition.index == idx) {
			return false;
		}
	}
	if (bind_data.reader_bind.filename_idx.IsValid() && bind_data.reader_bind.filename_idx.GetIndex() == idx) {
		return false;
	}
	bind_data.types[idx] = target_type;
	bind_data.columns[idx].type = target_type;
	return true;
}

//! The same, for the multi-file wrapper around read_single_csv_file - every file is bound against the schema of the
//! scan, so the pushed-down type has to be applied there too. That way the CSV reader converts to it itself, and
//! "ignore_errors" applies to the conversions that fail
static bool PushdownProjectionExpressionMultiFile(ClientContext &context,
                                                  const TableFunctionProjectionExpressionInput &input) {
	if (!PushdownProjectionExpression(context, input)) {
		return false;
	}
	auto &bind_data = input.get.bind_data->Cast<MultiFileBindData>();
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	const idx_t idx = input.get.GetColumnIds()[input.column_index].GetPrimaryIndex();
	if (idx < data.options.expected_types.size()) {
		data.options.expected_types[idx] = input.expr.Cast<BoundFunctionExpression>().GetReturnType();
	}
	return true;
}

TableFunction ReadCSVTableFunction::GetFunction() {
	// the multi-file CSV reader is the single-file CSV reader wrapped into a multi-file function
	auto read_csv = ReadCSVTableFunction::GetMultiFileFunction("read_csv");
	read_csv.serialize = CSVReaderSerialize;
	read_csv.deserialize = CSVReaderDeserialize;
	read_csv.projection_expression_pushdown = PushdownProjectionExpressionMultiFile;
	return read_csv;
}

TableFunction ReadCSVTableFunction::GetAutoFunction() {
	auto read_csv_auto = ReadCSVTableFunction::GetFunction();
	read_csv_auto.SetName("read_csv_auto");
	return read_csv_auto;
}

void ReadCSVTableFunction::RegisterFunction(BuiltinFunctions &set) {
	set.AddFunction(MultiFileReader::CreateFunctionSet(ReadCSVTableFunction::GetFunction()));
	set.AddFunction(MultiFileReader::CreateFunctionSet(ReadCSVTableFunction::GetAutoFunction()));
	// the single-file CSV reader that read_csv is built on
	TableFunctionSet single_file_set("read_single_csv_file");
	single_file_set.AddFunction(ReadCSVTableFunction::GetSingleFileFunction());
	set.AddFunction(std::move(single_file_set));
}

unique_ptr<TableRef> ReadCSVReplacement(ClientContext &context, ReplacementScanInput &input,
                                        optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	auto lower_name = StringUtil::Lower(table_name);
	// remove any compression
	if (StringUtil::EndsWith(lower_name, CompressionExtensionFromType(FileCompressionType::GZIP))) {
		lower_name = lower_name.substr(0, lower_name.size() - 3);
	} else if (StringUtil::EndsWith(lower_name, CompressionExtensionFromType(FileCompressionType::ZSTD))) {
		if (!Catalog::TryAutoLoad(context, "parquet")) {
			throw MissingExtensionException("parquet extension is required for reading zst compressed file");
		}
		lower_name = lower_name.substr(0, lower_name.size() - 4);
	}
	if (!StringUtil::EndsWith(lower_name, ".csv") && !StringUtil::Contains(lower_name, ".csv?") &&
	    !StringUtil::EndsWith(lower_name, ".tsv") && !StringUtil::Contains(lower_name, ".tsv?")) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::String(table_name));
	table_function->function = make_uniq<FunctionExpression>("read_csv_auto", std::move(children));

	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = Identifier(fs.ExtractBaseName(table_name));
	}

	return std::move(table_function);
}

void BuiltinFunctions::RegisterReadFunctions() {
	CSVCopyFunction::RegisterFunction(*this);
	ReadCSVTableFunction::RegisterFunction(*this);
	AddFunction(MultiFileReader::CreateFunctionSet(ReadDuckDBTableFunction::GetFunction()));
	auto &config = DBConfig::GetConfig(*transaction.db);
	config.replacement_scans.emplace_back(ReadCSVReplacement);
	config.replacement_scans.emplace_back(ReadDuckDBTableFunction::ReplacementScan);
}

} // namespace duckdb
