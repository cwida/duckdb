#include "duckdb/common/multi_file/table_function_multi_file.hpp"

#include <algorithm>

#include "duckdb/common/bind_helpers.hpp"

#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// States
//===--------------------------------------------------------------------===//
class TableFunctionMultiFileGlobalState : public GlobalTableFunctionState {
public:
	//! The operator the files are scanned for, and how many files that scan reads
	optional_ptr<const PhysicalOperator> op;
	idx_t file_count = 1;
	//! What the wrapped function has reported for the files of this scan - collected whenever a file may have
	//! counted more, so that a reader can be released without losing what it counted.
	//! "scanned" is handed over and cleared on every report - the profiler sums what each thread reports, so a
	//! scanned row group must be handed over exactly once. "total" is the size of the scan, which every thread
	//! reports identically and the profiler does not sum, so it is never cleared. "bytes scanned" is handed over
	//! like "scanned"
	mutex metrics_lock;
	idx_t collected_scanned = 0;
	idx_t collected_total = 0;
	idx_t collected_bytes_scanned = 0;
};

class TableFunctionMultiFileLocalState : public LocalTableFunctionState {
public:
	explicit TableFunctionMultiFileLocalState(ClientContext &context) : thread_context(context) {
	}

	//! Thread context used to initialize the local state of the wrapped function
	ThreadContext thread_context;
	//! The reader the local state below belongs to - kept alive so its resources can still be released after the
	//! multi file reader has moved on to the next file
	shared_ptr<BaseFileReader> reader;
	//! The local state of the wrapped function
	unique_ptr<LocalTableFunctionState> local_state;
	//! Whether what the first scan of the current batch counted has been collected
	bool batch_metrics_collected = true;
};

//===--------------------------------------------------------------------===//
// Reader
//===--------------------------------------------------------------------===//
TableFunctionFileReader::TableFunctionFileReader(TableFunction function_p, OpenFileInfo file_p,
                                                 named_argument_map_t named_parameters_p,
                                                 TableFunctionMultiFileSettings settings_p)
    : BaseFileReader(std::move(file_p)), function(std::move(function_p)),
      named_parameters(std::move(named_parameters_p)), settings(std::move(settings_p)), file_is_assigned(false),
      exhausted(false) {
}

TableFunctionFileReader::~TableFunctionFileReader() = default;

string TableFunctionFileReader::GetReaderType() const {
	return settings.reader_type;
}

bool TableFunctionFileReader::UseCastMap() const {
	// when the function can produce columns as a different type than it bound them, the conversion is pushed into
	// the function instead of being applied to its output
	return settings.supports_cast_map;
}

void TableFunctionFileReader::BindFunction(ClientContext &context, const TableFunctionFileReaderOptions &options,
                                           const MultiFileOptions &file_options, bool schema_only,
                                           optional_ptr<const FunctionData> file_bind_data) {
	if (!function.bind) {
		throw InternalException("Table function %s cannot be wrapped in a multi file function - it has no bind",
		                        function.name);
	}
	// a function that takes the file as ANY gets it together with the options to open it with - one that takes a
	// VARCHAR only gets its path
	vector<Value> inputs;
	if (function.GetSignature().GetParameter(0).GetType().id() == LogicalTypeId::ANY) {
		inputs.emplace_back(file.ToValue());
	} else {
		inputs.emplace_back(file.path);
	}
	auto parameters = named_parameters;
	function.GetSignature().FillNamedDefaults(context, parameters);
	vector<LogicalType> input_table_types;
	vector<Identifier> input_table_names;
	TableFunctionRef empty_ref;
	BoundTableFunction bound_function(function);
	TableFunctionBindInput bind_input(inputs, parameters, input_table_types, input_table_names,
	                                  function.function_info.get(), nullptr, bound_function, empty_ref);
	TableFunctionFileBindInput file_input;
	file_input.multi_file_options = file_options;
	file_input.multi_file_scan = options.multi_file_scan;
	file_input.schema_only = schema_only;
	file_input.file_bind_data = file_bind_data;
	if (!options.expected_names.empty()) {
		// the schema of the scan is known upfront - bind this file against that schema
		file_input.expected_names = options.expected_names;
		file_input.expected_types = options.expected_types;
		file_input.expected_bind_data = options.schema_bind_data.get();
	}
	bind_input.multi_file_input = file_input;
	names.clear();
	types.clear();
	bind_data = function.bind(context, bind_input, types, names);
	// a function can describe what it bound in more detail than its names and types - e.g. with the field ids of its
	// columns, and options that describe the file, which are exposed as the metadata of the reader of the file
	optional<BindInfo> bind_info;
	if (function.get_bind_info && bind_data) {
		TableFunctionGetBindInfoInput bind_info_input(context, bind_data.get());
		bind_info = function.get_bind_info(bind_info_input);
	}
	if (settings.get_file_columns && bind_data) {
		// the function describes the columns of its file itself - names and types alone would lose their nested
		// structure and the identifiers that map the files of a scan onto one another
		columns = settings.get_file_columns(context, *bind_data);
	} else if (bind_info && !bind_info->column_identifiers.empty()) {
		columns.clear();
		for (idx_t i = 0; i < names.size(); i++) {
			columns.push_back(MultiFileColumnDefinition::CreateNested(names[i], types[i]));
		}
		for (auto &entry : bind_info->column_identifiers) {
			if (entry.column_index >= columns.size()) {
				throw InternalException("Table function %s attached an identifier to column %llu of file \"%s\", but "
				                        "bound %llu",
				                        function.name, entry.column_index, file.path, columns.size());
			}
			columns[entry.column_index].ResolveChildPath(entry.child_path).identifier = entry.identifier;
		}
	} else {
		columns = MultiFileColumnDefinition::ColumnsFromNamesAndTypes(names, types);
	}
	metadata.clear();
	if (bind_info) {
		// the options are kept unordered - sort them, so the same file always has the same metadata
		vector<string> keys;
		for (auto &option : bind_info->options) {
			keys.push_back(option.first);
		}
		std::sort(keys.begin(), keys.end());
		for (auto &key : keys) {
			metadata.insert(key, bind_info->options[key]);
		}
	}

	cardinality = optional_idx();
	if (function.cardinality) {
		auto node_stats = function.cardinality(context, bind_data.get());
		if (node_stats && node_stats->has_estimated_cardinality) {
			cardinality = node_stats->estimated_cardinality;
		}
	}
}

