#include "duckdb/execution/operator/csv_scanner/global_csv_state.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/execution/operator/csv_scanner/scanner_boundary.hpp"
#include "duckdb/execution/operator/csv_scanner/skip_scanner.hpp"
#include "duckdb/execution/operator/persistent/csv_rejects_table.hpp"
#include "duckdb/main/appender.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/parallel/callback_async_task.hpp"

namespace duckdb {

CSVGlobalState::CSVGlobalState(ClientContext &context_p, ReadCSVData &csv_data_p,
                               const vector<Identifier> &column_names_p, idx_t total_file_count_p,
                               optional_ptr<const PhysicalOperator> scan_op_p)
    : context(context_p), csv_data(csv_data_p), scan_op(scan_op_p), total_file_count(total_file_count_p),
      column_names(column_names_p), sniffer_mismatch_error(csv_data_p.options.sniffer_user_mismatch_error) {
	auto &options = csv_data.options;
	// There are situations where we only support single threaded scanning
	auto system_threads = context.db->NumberOfThreads();
	bool many_csv_files = total_file_count > 1 && total_file_count > system_threads * 2;
	single_threaded = many_csv_files || !options.parallel;
	scanner_idx = 0;
	initialized = false;
}

// A task that loads the buffer on the async pool, sized for the read-ahead I/O budget
static unique_ptr<AsyncTask> BufferLoadTask(const shared_ptr<CSVBufferManager> &manager, const idx_t buffer_idx) {
	const idx_t io_size =
	    manager->HasKnownBufferRanges() ? manager->KnownBufferSize(buffer_idx) : manager->GetBufferSize();
	return make_uniq<CallbackAsyncTask>([manager, buffer_idx] { manager->GetBuffer(buffer_idx); }, io_size);
}

// Adds a load task when the buffer is not in memory
static void TryPushBufferLoadTask(const shared_ptr<CSVBufferManager> &manager, const idx_t buffer_idx,
                                  vector<unique_ptr<AsyncTask>> &io_tasks) {
	shared_ptr<CSVBufferHandle> buffer_handle;
	if (manager->GetBufferResidency(buffer_idx, buffer_handle) == CSVBufferResidency::NEEDS_LOAD) {
		io_tasks.push_back(BufferLoadTask(manager, buffer_idx));
	}
}

//! I/O tasks for the buffers of the claim's decode start that are not in memory
vector<unique_ptr<AsyncTask>> CSVCollectClaimIOTasks(CSVLocalState &lstate) {
	auto &manager = lstate.file_scan->buffer_manager;
	const idx_t start_buffer_idx = lstate.iterator.GetBufferIdx();
	vector<unique_ptr<AsyncTask>> io_tasks;
	if (manager->HasKnownBufferRanges() && start_buffer_idx >= manager->KnownBufferCount()) {
		// the claim starts past the last buffer (e.g. skipping the header consumed the whole file)
		return io_tasks;
	}
	TryPushBufferLoadTask(manager, start_buffer_idx, io_tasks);
	if (lstate.iterator.IsBoundarySet() &&
	    (!manager->HasKnownBufferRanges() ||
	     lstate.iterator.GetEndPos() >= manager->KnownBufferSize(start_buffer_idx))) {
		// a boundary reaching the end of its buffer also touches the next one, for straddling values
		// and first-line detection
		TryPushBufferLoadTask(manager, start_buffer_idx + 1, io_tasks);
	}
	return io_tasks;
}

void CSVGlobalState::FinishTask(CSVFileScan &scan) {
	auto started_tasks = scan.started_tasks.load();
	auto finished_tasks = ++scan.finished_tasks;
	if (finished_tasks == started_tasks) {
		// all scans finished for this file
		FinishFile(scan);
	} else if (finished_tasks > scan.started_tasks) {
		throw InternalException("Finished more tasks than were started for this file");
	}
}

void CSVGlobalState::FinishScan(unique_ptr<StringValueScanner> scanner) {
	if (!scanner) {
		return;
	}
	// We have to insert information for validation
	auto previous_file = scanner->csv_file_scan;
	previous_file->validator.Insert(scanner->scanner_idx, scanner->GetValidationLine());
	scanner.reset();
	FinishTask(*previous_file);
}

CSVLocalState::~CSVLocalState() {
	if (claim_state != ClaimState::PENDING) {
		return;
	}
	// the claim dies without ever being scanned: it still accounts its boundary lines
	file_scan->error_handler->Insert(iterator.GetBoundaryIdx(), 0);
	file_scan->error_handler->DontPrintErrorLine();
}

void CSVLocalState::Materialize() {
	D_ASSERT(claim_state == ClaimState::PENDING && !csv_reader);
	csv_reader =
	    make_uniq<StringValueScanner>(scanner_idx, file_scan->buffer_manager, file_scan->state_machine,
	                                  file_scan->error_handler, file_scan, false, iterator, STANDARD_VECTOR_SIZE, true);
	csv_reader->buffer_tracker = std::move(buffer_tracker);
	file_scan.reset();
	claim_state = ClaimState::MATERIALIZED;
}

bool CSVGlobalState::Next(shared_ptr<CSVFileScan> &current_file_ptr, CSVLocalState &lstate) {
	auto &current_file = *current_file_ptr;
	if (!initialized) {
		// initialize the boundary for this file
		current_boundary = current_file.start_iterator;
		current_boundary.SetCurrentBoundaryToPosition(single_threaded, current_file.options);
		current_buffer_in_use =
		    make_shared_ptr<CSVBufferUsage>(*current_file.buffer_manager, current_boundary.GetBufferIdx());
		initialized = true;
	} else {
		// produce the next boundary for this file
		if (current_boundary.done || !current_boundary.Next(*current_file.buffer_manager, current_file.options)) {
			// finished processing this file - return
			return false;
		}
	}
	if (!current_buffer_in_use || current_buffer_in_use->buffer_idx != current_boundary.GetBufferIdx()) {
		current_buffer_in_use =
		    make_shared_ptr<CSVBufferUsage>(*current_file.buffer_manager, current_boundary.GetBufferIdx());
	}
	++current_file.started_tasks;
	// The scanner itself is constructed by the decoding thread when the claim is first scanned
	lstate.scanner_idx = scanner_idx++;
	lstate.iterator = current_boundary;
	lstate.buffer_tracker = current_buffer_in_use;
	lstate.file_scan = current_file_ptr;
	lstate.claim_state = CSVLocalState::ClaimState::PENDING;
	return true;
}

void CSVGlobalState::FinishLaunchingTasks(CSVFileScan &file) {
	initialized = false;
	current_buffer_in_use.reset();
	// we are finished launching tasks for this file
	// finish a task to indicate we can begin cleanup once all scans are done
	FinishTask(file);
}

void CSVGlobalState::FinishFile(CSVFileScan &scan) {
	if (current_buffer_in_use && RefersToSameObject(current_buffer_in_use->buffer_manager, *scan.buffer_manager)) {
		current_buffer_in_use.reset();
	}
	scan.Finish();
	const bool ignore_or_store_errors =
	    csv_data.options.ignore_errors.GetValue() || csv_data.options.store_rejects.GetValue();
	if (!single_threaded && !ignore_or_store_errors) {
		// If we are running multithreaded and not ignoring errors, we must run the validator
		scan.validator.Verify();
	}
	scan.error_handler->ErrorIfAny();
	FillRejectsTable(scan);
}

void FillScanErrorTable(InternalAppender &scan_appender, idx_t scan_idx, idx_t file_idx, CSVFileScan &file) {
	CSVReaderOptions &options = file.options;
	// Add the row to the rejects table
	scan_appender.BeginRow();
	// 1. Scan Idx
	scan_appender.Append(scan_idx);
	// 2. File Idx
	scan_appender.Append(file_idx);
	// 3. File Path
	scan_appender.Append(string_t(file.GetFileName()));
	// 4. Delimiter
	scan_appender.Append(string_t(options.dialect_options.state_machine_options.delimiter.FormatValue()));
	// 5. Quote
	scan_appender.Append(string_t(options.dialect_options.state_machine_options.quote.FormatValue()));
	// 6. Escape
	scan_appender.Append(string_t(options.dialect_options.state_machine_options.escape.FormatValue()));
	// 7. NewLine Delimiter
	scan_appender.Append(string_t(options.NewLineIdentifierToString()));
	// 8. Skip Rows
	scan_appender.Append(Value::UINTEGER(NumericCast<uint32_t>(options.dialect_options.skip_rows.GetValue())));
	// 9. Has Header
	scan_appender.Append(Value::BOOLEAN(options.dialect_options.header.GetValue()));

	auto &types = file.GetTypes();
	auto &names = file.GetNames();

	// 10. List<Struct<Column-Name:Types>> {'col1': 'INTEGER', 'col2': 'VARCHAR'}
	std::ostringstream columns;
	columns << "{";
	for (idx_t i = 0; i < types.size(); i++) {
		columns << SQLString::ToString(names[i]) << ": " << SQLString::ToString(types[i].ToString());
		if (i != types.size() - 1) {
			columns << ",";
		}
	}
	columns << "}";
	scan_appender.Append(string_t(columns.str()));
	// 11. Date Format
	auto date_format = options.dialect_options.date_format[LogicalType::DATE].GetValue();
	if (!date_format.Empty()) {
		scan_appender.Append(string_t(date_format.format_specifier));
	} else {
		scan_appender.Append(Value());
	}

	// 12. Timestamp Format
	auto timestamp_format = options.dialect_options.date_format[LogicalType::TIMESTAMP].GetValue();
	if (!timestamp_format.Empty()) {
		scan_appender.Append(string_t(timestamp_format.format_specifier));
	} else {
		scan_appender.Append(Value());
	}

	// 13. The Extra User Arguments
	if (options.user_defined_parameters.empty()) {
		scan_appender.Append(Value());
	} else {
		auto parameters = options.GetUserDefinedParameters();
		scan_appender.Append(string_t(parameters));
	}
	// Finish the row to the rejects table
	scan_appender.EndRow();
}

void CSVGlobalState::FillRejectsTable(CSVFileScan &scan) {
	auto &options = csv_data.options;

	if (!options.store_rejects.GetValue()) {
		return;
	}
	auto limit = options.rejects_limit;
	auto rejects = CSVRejectsTable::GetOrCreate(context, options.rejects_scan_name.GetValue(),
	                                            options.rejects_table_name.GetValue());
	const lock_guard<mutex> lock(rejects->write_lock);
	auto &errors_table = rejects->GetErrorsTable(context);
	auto &scans_table = rejects->GetScansTable(context);
	InternalAppender errors_appender(context, errors_table);
	InternalAppender scans_appender(context, scans_table);
	idx_t scan_idx = context.transaction.GetActiveQuery();

	// the files of a scan report under a block of indexes, so that the index of a file within its scan identifies
	// it - that keeps the indexes deterministic when files are read in parallel, and keeps the files of one scan
	// apart from those of another scan in the same query
	const idx_t rejects_file_idx =
	    rejects->GetFileIndexBase(scan_idx, scan_op.get(), total_file_count) + scan.GetFileIndex();
	scan.error_handler->FillRejectsTable(errors_appender, rejects_file_idx, scan_idx, scan, *rejects, column_names,
	                                     limit);
	if (rejects->count != 0) {
		rejects->count = 0;
		FillScanErrorTable(scans_appender, scan_idx, rejects_file_idx, scan);
	}
	errors_appender.Close();
	scans_appender.Close();
}

} // namespace duckdb
