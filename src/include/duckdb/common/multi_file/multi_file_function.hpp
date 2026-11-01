//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/multi_file/multi_file_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include <numeric>

namespace duckdb {

struct MultiFileReaderInterface {
	virtual ~MultiFileReaderInterface();

	//! Whether the scans of this reader report operator.bytes_scanned
	virtual bool ReportsBytesScanned() const {
		return false;
	}

	virtual void InitializeInterface(ClientContext &context, MultiFileReader &reader, MultiFileList &file_list);
	//! Set the default multi-file options of this reader - called before any option is parsed
	virtual void InitializeFileOptions(MultiFileOptions &file_options);
	virtual unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context,
	                                                            optional_ptr<TableFunctionInfo> info) = 0;
	virtual bool ParseCopyOption(ClientContext &context, const Identifier &key, const vector<Value> &values,
	                             BaseFileReaderOptions &options, vector<Identifier> &expected_names,
	                             vector<LogicalType> &expected_types) = 0;
	virtual bool ParseOption(ClientContext &context, const Identifier &key, const Value &val,
	                         MultiFileOptions &file_options, BaseFileReaderOptions &options) = 0;
	virtual void FinalizeCopyBind(ClientContext &context, BaseFileReaderOptions &options,
	                              const vector<Identifier> &expected_names, const vector<LogicalType> &expected_types);
	virtual unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                         unique_ptr<BaseFileReaderOptions> options) = 0;
	virtual void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<Identifier> &names,
	                        MultiFileBindData &bind_data) = 0;
	//! Combine the schemas of a set of files that were bound individually into a single schema
	//! The default implementation combines the return types of the files by name
	virtual void CombineSchemas(ClientContext &context, const vector<shared_ptr<BaseUnionData>> &union_data,
	                            bool union_by_name, vector<LogicalType> &return_types, vector<Identifier> &names);
	virtual void FinalizeBindData(MultiFileBindData &multi_file_data);
	virtual void GetBindInfo(const TableFunctionData &bind_data, BindInfo &info);
	virtual optional_idx MaxThreads(const MultiFileBindData &bind_data_p, const MultiFileGlobalState &global_state,
	                                FileExpandResult expand_result);
	virtual optional_idx MaxThreads(ClientContext &context, const MultiFileBindData &bind_data_p,
	                                const MultiFileGlobalState &global_state, FileExpandResult expand_result);
	virtual unique_ptr<GlobalTableFunctionState>
	InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data, MultiFileGlobalState &global_state) = 0;
	virtual unique_ptr<LocalTableFunctionState> InitializeLocalState(ClientContext &, GlobalTableFunctionState &) = 0;

	virtual bool SupportsReadAhead(const MultiFileBindData &bind_data) const {
		return false;
	}
	virtual shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                                BaseUnionData &union_data,
	                                                const MultiFileBindData &bind_data_p) = 0;
	virtual shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                                const OpenFileInfo &file, idx_t file_idx,
	                                                const MultiFileBindData &bind_data) = 0;
	virtual shared_ptr<BaseFileReader> CreateReader(ClientContext &context, const OpenFileInfo &file,
	                                                BaseFileReaderOptions &options,
	                                                const MultiFileOptions &file_options);
	virtual void FinishReading(ClientContext &context, GlobalTableFunctionState &global_state,
	                           LocalTableFunctionState &local_state);
	//! Emit one chunk after every file has been read. Implementations must sync inside this function so
	//! chunk is produced once per scan
	virtual bool FinalizeScan(ClientContext &context, GlobalTableFunctionState &global_state, DataChunk &output) {
		return false;
	}
	virtual unique_ptr<NodeStatistics> GetCardinality(const MultiFileBindData &bind_data, idx_t file_count) {
		return nullptr;
	}
	virtual unique_ptr<NodeStatistics> GetCardinality(ClientContext &context, const MultiFileBindData &bind_data,
	                                                  idx_t file_count) {
		return GetCardinality(bind_data, file_count);
	}
	virtual void GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data, virtual_column_map_t &result);
	virtual unique_ptr<MultiFileReaderInterface> Copy();
	virtual FileGlobInput GetGlobInput();
};

template <class OP>
class MultiFileFunction : public TableFunction {
public:
	explicit MultiFileFunction(
	    Identifier name_p, MultiFileReader::MultiFileParameters parameters = MultiFileReader::MultiFileParameters::ALL)
	    : TableFunction(std::move(name_p), FunctionSignature().AddPositionalOnly("path", LogicalType::VARCHAR),
	                    MultiFileScan, MultiFileBind, MultiFileInitGlobal, MultiFileInitLocal) {
		cardinality = MultiFileCardinality;
		table_scan_progress = MultiFileProgress;
		get_partition_data = MultiFileGetPartitionData;
		get_bind_info = MultiFileGetBindInfo;
		projection_pushdown = true;
		pushdown_complex_filter = MultiFileComplexFilterPushdown;
		get_partition_info = MultiFileGetPartitionInfo;
		get_virtual_columns = MultiFileGetVirtualColumns;
		get_metrics = MultiFileGetMetrics;
		MultiFileReader::AddParameters(*this, parameters);
	}

	static bool IsEmptyResult(const MultiFileBindData &bind_data) {
		return bind_data.file_options.allow_empty && bind_data.file_list->IsEmpty();
	}

	static unique_ptr<FunctionData> MultiFileBindInternal(ClientContext &context,
	                                                      unique_ptr<MultiFileReader> multi_file_reader_p,
	                                                      shared_ptr<MultiFileList> multi_file_list_p,
	                                                      vector<LogicalType> &return_types, vector<Identifier> &names,
	                                                      MultiFileOptions file_options_p,
	                                                      unique_ptr<BaseFileReaderOptions> options_p,
	                                                      unique_ptr<MultiFileReaderInterface> interface_p) {
		file_options_p.ValidateBoundSchema();
		auto &interface = *interface_p;

		auto result = make_uniq<MultiFileBindData>();
		result->multi_file_reader = std::move(multi_file_reader_p);
		result->file_list = std::move(multi_file_list_p);
		// auto-detect hive partitioning
		result->file_options = std::move(file_options_p);
		result->bind_data = interface.InitializeBindData(*result, std::move(options_p));
		result->interface = std::move(interface_p);

		if (IsEmptyResult(*result)) {
			result->types.emplace_back(LogicalType::BOOLEAN);
			result->names.emplace_back("empty");
			result->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(result->names, result->types);
			return_types = result->types;
			names = result->names;
			return std::move(result);
		}

		// NOTE: the types are checked first on purpose - asking a file list whether it is empty expands it, and a
		// list that is built lazily (e.g. Iceberg's) is not ready to be expanded before its reader has bound
		if (!return_types.empty() && result->file_list->IsEmpty()) {
			// restoring a serialized plan whose files were all pruned away by filter pushdown - there is no file
			// left to bind the readers on, but the schema is already known so we can use it as-is
			result->types = return_types;
			result->names = names;
			result->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(result->names, result->types);
			return std::move(result);
		}

		// now bind the readers
		// there are two ways of binding the readers
		// (1) MultiFileReader::Bind -> custom bind, used only for certain lakehouse extensions
		// (2) Interface::BindReader
		bool bound_on_first_file = true;
		if (result->multi_file_reader->Bind(result->file_options, *result->file_list, result->types, result->names,
		                                    result->reader_bind)) {
			// with the 'schema' option, hive partitioning is asked for (hive_partitioning or hive_types) rather than
			// detected; the keys' types still are. Readers that bind a schema of their own skip this
			if (!result->file_options.schema.empty() &&
			    (result->file_options.hive_partitioning || !result->file_options.hive_types_schema.empty())) {
				result->file_options.AutoDetectHivePartitioning(*result->file_list, context,
				                                                /* detect_partitioning = */ false);
			}
			result->multi_file_reader->BindOptions(result->file_options, *result->file_list, result->types,
			                                       result->names, result->reader_bind);
			bound_on_first_file = false;
		} else {
			result->file_options.AutoDetectHivePartitioning(*result->file_list, context);
			interface.BindReader(context, result->types, result->names, *result);
		}
		interface.FinalizeBindData(*result);
		if (result->file_options.file_row_number) {
			// the column is read from the row number virtual column, which not every reader provides
			virtual_column_map_t virtual_columns;
			interface.GetVirtualColumns(context, *result, virtual_columns);
			auto entry = virtual_columns.find(MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER);
			if (entry == virtual_columns.end()) {
				throw BinderException("The file_row_number option is not supported by this reader");
			}
			result->virtual_columns.insert(*entry);
		}

		if (return_types.empty()) {
			// no expected types - just copy the types
			return_types = result->types;
			names = result->names;
		} else {
			// We're deserializing from a previously successful bind call
			// verify that the amount of columns still matches
			if (return_types.size() != result->types.size()) {
				string file_string;
				if (!result->file_options.union_by_name && bound_on_first_file) {
					file_string = result->file_list->GetFirstFile().path;
				} else {
					for (auto &file : result->file_list->GetAllFiles()) {
						if (!file_string.empty()) {
							file_string += ",";
						}
						file_string += file.path;
					}
				}
				string extended_error;
				extended_error = "Table schema: ";
				for (idx_t col_idx = 0; col_idx < return_types.size(); col_idx++) {
					if (col_idx > 0) {
						extended_error += ", ";
					}
					extended_error += names[col_idx] + " " + return_types[col_idx].ToString();
				}
				extended_error += "\nParquet schema: ";
				for (idx_t col_idx = 0; col_idx < result->types.size(); col_idx++) {
					if (col_idx > 0) {
						extended_error += ", ";
					}
					extended_error += result->names[col_idx] + " " + result->types[col_idx].ToString();
				}
				extended_error += "\n\nPossible solutions:";
				extended_error += "\n* Manually specify which columns to insert using \"INSERT INTO tbl SELECT ... "
				                  "FROM read_parquet(...)\"";
				throw ConversionException(
				    "Failed to read file(s) \"%s\" - column count mismatch: expected %d columns but found %d\n%s",
				    file_string, return_types.size(), result->types.size(), extended_error);
			}
			// expected types - overwrite the types we want to read instead
			result->types = return_types;
			result->table_columns = IdentifiersToStrings(names);
		}
		result->columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(result->names, result->types);
		result->file_options.VerifyColumnStatistics(result->names, result->types);
		return std::move(result);
	}