unique_ptr<BaseStatistics> TableFunctionUnionData::GetStatistics(ClientContext &context, const Identifier &name) {
	if (!statistics || !bind_data) {
		return nullptr;
	}
	for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
		if (Identifier(names[col_idx]) == name) {
			return statistics(context, bind_data.get(), col_idx);
		}
	}
	return nullptr;
}

shared_ptr<BaseUnionData> TableFunctionFileReader::GetUnionData(idx_t file_idx) {
	auto result = make_shared_ptr<TableFunctionUnionData>(file);
	result->names = IdentifiersToStrings(names);
	result->types = types;
	result->cardinality = cardinality;
	result->bind_data = bind_data;
	result->statistics = function.statistics;
	if (file_idx == 0) {
		// keep the first reader around so we don't need to bind it again
		result->reader = shared_from_this();
	}
	return std::move(result);
}

unique_ptr<BaseStatistics> TableFunctionFileReader::GetStatistics(ClientContext &context, const Identifier &name) {
	if (!function.statistics || !bind_data) {
		return nullptr;
	}
	for (idx_t col_idx = 0; col_idx < names.size(); col_idx++) {
		if (names[col_idx] == name) {
			return function.statistics(context, bind_data.get(), col_idx);
		}
	}
	return nullptr;
}

unique_ptr<BaseStatistics> TableFunctionFileReader::GetVirtualColumnStatistics(ClientContext &context,
                                                                               column_t virtual_column_id) {
	if (!function.statistics || !bind_data) {
		return nullptr;
	}
	return function.statistics(context, bind_data.get(), virtual_column_id);
}

void TableFunctionFileReader::AddVirtualColumn(column_t virtual_column_id) {
	if (!function.get_virtual_columns) {
		throw NotImplementedException("Table function %s does not support reading virtual columns", function.name);
	}
	// our caller has given this virtual column an index of its own, past the columns the function bound - the
	// function is told which virtual column that index stands for when it is initialized
	D_ASSERT(!columns.empty());
	virtual_columns[columns.size() - 1] = virtual_column_id;
}

void TableFunctionFileReader::CollectMetrics(ClientContext &context, GlobalTableFunctionState &gstate_p) {
	if (!function.get_metrics || !global_state) {
		return;
	}
	auto &gstate = gstate_p.Cast<TableFunctionMultiFileGlobalState>();
	// several threads can collect the metrics of a file at once - they are asked for under the lock, so that the
	// growth of the file is added only once
	lock_guard<mutex> guard(gstate.metrics_lock);
	OperatorMetrics file_metrics;
	TableFunctionGetMetricsInput input(context, bind_data.get(), nullptr, global_state.get(), file_metrics);
	function.get_metrics(input);
	gstate.collected_scanned += file_metrics.row_groups_scanned;
	gstate.collected_bytes_scanned += file_metrics.bytes_scanned;
	// the function reports the size of its file as a whole - only add what it has grown by since we last asked
	gstate.collected_total += file_metrics.total_row_groups_to_scan - collected_file_total;
	collected_file_total = file_metrics.total_row_groups_to_scan;
}

void TableFunctionFileReader::FinishFile(ClientContext &context, GlobalTableFunctionState &gstate) {
	// this reader is about to be released - collect what the wrapped function counted for its file
	CollectMetrics(context, gstate);
}

