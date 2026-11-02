#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/table/range.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_handle.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/function/table/read_csv.hpp"

namespace duckdb {

struct CSVSniffFunctionData : public TableFunctionData {
	CSVSniffFunctionData() {
	}
	string path;
	// The CSV reader options
	CSVReaderOptions options;
	// If we want to force the match of the sniffer types
	bool force_match = true;
};

struct CSVSniffGlobalState : public GlobalTableFunctionState {
	CSVSniffGlobalState() {
	}
	bool done = false;
};

static unique_ptr<GlobalTableFunctionState> CSVSniffInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	return make_uniq<CSVSniffGlobalState>();
}

static unique_ptr<FunctionData> CSVSniffBind(ClientContext &context, TableFunctionBindInput &input,
                                             vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto result = make_uniq<CSVSniffFunctionData>();
	if (input.inputs[0].IsNull()) {
		throw BinderException("sniff_csv cannot take NULL as a file path parameter");
	}
	result->path = input.inputs[0].ToString();
	auto it = input.named_parameters.find("auto_detect");
	if (it != input.named_parameters.end()) {
		if (it->second.IsNull()) {
			throw BinderException("%s expects a non-null boolean value (e.g. TRUE or 1)", it->first);
		}
		if (!it->second.GetValue<bool>()) {
			throw InvalidInputException("sniff_csv function does not accept auto_detect variable set to false");
		}
		// otherwise remove it
		input.named_parameters.erase(it);
	}

	// If we want to force the match of the sniffer
	it = input.named_parameters.find("force_match");
	if (it != input.named_parameters.end()) {
		if (it->second.IsNull()) {
			throw BinderException("%s expects a non-null boolean value (e.g. TRUE or 1)", it->first);
		}
		result->force_match = it->second.GetValue<bool>();
		input.named_parameters.erase(it);
	}
	MultiFileOptions file_options;
	result->options.FromNamedParameters(input.named_parameters, context, file_options);
	result->options.Verify(file_options);

	// We want to return the whole CSV Configuration
	// 1. Delimiter
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("Delimiter");
	// 2. Quote
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("Quote");
	// 3. Escape
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("Escape");
	// 4. NewLine Delimiter
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("NewLineDelimiter");
	// 5. Comment
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("Comment");
	// 6. Skip Rows
	return_types.emplace_back(LogicalType::UBIGINT);
	names.emplace_back("SkipRows");
	// 7. Has Header
	return_types.emplace_back(LogicalType::BOOLEAN);
	names.emplace_back("HasHeader");
	// 8. List<Struct<Column-Name:Types>>
	child_list_t<LogicalType> struct_children {{"name", LogicalType::VARCHAR}, {"type", LogicalType::VARCHAR}};
	auto list_child = LogicalType::STRUCT(struct_children);
	return_types.emplace_back(LogicalType::LIST(list_child));
	names.emplace_back("Columns");
	// 9. Date Format
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("DateFormat");
	// 10. Timestamp Format
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("TimestampFormat");
	// 11. CSV read function with all the options used
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("UserArguments");
	// 12. CSV read function with all the options used
	return_types.emplace_back(LogicalType::VARCHAR);
	names.emplace_back("Prompt");
	return std::move(result);
}

string FormatOptions(char opt) {
	if (opt == '\'') {
		return "''";
	}
	if (opt == '\0') {
		return "";
	}
	string result;
	result += opt;
	return result;
}

string FormatOptions(string opt) {
	if (opt.size() == 1) {
		return FormatOptions(opt[0]);
	}
	return opt;
}