	static unique_ptr<FunctionData> MultiFileBind(ClientContext &context, TableFunctionBindInput &input,
	                                              vector<LogicalType> &return_types, vector<Identifier> &names) {
		return MultiFileBindInterface(context, input, return_types, names, OP::CreateInterface(context));
	}

	//! Bind using an explicitly provided interface - used by wrappers that construct the interface themselves
	static unique_ptr<FunctionData> MultiFileBindInterface(ClientContext &context, TableFunctionBindInput &input,
	                                                       vector<LogicalType> &return_types, vector<Identifier> &names,
	                                                       unique_ptr<MultiFileReaderInterface> interface_p) {
		auto interface = std::move(interface_p);
		auto multi_file_reader = MultiFileReader::Create(input.table_function);

		auto glob_input = multi_file_reader->GetGlobInput(*interface);

		MultiFileOptions file_options;
		interface->InitializeFileOptions(file_options);
		for (auto &kv : input.named_parameters) {
			if (kv.first == "allow_empty") {
				multi_file_reader->ParseOption(kv.first, kv.second, file_options, context);
				if (file_options.allow_empty) {
					glob_input.allow_empty = true;
				}
				break;
			}
		}

		auto file_list = multi_file_reader->CreateFileList(context, input.inputs[0], glob_input);

		interface->InitializeInterface(context, *multi_file_reader, *file_list);

		auto options = interface->InitializeOptions(context, input.info);
		for (auto &kv : input.named_parameters) {
			if (multi_file_reader->ParseOption(kv.first, kv.second, file_options, context)) {
				continue;
			}
			if (interface->ParseOption(context, kv.first, kv.second, file_options, *options)) {
				continue;
			}
			throw NotImplementedException("Unimplemented option %s", kv.first);
		}
		return MultiFileBindInternal(context, std::move(multi_file_reader), std::move(file_list), return_types, names,
		                             std::move(file_options), std::move(options), std::move(interface));
	}

	static unique_ptr<FunctionData> MultiFileBindCopy(ClientContext &context, CopyFromFunctionBindInput &input,
	                                                  vector<Identifier> &expected_names,
	                                                  vector<LogicalType> &expected_types) {
		return MultiFileBindCopyInterface(context, input, expected_names, expected_types, OP::CreateInterface(context));
	}

	//! Bind a COPY using an explicitly provided interface - used by wrappers that construct the interface themselves
	static unique_ptr<FunctionData> MultiFileBindCopyInterface(ClientContext &context, CopyFromFunctionBindInput &input,
	                                                           vector<Identifier> &expected_names,
	                                                           vector<LogicalType> &expected_types,
	                                                           unique_ptr<MultiFileReaderInterface> interface_p) {
		auto interface = std::move(interface_p);
		auto multi_file_reader = MultiFileReader::CreateDefault("COPY");
		vector<string> paths = {input.info.file_path};
		auto glob_input = multi_file_reader->GetGlobInput(*interface);
		auto file_list = multi_file_reader->CreateFileList(context, paths, glob_input);

		interface->InitializeInterface(context, *multi_file_reader, *file_list);

		auto options = interface->InitializeOptions(context, nullptr);
		MultiFileOptions file_options;
		interface->InitializeFileOptions(file_options);
		file_options.auto_detect_hive_partitioning = false;

		for (auto &[option_name, option_values] : input.info.options) {
			if (multi_file_reader->ParseCopyOption(option_name, option_values, file_options)) {
				continue;
			}
			if (interface->ParseCopyOption(context, option_name, option_values, *options, expected_names,
			                               expected_types)) {
				continue;
			}
			throw NotImplementedException("Unsupported option for COPY FROM: %s", option_name);
		}
		interface->FinalizeCopyBind(context, *options, expected_names, expected_types);

		return MultiFileBindInternal(context, std::move(multi_file_reader), std::move(file_list), expected_types,
		                             expected_names, std::move(file_options), std::move(options), std::move(interface));
	}

	static unique_ptr<MultiFileList> MultiFileFilterPushdown(ClientContext &context, const MultiFileBindData &data,
	                                                         const vector<ColumnIndex> &column_indexes,
	                                                         optional_ptr<TableFilterSet> filters) {
		if (!filters) {
			return nullptr;
		}
		MultiFileDynamicPushdownInfo pushdown_info(context, data.file_options, data.names, data.types, column_indexes,
		                                           *filters);
		auto new_list = data.multi_file_reader->DynamicFilterPushdown(*data.file_list, pushdown_info);
		return new_list;
	}

	// Queries the metadataprovider for another file to scan, updating the files/reader lists in the process.
	// Returns true if resized
	static bool TryGetNextFile(MultiFileGlobalState &global_state, unique_lock<mutex> &parallel_lock) {
		D_ASSERT(parallel_lock.owns_lock());
		OpenFileInfo scanned_file;
		if (!global_state.file_list.Scan(global_state.file_list_scan, scanned_file)) {
			return false;
		}

		// Push the file in the reader data, to be opened later
		global_state.readers.push_back(make_uniq<MultiFileReaderData>(scanned_file));

		return true;
	}