TableFunctionInitInput TableFunctionFileReader::GetInitInput(TableFunctionFileInitInput &file_input) const {
	// the multi file reader does the projection/filter pruning itself - the wrapped function only needs to emit the
	// (local) columns it is asked for, in order
	vector<idx_t> projection_ids;
	TableFunctionInitInput input(bind_data.get(), column_indexes, projection_ids, filters.get());
	input.op = scan_op;
	if (!virtual_columns.empty()) {
		file_input.virtual_columns = virtual_columns;
	}
	if (deletion_filter) {
		file_input.deletion_filter = *deletion_filter;
	}
	if (!expression_map.empty()) {
		file_input.expression_map = expression_map;
	}
	if (!filter_global_indices.empty()) {
		file_input.filter_global_indices = filter_global_indices;
	}
	file_input.file_index = file_list_idx;
	file_input.file_count = scan_file_count;
	if (!cast_map.empty()) {
		file_input.cast_map = cast_map;
	}
	input.multi_file_input = file_input;
	return input;
}

optional_idx TableFunctionFileReader::MaxThreads(ClientContext &context, GlobalTableFunctionState &gstate) {
	SetScanState(gstate);
	InitializeFunctionState(context);
	if (!global_state) {
		// no global state - the wrapped function is scanned by a single thread
		return 1;
	}
	return global_state->MaxThreads();
}

void TableFunctionFileReader::SetScanState(GlobalTableFunctionState &gstate) {
	auto &scan_state = gstate.Cast<TableFunctionMultiFileGlobalState>();
	scan_op = scan_state.op;
	scan_file_count = scan_state.file_count;
}

void TableFunctionFileReader::InitializeFunctionState(ClientContext &context) {
	CallOnce(initialize_once, [&] {
		if (!function.init_global) {
			return;
		}
		TableFunctionFileInitInput file_input;
		auto init_input = GetInitInput(file_input);
		global_state = function.init_global(context, init_input);
	});
}

void TableFunctionFileReader::PrepareReader(ClientContext &context, GlobalTableFunctionState &gstate) {
	SetScanState(gstate);
	InitializeFunctionState(context);
}

void TableFunctionFileReader::PrepareReadAhead(ClientContext &context, GlobalTableFunctionState &gstate) {
	if (!settings.prepare_read_ahead) {
		return;
	}
	// the file has been opened on the read-ahead pool - let the function pre-open what its scan needs, so that
	// claiming a batch does no I/O while the global lock of the scan is held
	SetScanState(gstate);
	InitializeFunctionState(context);
	TableFunctionInput input(bind_data.get(), nullptr, global_state.get());
	settings.prepare_read_ahead(context, input);
}

bool TableFunctionFileReader::TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
                                                LocalTableFunctionState &lstate_p) {
	if (exhausted) {
		return false;
	}
	// the initial reader obtained during binding is never prepared - initialize it here instead
	SetScanState(gstate);
	InitializeFunctionState(context);
	auto &lstate = lstate_p.Cast<TableFunctionMultiFileLocalState>();
	lstate.batch_metrics_collected = false;
	if (!function.init_local) {
		// the wrapped function has no local state - it can only be scanned by a single thread
		bool expected = false;
		if (!file_is_assigned.compare_exchange_strong(expected, true)) {
			return false;
		}
		lstate.local_state = nullptr;
	} else if (lstate.reader.get() != this && !(settings.reuses_local_state && lstate.local_state)) {
		// we are moving to a new file - initialize a local state for it, unless the function carries what it has
		// learned so far over to the next file in the one it already has
		TableFunctionFileInitInput file_input;
		auto init_input = GetInitInput(file_input);
		ExecutionContext execution_context(context, lstate.thread_context, nullptr);
		lstate.local_state = function.init_local(execution_context, init_input, global_state.get());
	}
	lstate.reader = shared_from_this();
	if (settings.claim_batch) {
		// the function scans the file in batches - claim one, so that every batch becomes a scan of its own that
		// the multi file reader can put back in order
		TableFunctionInput input(bind_data.get(), lstate.local_state.get(), global_state.get());
		return settings.claim_batch(context, input);
	}
	return true;
}

void TableFunctionFileReader::PrepareScan(ClientContext &context, GlobalTableFunctionState &,
                                          LocalTableFunctionState &lstate_p) {
	if (!settings.prepare_batch) {
		return;
	}
	auto &lstate = lstate_p.Cast<TableFunctionMultiFileLocalState>();
	TableFunctionInput input(bind_data.get(), lstate.local_state.get(), global_state.get());
	settings.prepare_batch(context, input);
}

