#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/common/mutex.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_schema_inference.hpp"

namespace duckdb {

//! Bind data of read_single_json_file - the regular JSON scan data plus the single file that is read
struct ReadSingleJSONFileData : public JSONScanData {
	OpenFileInfo file;

	//! Hand over the reader that detected the schema during the bind, if it has not been claimed yet
	shared_ptr<JSONReader> TakeBindReader() const {
		lock_guard<mutex> guard(bind_reader_lock);
		return std::move(bind_reader);
	}

	void SetBindReader(shared_ptr<JSONReader> reader) {
		lock_guard<mutex> guard(bind_reader_lock);
		bind_reader = std::move(reader);
	}

private:
	mutable mutex bind_reader_lock;
	//! The reader that read this file while detecting the schema - the scan continues with it, so that files that
	//! can only be read once (e.g. /dev/stdin) do not need to be opened again
	mutable shared_ptr<JSONReader> bind_reader;
};

struct ReadSingleJSONFileGlobalState : public GlobalTableFunctionState {
public:
	ReadSingleJSONFileGlobalState(ClientContext &context, const ReadSingleJSONFileData &json_data)
	    : state(context, json_data, 1), reader(json_data.TakeBindReader()) {
		if (reader) {
			// continue with the reader that detected the schema - it has already read (part of) the file
			reader->Reset();
		} else {
			reader = make_shared_ptr<JSONReader>(context, json_data.options, json_data.file);
		}
		reader->scanning = true;
	}

public:
	idx_t MaxThreads() const override {
		if (!state.enable_parallel_scans) {
			return 1;
		}
		return state.json_data.max_threads.IsValid() ? state.json_data.max_threads.GetIndex() : idx_t(MAX_THREADS);
	}

public:
	JSONScanGlobalState state;
	shared_ptr<JSONReader> reader;
	//! Assigning the next part of the file to a thread is done single-threadedly
	mutex lock;
};

struct ReadSingleJSONFileLocalState : public LocalTableFunctionState {
public:
	ReadSingleJSONFileLocalState(ClientContext &context, JSONScanGlobalState &gstate) : state(context, gstate) {
		// the transform options are thread-local
		state.transform_options = gstate.transform_options;
	}

public:
	JSONScanLocalState state;
	//! Whether we have a part of the file assigned to us that we still need to read
	bool scan_initialized = false;
	//! Whether our caller claims the batches we read - see table_function_claim_batch_t
	bool claimed_externally = false;
};

static unique_ptr<FunctionData> ReadSingleJSONFileBind(ClientContext &context, TableFunctionBindInput &input,
                                                       vector<LogicalType> &return_types, vector<Identifier> &names) {
	auto &file_input = TableFunctionFileBindInput::Get(input);
	auto result = make_uniq<ReadSingleJSONFileData>();
	auto &options = result->options;
	if (input.info) {
		auto &scan_info = input.info->Cast<JSONScanInfo>();
		options.type = scan_info.type;
		options.format = scan_info.format;
		options.record_type = scan_info.record_type;
		options.auto_detect = scan_info.auto_detect;
		if (scan_info.type == JSONScanType::READ_JSON_OBJECTS) {
			// read_json_objects always emits a single JSON column called "json"
			options.sql_type_list.push_back(LogicalType::JSON());
			options.name_list.emplace_back("json");
		}
	}
	for (auto &kv : input.named_parameters) {
		if (JSONScan::ParseOption(context, kv.first, kv.second, options)) {
			continue;
		}
		throw NotImplementedException("Unimplemented option %s", kv.first);
	}
	if (options.auto_detect_specified && !options.auto_detect && !options.format_specified) {
		// auto-detection was explicitly turned off - we do not detect the format either
		options.format = JSONFormat::NEWLINE_DELIMITED;
	}
	if (input.inputs[0].IsNull()) {
		throw BinderException("read_single_json_file requires a non-NULL file name");
	}
	if (file_input.expected_bind_data && file_input.HasExpectedSchema()) {
		// the schema of the scan was already determined - read this file exactly the way it was determined, so that
		// every file of the scan produces the same columns from the same JSON keys
		auto &source = file_input.expected_bind_data->Cast<ReadSingleJSONFileData>();
		result->file = TableFunctionFileBindInput::GetFile(input);
		result->options.record_type = source.options.record_type;
		result->options.geojson = source.options.geojson;
		result->options.auto_detect = false;
		result->options.name_list = *file_input.expected_names;
		result->options.sql_type_list = *file_input.expected_types;
		result->key_names = source.key_names;
		result->feature_columns = source.feature_columns;
		// the date/timestamp formats that auto-detection settled on are part of how the scan is read
		result->date_format_map = make_uniq<DateFormatMap>(*source.date_format_map);
		JSONScan::FinalizeBind(*result, result->options.name_list);
		names = result->options.name_list;
		return_types = result->options.sql_type_list;
		return std::move(result);
	}
	if (file_input.HasExpectedSchema()) {
		// the schema is known but not how it was determined (COPY takes its columns from the target table) - read
		// this file using those columns
		options.name_list = *file_input.expected_names;
		options.sql_type_list = *file_input.expected_types;
	}
	result->file = TableFunctionFileBindInput::GetFile(input);

	// keep the detected structure around - it is used to combine the schema of this file with that of other files
	// when this function is wrapped into a multi-file function. This is only needed when the columns are not known
	result->keep_structure = options.name_list.empty();

	SimpleMultiFileList file_list(vector<OpenFileInfo> {result->file});
	vector<shared_ptr<JSONReader>> sampled_readers;
	JSONScan::BindSchema(context, *result, file_list, sampled_readers, return_types, names);
	JSONScan::FinalizeBind(*result, names);
	if (!sampled_readers.empty() && sampled_readers[0]) {
		// keep the reader that detected the schema around - the scan continues with it
		result->SetBindReader(std::move(sampled_readers[0]));
	}
	return std::move(result);
}

static unique_ptr<GlobalTableFunctionState> ReadSingleJSONFileInitGlobal(ClientContext &context,
                                                                         TableFunctionInitInput &input) {
	auto &json_data = input.bind_data->Cast<ReadSingleJSONFileData>();
	auto result = make_uniq<ReadSingleJSONFileGlobalState>(context, json_data);
	auto &gstate = result->state;

	// perform projection pushdown - the JSON reader extracts columns by name, so we only need the projected names
	for (idx_t col_idx = 0; col_idx < input.column_indexes.size(); col_idx++) {
		auto &column_index = input.column_indexes[col_idx];
		const auto col_id = column_index.GetPrimaryIndex();
		if (IsVirtualColumn(col_id)) {
			continue;
		}
		gstate.names.push_back(json_data.key_names[col_id]);
		gstate.column_ids.push_back(col_idx);
		gstate.column_indices.push_back(column_index);
		if (!json_data.feature_columns.empty()) {
			gstate.feature_columns.push_back(json_data.feature_columns[col_id]);
		}
	}
	if (gstate.names.size() < json_data.key_names.size()) {
		// if we don't need all columns present in the file we don't error on unseen columns
		gstate.transform_options.error_unknown_key = false;
	}
	if (gstate.enable_parallel_scans) {
		// if we are doing parallel scans we need to open the file here
		result->reader->Initialize(gstate.allocator, gstate.buffer_capacity);
	}
	return std::move(result);
}

static unique_ptr<LocalTableFunctionState> ReadSingleJSONFileInitLocal(ExecutionContext &context,
                                                                       TableFunctionInitInput &input,
                                                                       GlobalTableFunctionState *global_state) {
	auto &gstate = global_state->Cast<ReadSingleJSONFileGlobalState>();
	return make_uniq<ReadSingleJSONFileLocalState>(context.client, gstate.state);
}

//! Assign the next batch to this thread - the JSON reader hands out one buffer at a time
static bool ReadSingleJSONFileClaimBatch(ClientContext &context, TableFunctionInput &input) {
	auto &gstate = input.global_state->Cast<ReadSingleJSONFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	// our caller hands out the parts of the file, so we must not claim the next one ourselves
	lstate.claimed_externally = true;

	lock_guard<mutex> guard(gstate.lock);
	lstate.state.GetScanState().ResetForNextBuffer();
	if (!lstate.state.TryInitializeScan(gstate.state, *gstate.reader)) {
		return false;
	}
	lstate.scan_initialized = true;
	return true;
}

//! Release the batch this thread was reading - this also reports any errors that were found in it
static void ReadSingleJSONFileFinishBatch(ClientContext &context, TableFunctionInput &input) {
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	lstate.state.GetScanState().ResetForNextBuffer();
	lstate.scan_initialized = false;
}

static void ReadSingleJSONFileFunction(ClientContext &context, TableFunctionInput &input, DataChunk &output) {
	auto &gstate = input.global_state->Cast<ReadSingleJSONFileGlobalState>();
	auto &lstate = input.local_state->Cast<ReadSingleJSONFileLocalState>();
	auto &reader = *gstate.reader;

	while (true) {
		if (!lstate.scan_initialized) {
			if (lstate.claimed_externally) {
				// the next part of the file is claimed by our caller
				return;
			}
			lock_guard<mutex> guard(gstate.lock);
			lstate.state.GetScanState().ResetForNextBuffer();
			if (!lstate.state.TryInitializeScan(gstate.state, reader)) {
				// there is nothing left for us to read in this file
				return;
			}
			lstate.scan_initialized = true;
		}
		switch (gstate.state.json_data.options.type) {
		case JSONScanType::READ_JSON:
			ReadJSONFunction(context, reader, gstate.state, lstate.state, output);
			break;
		case JSONScanType::READ_JSON_OBJECTS:
			ReadJSONObjectsFunction(context, reader, gstate.state, lstate.state, output);
			break;
		default:
			throw InternalException("Unsupported scan type for read_single_json_file");
		}
		reader.CountBytesScanned();
		if (output.size() != 0) {
			return;
		}
		// we have exhausted the part of the file that was assigned to us - try to grab the next part
		lstate.scan_initialized = false;
	}
}

//! Combine the schemas of several JSON files by merging the structures that were detected for them - this gives the
//! same schema as running the auto-detection over all of the files at once
static unique_ptr<FunctionData> ReadSingleJSONFileCombineSchema(ClientContext &context,
                                                                TableFunctionCombineSchemaInput &input,
                                                                vector<LogicalType> &return_types,
                                                                vector<Identifier> &names) {
	if (input.bind_data.empty()) {
		return nullptr;
	}
	auto &first_file = input.bind_data[0].get().Cast<ReadSingleJSONFileData>();
	JSONStructureNode merged(first_file.options.case_insensitive_keys);
	for (auto &bind_data : input.bind_data) {
		auto &json_data = bind_data.get().Cast<ReadSingleJSONFileData>();
		if (!json_data.structure) {
			// the schema of this file was not auto-detected - fall back to combining the types
			return nullptr;
		}
		JSONStructure::MergeNodes(merged, *json_data.structure);
	}
	// the result describes how every file of the scan is read - it is the bind the auto-detection would have
	// produced if it had run over all of the sampled files at once
	auto result = make_uniq<ReadSingleJSONFileData>();
	result->options = first_file.options;
	if (first_file.record_type_auto_detected) {
		// the record type is re-detected on the combined structure
		result->options.record_type = JSONRecordType::AUTO_DETECT;
	}
	// the date/timestamp formats that were settled on while detecting the structure are carried over
	result->date_format_map = make_uniq<DateFormatMap>(*first_file.date_format_map);
	// the merged structure describes the schema of the scan by itself
	return_types.clear();
	names.clear();
	JSONScan::StructureToColumns(context, result->options, merged, result->feature_columns, return_types, names);

	// the JSON reader looks columns up by their exact key, so the keys are kept before the column names that are
	// duplicates for us (e.g. "id" and "Id") are renamed
	result->key_names = IdentifiersToStrings(names);
	JSONScan::DeduplicateColumnNames(names);
	result->options.name_list = names;
	result->options.sql_type_list = return_types;
	result->options.auto_detect = false;
	JSONScan::FinalizeBind(*result, names);
	return std::move(result);
}

static double ReadSingleJSONFileProgress(ClientContext &context, const FunctionData *bind_data,
                                         const GlobalTableFunctionState *global_state) {
	if (!global_state) {
		return 0;
	}
	return global_state->Cast<ReadSingleJSONFileGlobalState>().reader->GetProgress();
}

static unique_ptr<NodeStatistics> ReadSingleJSONFileCardinality(ClientContext &context, const FunctionData *bind_data) {
	auto &json_data = bind_data->Cast<ReadSingleJSONFileData>();
	idx_t per_file_cardinality = 42;
	if (json_data.estimated_cardinality_per_file.IsValid()) {
		per_file_cardinality = json_data.estimated_cardinality_per_file.GetIndex();
	}
	return make_uniq<NodeStatistics>(per_file_cardinality);
}

//! The bytes scanned are drained, so each is reported once - the profiler sums what every thread reports
static void ReadSingleJSONFileGetMetrics(TableFunctionGetMetricsInput &input) {
	if (!input.global_state) {
		return;
	}
	auto &gstate = input.global_state->Cast<ReadSingleJSONFileGlobalState>();
	if (gstate.reader) {
		input.operator_metrics.bytes_scanned += gstate.reader->bytes_scanned_unreported.exchange(0);
	}
}

TableFunction JSONFunctions::GetReadSingleJSONFileTableFunction(shared_ptr<JSONScanInfo> function_info) {
	const auto scan_type = function_info->type;
	TableFunction table_function(
	    "read_single_json_file", FunctionSignature().AddPositionalOnly("file", LogicalType::ANY),
	    ReadSingleJSONFileFunction, ReadSingleJSONFileBind, ReadSingleJSONFileInitGlobal, ReadSingleJSONFileInitLocal);
	JSONScan::TableFunctionDefaults(table_function);
	if (scan_type != JSONScanType::READ_JSON_OBJECTS) {
		// read_json_objects always emits a single JSON column - it has no schema options
		JSONScan::AddReadJSONParameters(table_function);
		JSONScan::AddAutoDetectParameters(table_function);
	}
	table_function.table_scan_progress = ReadSingleJSONFileProgress;
	table_function.cardinality = ReadSingleJSONFileCardinality;
	table_function.get_metrics = ReadSingleJSONFileGetMetrics;
	table_function.function_info = std::move(function_info);
	return table_function;
}

TableFunction JSONFunctions::GetJSONTableFunction(Identifier name, shared_ptr<JSONScanInfo> function_info) {
	// every JSON read function is the single-file JSON reader wrapped into a multi-file function
	auto single_file_function = GetReadSingleJSONFileTableFunction(std::move(function_info));
	TableFunctionMultiFileSettings settings;
	settings.glob_input = FileGlobInput(FileGlobOptions::FALLBACK_GLOB, "json");
	settings.reader_type = "JSON";
	// the schema is determined by combining the schemas of up to 32 files - the keys of the files are unified, so a
	// file does not need to have every column of the combined schema
	settings.maximum_sample_files = 32;
	settings.combine_schema = ReadSingleJSONFileCombineSchema;
	settings.claim_batch = ReadSingleJSONFileClaimBatch;
	settings.finish_batch = ReadSingleJSONFileFinishBatch;
	settings.reports_bytes_scanned = true;
	return TableFunctionMultiFileWrapper::CreateFunction(std::move(single_file_function), std::move(name),
	                                                     std::move(settings));
}

static TableFunctionSet CreateJSONFunctionSet(Identifier name, shared_ptr<JSONScanInfo> function_info) {
	return MultiFileReader::CreateFunctionSet(
	    JSONFunctions::GetJSONTableFunction(std::move(name), std::move(function_info)));
}

static shared_ptr<JSONScanInfo> ReadJSONInfo(JSONFormat format) {
	return make_shared_ptr<JSONScanInfo>(JSONScanType::READ_JSON, format, JSONRecordType::AUTO_DETECT, true);
}

static shared_ptr<JSONScanInfo> ReadJSONObjectsInfo(JSONFormat format) {
	return make_shared_ptr<JSONScanInfo>(JSONScanType::READ_JSON_OBJECTS, format, JSONRecordType::RECORDS, false);
}

TableFunctionSet JSONFunctions::GetReadSingleJSONFileFunction() {
	TableFunctionSet function_set("read_single_json_file");
	function_set.AddFunction(GetReadSingleJSONFileTableFunction(ReadJSONInfo(JSONFormat::AUTO_DETECT)));
	return function_set;
}

TableFunctionSet JSONFunctions::GetReadJSONFunction() {
	return CreateJSONFunctionSet("read_json", ReadJSONInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONFunction() {
	return CreateJSONFunctionSet("read_ndjson", ReadJSONInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONAutoFunction() {
	return CreateJSONFunctionSet("read_json_auto", ReadJSONInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONAutoFunction() {
	return CreateJSONFunctionSet("read_ndjson_auto", ReadJSONInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONObjectsFunction() {
	return CreateJSONFunctionSet("read_json_objects", ReadJSONObjectsInfo(JSONFormat::AUTO_DETECT));
}

TableFunctionSet JSONFunctions::GetReadNDJSONObjectsFunction() {
	return CreateJSONFunctionSet("read_ndjson_objects", ReadJSONObjectsInfo(JSONFormat::NEWLINE_DELIMITED));
}

TableFunctionSet JSONFunctions::GetReadJSONObjectsAutoFunction() {
	return CreateJSONFunctionSet("read_json_objects_auto", ReadJSONObjectsInfo(JSONFormat::AUTO_DETECT));
}

} // namespace duckdb