	static ReaderInitializeType InitializeReader(MultiFileReaderData &reader_data, const MultiFileBindData &bind_data,
	                                             const vector<ColumnIndex> &global_column_ids,
	                                             optional_ptr<TableFilterSet> table_filters, ClientContext &context,
	                                             optional_idx file_idx, MultiFileGlobalState &global_state) {
		auto &reader = *reader_data.reader;
		// Mark the file in the file list we are scanning here
		reader.file_list_idx = file_idx;

		// 'reader_bind.schema' could be set explicitly by:
		// 1. The MultiFileReader::Bind call
		// 2. The 'schema' parquet option
		auto &global_columns = bind_data.reader_bind.schema.empty() ? bind_data.columns : bind_data.reader_bind.schema;
		return bind_data.multi_file_reader->InitializeReader(reader_data, bind_data, global_columns, global_column_ids,
		                                                     table_filters, context, global_state);
	}

	static bool TrySkipFileFromFilters(ClientContext &context, const MultiFileBindData &bind_data,
	                                   MultiFileGlobalState &global_state, MultiFileReaderData &reader_data,
	                                   idx_t file_index) {
		D_ASSERT(reader_data.file_state == MultiFileFileState::UNOPENED);
		if (reader_data.union_data || !MultiFileReader::CanSkipFileFromFilters(
		                                  context, reader_data.file_to_be_opened, file_index, bind_data.file_options,
		                                  bind_data.reader_bind, global_state.column_indexes, global_state.filters)) {
			return false;
		}
		reader_data.file_state = MultiFileFileState::SKIPPED;
		return true;
	}

	//! Opens a file already marked OPENING. The parallel lock must not be held on entry, it is re-locked on return.
	static bool OpenMarkedFile(ClientContext &context, const MultiFileBindData &bind_data,
	                           MultiFileGlobalState &global_state, MultiFileReaderData &current_reader_data,
	                           idx_t current_file_index, unique_lock<mutex> &parallel_lock) {
		D_ASSERT(!parallel_lock.owns_lock());
		D_ASSERT(current_reader_data.file_state == MultiFileFileState::OPENING);
		try {
			// hold the lock on the file we are opening, so other threads can wait for the file to be opened
			unique_lock<mutex> file_lock(*current_reader_data.file_mutex);

			bool can_skip_file = false;
			if (current_reader_data.union_data) {
				auto &union_data = *current_reader_data.union_data;
				current_reader_data.reader = bind_data.multi_file_reader->CreateReader(
				    context, *global_state.global_state, union_data, bind_data);
			} else {
				current_reader_data.reader = bind_data.multi_file_reader->CreateReader(
				    context, *global_state.global_state, current_reader_data.file_to_be_opened, current_file_index,
				    bind_data);
			}
			auto init_result = InitializeReader(current_reader_data, bind_data, global_state.column_indexes,
			                                    global_state.filters, context, current_file_index, global_state);
			if (init_result == ReaderInitializeType::SKIP_READING_FILE) {
				//! File can be skipped entirely, close it and move on
				can_skip_file = true;
			} else {
				current_reader_data.reader->PrepareReader(context, *global_state.global_state);
				if (global_state.read_ahead) {
					current_reader_data.reader->PrepareReadAhead(context, *global_state.global_state);
				}
			}

			// Now re-lock the state and add the reader
			parallel_lock.lock();
			global_state.files_opened++;
			if (can_skip_file) {
				current_reader_data.file_state = MultiFileFileState::SKIPPED;
				// release the reader so its file handle is closed; skipped files are
				// never scanned, so nothing else needs the reader
				current_reader_data.reader = nullptr;
				return false;
			}
			current_reader_data.file_state = MultiFileFileState::OPEN;
			return true;
		} catch (...) {
			parallel_lock.lock();
			global_state.error_opening_file = true;
			throw;
		}
	}

	static bool OpenFile(ClientContext &context, const MultiFileBindData &bind_data, MultiFileGlobalState &global_state,
	                     MultiFileReaderData &current_reader_data, idx_t current_file_index,
	                     unique_lock<mutex> &parallel_lock) {
		D_ASSERT(parallel_lock.owns_lock());
		D_ASSERT(current_reader_data.file_state == MultiFileFileState::UNOPENED);
		current_reader_data.file_state = MultiFileFileState::OPENING;
		parallel_lock.unlock();
		return OpenMarkedFile(context, bind_data, global_state, current_reader_data, current_file_index, parallel_lock);
	}

	//! Record an error of an async file open. Read-ahead is optional, and a scan drains the opens it scheduled
	//! before it goes away - without a read-ahead to report to, the error only marks the scan as failed
	static void PushAsyncOpenError(MultiFileGlobalState &gstate, ErrorData error) {
		if (gstate.read_ahead) {
			gstate.read_ahead->PushError(std::move(error));
		}
		gstate.error_opening_file = true;
	}

	//! Open a file on the read-ahead async pool. Runs off the operator thread; records errors instead of throwing.
	static void OpenMarkedFileAsync(ClientContext &context, const MultiFileBindData &bind_data,
	                                MultiFileGlobalState &gstate, MultiFileReaderData &reader_data, idx_t file_index,
	                                const shared_ptr<ReadAheadOpenFile> &open_file) {
		// OpenMarkedFile expects the parallel lock unlocked on entry and locks it before it returns
		unique_lock<mutex> parallel_lock(gstate.lock, std::defer_lock);
		try {
			if (OpenMarkedFile(context, bind_data, gstate, reader_data, file_index, parallel_lock)) {
				// keep the file counted while jobs retain the reader
				reader_data.reader->read_ahead_open_file = open_file;
			}
		} catch (std::exception &ex) {
			if (!parallel_lock.owns_lock()) {
				parallel_lock.lock();
			}
			PushAsyncOpenError(gstate, ErrorData(ex));
		} catch (...) { // LCOV_EXCL_START
			if (!parallel_lock.owns_lock()) {
				parallel_lock.lock();
			}
			PushAsyncOpenError(gstate, ErrorData("Unknown exception while opening a file"));
		} // LCOV_EXCL_STOP
		gstate.async_open_settled.notify_all();
	}

	//! Schedule async opens for upcoming unopened files on the async pool, ahead of decoding. Lock held on entry.
	//! Always schedules the front unopened file (forward progress), then fills up to the open-ahead window.
	static void ScheduleFileOpens(ClientContext &context, const MultiFileBindData &bind_data,
	                              MultiFileGlobalState &gstate, unique_lock<mutex> &parallel_lock) {
		auto &read_ahead = *gstate.read_ahead;
		idx_t file_index = gstate.file_index;
		idx_t files_ahead = 0;
		bool progress_guaranteed = false;
		while (!progress_guaranteed || read_ahead.CanScheduleOpen(files_ahead)) {
			const bool has_file_to_read = file_index < gstate.readers.size();
			if (!has_file_to_read && !TryGetNextFile(gstate, parallel_lock)) {
				return;
			}
			auto current_file_index = file_index++;
			auto &reader_data = *gstate.readers[current_file_index];
			switch (reader_data.file_state) {
			case MultiFileFileState::UNOPENED:
				if (TrySkipFileFromFilters(context, bind_data, gstate, reader_data, current_file_index)) {
					break; // skipped file - keep scanning for a real file to guarantee progress
				}
				reader_data.file_state = MultiFileFileState::OPENING;
				{
					MultiFileReaderData *reader_ptr = &reader_data;
					auto open_file = make_shared_ptr<ReadAheadOpenFile>();
					read_ahead.ScheduleFileOpen(
					    [&context, &bind_data, &gstate, reader_ptr, current_file_index, open_file]() {
						    OpenMarkedFileAsync(context, bind_data, gstate, *reader_ptr, current_file_index, open_file);
					    },
					    [&gstate]() {
						    // the reader stays in OPENING, so tell every waiter to stop instead of polling forever
						    gstate.error_opening_file = true;
						    gstate.async_open_settled.notify_all();
					    });
				}
				files_ahead++;
				progress_guaranteed = true;
				break;
			case MultiFileFileState::OPENING:
			case MultiFileFileState::OPEN:
				// the front file is already being/been opened - forward progress is guaranteed
				files_ahead++;
				progress_guaranteed = true;
				break;
			default:
				break; // SKIPPED / CLOSED - keep scanning ahead
			}
		}
	}