AsyncResult TableFunctionFileReader::ScheduleIO(ClientContext &context, GlobalTableFunctionState &gstate,
                                                LocalTableFunctionState &lstate_p) {
	if (!settings.schedule_io) {
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	auto &lstate = lstate_p.Cast<TableFunctionMultiFileLocalState>();
	TableFunctionInput input(bind_data.get(), lstate.local_state.get(), global_state.get());
	auto result = settings.schedule_io(context, input);
	// the file may already have been finished while this batch was queued - collect what scheduling it counted
	CollectMetrics(context, gstate);
	return result;
}

AsyncResult TableFunctionFileReader::Scan(ClientContext &context, GlobalTableFunctionState &gstate,
                                          LocalTableFunctionState &lstate_p, DataChunk &chunk) {
	auto &lstate = lstate_p.Cast<TableFunctionMultiFileLocalState>();
	TableFunctionInput input(bind_data.get(), lstate.local_state.get(), global_state.get());
	// the wrapped function is always run synchronously - the multi file reader drives the async results itself
	input.async_result = AsyncResultType::IMPLICIT;
	input.results_execution_mode = AsyncResultsExecutionMode::SYNCHRONOUS;
	function.function(context, input, chunk);
	if (!lstate.batch_metrics_collected || chunk.size() == 0) {
		// the file may already have been finished while this batch was scanned, so what the scan counted is collected
		// here - once the first scan of the batch has run (Parquet counts a row group then), and again when the batch
		// ends (a function that counts as it reads, like CSV, counts until then)
		CollectMetrics(context, gstate);
		lstate.batch_metrics_collected = true;
	}
	if (chunk.size() == 0 && !settings.claim_batch) {
		// an empty chunk signals the end of the scan for this thread - when the function scans in batches it only
		// signals the end of the current batch, and the next batch is claimed by TryInitializeScan
		exhausted = true;
	}
	return AsyncResult::FromChunk(chunk);
}

void TableFunctionFileReader::FinishBatch(ClientContext &context, LocalTableFunctionState &local_state) {
	if (!settings.finish_batch) {
		return;
	}
	TableFunctionInput input(bind_data.get(), &local_state, global_state.get());
	settings.finish_batch(context, input);
}

double TableFunctionFileReader::GetProgressInFile(ClientContext &context) {
	if (!function.table_scan_progress) {
		return 0;
	}
	return function.table_scan_progress(context, bind_data.get(), global_state.get());
}

InsertionOrderPreservingMap<Value> TableFunctionFileReader::GetMetadata() const {
	return metadata;
}

//===--------------------------------------------------------------------===//
// Interface
//===--------------------------------------------------------------------===//
TableFunctionMultiFileWrapper::TableFunctionMultiFileWrapper(TableFunction function_p,
                                                             TableFunctionMultiFileSettings settings_p)
    : function(std::move(function_p)), settings(std::move(settings_p)) {
}

unique_ptr<MultiFileReaderInterface> TableFunctionMultiFileWrapper::CreateInterface(ClientContext &context) {
	throw InternalException("TableFunctionMultiFileWrapper is constructed from the function info instead");
}

unique_ptr<MultiFileReaderInterface> TableFunctionMultiFileWrapper::Copy() {
	return make_uniq<TableFunctionMultiFileWrapper>(function, settings);
}

FileGlobInput TableFunctionMultiFileWrapper::GetGlobInput() {
	return settings.glob_input;
}

void TableFunctionMultiFileWrapper::InitializeFileOptions(MultiFileOptions &file_options) {
	file_options.maximum_sample_files = settings.maximum_sample_files;
	file_options.sampled_schema_is_union = settings.sampled_schema_is_union;
}

unique_ptr<BaseFileReaderOptions> TableFunctionMultiFileWrapper::InitializeOptions(ClientContext &context,
                                                                                   optional_ptr<TableFunctionInfo>) {
	return make_uniq<TableFunctionFileReaderOptions>();
}

optional<pair<Identifier, LogicalType>> TableFunctionMultiFileWrapper::GetDeclaredOption(const Identifier &key) const {
	auto &signature = function.GetSignature();
	auto index = signature.GetParameterIndexByName(key);
	if (index.IsValid()) {
		auto &param = signature.GetParameter(index.GetIndex());
		if (param.GetKind() != FunctionParameterKind::KEYWORD_ONLY) {
			return {};
		}
		return make_pair(param.GetName(), param.GetType());
	}
	auto option_schema = signature.GetTypedKwargs();
	auto option = option_schema ? option_schema->Find(key) : nullptr;
	if (!option) {
		return {};
	}
	return make_pair(option->name, option->type);
}

bool TableFunctionMultiFileWrapper::ParseNamedParameter(const Identifier &key, const Value &val,
                                                        TableFunctionFileReaderOptions &options) const {
	auto declared_option = GetDeclaredOption(key);
	if (!declared_option) {
		return false;
	}
	// under the name of the option, whichever alias was used
	options.named_parameters[declared_option->first] = val;
	return true;
}

bool TableFunctionMultiFileWrapper::ParseOption(ClientContext &context, const Identifier &key, const Value &val,
                                                MultiFileOptions &file_options, BaseFileReaderOptions &options_p) {
	if (!ParseNamedParameter(key, val, options_p.Cast<TableFunctionFileReaderOptions>())) {
		return false;
	}
	if (!settings.sample_files_parameter.empty() && key == settings.sample_files_parameter) {
		// this parameter of the wrapped function also sets how many files are sampled to determine the schema - a
		// value we cannot use is left to the function to report, in its own words, when it binds
		file_options.TrySetMaximumSampleFiles(val);
	}
	return true;
}

bool TableFunctionMultiFileWrapper::ParseCopyOption(ClientContext &context, const Identifier &key,
                                                    const vector<Value> &values, BaseFileReaderOptions &options_p,
                                                    vector<Identifier> &, vector<LogicalType> &) {
	// COPY supports exactly the named parameters of the wrapped function - the only difference is that COPY passes
	// the values as a list, and that a bare option (e.g. "auto_detect") means "true"
	auto declared_option = GetDeclaredOption(key);
	if (!declared_option) {
		return false;
	}
	auto &type = declared_option->second;
	Value val;
	if (type.id() == LogicalTypeId::LIST || (type.id() == LogicalTypeId::ANY && values.size() != 1)) {
		// COPY passes the elements of a list-valued option as separate values - an option that takes any value is
		// given the list as-is, so that it can report what it expected itself
		val = ConvertVectorToValue(values);
	} else if (values.size() > 1) {
		throw BinderException("COPY parameter %s expects a single argument", key);
	} else if (values.empty()) {
		// a bare option is a shorthand for setting a flag - only boolean parameters can be given like that
		if (type.id() != LogicalTypeId::BOOLEAN) {
			throw BinderException("COPY parameter %s expects a single argument", key);
		}
		val = Value::BOOLEAN(true);
	} else if (type.id() == LogicalTypeId::ANY || values[0].type() == type) {
		val = values[0];
	} else {
		val = values[0].DefaultCastAs(type);
	}
	return ParseNamedParameter(key, val, options_p.Cast<TableFunctionFileReaderOptions>());
}

void TableFunctionMultiFileWrapper::FinalizeCopyBind(ClientContext &context, BaseFileReaderOptions &options_p,
                                                     const vector<Identifier> &expected_names,
                                                     const vector<LogicalType> &expected_types) {
	// COPY takes its columns from the target table - read every file using those columns
	auto &options = options_p.Cast<TableFunctionFileReaderOptions>();
	options.expected_names = expected_names;
	options.expected_types = expected_types;
}

unique_ptr<TableFunctionData>
TableFunctionMultiFileWrapper::InitializeBindData(MultiFileBindData &multi_file_data,
                                                  unique_ptr<BaseFileReaderOptions> options_p) {
	auto result = make_uniq<TableFunctionMultiFileData>();
	// the options carry the expected schema when it is known upfront (COPY takes it from the target table)
	result->options = std::move(options_p->Cast<TableFunctionFileReaderOptions>());
	return std::move(result);
}

optional_idx TableFunctionMultiFileWrapper::MaxThreads(ClientContext &context, const MultiFileBindData &bind_data,
                                                       const MultiFileGlobalState &global_state,
                                                       FileExpandResult expand_result) {
	if (expand_result == FileExpandResult::MULTIPLE_FILES) {
		// with multiple files the multi file reader parallelizes over the files
		return optional_idx();
	}
	// a single file - the parallelism is entirely determined by the wrapped function
	if (global_state.readers.empty() || global_state.readers[0]->file_state != MultiFileFileState::OPEN) {
		// the file has not been opened yet - we cannot know how many threads it wants
		return optional_idx();
	}
	return global_state.readers[0]->reader->Cast<TableFunctionFileReader>().MaxThreads(context,
	                                                                                   *global_state.global_state);
}

void TableFunctionMultiFileWrapper::CombineSchemas(ClientContext &context,
                                                   const vector<shared_ptr<BaseUnionData>> &union_data,
                                                   bool union_by_name, vector<LogicalType> &return_types,
                                                   vector<Identifier> &names) {
	schema_combined = true;
	// combine the types of the files by name - the function can replace or adjust the result
	MultiFileReaderInterface::CombineSchemas(context, union_data, union_by_name, return_types, names);
	if (settings.combine_schema) {
		vector<reference<const FunctionData>> bind_data;
		bool have_all_bind_data = true;
		for (auto &data : union_data) {
			auto &function_data = data->Cast<TableFunctionUnionData>().bind_data;
			if (!function_data) {
				have_all_bind_data = false;
				break;
			}
			bind_data.emplace_back(*function_data);
		}
		if (have_all_bind_data) {
			// when the function returns bind data, every file is read using it - otherwise the files are bound
			// individually and reconciled with the schema below
			TableFunctionCombineSchemaInput input(bind_data, union_by_name);
			combined_bind_data = settings.combine_schema(context, input, return_types, names);
		}
	}
	combined_names = names;
	combined_types = return_types;
	if (combined_bind_data) {
		// every file is re-bound against the schema the function combined - what they bound to on their own is stale
		ReleaseBindData(union_data);
	}
}

void TableFunctionMultiFileWrapper::GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data,
                                                      virtual_column_map_t &result) {
	if (!function.get_virtual_columns) {
		return;
	}
	// the virtual columns of the scan are those the wrapped function can produce for a file of it
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	for (auto &entry : function.get_virtual_columns(context, data.options.schema_bind_data.get())) {
		result.insert(entry);
	}
}

