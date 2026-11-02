#include "duckdb/execution/operator/csv_scanner/csv_file_scanner.hpp"

#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/csv_scanner/skip_scanner.hpp"
#include "duckdb/function/table/read_csv.hpp"

namespace duckdb {

CSVFileScan::CSVFileScan(ClientContext &context, const OpenFileInfo &file_p, CSVReaderOptions options_p,
                         const MultiFileOptions &file_options, const vector<Identifier> &names,
                         const vector<LogicalType> &types, CSVSchema &file_schema, bool per_file_single_threaded,
                         shared_ptr<CSVBufferManager> buffer_manager_p, bool fixed_schema)
    : file(file_p), buffer_manager(std::move(buffer_manager_p)),
      error_handler(
          make_shared_ptr<CSVErrorHandler>(options_p.ignore_errors.GetValue(), options_p.store_rejects.GetValue())),
      options(std::move(options_p)) {
	// Initialize Buffer Manager
	if (!buffer_manager) {
		buffer_manager = CSVBufferManager::Open(context, options, file, per_file_single_threaded);
	}
	// Initialize On Disk and Size of file
	on_disk_file = buffer_manager->file_handle->OnDiskFile();
	file_size = buffer_manager->file_handle->FileSize();
	// Initialize State Machine
	auto &state_machine_cache = CSVStateMachineCache::Get(context);

	SetNamesAndTypes(names, types);
	if (options.auto_detect) {
		if (fixed_schema) {
			// schema of the file is fixed - only run the sniffer
			CSVSniffer sniffer(options, file_options, buffer_manager, state_machine_cache);
			sniffer.SniffCSV();
		} else if (file_schema.Empty()) {
			throw InternalException("CSV File Scanner cannot be created without a schema");
		} else if (buffer_manager->file_handle->FileSize() > 0) {
			options.file_path = file.path;
			CSVSniffer sniffer(options, file_options, buffer_manager, state_machine_cache, false);
			auto result = sniffer.AdaptiveSniff(file_schema);
			SetNamesAndTypes(result.names, result.return_types);
		}
	}
	if (options.dialect_options.num_cols == 0) {
		// We need to define the number of columns, if the sniffer is not running this must be in the sql_type_list
		options.dialect_options.num_cols = options.sql_type_list.size();
	}
	if (options.dialect_options.state_machine_options.new_line == NewLineIdentifier::NOT_SET) {
		options.dialect_options.state_machine_options.new_line = CSVSniffer::DetectNewLineDelimiter(*buffer_manager);
	}
	state_machine = make_shared_ptr<CSVStateMachine>(
	    state_machine_cache.Get(options.dialect_options.state_machine_options), options);
}

CSVFileScan::CSVFileScan(ClientContext &context, const OpenFileInfo &file_p, const CSVReaderOptions &options_p,
                         const MultiFileOptions &file_options)
    : file(file_p), error_handler(make_shared_ptr<CSVErrorHandler>(options_p.ignore_errors.GetValue(),
                                                                   options_p.store_rejects.GetValue())),
      options(options_p) {
	buffer_manager = CSVBufferManager::Open(context, options, file);
	// Initialize On Disk and Size of file
	on_disk_file = buffer_manager->file_handle->OnDiskFile();
	file_size = buffer_manager->file_handle->FileSize();
	// Sniff it (We only really care about dialect detection, if types or number of columns are different this will
	// error out during scanning)
	auto &state_machine_cache = CSVStateMachineCache::Get(context);
	// We sniff file if it has not been sniffed yet and either auto-detect is on, or union by name is on
	if ((options.auto_detect || file_options.union_by_name) && options.dialect_options.num_cols == 0) {
		CSVSniffer sniffer(options, file_options, buffer_manager, state_machine_cache);
		auto sniffer_result = sniffer.SniffCSV();
		if (names.empty()) {
			SetNamesAndTypes(sniffer_result.names, sniffer_result.return_types);
		}
	}
	if (options.dialect_options.num_cols == 0) {
		// We need to define the number of columns, if the sniffer is not running this must be in the sql_type_list
		options.dialect_options.num_cols = options.sql_type_list.size();
	}
	// Initialize State Machine
	state_machine = make_shared_ptr<CSVStateMachine>(
	    state_machine_cache.Get(options.dialect_options.state_machine_options), options);
	SetStart();
}

void CSVFileScan::SetStart() {
	idx_t rows_to_skip = options.GetSkipRows() + state_machine->dialect_options.header.GetValue();

	if (rows_to_skip == 0) {
		start_iterator.first_one = true;
		return;
	}
	SkipScanner skip_scanner(buffer_manager, state_machine, error_handler, rows_to_skip);
	skip_scanner.ParseChunk();
	skipped_rows = skip_scanner.GetLinesRead() - rows_to_skip;
	start_iterator = skip_scanner.GetIterator();
}

void CSVFileScan::SetNamesAndTypes(const vector<Identifier> &names_p, const vector<LogicalType> &types_p) {
	names = IdentifiersToStrings(names_p);
	types = types_p;
	columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(names_p, types);
}

void CSVFileScan::InitializeFileNamesTypes() {
	if (column_ids.empty()) {
		// This means that the columns from this file are irrelevant.
		// just read the first column
		file_types.emplace_back(LogicalType::VARCHAR);
		projected_columns.insert(0);
		projection_ids.emplace_back(0, 0);
		return;
	}

	for (idx_t i = 0; i < column_ids.size(); i++) {
		auto col_idx = MultiFileLocalIndex(i);
		auto column_id = column_ids[col_idx];
		file_types.emplace_back(types[column_id.GetId()]);
		projected_columns.insert(column_id.GetId());
		projection_ids.emplace_back(column_id.GetId(), col_idx);
	}

	if (column_ids.empty()) {
		file_types = types;
	}

	// We need to be sure that our types are also following the cast_map
	if (!cast_map.empty()) {
		for (idx_t i = 0; i < column_ids.size(); i++) {
			auto local_idx = MultiFileLocalIndex(i);
			auto entry = cast_map.find(column_ids[local_idx].GetId());
			if (entry != cast_map.end()) {
				file_types[i] = entry->second;
			}
		}
	}

	// We sort the types on the order of the parsed chunk
	std::sort(projection_ids.begin(), projection_ids.end());
	vector<LogicalType> sorted_types;
	for (idx_t i = 0; i < projection_ids.size(); ++i) {
		sorted_types.push_back(file_types[projection_ids[i].second]);
	}
	file_types = sorted_types;
}

const vector<string> &CSVFileScan::GetNames() {
	return names;
}
const vector<LogicalType> &CSVFileScan::GetTypes() {
	return types;
}

void CSVFileScan::InitializeProjection() {
	for (idx_t i = 0; i < options.dialect_options.num_cols; i++) {
		column_ids.push_back(MultiFileLocalColumnId(i));
	}
}

void CSVFileScan::Finish() {
	buffer_manager.reset();
}

double CSVFileScan::GetProgressInFile(ClientContext &context) {
	auto manager = buffer_manager;
	if (!manager) {
		// We are done with this file, so it's 100%
		return 100.0;
	}
	if (file_size == 0) {
		// empty file
		return 100.0;
	}
	auto &file_handle = *manager->file_handle;
	if (file_handle.compression_type == FileCompressionType::GZIP ||
	    file_handle.compression_type == FileCompressionType::ZSTD) {
		// compressed file: the file is decompressed ahead of parsing - the progress is the fraction of the file that
		// was decompressed, times the fraction of the decompressed bytes that was parsed
		auto decompressed_fraction = file_handle.GetProgress() / static_cast<double>(file_size);
		auto decompressed_bytes = file_handle.UncompressedBytesRead();
		double parsed_fraction = 0;
		if (decompressed_bytes > 0) {
			parsed_fraction = MinValue<double>(static_cast<double>(bytes_read.load(std::memory_order_relaxed)) /
			                                       static_cast<double>(decompressed_bytes),
			                                   1.0);
		}
		auto progress = compressed_progress.Update(
		    ProgressData {MinValue<double>(decompressed_fraction, 1.0) * parsed_fraction, 1.0, false});
		return progress.done * 100.0;
	}
	return static_cast<double>(bytes_read.load(std::memory_order_relaxed)) / static_cast<double>(file_size) * 100.0;
}

} // namespace duckdb