	//! Helper function that try to start opening a next file. Parallel lock should be locked when calling.
	static bool TryOpenNextFile(ClientContext &context, const MultiFileBindData &bind_data,
	                            MultiFileGlobalState &global_state, unique_lock<mutex> &parallel_lock) {
		if (!parallel_lock.owns_lock()) {
			InternalException::Throw("parallel_lock is not held in TryOpenNextFile, this should not happen");
		}

		const auto file_lookahead_limit = TaskScheduler::GetScheduler(context).NumberOfThreads();

		idx_t file_index = global_state.file_index;
		idx_t i = 0;
		while (i < file_lookahead_limit) {
			// Try to get new files to open in this loop too otherwise we will only ever open 1 file ahead
			const bool has_file_to_read = file_index < global_state.readers.size();
			if (!has_file_to_read && !TryGetNextFile(global_state, parallel_lock)) {
				return false;
			}
			auto current_file_index = file_index++;

			auto &current_reader_data = *global_state.readers[current_file_index];
			if (current_reader_data.file_state == MultiFileFileState::UNOPENED) {
				if (TrySkipFileFromFilters(context, bind_data, global_state, current_reader_data, current_file_index)) {
					//! Intentionally do not increase 'i'
					continue;
				}

				if (!OpenFile(context, bind_data, global_state, current_reader_data, current_file_index,
				              parallel_lock)) {
					//! Intentionally do not increase 'i'
					continue;
				}
				return true;
			}
			i++;
		}

		return false;
	}

	//! Wait for a file to become available. Parallel lock should be locked when calling.
	static void WaitForFile(idx_t file_index, MultiFileGlobalState &global_state, unique_lock<mutex> &parallel_lock) {
		while (true) {
			// Get pointer to file mutex before unlocking
			auto &file_mutex = *global_state.readers[file_index]->file_mutex;

			// To get the file lock, we first need to release the parallel_lock to prevent deadlocking. Note that this
			// requires getting the ref to the file mutex pointer with the lock still held: readers get be resized
			parallel_lock.unlock();
			unique_lock<mutex> current_file_lock(file_mutex);
			parallel_lock.lock();

			// Here we have both locks which means we can stop waiting if:
			// - the thread opening the file is done and the file is available
			// - the thread opening the file has failed
			// - the file was somehow scanned till the end while we were waiting
			if (!HasFilesToRead(global_state, parallel_lock) ||
			    global_state.readers[global_state.file_index]->file_state != MultiFileFileState::OPENING ||
			    global_state.error_opening_file) {
				return;
			}
		}
	}

	//! Wait for the front file's async open. Parallel lock should be locked when calling, it is held on return.
	static void WaitForAsyncOpen(ClientContext &context, MultiFileGlobalState &gstate,
	                             unique_lock<mutex> &parallel_lock) {
		D_ASSERT(parallel_lock.owns_lock());
		auto &read_ahead = *gstate.read_ahead;
		auto still_opening = [&]() {
			return HasFilesToRead(gstate, parallel_lock) && !gstate.error_opening_file &&
			       gstate.readers[gstate.file_index]->file_state == MultiFileFileState::OPENING;
		};
		while (still_opening()) {
			parallel_lock.unlock();
			// the open may be queued behind other async work or the async pool may be gone, so run tasks inline
			const bool ran_task = read_ahead.TryRunPendingTask();
			parallel_lock.lock();
			if (ran_task || !still_opening()) {
				continue;
			}
			// the open is in flight: sleep until it settles; the timeout bounds interrupt latency and
			// covers a cancellation that signals without the lock
			gstate.async_open_settled.wait_for(parallel_lock, std::chrono::milliseconds(10));
			parallel_lock.unlock();
			context.InterruptCheck();
			parallel_lock.lock();
		}
	}

	static void InitializeDecodeChunk(ClientContext &context, MultiFileLocalState &lstate,
	                                  MultiFileGlobalState &gstate) {
		auto &job = *lstate.job;
		auto &reader = *job.reader;
		auto &reader_data = *job.reader_data;
		auto &projection_ids = gstate.projection_ids;

		if (!reader_data.extra_columns.empty()) {
			if (!gstate.multi_file_reader_state || !gstate.multi_file_reader_state->supports_local_extra_columns) {
				throw InternalException(
				    "MultiFileReader added local extra columns without declaring support in its global state");
			}
		}
		if (!projection_ids.empty()) {
			vector<LogicalType> extra_columns;
			if (gstate.multi_file_reader_state) {
				extra_columns = reader_data.GetExtraColumns(*gstate.multi_file_reader_state);
			}
			const auto expected_expression_count = gstate.scanned_types.size() + extra_columns.size();
			if (reader_data.expressions.size() != expected_expression_count) {
				throw InternalException(
				    "MultiFileReader input schema contains %llu columns, but the reader produced %llu expressions",
				    expected_expression_count, reader_data.expressions.size());
			}
			for (idx_t i = 0; i < gstate.scanned_types.size(); i++) {
				if (reader_data.expressions[i]->GetReturnType() != gstate.scanned_types[i]) {
					throw InternalException(
					    "MultiFileReader input column %llu has type %s, but the reader expression has type %s", i,
					    gstate.scanned_types[i], reader_data.expressions[i]->GetReturnType());
				}
			}
			for (idx_t i = 0; i < extra_columns.size(); i++) {
				auto expression_idx = gstate.scanned_types.size() + i;
				if (reader_data.expressions[expression_idx]->GetReturnType() != extra_columns[i]) {
					throw InternalException(
					    "MultiFileReader extra column %llu has type %s, but the reader expression has type %s", i,
					    extra_columns[i], reader_data.expressions[expression_idx]->GetReturnType());
				}
			}
		}
		//! Initialize the intermediate chunk to be used by the underlying reader before being finalized
		vector<LogicalType> intermediate_chunk_types;
		auto &local_column_ids = reader.column_indexes;
		auto &local_columns = reader.GetColumns();
		for (idx_t i = 0; i < local_column_ids.size(); i++) {
			auto &local_id = local_column_ids[i];

			auto primary_index = local_id.GetPrimaryIndex();
			auto cast_entry = reader.cast_map.find(primary_index);
			auto expr_entry = reader.expression_map.find(ProjectionIndex(i));
			if (cast_entry != reader.cast_map.end()) {
				intermediate_chunk_types.push_back(cast_entry->second);
			} else if (expr_entry != reader.expression_map.end()) {
				auto &expression = expr_entry->second.expression;
				intermediate_chunk_types.push_back(expression->GetReturnType());
			} else if (local_id.IsRowIdColumn()) {
				//! FIXME: should this generically check for all virtual columns??
				intermediate_chunk_types.push_back(LogicalType::ROW_TYPE);
			} else {
				auto &col = local_columns[primary_index];
				//! FIXME: do we need to check whether the MultiFileInfo implementation supports pushdown ??
				if (local_id.HasType()) {
					intermediate_chunk_types.push_back(local_id.GetScanType());
				} else {
					intermediate_chunk_types.push_back(col.type);
				}
			}
		}
		lstate.scan_chunk.Destroy();
		lstate.scan_chunk.Initialize(BufferAllocator::Get(context), intermediate_chunk_types);

		auto &executor = lstate.executor;
		executor.ClearExpressions();
		if (!projection_ids.empty()) {
			for (auto &id : projection_ids) {
				executor.AddExpression(*reader_data.expressions[id]);
			}
		} else {
			for (auto &expr : reader_data.expressions) {
				executor.AddExpression(*expr);
			}
		}
		lstate.scan_chunk_file_index = job.file_index;
	}