void TableFunctionMultiFileWrapper::FinalizeBindData(MultiFileBindData &multi_file_data) {
	auto &data = multi_file_data.bind_data->Cast<TableFunctionMultiFileData>();
	if (multi_file_data.initial_reader) {
		// the cardinality estimate comes from the first file - keep it, as a filter pushdown can prune that file's
		// reader before the optimizer asks for the estimate
		data.cardinality = multi_file_data.initial_reader->Cast<TableFunctionFileReader>().GetCardinality();
	}
	if (!multi_file_data.file_options.schema.empty()) {
		// the schema was given up front, so BindReader never ran to find out whether the scan reads several files
		data.options.multi_file_scan = multi_file_data.file_list->GetExpandResult() == FileExpandResult::MULTIPLE_FILES;
		return;
	}
	if (!schema_combined || !combined_bind_data) {
		// either the schema comes from a single file, or the schemas were combined by type. In the latter case every
		// file is bound on its own and the column mapper reconciles it with the combined schema - only a schema the
		// function combined itself can be used to read every file
		return;
	}
	data.options.expected_names = std::move(combined_names);
	data.options.expected_types = std::move(combined_types);
	data.options.schema_bind_data = std::move(combined_bind_data);
	// the readers that were opened to combine the schemas were bound before the combined schema was known - release
	// them so that every file is bound against the combined schema instead
	multi_file_data.union_readers.clear();
	multi_file_data.initial_reader = nullptr;
}