static void CSVSniffFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &global_state = data_p.global_state->Cast<CSVSniffGlobalState>();
	// Are we done?
	if (global_state.done) {
		return;
	}
	const CSVSniffFunctionData &data = data_p.bind_data->Cast<CSVSniffFunctionData>();

	auto &fs = FileSystem::GetFileSystem(context);
	auto files = fs.GlobFiles(data.path, FileGlobOptions::DISALLOW_EMPTY);
	if (files.size() > 1) {
		throw NotImplementedException("sniff_csv does not operate on more than one file yet");
	}

	// We must run the sniffer.
	auto sniffer_options = data.options;
	sniffer_options.file_path = files[0].path;

	auto buffer_manager = CSVBufferManager::Open(context, sniffer_options, sniffer_options.file_path, false);
	MultiFileOptions file_options;
	CSVSniffer sniffer(sniffer_options, file_options, buffer_manager, CSVStateMachineCache::Get(context));
	auto sniffer_result = sniffer.SniffCSV(data.force_match);
	if (sniffer.EmptyOrOnlyHeader()) {
		for (idx_t i = 0; i < sniffer_result.return_types.size(); i++) {
			if (!sniffer_options.sql_types_per_column.empty()) {
				if (sniffer_options.sql_types_per_column.find(sniffer_result.names[i]) !=
				    sniffer_options.sql_types_per_column.end()) {
					continue;
				}
			} else if (i < sniffer_options.sql_type_list.size()) {
				continue;
			}
			// we default to varchar if all files are empty or only have a header after all the sniffing
			sniffer_result.return_types[i] = LogicalType::VARCHAR;
		}
	}
	for (auto &type : sniffer_result.return_types) {
		if (type.id() == LogicalTypeId::SQLNULL) {
			type = LogicalType::VARCHAR;
		}
	}
	string str_opt;
	string separator = ", ";
	// Set output (called after all Appends below)

	// 1. Delimiter
	str_opt = sniffer_options.dialect_options.state_machine_options.delimiter.FormatValue();
	output.data[0].Append(Value(str_opt));
	// 2. Quote
	str_opt = sniffer_options.dialect_options.state_machine_options.quote.FormatValue();
	output.data[1].Append(Value(str_opt));
	// 3. Escape
	str_opt = sniffer_options.dialect_options.state_machine_options.escape.FormatValue();
	output.data[2].Append(Value(str_opt));
	// 4. NewLine Delimiter
	auto new_line_identifier = sniffer_options.NewLineIdentifierToString();
	output.data[3].Append(Value(new_line_identifier));
	// 5. Comment
	str_opt = sniffer_options.dialect_options.state_machine_options.comment.FormatValue();
	output.data[4].Append(Value(str_opt));
	// 6. Skip Rows
	auto skip_rows = sniffer_options.dialect_options.skip_rows.GetValue();
	output.data[5].Append(Value::UBIGINT(skip_rows));
	// 7. Has Header
	auto has_header = Value::BOOLEAN(sniffer_options.dialect_options.header.GetValue());
	output.data[6].Append(has_header);
	// 8. List<Struct<Column-Name:Types>> {'col1': 'INTEGER', 'col2': 'VARCHAR'}
	vector<Value> values;
	std::ostringstream columns;
	columns << "{";
	for (idx_t i = 0; i < sniffer_result.return_types.size(); i++) {
		child_list_t<Value> struct_children {{"name", sniffer_result.names[i]},
		                                     {"type", {sniffer_result.return_types[i].ToString()}}};
		values.emplace_back(Value::STRUCT(struct_children));
		columns << SQLString::ToString(sniffer_result.names[i].GetIdentifierName()) << ": "
		        << SQLString::ToString(sniffer_result.return_types[i].ToString());
		if (i != sniffer_result.return_types.size() - 1) {
			columns << separator;
		}
	}
	columns << "}";
	output.data[7].Append(Value::LIST(values));
	// 9. Date Format
	auto date_format = sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue();
	if (!date_format.Empty()) {
		output.data[8].Append(Value(date_format.format_specifier));
	} else {
		bool has_date = false;
		for (auto &c_type : sniffer_result.return_types) {
			if (c_type.id() == LogicalTypeId::DATE) {
				has_date = true;
				break;
			}
		}
		// Must be ISO 8601
		output.data[8].Append(has_date ? Value("%Y-%m-%d") : Value(nullptr));
	}

	// 10. Timestamp Format
	auto timestamp_format = sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue();
	if (!timestamp_format.Empty()) {
		output.data[9].Append(Value(timestamp_format.format_specifier));
	} else {
		output.data[9].Append(Value(nullptr));
	}

	// 11. The Extra User Arguments
	if (data.options.user_defined_parameters.empty()) {
		output.data[10].Append(Value());
	} else {
		output.data[10].Append(Value(data.options.GetUserDefinedParameters()));
	}

	// 12. csv_read string
	std::ostringstream csv_read;

	// Base, Path and auto_detect=false
	csv_read << "FROM read_csv(" << SQLString::ToString(files[0].path) << separator << "auto_detect=false" << separator;
	// 10.1. Delimiter
	if (!sniffer_options.dialect_options.state_machine_options.delimiter.IsSetByUser()) {
		csv_read << "delim="
		         << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.delimiter.GetValue())
		         << "'" << separator;
	}
	// 11.2. Quote
	if (!sniffer_options.dialect_options.state_machine_options.quote.IsSetByUser()) {
		csv_read << "quote="
		         << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.quote.GetValue()) << "'"
		         << separator;
	}
	// 11.3. Escape
	if (!sniffer_options.dialect_options.state_machine_options.escape.IsSetByUser()) {
		csv_read << "escape="
		         << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.escape.GetValue()) << "'"
		         << separator;
	}
	// 11.4. NewLine Delimiter
	if (!sniffer_options.dialect_options.state_machine_options.new_line.IsSetByUser()) {
		if (new_line_identifier != "mix") {
			csv_read << "new_line="
			         << "'" << new_line_identifier << "'" << separator;
		}
	}
	// 11.5. Skip Rows
	if (!sniffer_options.dialect_options.skip_rows.IsSetByUser()) {
		csv_read << "skip=" << sniffer_options.dialect_options.skip_rows.GetValue() << separator;
	}

	// 11.6. Comment
	if (!sniffer_options.dialect_options.state_machine_options.comment.IsSetByUser()) {
		csv_read << "comment="
		         << "'" << FormatOptions(sniffer_options.dialect_options.state_machine_options.comment.GetValue())
		         << "'" << separator;
	}

	// 11.7. Has Header
	if (!sniffer_options.dialect_options.header.IsSetByUser()) {
		csv_read << "header=" << has_header << separator;
	}
	// 11.8. column={'col1': 'INTEGER', 'col2': 'VARCHAR'}
	csv_read << "columns=" << columns.str();
	// 11.9. Date Format
	if (!sniffer_options.dialect_options.date_format[LogicalType::DATE].IsSetByUser()) {
		if (!sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue().format_specifier.empty()) {
			csv_read << separator << "dateformat="
			         << "'"
			         << sniffer_options.dialect_options.date_format[LogicalType::DATE].GetValue().format_specifier
			         << "'";
		} else {
			for (auto &c_type : sniffer_result.return_types) {
				// Must be ISO 8601
				if (c_type.id() == LogicalTypeId::DATE) {
					csv_read << separator << "dateformat="
					         << "'%Y-%m-%d'";
					break;
				}
			}
		}
	}
	// 11.10. Timestamp Format
	if (!sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].IsSetByUser()) {
		if (!sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue().format_specifier.empty()) {
			csv_read << separator << "timestampformat="
			         << "'"
			         << sniffer_options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue().format_specifier
			         << "'";
		}
	}
	// 11.11 User Arguments
	if (!data.options.user_defined_parameters.empty()) {
		csv_read << separator << data.options.GetUserDefinedParameters();
	}
	csv_read << ");";
	output.data[11].Append(Value(csv_read.str()));
	global_state.done = true;
}

void CSVSnifferFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunction csv_sniffer("sniff_csv", FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
	                          CSVSniffFunction, CSVSniffBind, CSVSniffInitGlobal);
	// Accept same options as the actual csv reader
	ReadCSVTableFunction::ReadCSVAddNamedParameters(csv_sniffer);
	MultiFileReader::AddParameters(csv_sniffer);
	csv_sniffer.GetSignature().ExtendTypedKwargs(
	    [](TypedKwargs &options) { options.Add("force_match", LogicalType::BOOLEAN); });
	set.AddFunction(csv_sniffer);
}
} // namespace duckdb