	static MultiFileClaimResult ClaimNextJobInternal(ClientContext &context, const MultiFileBindData &bind_data,
	                                                 MultiFileGlobalState &gstate,
	                                                 ScanReadAheadJobWrapper<MultiFileScanJobState> &job) {
		unique_lock<mutex> parallel_lock(gstate.lock);

		while (true) {
			if (gstate.error_opening_file) {
				// the flag only says a file failed, the error itself lives on the read-ahead - report before ending
				if (gstate.read_ahead) {
					gstate.read_ahead->ThrowIfError();
				}
				return MultiFileClaimResult::EXHAUSTED;
			}

			//! If we don't have a file to read, and the MultiFileList has no new file for us - end the scan
			if (!HasFilesToRead(gstate, parallel_lock) && !TryGetNextFile(gstate, parallel_lock)) {
				bind_data.interface->FinishReading(context, *gstate.global_state, *job.scan_state);
				return MultiFileClaimResult::EXHAUSTED;
			}

			auto &current_reader_data = *gstate.readers[gstate.file_index];
			if (current_reader_data.file_state == MultiFileFileState::OPEN) {
				if (current_reader_data.reader->TryInitializeScan(context, *gstate.global_state, *job.scan_state)) {
					job.reader = current_reader_data.reader;
					if (!job.reader) {
						InternalException::Throw("MultiFileReader was moved");
					}
					// The current reader has data left to be scanned
					job.reader_data = current_reader_data;
					job.batch_index = gstate.batch_index++;
					job.file_index = gstate.file_index;
					parallel_lock.unlock();
					job.reader->PrepareScan(context, *gstate.global_state, *job.scan_state);
					return MultiFileClaimResult::CLAIMED;
				} else {
					// Set state to the next file
					++gstate.file_index;

					// Close current file
					current_reader_data.file_state = MultiFileFileState::CLOSED;

					//! Finish processing the file
					current_reader_data.reader->FinishFile(context, *gstate.global_state);
					current_reader_data.closed_reader = current_reader_data.reader;
					current_reader_data.reader = nullptr;
					continue;
				}
			} else if (current_reader_data.file_state == MultiFileFileState::SKIPPED) {
				//! This file does not need to be opened or closed, the filters have determined that this file can be
				//! skipped entirely
				++gstate.file_index;
				continue;
			}

			if (gstate.read_ahead) {
				// read-ahead: open files on the async pool ahead of decoding, then wait for the front one
				ScheduleFileOpens(context, bind_data, gstate, parallel_lock);
			} else if (TryOpenNextFile(context, bind_data, gstate, parallel_lock)) {
				continue;
			}

			// Check if the current file is being opened, in that case we need to wait for it.
			if (current_reader_data.file_state == MultiFileFileState::OPENING) {
				if (gstate.read_ahead) {
					WaitForAsyncOpen(context, gstate, parallel_lock);
				} else {
					WaitForFile(gstate.file_index, gstate, parallel_lock);
				}
			}
		}
	}

	static bool ClaimNextJob(ClientContext &context, const MultiFileBindData &bind_data, MultiFileGlobalState &gstate,
	                         ScanReadAheadJobWrapper<MultiFileScanJobState> &job) {
		return ClaimNextJobInternal(context, bind_data, gstate, job) == MultiFileClaimResult::CLAIMED;
	}

	static unique_ptr<LocalTableFunctionState>
	MultiFileInitLocal(ExecutionContext &context, TableFunctionInitInput &input, GlobalTableFunctionState *gstate_p) {
		auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
		auto &gstate = gstate_p->Cast<MultiFileGlobalState>();

		if (IsEmptyResult(bind_data)) {
			return nullptr;
		}

		auto result = make_uniq<MultiFileLocalState>(context.client);

		if (gstate.read_ahead) {
			// read-ahead jobs bring their own scan state, start empty and claim a job in the first Scan() call
			return std::move(result);
		}

		result->job = make_uniq<ScanReadAheadJobWrapper<MultiFileScanJobState>>();
		result->job->scan_state = bind_data.interface->InitializeLocalState(context.client, *gstate.global_state);

		if (!ClaimNextJob(context.client, bind_data, gstate, *result->job)) {
			// keep the local state so the scan can still emit FinalizeScan output
			result->job.reset();
			return std::move(result);
		}
		result->job_state = MultiFileJobState::SCHEDULE;
		return std::move(result);
	}

	static void InitializeReadAhead(ClientContext &context, const MultiFileBindData &bind_data,
	                                MultiFileGlobalState &gstate) {
		if (!bind_data.interface->SupportsReadAhead(bind_data)) {
			return;
		}
		gstate.read_ahead = ScanReadAhead::Create(context);
	}

	static unique_ptr<GlobalTableFunctionState> MultiFileInitGlobal(ClientContext &context,
	                                                                TableFunctionInitInput &input) {
		auto &bind_data = input.bind_data->CastNoConst<MultiFileBindData>();
		unique_ptr<MultiFileGlobalState> result;

		if (IsEmptyResult(bind_data)) {
			result = make_uniq<MultiFileGlobalState>(*bind_data.file_list);
			result->file_list.InitializeScan(result->file_list_scan);
			result->file_index = 0;
			result->column_indexes = input.column_indexes;
			result->filters = input.filters.get();
			result->op = input.op;
			result->max_threads = 1;
			return std::move(result);
		}

		// before instantiating a scan trigger a dynamic filter pushdown if possible
		auto new_list = MultiFileFilterPushdown(context, bind_data, input.column_indexes, input.filters);
		if (new_list) {
			result = make_uniq<MultiFileGlobalState>(std::move(new_list));
		} else {
			result = make_uniq<MultiFileGlobalState>(*bind_data.file_list);
		}
		auto &file_list = result->file_list;
		file_list.InitializeScan(result->file_list_scan);

		auto &global_columns = bind_data.reader_bind.schema.empty() ? bind_data.columns : bind_data.reader_bind.schema;
		result->multi_file_reader_state = bind_data.multi_file_reader->InitializeGlobalState(
		    context, bind_data.file_options, bind_data.reader_bind, file_list, global_columns, input.column_indexes);

		if (file_list.IsEmpty()) {
			result->readers.clear();
		} else if (!bind_data.union_readers.empty()) {
			for (auto &reader : bind_data.union_readers) {
				result->readers.push_back(make_uniq<MultiFileReaderData>(reader));
			}
			if (result->readers.size() != file_list.GetTotalFileCount()) {
				result->readers.clear();
			}
		} else if (bind_data.initial_reader) {
			// we can only use the initial reader if it was constructed from the first file
			if (bind_data.initial_reader->GetFileName() == file_list.GetFirstFile().path) {
				result->readers.push_back(make_uniq<MultiFileReaderData>(std::move(bind_data.initial_reader)));
			}
		}

		result->file_index = 0;
		result->column_indexes = input.column_indexes;
		result->filters = input.filters.get();
		result->op = input.op;
		result->global_state = bind_data.interface->InitializeGlobalState(context, bind_data, *result);
		result->max_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();

		// Ensure all readers are initialized and FileListScan is sync with readers list
		for (auto &reader_data : result->readers) {
			OpenFileInfo file_name;
			idx_t file_idx = result->file_list_scan.current_file_idx;
			file_list.Scan(result->file_list_scan, file_name);
			if (reader_data->union_data) {
				if (file_name.path != reader_data->union_data->GetFileName()) {
					InternalException::Throw("Mismatch in filename order and union reader order in multi file scan");
				}
			} else {
				D_ASSERT(reader_data->reader);
				if (file_name.path != reader_data->reader->GetFileName()) {
					InternalException::Throw("Mismatch in filename order and reader order in multi file scan");
				}
				auto init_result = InitializeReader(*reader_data, bind_data, input.column_indexes, input.filters,
				                                    context, file_idx, *result);
				result->files_opened++;
				if (init_result == ReaderInitializeType::SKIP_READING_FILE) {
					//! File can be skipped entirely, close it and move on
					reader_data->file_state = MultiFileFileState::SKIPPED;
					reader_data->reader = nullptr;
					result->file_index++;
				}
			}
		}

		auto expand_result = bind_data.file_list->GetExpandResult();
		auto max_threads = bind_data.interface->MaxThreads(context, bind_data, *result, expand_result);
		if (max_threads.IsValid()) {
			result->max_threads = MinValue<idx_t>(result->max_threads, max_threads.GetIndex());
		}
		bool require_extra_columns =
		    result->multi_file_reader_state && result->multi_file_reader_state->RequiresExtraColumns();
		if (input.CanRemoveFilterColumns() || require_extra_columns) {
			if (!input.projection_ids.empty()) {
				result->projection_ids = input.projection_ids;
			} else {
				result->projection_ids.resize(input.column_indexes.size());
				for (idx_t i = 0; i < input.column_indexes.size(); i++) {
					result->projection_ids[i] = i;
				}
			}

			const auto table_types = bind_data.types;
			for (const auto &col_idx : input.column_indexes) {
				auto column_id = col_idx.GetPrimaryIndex();
				if (col_idx.IsVirtualColumn()) {
					auto entry = bind_data.virtual_columns.find(column_id);
					if (entry == bind_data.virtual_columns.end()) {
						InternalException::Throw("MultiFileReader - virtual column definition not found");
					}
					result->scanned_types.emplace_back(entry->second.type);
				} else {
					result->scanned_types.push_back(col_idx.HasType() ? col_idx.GetScanType() : table_types[column_id]);
				}
			}
		}
		InitializeReadAhead(context, bind_data, *result);
		return std::move(result);
	}