//! Drop the per-file bind data of a scan whose schema the function combined itself - every file is re-bound against
//! that combined schema, so what the files bound to on their own is of no use anymore
void TableFunctionMultiFileWrapper::ReleaseBindData(const vector<shared_ptr<BaseUnionData>> &union_data) {
	for (auto &data : union_data) {
		data->Cast<TableFunctionUnionData>().bind_data.reset();
	}
}

void TableFunctionMultiFileWrapper::BindReader(ClientContext &context, vector<LogicalType> &return_types,
                                               vector<Identifier> &names, MultiFileBindData &bind_data) {
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	// whether the scan reads several files. The file list is only asked here, where it has been created and is
	// about to be read anyway - a list that is built lazily is not ready to be expanded any earlier
	data.options.multi_file_scan = bind_data.file_list->GetExpandResult() == FileExpandResult::MULTIPLE_FILES;
	if (data.HasExpectedSchema()) {
		// the schema is already known (COPY) - there is no need to sample any files to determine it
		bind_data.file_options.maximum_sample_files = 1;
	}
	bind_data.reader_bind = bind_data.multi_file_reader->BindReader(context, return_types, names, *bind_data.file_list,
	                                                                bind_data, data.options, bind_data.file_options);
	if (!schema_combined && !data.HasExpectedSchema() && bind_data.initial_reader) {
		// the schema was taken from a single file - read every other file the same way that file is read
		auto &reader = bind_data.initial_reader->Cast<TableFunctionFileReader>();
		data.options.expected_names = reader.names;
		data.options.expected_types = reader.types;
		data.options.schema_bind_data = reader.bind_data;
	}
}

unique_ptr<GlobalTableFunctionState>
TableFunctionMultiFileWrapper::InitializeGlobalState(ClientContext &, MultiFileBindData &,
                                                     MultiFileGlobalState &global_state) {
	auto result = make_uniq<TableFunctionMultiFileGlobalState>();
	result->op = global_state.op;
	result->file_count = global_state.file_list.GetTotalFileCount();
	return std::move(result);
}

unique_ptr<LocalTableFunctionState> TableFunctionMultiFileWrapper::InitializeLocalState(ClientContext &context,
                                                                                        GlobalTableFunctionState &) {
	return make_uniq<TableFunctionMultiFileLocalState>(context);
}

bool TableFunctionMultiFileWrapper::SupportsReadAhead(const MultiFileBindData &bind_data) const {
	if (!settings.supports_read_ahead || !settings.schedule_io) {
		return false;
	}
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	auto file_bind_data = data.options.schema_bind_data;
	if (!file_bind_data && !bind_data.union_readers.empty()) {
		// the schema was combined from the files rather than taken from one of them - ask the first of them
		file_bind_data = bind_data.union_readers[0]->Cast<TableFunctionUnionData>().bind_data;
	}
	return settings.supports_read_ahead(file_bind_data.get());
}

void TableFunctionMultiFileWrapper::FinishReading(ClientContext &context, GlobalTableFunctionState &,
                                                  LocalTableFunctionState &lstate_p) {
	auto &lstate = lstate_p.Cast<TableFunctionMultiFileLocalState>();
	if (!lstate.reader || !lstate.local_state) {
		return;
	}
	lstate.reader->Cast<TableFunctionFileReader>().FinishBatch(context, *lstate.local_state);
}

shared_ptr<BaseFileReader> TableFunctionMultiFileWrapper::CreateReader(ClientContext &context, const OpenFileInfo &file,
                                                                       BaseFileReaderOptions &options_p,
                                                                       const MultiFileOptions &file_options) {
	auto &options = options_p.Cast<TableFunctionFileReaderOptions>();
	auto result = make_shared_ptr<TableFunctionFileReader>(function, file, options.named_parameters, settings);
	// this overload binds a file only to determine the schema of the scan - the file is read by a reader of its own
	result->BindFunction(context, options, file_options, true);
	return std::move(result);
}

