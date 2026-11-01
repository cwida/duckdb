#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"

#include "duckdb/execution/operator/csv_scanner/csv_buffer.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"

namespace duckdb {

static idx_t DataRowsSniffed(const CSVSniffer &sniffer, const CSVReaderOptions &options) {
	const auto lines_sniffed = sniffer.LinesSniffed();
	const idx_t inferred_skip_rows =
	    options.dialect_options.skip_rows.IsSetByUser() ? 0 : options.dialect_options.skip_rows.GetValue();
	const idx_t header_rows = options.dialect_options.header.GetValue() ? 1 : 0;
	return lines_sniffed - MinValue(lines_sniffed, inferred_skip_rows + header_rows);
}

//! Function to do schema discovery over one CSV file or a list/glob of CSV files
CSVSchema CSVSchemaDiscovery::SchemaDiscovery(ClientContext &context, shared_ptr<CSVBufferManager> &buffer_manager,
                                              CSVReaderOptions &options, const MultiFileOptions &file_options,
                                              vector<LogicalType> &return_types, vector<Identifier> &names,
                                              MultiFileList &multi_file_list, bool replace_null_with_varchar) {
	vector<CSVSchema> schemas;
	const auto option_og = options;

	MultiFileListScanData file_scan;
	multi_file_list.InitializeScan(file_scan);
	OpenFileInfo file;
	if (!multi_file_list.Scan(file_scan, file)) {
		throw InvalidInputException("No columns found in CSV files. Provide the columns option or ensure at least one "
		                            "file contains a header or data row.");
	}

	// Here what we want to do is to sniff a given number of lines, if we have many files, we might go through them
	// to reach the number of lines.
	const idx_t required_number_of_lines = options.GetSampleSize();

	idx_t total_number_of_rows = 0;
	idx_t current_file = 0;
	options.file_path = file.path;

	buffer_manager = CSVBufferManager::Open(context, options, options.file_path, false);
	idx_t only_header_or_empty_files = 0;

	{
		CSVSniffer sniffer(options, file_options, buffer_manager, CSVStateMachineCache::Get(context));
		auto sniffer_result = sniffer.SniffCSV();
		const auto rows_read = DataRowsSniffed(sniffer, options);

		schemas.emplace_back(sniffer_result.names, sniffer_result.return_types, file.path, rows_read,
		                     buffer_manager->GetBuffer(0)->actual_size == 0);
		total_number_of_rows += sniffer.LinesSniffed();
		current_file++;
		if (sniffer.EmptyOrOnlyHeader()) {
			only_header_or_empty_files++;
		}
	}

	// We do a copy of the options to not pollute the options of the first file.
	const idx_t max_files_to_sniff =
	    options.files_to_sniff == -1 ? NumericLimits<idx_t>::Maximum() : NumericCast<idx_t>(options.files_to_sniff);
	while (total_number_of_rows < required_number_of_lines && current_file < max_files_to_sniff &&
	       multi_file_list.Scan(file_scan, file)) {
		auto option_copy = option_og;
		if (required_number_of_lines != NumericLimits<idx_t>::Maximum()) {
			option_copy.SetSampleSize(required_number_of_lines - total_number_of_rows);
		}
		option_copy.file_path = file.path;
		auto file_buffer_manager = CSVBufferManager::Open(context, option_copy, option_copy.file_path, false);
		// TODO: We could cache the sniffer to be reused during scanning. Currently that's an exercise left to the
		// reader
		CSVSniffer sniffer(option_copy, file_options, file_buffer_manager, CSVStateMachineCache::Get(context));
		auto sniffer_result = sniffer.SniffCSV();
		const auto rows_read = DataRowsSniffed(sniffer, option_copy);
		if (file_buffer_manager->GetBuffer(0)->actual_size == 0) {
			schemas.emplace_back(true);
		} else {
			schemas.emplace_back(sniffer_result.names, sniffer_result.return_types, option_copy.file_path, rows_read);
		}
		total_number_of_rows += sniffer.LinesSniffed();
		if (sniffer.EmptyOrOnlyHeader()) {
			only_header_or_empty_files++;
		}
		current_file++;
	}

	// We might now have multiple schemas, we need to go through them to define the one true schema
	CSVSchema best_schema;
	for (auto &schema : schemas) {
		if (best_schema.Empty()) {
			// A schema is bettah than no schema
			best_schema = schema;
		} else if (best_schema.GetRowsRead() == 0) {
			// If the best-schema has no data-rows, that's easy; we just take the new schema
			best_schema = schema;
		} else if (schema.GetRowsRead() != 0) {
			// We might have conflicting-schemas, we must merge them
			best_schema.MergeSchemas(schema, options.null_padding);
		}
	}

	// At this point, replace a sqlnull with varchar for the type
	if (replace_null_with_varchar) {
		best_schema.ReplaceNullWithVarchar();
	}

	if (names.empty()) {
		names = StringsToIdentifiers(best_schema.GetNames());
		return_types = best_schema.GetTypes();
	}
	if (names.empty() && return_types.empty()) {
		throw InvalidInputException("No columns found in CSV files. Provide the columns option or ensure at least one "
		                            "file contains a header or data row.");
	}
	if (replace_null_with_varchar && only_header_or_empty_files == current_file && !options.columns_set) {
		for (idx_t i = 0; i < return_types.size(); i++) {
			if (!options.sql_types_per_column.empty()) {
				if (options.sql_types_per_column.find(names[i]) != options.sql_types_per_column.end()) {
					continue;
				}
			} else if (i < options.sql_type_list.size()) {
				continue;
			}
			// we default to varchar if all files are empty or only have a header after all the sniffing
			return_types[i] = LogicalType::VARCHAR;
		}
	}
	return best_schema;
}

} // namespace duckdb