	static OperatorPartitionData MultiFileGetPartitionData(ClientContext &context,
	                                                       TableFunctionGetPartitionInput &input) {
		auto &bind_data = input.bind_data->CastNoConst<MultiFileBindData>();
		auto &data = input.local_state->Cast<MultiFileLocalState>();
		auto &gstate = input.global_state->Cast<MultiFileGlobalState>();
		if (data.finalize_batch_index.IsValid()) {
			if (input.partition_info.RequiresPartitionColumns()) {
				InternalException::Throw("Cannot get partition columns for FinalizeScan output");
			}
			return OperatorPartitionData(data.finalize_batch_index.GetIndex());
		}
		auto &job = *data.job;
		OperatorPartitionData partition_data(job.batch_index);
		bind_data.multi_file_reader->GetPartitionData(context, bind_data.reader_bind, *job.reader_data,
		                                              gstate.multi_file_reader_state, input.partition_info,
		                                              partition_data);
		return partition_data;
	}

	//! Emit the current output to the caller, or signal the loop to continue when there is nothing to emit yet.
	static bool EmitOutput(TableFunctionInput &data_p, DataChunk &output) {
		if (output.size() == 0 && data_p.results_execution_mode == AsyncResultsExecutionMode::SYNCHRONOUS) {
			return false;
		}
		data_p.async_result = SourceResultType::HAVE_MORE_OUTPUT;
		return true;
	}

	static MultiFileDecodeResult DecodeCurrentJob(ClientContext &context, TableFunctionInput &data_p,
	                                              MultiFileLocalState &data, MultiFileGlobalState &gstate,
	                                              MultiFileBindData &bind_data, DataChunk &output) {
		auto &job = *data.job;
		if (data.scan_chunk_file_index != job.file_index) {
			// if the file changes we need to initialize the chunk again
			InitializeDecodeChunk(context, data, gstate);
		}
		auto &scan_chunk = data.scan_chunk;
		if (!data.resuming_blocked_scan) {
			// A BLOCKED scan leaves partial data in the chunk (e.g. filter prefetch from parquet) that the resume must
			// keep. Hence, we only reset if we are not resuming from a blocked scan.
			scan_chunk.Reset();
		}
		auto res = job.reader->Scan(context, *gstate.global_state, *job.scan_state, scan_chunk);

		data.resuming_blocked_scan = res.GetResultType() == AsyncResultType::BLOCKED;
		if (res.GetResultType() == AsyncResultType::BLOCKED) {
			return data_p.HandleBlocked(context, res) ? MultiFileDecodeResult::RETURN_TO_CALLER
			                                          : MultiFileDecodeResult::CONTINUE;
		}

		output.SetChildCardinality(scan_chunk.size());
		if (scan_chunk.size() > 0) {
			data.rows_scanned += scan_chunk.size();
			bind_data.multi_file_reader->FinalizeChunk(context, bind_data, *job.reader, *job.reader_data, scan_chunk,
			                                           output, data.executor, gstate.multi_file_reader_state);
			output.SetChildCardinality(output.size());
		}
		if (res.GetResultType() == AsyncResultType::HAVE_MORE_OUTPUT) {
			// More chunks left on this batch, lets keep going
			return EmitOutput(data_p, output) ? MultiFileDecodeResult::RETURN_TO_CALLER
			                                  : MultiFileDecodeResult::CONTINUE;
		}
		if (res.GetResultType() != AsyncResultType::FINISHED) {
			throw InternalException("Unexpected result in MultiFileScan, must be FINISHED, is %s",
			                        EnumUtil::ToChars(res.GetResultType()));
		}
		return MultiFileDecodeResult::JOB_FINISHED;
	}

	//! Claims the next job and schedules its I/O, filling io_tasks when the I/O was detached to the pool.
	//! Returns null when there are no more jobs to produce.
	static unique_ptr<ScanReadAheadJobWrapper<MultiFileScanJobState>>
	ProduceJob(ClientContext &context, const MultiFileBindData &bind_data, MultiFileGlobalState &gstate,
	           vector<unique_ptr<AsyncTask>> &io_tasks) {
		auto job = make_uniq<ScanReadAheadJobWrapper<MultiFileScanJobState>>();
		// jobs recycle finished scan states, create a fresh one when none was available
		job->scan_state = gstate.state_pool.TryPop();
		if (!job->scan_state) {
			job->scan_state = bind_data.interface->InitializeLocalState(context, *gstate.global_state);
		}
		if (!ClaimNextJob(context, bind_data, gstate, *job)) {
			return nullptr;
		}
		auto scheduled = job->reader->ScheduleIO(context, *gstate.global_state, *job->scan_state);
		if (scheduled.GetResultType() == AsyncResultType::BLOCKED) {
			// if we got blocked, we have tasks for the pool
			io_tasks = scheduled.ExtractAsyncTasks();
		}
		return job;
	}

	static ScanReadAheadAcquire AcquireNextPerThread(ClientContext &context, TableFunctionInput &input,
	                                                 MultiFileLocalState &lstate, MultiFileGlobalState &gstate,
	                                                 MultiFileBindData &bind_data) {
		if (lstate.job_state == MultiFileJobState::NONE) {
			if (!lstate.job || !ClaimNextJob(context, bind_data, gstate, *lstate.job)) {
				return ScanReadAheadAcquire::EXHAUSTED;
			}
			lstate.job_state = MultiFileJobState::SCHEDULE;
		}
		if (lstate.job_state == MultiFileJobState::SCHEDULE) {
			auto &job = *lstate.job;
			auto scheduled = job.reader->ScheduleIO(context, *gstate.global_state, *job.scan_state);
			lstate.job_state = MultiFileJobState::DECODE;
			if (scheduled.GetResultType() == AsyncResultType::BLOCKED && input.HandleBlocked(context, scheduled)) {
				return ScanReadAheadAcquire::PARKED;
			}
		}
		return ScanReadAheadAcquire::ACQUIRED;
	}