shared_ptr<BaseFileReader> TableFunctionMultiFileWrapper::CreateReader(ClientContext &context,
                                                                       GlobalTableFunctionState &,
                                                                       const OpenFileInfo &file, idx_t,
                                                                       const MultiFileBindData &bind_data) {
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	auto result = make_shared_ptr<TableFunctionFileReader>(function, file, data.options.named_parameters, settings);
	result->BindFunction(context, data.options, bind_data.file_options);
	return std::move(result);
}

shared_ptr<BaseFileReader> TableFunctionMultiFileWrapper::CreateReader(ClientContext &context,
                                                                       GlobalTableFunctionState &,
                                                                       BaseUnionData &union_data,
                                                                       const MultiFileBindData &bind_data) {
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	auto &union_entry = union_data.Cast<TableFunctionUnionData>();
	auto result =
	    make_shared_ptr<TableFunctionFileReader>(function, union_data.file, data.options.named_parameters, settings);
	// this file was already bound to combine the schemas - the bind can reuse what that read from the file
	result->BindFunction(context, data.options, bind_data.file_options, false, union_entry.bind_data.get());
	return std::move(result);
}

unique_ptr<NodeStatistics> TableFunctionMultiFileWrapper::GetCardinality(ClientContext &context,
                                                                         const MultiFileBindData &bind_data,
                                                                         idx_t file_count) {
	auto &data = bind_data.bind_data->Cast<TableFunctionMultiFileData>();
	if (!data.cardinality.IsValid()) {
		return nullptr;
	}
	return make_uniq<NodeStatistics>(data.cardinality.GetIndex() * file_count);
}

//===--------------------------------------------------------------------===//
// Function creation
//===--------------------------------------------------------------------===//
using TableFunctionMultiFileFunction = MultiFileFunction<TableFunctionMultiFileWrapper>;

unique_ptr<FunctionData> TableFunctionMultiFileWrapper::MultiFileBindWith(
    ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types, vector<Identifier> &names,
    TableFunction single_file_function, TableFunctionMultiFileSettings settings) {
	return TableFunctionMultiFileFunction::MultiFileBindInterface(
	    context, input, return_types, names,
	    make_uniq<TableFunctionMultiFileWrapper>(std::move(single_file_function), std::move(settings)));
}

static unique_ptr<FunctionData> TableFunctionMultiFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                           vector<LogicalType> &return_types,
                                                           vector<Identifier> &names) {
	if (!input.table_function.function_info) {
		// the wrapped function is reached through the function info, which a caller binding the function itself has
		// to pass along
		throw InvalidInputException("Multi-file table function %s was bound without its function info",
		                            input.table_function.GetName());
	}
	auto &info = input.table_function.function_info->Cast<TableFunctionMultiFileInfo>();
	return TableFunctionMultiFileWrapper::MultiFileBindWith(context, input, return_types, names, info.function,
	                                                        info.settings);
}

unique_ptr<FunctionData> TableFunctionMultiFileWrapper::MultiFileBindCopy(ClientContext &context,
                                                                          CopyFromFunctionBindInput &input,
                                                                          vector<Identifier> &expected_names,
                                                                          vector<LogicalType> &expected_types) {
	auto &info = input.tf.function_info->Cast<TableFunctionMultiFileInfo>();
	return MultiFileBindCopyWith(context, input, expected_names, expected_types, info.function, info.settings);
}

unique_ptr<FunctionData> TableFunctionMultiFileWrapper::MultiFileBindCopyWith(
    ClientContext &context, CopyFromFunctionBindInput &input, vector<Identifier> &expected_names,
    vector<LogicalType> &expected_types, TableFunction single_file_function, TableFunctionMultiFileSettings settings) {
	return TableFunctionMultiFileFunction::MultiFileBindCopyInterface(
	    context, input, expected_names, expected_types,
	    make_uniq<TableFunctionMultiFileWrapper>(std::move(single_file_function), std::move(settings)));
}

