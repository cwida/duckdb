#include "duckdb/main/relation/read_csv_relation.hpp"

#include "duckdb/execution/operator/csv_scanner/csv_buffer_manager.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_reader_options.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/common/multi_file/union_by_name.hpp"

namespace duckdb {

void ReadCSVRelation::InitializeAlias(const vector<string> &input) {
	D_ASSERT(!input.empty());
	const auto &csv_file = input[0];
	alias = StringUtil::Split(csv_file, ".")[0];
}

CSVReaderOptions ReadCSVRelationBind(const shared_ptr<ClientContext> &context, const vector<string> &input,
                                     named_parameter_map_t &options, vector<ColumnDefinition> &columns,
                                     MultiFileOptions &file_options) {
	auto file_list = MultiFileReader::CreateValueFromFileList(input);

	auto multi_file_reader = MultiFileReader::CreateDefault("ReadCSVRelation");
	vector<OpenFileInfo> files;
	files = multi_file_reader->CreateFileList(*context, file_list)->GetAllFiles();

	D_ASSERT(!files.empty());

	auto &file_name = files[0];
	CSVReaderOptions csv_options;
	csv_options.file_path = file_name.path;
	vector<string> empty;
	named_argument_map_t named_arguments;
	for (auto &option : options) {
		named_arguments.insert(option.first, option.second);
	}
	csv_options.FromNamedParameters(named_arguments, *context, file_options);

	// Run the auto-detect, populating the options with the detected settings
	SimpleMultiFileList multi_file_list(files);

	if (file_options.union_by_name) {
		vector<LogicalType> types;
		vector<Identifier> names;
		// sniff every file and unify the columns of the files by name
		identifier_map_t<idx_t> union_names_map;
		for (auto &file : files) {
			auto file_csv_options = csv_options;
			file_csv_options.file_path = file.path;
			CSVFileScan file_scan(*context, file, file_csv_options, file_options);
			UnionByName::CombineUnionTypes(file_scan.GetNames(), file_scan.GetTypes(), types, names, union_names_map);
		}
		if (!csv_options.sql_types_per_column.empty()) {
			const auto exception = CSVError::ColumnTypesError(csv_options.sql_types_per_column, names);
			if (!exception.error_message.empty()) {
				throw BinderException(exception.error_message);
			}
			for (idx_t i = 0; i < names.size(); i++) {
				auto it = csv_options.sql_types_per_column.find(names[i]);
				if (it != csv_options.sql_types_per_column.end()) {
					types[i] = csv_options.sql_type_list[it->second];
				}
			}
		}
		D_ASSERT(names.size() == types.size());
		for (idx_t i = 0; i < names.size(); i++) {
			columns.emplace_back(names[i], types[i]);
		}
	} else {
		if (csv_options.auto_detect) {
			vector<LogicalType> return_types;
			vector<Identifier> names;
			shared_ptr<CSVBufferManager> buffer_manager;
			CSVSchemaDiscovery::SchemaDiscovery(*context, buffer_manager, csv_options, file_options, return_types,
			                                    names, multi_file_list);
			for (idx_t i = 0; i < return_types.size(); i++) {
				columns.emplace_back(names[i], return_types[i]);
			}
		} else {
			for (idx_t i = 0; i < csv_options.sql_type_list.size(); i++) {
				D_ASSERT(csv_options.name_list.size() == csv_options.sql_type_list.size());
				columns.emplace_back(csv_options.name_list[i], csv_options.sql_type_list[i]);
			}
		}
		// After sniffing we can consider these set, so they are exported as named parameters
		// FIXME: This is horribly hacky, should be refactored at some point
		csv_options.dialect_options.state_machine_options.escape.ChangeSetByUserTrue();
		csv_options.dialect_options.state_machine_options.delimiter.ChangeSetByUserTrue();
		csv_options.dialect_options.state_machine_options.quote.ChangeSetByUserTrue();
		csv_options.dialect_options.state_machine_options.comment.ChangeSetByUserTrue();
		csv_options.dialect_options.header.ChangeSetByUserTrue();
		csv_options.dialect_options.skip_rows.ChangeSetByUserTrue();
	}
	return csv_options;
}

ReadCSVRelation::ReadCSVRelation(const shared_ptr<ClientContext> &context, const vector<string> &input,
                                 named_parameter_map_t &&options, string alias_p)
    : TableFunctionRelation(context, "read_csv_auto", {MultiFileReader::CreateValueFromFileList(input)}, nullptr,
                            false),
      alias(std::move(alias_p)) {
	MultiFileOptions file_options;

	InitializeAlias(input);
	CSVReaderOptions csv_options;
	context->RunFunctionInTransaction(
	    [&]() { csv_options = ReadCSVRelationBind(context, input, options, columns, file_options); });

	// Capture the options potentially set/altered by the auto-detection phase
	csv_options.ToNamedParameters(options);

	// No need to auto-detect again
	options["auto_detect"] = Value::BOOLEAN(false);
	SetNamedParameters(std::move(options));

	child_list_t<Value> column_names;
	for (idx_t i = 0; i < columns.size(); i++) {
		column_names.emplace_back(make_pair(columns[i].Name(), Value(columns[i].Type().ToString())));
	}

	if (!file_options.union_by_name) {
		AddNamedParameter("columns", Value::STRUCT(std::move(column_names)));
	}
	RemoveNamedParameterIfExists("names");
	RemoveNamedParameterIfExists("types");
	RemoveNamedParameterIfExists("dtypes");
}

Identifier ReadCSVRelation::GetAlias() {
	return Identifier(alias);
}

} // namespace duckdb