	static ScanReadAheadAcquire AcquireNextReadAhead(ClientContext &context, TableFunctionInput &data_p,
	                                                 MultiFileLocalState &lstate, MultiFileGlobalState &gstate,
	                                                 MultiFileBindData &bind_data) {
		auto &read_ahead = *gstate.read_ahead;
		if (lstate.job_state == MultiFileJobState::WAIT_IO) {
			// resuming after parking, the job's I/O has completed
			read_ahead.WaitForJob(*lstate.job);
			lstate.job_state = MultiFileJobState::DECODE;
			return ScanReadAheadAcquire::ACQUIRED;
		}
		if (!read_ahead.CanScheduleOpen(0)) {
			// the open file budget is full and the finished job may hold the last reference to its reader
			lstate.job.reset();
		}
		unique_ptr<ScanReadAheadJob> claimed;
		auto acquired = read_ahead.AcquireJob(
		    context, data_p,
		    [&](vector<unique_ptr<AsyncTask>> &io_tasks) { return ProduceJob(context, bind_data, gstate, io_tasks); },
		    claimed);
		if (acquired == ScanReadAheadAcquire::EXHAUSTED) {
			// finish scan states left in the recycle pool so per-file accounting completes
			auto pooled_states = gstate.state_pool.TakeAll();
			unique_lock<mutex> parallel_lock(gstate.lock);
			for (auto &state : pooled_states) {
				bind_data.interface->FinishReading(context, *gstate.global_state, *state);
			}
			return acquired;
		}
		lstate.job =
		    unique_ptr_cast<ScanReadAheadJob, ScanReadAheadJobWrapper<MultiFileScanJobState>>(std::move(claimed));
		lstate.job_state =
		    acquired == ScanReadAheadAcquire::PARKED ? MultiFileJobState::WAIT_IO : MultiFileJobState::DECODE;
		return acquired;
	}

	static void MultiFileScan(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		if (!data_p.local_state) {
			data_p.async_result = SourceResultType::FINISHED;
			return;
		}
		auto &data = data_p.local_state->Cast<MultiFileLocalState>();
		auto &gstate = data_p.global_state->Cast<MultiFileGlobalState>();
		auto &bind_data = data_p.bind_data->CastNoConst<MultiFileBindData>();

		do {
			// Acquire a job whenever we don't hold one ready to decode
			if (data.job_state != MultiFileJobState::DECODE) {
				auto acquired = gstate.read_ahead ? AcquireNextReadAhead(context, data_p, data, gstate, bind_data)
				                                  : AcquireNextPerThread(context, data_p, data, gstate, bind_data);
				switch (acquired) {
				case ScanReadAheadAcquire::PARKED:
					return;
				case ScanReadAheadAcquire::EXHAUSTED:
					if (bind_data.interface->FinalizeScan(context, *gstate.global_state, output)) {
						// finalized output has no job, give it its own batch index for GetPartitionData
						if (!data.finalize_batch_index.IsValid()) {
							lock_guard<mutex> guard(gstate.lock);
							data.finalize_batch_index = gstate.batch_index++;
						}
						data_p.async_result = SourceResultType::HAVE_MORE_OUTPUT;
						return;
					}
					data_p.async_result = SourceResultType::FINISHED;
					return;
				case ScanReadAheadAcquire::ACQUIRED:
					break;
				}
			}
			switch (DecodeCurrentJob(context, data_p, data, gstate, bind_data, output)) {
			case MultiFileDecodeResult::CONTINUE:
				continue;
			case MultiFileDecodeResult::RETURN_TO_CALLER:
				return;
			case MultiFileDecodeResult::JOB_FINISHED: {
				if (gstate.read_ahead) {
					// hand the scan state back so learned reader state carries over to jobs created later
					gstate.state_pool.Push(std::move(data.job->scan_state));
				}
				data.job_state = MultiFileJobState::NONE;
				// emit any trailing chunk, then loop to acquire the next job
				if (EmitOutput(data_p, output)) {
					return;
				}
				continue;
			}
			}
		} while (true);
	}

	static unique_ptr<BaseStatistics> MultiFileScanStatsExtended(ClientContext &context,
	                                                             TableFunctionGetStatisticsInput &input) {
		auto result = MultiFileScanStatsInternal(context, input);
		if (!result) {
			return nullptr;
		}
		auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
		auto &column_index = input.column_index;
		if (!column_index.IsVirtualColumn() && !column_index.IsPushdownExtract() &&
		    result->GetType() != bind_data.types[column_index.GetPrimaryIndex()]) {
			// the column is read as a type other than the one the file stores it as - the statistics of the file
			// describe the stored type, so they say nothing about the column the scan produces
			return nullptr;
		}
		return result;
	}

	static unique_ptr<BaseStatistics> MultiFileScanStatsInternal(ClientContext &context,
	                                                             TableFunctionGetStatisticsInput &input) {
		auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
		auto &column_index = input.column_index;

		if (!bind_data.initial_reader) {
			// no reader
			return nullptr;
		}
		// scanning single file and we have the metadata read already
		if (column_index.IsVirtualColumn()) {
			return nullptr;
		}

		auto primary_index = column_index.GetPrimaryIndex();
		const auto &col_name = bind_data.names[primary_index];
		auto &file_row_number_idx = bind_data.reader_bind.file_row_number_idx;
		if (file_row_number_idx.IsValid() && file_row_number_idx.GetIndex() == primary_index) {
			// the column is read from the row number virtual column of the reader
			if (bind_data.file_list->GetExpandResult() == FileExpandResult::MULTIPLE_FILES) {
				return nullptr;
			}
			return bind_data.initial_reader->GetVirtualColumnStatistics(
			    context, MultiFileReader::COLUMN_IDENTIFIER_FILE_ROW_NUMBER);
		}

		// a hive partitioning column overrides any file column of the same name - the statistics stored in the
		// file describe the overridden column and can even have a different type, so they cannot be used here
		for (auto &hive_partitioning_index : bind_data.reader_bind.hive_partitioning_indexes) {
			if (hive_partitioning_index.index == primary_index) {
				return nullptr;
			}
		}

		// statistics the caller gave for the column (the "column_statistics" option) stand in for the files' -
		// they describe the scan as a whole, which is what the optimizer asks about, where a file's describe one file
		if (!bind_data.file_options.column_statistics.IsNull()) {
			auto explicit_stats = bind_data.file_options.GetColumnStatistics(col_name, bind_data.types[primary_index]);
			if (explicit_stats) {
				if (!column_index.IsPushdownExtract()) {
					return explicit_stats;
				}
				auto storage_index = StorageIndex::FromColumnIndex(column_index);
				return explicit_stats->PushdownExtract(storage_index.GetChildIndexes()[0]);
			}
		}

		if (!bind_data.file_options.bound_schema.empty() && bind_data.IsGeneratedColumn(primary_index)) {
			// A physical shadow's bounds do not describe the generated path. Explicit scan statistics above still do.
			return nullptr;
		}
		// NOTE: we do not want to parse the file metadata for the sole purpose of getting column statistics
		const bool multiple_files = bind_data.file_list->GetExpandResult() == FileExpandResult::MULTIPLE_FILES;
		if (multiple_files && !bind_data.file_options.union_by_name) {
			// multiple files, but no union_by_name: no luck!
			return nullptr;
		}
		auto result = bind_data.initial_reader->GetStatistics(context, col_name);
		if (result && !bind_data.file_options.bound_schema.empty() &&
		    !bind_data.MatchesPhysicalColumn(primary_index, col_name, result->GetType())) {
			// Check the whole root before interpreting an extracted child's positional statistics.
			return nullptr;
		}
		if (result && multiple_files) {
			for (idx_t i = 1; i < bind_data.union_readers.size(); i++) {
				auto &union_reader = *bind_data.union_readers[i];
				auto stats = union_reader.GetStatistics(context, col_name);
				if (!stats || result->GetType() != stats->GetType()) {
					return nullptr;
				}
				result->Merge(*stats);
			}
		}
		if (!result || !column_index.IsPushdownExtract()) {
			return result;
		}
		auto storage_index = StorageIndex::FromColumnIndex(column_index);
		return result->PushdownExtract(storage_index.GetChildIndexes()[0]);
	}