//! Report the metrics of the scan: the shared multi-file ones, plus those the wrapped function keeps for each of
//! the files that have been opened
static void TableFunctionMultiFileGetMetrics(TableFunctionGetMetricsInput &input) {
	TableFunctionMultiFileFunction::MultiFileGetMetrics(input);
	if (!input.global_state) {
		return;
	}
	auto &gstate = input.global_state->Cast<MultiFileGlobalState>();
	if (!gstate.global_state) {
		return;
	}
	auto &scan_state = gstate.global_state->Cast<TableFunctionMultiFileGlobalState>();
	{
		// collect what the files that are still open have counted so far
		lock_guard<mutex> guard(gstate.lock);
		for (auto &reader_data : gstate.readers) {
			// a file that is still being opened has its reader assigned under its own lock - skip it until it is open
			if (!reader_data || reader_data->file_state != MultiFileFileState::OPEN || !reader_data->reader) {
				continue;
			}
			reader_data->reader->Cast<TableFunctionFileReader>().CollectMetrics(input.context, scan_state);
		}
	}
	if (input.local_state) {
		// a batch this thread stopped short, e.g. at a LIMIT, can have counted more for a file that is already finished
		auto &local = input.local_state->Cast<MultiFileLocalState>();
		if (local.job && local.job->reader) {
			local.job->reader->Cast<TableFunctionFileReader>().CollectMetrics(input.context, scan_state);
		}
	}
	lock_guard<mutex> metrics_guard(scan_state.metrics_lock);
	// the row groups scanned are handed over once - the profiler sums what every thread reports
	input.operator_metrics.row_groups_scanned += scan_state.collected_scanned;
	scan_state.collected_scanned = 0;
	input.operator_metrics.bytes_scanned += scan_state.collected_bytes_scanned;
	scan_state.collected_bytes_scanned = 0;
	// the size of the scan is reported as-is - it is not summed across the threads that report it
	input.operator_metrics.total_row_groups_to_scan = scan_state.collected_total;
}

vector<PartitionStatistics> TableFunctionMultiFileWrapper::GetPartitionStats(ClientContext &context,
                                                                             GetPartitionStatsInput &input) {
	vector<PartitionStatistics> result;
	auto &bind_data = input.bind_data->Cast<MultiFileBindData>();
	if (bind_data.file_list->GetExpandResult() != FileExpandResult::SINGLE_FILE || !bind_data.initial_reader) {
		return result;
	}
	auto &reader = bind_data.initial_reader->Cast<TableFunctionFileReader>();
	auto &function = reader.GetFunction();
	if (!function.get_partition_stats || !reader.bind_data) {
		return result;
	}
	BoundTableFunction bound_function(function);
	GetPartitionStatsInput file_input(bound_function, reader.bind_data.get());
	return function.get_partition_stats(context, file_input);
}

TableFunction TableFunctionMultiFileWrapper::CreateFunction(TableFunction single_file_function, Identifier name,
                                                            TableFunctionMultiFileSettings settings,
                                                            table_function_bind_t bind) {
	auto &wrapped_signature = single_file_function.GetSignature();
	if (wrapped_signature.GetPositionalParameterCount() != 1 ||
	    (wrapped_signature.GetParameter(0).GetType() != LogicalType::VARCHAR &&
	     wrapped_signature.GetParameter(0).GetType().id() != LogicalTypeId::ANY)) {
		throw InternalException("Only table functions taking a single file - an ANY file or a VARCHAR file path - can "
		                        "be wrapped in a multi file function, %s does not",
		                        single_file_function.name);
	}
	if (settings.reader_type.empty()) {
		settings.reader_type = name.GetIdentifierName();
	}
	TableFunctionMultiFileFunction result(std::move(name));
	result.bind = bind ? bind : TableFunctionMultiFileBind;
	// forward the options and the pushdown capabilities of the wrapped function
	auto &signature = result.GetSignature();
	auto wrapped_options = single_file_function.GetSignature().GetTypedKwargs();
	if (wrapped_options) {
		signature.ExtendTypedKwargs([&](TypedKwargs &options) { options = options.Merge(*wrapped_options); });
	}
	result.projection_pushdown = single_file_function.projection_pushdown;
	result.filter_pushdown = single_file_function.filter_pushdown;
	result.filter_prune = single_file_function.filter_prune;
	result.supports_pushdown_type = single_file_function.supports_pushdown_type;
	result.late_materialization = single_file_function.late_materialization;
	if (single_file_function.get_partition_stats) {
		// the row groups of the scan are those of its files
		result.get_partition_stats = GetPartitionStats;
	}
	if (single_file_function.get_metrics) {
		// the metrics of the scan include those the wrapped function keeps per file
		result.get_metrics = TableFunctionMultiFileGetMetrics;
	}
	if (single_file_function.statistics) {
		// the statistics of the scan are those of its files, combined
		result.statistics_extended = TableFunctionMultiFileFunction::MultiFileScanStatsExtended;
	}
	// NOTE: callbacks that take bind data are NOT forwarded - through the wrapper they are handed the bind data of
	// the multi-file scan, not the bind data the wrapped function produced for a file. A format that wants one sets
	// it on the function this returns
	if (!bind) {
		// the bind below reads the wrapped function from here - a function that brought its own bind does not need
		// this, and leaves the slot to whoever binds it
		result.function_info =
		    make_shared_ptr<TableFunctionMultiFileInfo>(std::move(single_file_function), std::move(settings));
	}
	return std::move(result);
}

TableFunctionSet TableFunctionMultiFileWrapper::CreateFunctionSet(TableFunction single_file_function, Identifier name,
                                                                  TableFunctionMultiFileSettings settings,
                                                                  table_function_bind_t bind) {
	return MultiFileReader::CreateFunctionSet(
	    CreateFunction(std::move(single_file_function), std::move(name), std::move(settings), bind));
}

} // namespace duckdb