	static double MultiFileProgress(ClientContext &context, const FunctionData *bind_data_p,
	                                const GlobalTableFunctionState *global_state) {
		auto &gstate = global_state->Cast<MultiFileGlobalState>();

		// get the file count - for >100 files we allow a lower bound to be given instead
		auto count_info = gstate.file_list.GetFileCount(100);
		while (count_info.type != FileExpansionType::ALL_FILES_EXPANDED) {
			// the entire glob has not yet been expanded - we don't know how many files there are exactly
			// we try to postpone expanding the glob by only expanding it once we have scanned 1% of the files
			// check if we have reached AT LEAST 1% of progress
			idx_t one_percent_min = count_info.count / 100;
			if (gstate.completed_file_index < one_percent_min) {
				// we have not - just report 0%
				return 0.0;
			}
			// we have reached 1% given the currently known (incomplete) list of files
			// retrieve more files to scan
			count_info = gstate.file_list.GetFileCount(count_info.count + 1);
		}
		auto total_count = count_info.count;
		if (total_count == 0) {
			return 100.0;
		}
		unique_lock<mutex> parallel_lock(gstate.lock);
		double total_progress = 100.0 * static_cast<double>(gstate.completed_file_index);
		// iterate over the files we haven't completed yet
		for (idx_t i = gstate.completed_file_index; i <= gstate.file_index && i < gstate.readers.size(); i++) {
			auto &reader_data_ptr = gstate.readers[i];
			if (!reader_data_ptr) {
				continue;
			}
			auto &reader_data = *reader_data_ptr;
			// Initialize progress_in_file with a default value to avoid uninitialized variable usage
			double progress_in_file = 0.0;
			if (reader_data.file_state == MultiFileFileState::OPEN) {
				// file is currently open - get the progress within the file
				progress_in_file = reader_data.reader->GetProgressInFile(context);
			} else if (reader_data.file_state == MultiFileFileState::SKIPPED) {
				// file was skipped (e.g. pruned by a filter) - there is nothing left to read
				progress_in_file = 100.0;
			} else if (reader_data.file_state == MultiFileFileState::CLOSED) {
				// file has been closed - check if the reader is still in use
				auto reader = reader_data.closed_reader.lock();
				if (!reader) {
					// reader has been destroyed - we are done with this file
					progress_in_file = 100.0;
				} else {
					// file is still being read
					progress_in_file = reader->GetProgressInFile(context);
				}
			}
			progress_in_file = MaxValue<double>(0.0, MinValue<double>(100.0, progress_in_file));
			total_progress += progress_in_file;
			if (i == gstate.completed_file_index && progress_in_file >= 100) {
				// if the progress in this file is 100, we have completed the file
				// we don't need to check anymore in the next iteration
				gstate.completed_file_index++;
			}
		}
		return total_progress / static_cast<double>(total_count);
	}

	static unique_ptr<NodeStatistics> MultiFileCardinality(ClientContext &context, const FunctionData *bind_data) {
		auto &data = bind_data->Cast<MultiFileBindData>();
		if (IsEmptyResult(data)) {
			return make_uniq<NodeStatistics>(idx_t(0));
		}
		auto file_list_cardinality_estimate = data.file_list->GetCardinality(context);
		if (file_list_cardinality_estimate) {
			return file_list_cardinality_estimate;
		}
		if (data.file_options.union_by_name) {
			// for UNION BY NAME - check if we can get a cardinality estimate from the (already opened) union readers
			bool has_exact_cardinality = true;
			idx_t cardinality = 0;
			for (auto &union_data : data.union_readers) {
				auto file_cardinality = union_data->TryGetCardinalityEstimate();
				if (!file_cardinality.IsValid()) {
					has_exact_cardinality = false;
					break;
				}
				cardinality += file_cardinality.GetIndex();
			}
			if (has_exact_cardinality) {
				return make_uniq<NodeStatistics>(cardinality);
			}
		}
		// get the file count - for >500 files we allow an estimate
		auto count_info = data.file_list->GetFileCount(500);
		idx_t estimated_file_count = count_info.count;
		if (count_info.type != FileExpansionType::ALL_FILES_EXPANDED) {
			// not all files have been expanded - it's probably twice as many files
			estimated_file_count *= 2;
		}
		return data.interface->GetCardinality(context, data, estimated_file_count);
	}

	static void MultiFileComplexFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
	                                           vector<unique_ptr<Expression>> &filters) {
		auto &data = bind_data_p->Cast<MultiFileBindData>();

		MultiFilePushdownInfo info(get.table_index, data.names, get.GetColumnIds(), get.extra_info);
		auto new_list =
		    data.multi_file_reader->ComplexFilterPushdown(context, *data.file_list, data.file_options, info, filters);

		if (new_list) {
			data.file_list = std::move(new_list);
			MultiFileReader::PruneReaders(data, *data.file_list);
		}
	}

	static BindInfo MultiFileGetBindInfo(TableFunctionGetBindInfoInput &input) {
		BindInfo bind_info(ScanType::EXTERNAL);
		auto &bind_data = input.bind_data->Cast<MultiFileBindData>();

		vector<Value> file_path;
		for (const auto &file : bind_data.file_list->GetDisplayFileList()) {
			file_path.emplace_back(file.path);
		}

		// LCOV_EXCL_START
		bind_info.InsertOption("file_path", Value::LIST(LogicalType::VARCHAR, file_path));
		bind_data.interface->GetBindInfo(*bind_data.bind_data, bind_info);
		bind_data.file_options.AddBatchInfo(bind_info);
		// LCOV_EXCL_STOP
		return bind_info;
	}

	static TablePartitionInfo MultiFileGetPartitionInfo(ClientContext &context, TableFunctionPartitionInput &input) {
		auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
		if (IsEmptyResult(bind_data)) {
			return TablePartitionInfo::NOT_PARTITIONED;
		}
		return bind_data.multi_file_reader->GetPartitionInfo(context, bind_data.reader_bind, input);
	}

	static virtual_column_map_t MultiFileGetVirtualColumns(ClientContext &context,
	                                                       optional_ptr<FunctionData> bind_data_p) {
		auto &bind_data = bind_data_p->Cast<MultiFileBindData>();
		virtual_column_map_t result;
		if (IsEmptyResult(bind_data)) {
			return result;
		}
		MultiFileReader::GetVirtualColumns(context, bind_data.reader_bind, result);

		bind_data.interface->GetVirtualColumns(context, bind_data, result);

		bind_data.virtual_columns = result;
		return result;
	}

	static void MultiFileGetMetrics(TableFunctionGetMetricsInput &input) {
		auto &gstate = input.global_state->Cast<MultiFileGlobalState>();
		if (input.local_state) {
			auto &local = input.local_state->Cast<MultiFileLocalState>();
			input.operator_metrics.rows_scanned = local.rows_scanned;
		}
		if (input.bind_data && input.bind_data->Cast<MultiFileBindData>().interface->ReportsBytesScanned()) {
			input.operator_metrics.bytes_scanned_reported = true;
		}
		auto files_loaded = gstate.files_opened.load();
		input.operator_metrics.AddExtraInfo("Total Files Read", std::to_string(files_loaded));

		constexpr size_t FILE_NAME_LIST_LIMIT = 5;
		auto file_paths = gstate.file_list.GetDisplayFileList(FILE_NAME_LIST_LIMIT + 1);
		if (!file_paths.empty()) {
			vector<std::string> file_path_names;
			for (idx_t i = 0; i < MinValue<idx_t>(file_paths.size(), FILE_NAME_LIST_LIMIT); i++) {
				file_path_names.push_back(file_paths[i].path);
			}
			if (files_loaded > FILE_NAME_LIST_LIMIT) {
				file_path_names.push_back("...");
			}
			auto list_of_types = StringUtil::Join(file_path_names, ", ");
			input.operator_metrics.AddExtraInfo("Filename(s)", list_of_types);
		}
	}

private:
	static bool HasFilesToRead(MultiFileGlobalState &gstate, unique_lock<mutex> &parallel_lock) {
		D_ASSERT(parallel_lock.owns_lock());
		return (gstate.file_index.load() < gstate.readers.size());
	}
};

} // namespace duckdb
