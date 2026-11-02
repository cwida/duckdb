#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/common/csv_writer.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/multi_file/multi_file_reader.hpp"
#include "duckdb/common/serializer/async_file_writer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/operator/csv_scanner/csv_schema_discovery.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/parser/parsed_data/copy_info.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
namespace duckdb {

void AreOptionsEqual(char str_1, char str_2, const string &name_str_1, const string &name_str_2) {
	if (str_1 == '\0' || str_2 == '\0') {
		return;
	}
	if (str_1 == str_2) {
		throw BinderException("%s must not appear in the %s specification and vice versa", name_str_1, name_str_2);
	}
}

void SubstringDetection(char str_1, string &str_2, const string &name_str_1, const string &name_str_2) {
	if (str_1 == '\0' || str_2.empty()) {
		return;
	}
	if (str_2.find(str_1) != string::npos) {
		throw BinderException("%s must not appear in the %s specification and vice versa", name_str_1, name_str_2);
	}
}

void StringDetection(const string &str_1, const string &str_2, const string &name_str_1, const string &name_str_2) {
	if (str_1.empty() || str_2.empty()) {
		return;
	}
	if (str_2.find(str_1) != string::npos) {
		throw BinderException("%s must not appear in the %s specification and vice versa", name_str_1, name_str_2);
	}
}

//===--------------------------------------------------------------------===//
// Bind
//===--------------------------------------------------------------------===//

void BaseCSVData::Finalize() {
	auto delimiter_string = options.dialect_options.state_machine_options.delimiter.GetValue();

	// quote and delimiter must not be substrings of each other
	SubstringDetection(options.dialect_options.state_machine_options.quote.GetValue(), delimiter_string, "QUOTE",
	                   "DELIMITER");

	// escape and delimiter must not be substrings of each other
	SubstringDetection(options.dialect_options.state_machine_options.escape.GetValue(), delimiter_string, "ESCAPE",
	                   "DELIMITER");

	// escape and quote must not be substrings of each other (but can be the same)
	if (options.dialect_options.state_machine_options.quote != options.dialect_options.state_machine_options.escape) {
		AreOptionsEqual(options.dialect_options.state_machine_options.quote.GetValue(),
		                options.dialect_options.state_machine_options.escape.GetValue(), "QUOTE", "ESCAPE");
	}

	// comment and quote must not be substrings of each other
	AreOptionsEqual(options.dialect_options.state_machine_options.comment.GetValue(),
	                options.dialect_options.state_machine_options.quote.GetValue(), "COMMENT", "QUOTE");

	// delimiter and comment must not be substrings of each other
	SubstringDetection(options.dialect_options.state_machine_options.comment.GetValue(), delimiter_string, "COMMENT",
	                   "DELIMITER");

	// quote and delimiter must not be substrings of each other
	SubstringDetection(options.thousands_separator, options.decimal_separator, "THOUSANDS", "DECIMAL_SEPARATOR");

	// null string and delimiter must not be substrings of each other
	for (auto &null_str : options.null_str) {
		if (!null_str.empty()) {
			StringDetection(options.dialect_options.state_machine_options.delimiter.GetValue(), null_str, "DELIMITER",
			                "NULL");

			// quote and nullstr must not be substrings of each other
			SubstringDetection(options.dialect_options.state_machine_options.quote.GetValue(), null_str, "QUOTE",
			                   "NULL");

			// Validate the nullstr against the escape character
			const char escape = options.dialect_options.state_machine_options.escape.GetValue();
			// Allow nullstr to be escape character + some non-special character, e.g., "\N" (MySQL default).
			// In this case, only unquoted occurrences of the nullstr will be recognized as null values.
			if (options.dialect_options.state_machine_options.strict_mode == false && null_str.size() == 2 &&
			    null_str[0] == escape && null_str[1] != '\0') {
				continue;
			}
			SubstringDetection(escape, null_str, "ESCAPE", "NULL");
		}
	}

	if (!options.prefix.empty() || !options.suffix.empty()) {
		if (options.prefix.empty() || options.suffix.empty()) {
			throw BinderException("COPY ... (FORMAT CSV) must have both PREFIX and SUFFIX, or none at all");
		}
		if (options.dialect_options.header.GetValue()) {
			throw BinderException("COPY ... (FORMAT CSV)'s HEADER cannot be combined with PREFIX/SUFFIX");
		}
	}
}

static vector<unique_ptr<Expression>> CreateCastExpressions(WriteCSVData &bind_data, ClientContext &context,
                                                            const vector<Identifier> &names,
                                                            const vector<LogicalType> &sql_types) {
	auto &options = bind_data.options;
	auto &formats = options.write_date_format;

	bool has_dateformat = !formats[LogicalTypeId::DATE].IsNull();
	bool has_timestampformat = !formats[LogicalTypeId::TIMESTAMP].IsNull();

	// Create the bound expressions (cast, strftime, etc..)
	vector<unique_ptr<Expression>> expressions;
	for (idx_t i = 0; i < sql_types.size(); i++) {
		auto &type = sql_types[i];
		auto &name = names[i];
		auto column = make_uniq_base<Expression, BoundReferenceExpression>(name, type, i);

		bool is_timestamp = type.id() == LogicalTypeId::TIMESTAMP || type.id() == LogicalTypeId::TIMESTAMP_TZ;
		unique_ptr<Expression> expr;
		if ((has_dateformat && type.id() == LogicalTypeId::DATE) || (has_timestampformat && is_timestamp)) {
			// strftime(<name>, 'format')
			auto &format =
			    type.id() == LogicalTypeId::DATE ? formats[LogicalTypeId::DATE] : formats[LogicalTypeId::TIMESTAMP];
			vector<unique_ptr<Expression>> children;
			children.push_back(std::move(column));
			children.push_back(make_uniq<BoundConstantExpression>(format));
			FunctionBinder function_binder(context);
			expr = function_binder.BindScalarFunction(Identifier::DefaultSchema(), "strftime", std::move(children));
		} else {
			// CAST <name> AS VARCHAR
			expr = std::move(column);
		}
		expressions.push_back(BoundCastExpression::AddCastToType(context, std::move(expr), LogicalType::VARCHAR));
	}

	return expressions;
}

static unique_ptr<FunctionData> WriteCSVBind(ClientContext &context, CopyFunctionBindInput &input,
                                             const vector<Identifier> &names, const vector<LogicalType> &sql_types) {
	auto bind_data = make_uniq<WriteCSVData>(names);

	// check all the options in the copy info
	for (auto &[option_name, option_values] : input.info.options) {
		bind_data->options.SetWriteOption(option_name, ConvertVectorToValue(option_values));
	}
	// verify the parsed options
	if (bind_data->options.force_quote.empty()) {
		// no FORCE_QUOTE specified: initialize to false
		bind_data->options.force_quote.resize(names.size(), false);
	}
	bind_data->Finalize();

	auto &compression = bind_data->options.compression;
	if (compression == FileCompressionType::GZIP || compression == FileCompressionType::ZSTD) {
		if (!IsFileCompressed(input.file_extension, compression)) {
			input.file_extension += CompressionExtensionFromType(compression);
		}
	}

	auto expressions = CreateCastExpressions(*bind_data, context, names, sql_types);
	bind_data->cast_expressions = std::move(expressions);

	return std::move(bind_data);
}

static void CSVListCopyOptions(ClientContext &context, CopyOptionsInput &input) {
	auto &copy_options = input.options;
	copy_options["auto_detect"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["sample_size"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["skip"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["max_line_size"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["maximum_line_size"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["ignore_errors"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["buffer_size"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["decimal_separator"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);
	copy_options["null_padding"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["parallel"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["allow_quoted_nulls"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["store_rejects"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_ONLY);
	copy_options["force_not_null"] = CopyOption(LogicalType::LIST(LogicalType::VARCHAR), CopyOptionMode::READ_ONLY);
	copy_options["rejects_table"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);
	copy_options["rejects_scan"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);
	copy_options["rejects_limit"] = CopyOption(LogicalType::BIGINT, CopyOptionMode::READ_ONLY);
	copy_options["encoding"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);
	copy_options["thousands"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_ONLY);

	copy_options["force_quote"] = CopyOption(LogicalType::LIST(LogicalType::VARCHAR), CopyOptionMode::WRITE_ONLY);
	copy_options["prefix"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);
	copy_options["suffix"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::WRITE_ONLY);

	copy_options["new_line"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["date_format"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["dateformat"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["timestamp_format"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["timestampformat"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["quote"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["comment"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["delim"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["delimiter"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["sep"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["separator"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["escape"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["header"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_WRITE);
	copy_options["nullstr"] = CopyOption(LogicalType::ANY, CopyOptionMode::READ_WRITE);
	copy_options["null"] = CopyOption(LogicalType::ANY, CopyOptionMode::READ_WRITE);
	copy_options["compression"] = CopyOption(LogicalType::VARCHAR, CopyOptionMode::READ_WRITE);
	copy_options["strict_mode"] = CopyOption(LogicalType::BOOLEAN, CopyOptionMode::READ_WRITE);
}

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
struct LocalWriteCSVData : public LocalFunctionData {
public:
	LocalWriteCSVData(ClientContext &context, vector<unique_ptr<Expression>> &expressions, const idx_t &flush_size)
	    : executor(context, expressions), writer_local_state(context, flush_size) {
	}

public:
	//! Used to execute the expressions that transform input -> string
	ExpressionExecutor executor;
	//! A chunk with VARCHAR columns to cast intermediates into
	DataChunk cast_chunk;
	//! Local state for the CSV writer
	CSVWriterState writer_local_state;
};

static unique_ptr<AsyncFileWriter> OpenCSVFileWriter(ClientContext &context, const string &file_path,
                                                     FileCompressionType compression) {
	auto &fs = FileSystem::GetFileSystem(context);
	auto flags = FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_FILE_CREATE_NEW | std::move(compression);
	// checking a remote path costs a request, so a remote output is opened like one that may exist
	if (!fs.IsRemoteFile(file_path) && !fs.FileExists(file_path) && !fs.IsPipe(file_path)) {
		flags |= FileFlags::FILE_FLAGS_EXCLUSIVE_CREATE;
	}
	return make_uniq<AsyncFileWriter>(QueryContext(context), fs, file_path, flags);
}

struct GlobalWriteCSVData : public GlobalFunctionData {
	GlobalWriteCSVData(CSVReaderOptions &options, ClientContext &context, const string &file_path,
	                   FileCompressionType compression_p)
	    : file_writer(OpenCSVFileWriter(context, file_path, std::move(compression_p))), writer(options, *file_writer),
	      compression(file_writer->GetFileCompressionType()) {
	}

	CSVWriter &GetWriter() {
		return writer;
	}

	idx_t FileSize() {
		if (compression == FileCompressionType::UNCOMPRESSED) {
			return writer.BytesWritten();
		}
		return writer.FileSize();
	}

	unique_ptr<CSVWriterState> GetLocalState(ClientContext &context, const idx_t flush_size) {
		{
			lock_guard<mutex> guard(local_state_lock);
			if (!local_states.empty()) {
				auto result = std::move(local_states.back());
				local_states.pop_back();
				return result;
			}
		}
		auto result = make_uniq<CSVWriterState>(context, flush_size);
		result->require_manual_flush = true;
		return result;
	}

	void StoreLocalState(unique_ptr<CSVWriterState> lstate) {
		lock_guard<mutex> guard(local_state_lock);
		lstate->Reset();
		local_states.push_back(std::move(lstate));
	}

	void Flush(CSVWriterState &local_state) {
		writer.Flush(local_state);
	}

	void Close() {
		file_writer->Close();
	}

private:
	unique_ptr<AsyncFileWriter> file_writer;
	CSVWriter writer;
	FileCompressionType compression;
	mutex local_state_lock;
	vector<unique_ptr<CSVWriterState>> local_states;
};

static unique_ptr<LocalFunctionData> WriteCSVInitializeLocal(ExecutionContext &context, FunctionData &bind_data) {
	auto &csv_data = bind_data.Cast<WriteCSVData>();
	auto local_data = make_uniq<LocalWriteCSVData>(context.client, csv_data.cast_expressions, csv_data.flush_size);

	// create the chunk with VARCHAR types
	vector<LogicalType> types;
	types.resize(csv_data.options.name_list.size(), LogicalType::VARCHAR);

	local_data->cast_chunk.Initialize(Allocator::Get(context.client), types);
	return std::move(local_data);
}

static unique_ptr<GlobalFunctionData> WriteCSVInitializeGlobal(ClientContext &context, FunctionData &bind_data,
                                                               const string &file_path) {
	auto &csv_data = bind_data.Cast<WriteCSVData>();
	auto &options = csv_data.options;
	auto global_data = make_uniq<GlobalWriteCSVData>(options, context, file_path, options.compression);

	global_data->GetWriter().Initialize();

	return std::move(global_data);
}

static void WriteCSVChunkInternal(CSVWriter &writer, CSVWriterState &writer_local_state, DataChunk &cast_chunk,
                                  DataChunk &input, ExpressionExecutor &executor) {
	// first cast the columns of the chunk to varchar
	cast_chunk.Reset();

	executor.Execute(input, cast_chunk);

	writer.WriteChunk(cast_chunk, writer_local_state);
}

static void WriteCSVSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                         LocalFunctionData &lstate, DataChunk &input) {
	auto &local_data = lstate.Cast<LocalWriteCSVData>();
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();

	WriteCSVChunkInternal(global_state.GetWriter(), local_data.writer_local_state, local_data.cast_chunk, input,
	                      local_data.executor);
}

//===--------------------------------------------------------------------===//
// Combine
//===--------------------------------------------------------------------===//
static void WriteCSVCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                            LocalFunctionData &lstate) {
	auto &local_data = lstate.Cast<LocalWriteCSVData>();
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();
	global_state.Flush(local_data.writer_local_state);
}

//===--------------------------------------------------------------------===//
// Finalize
//===--------------------------------------------------------------------===//
void WriteCSVFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate) {
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();
	auto &writer = global_state.GetWriter();
	auto &csv_data = bind_data.Cast<WriteCSVData>();
	auto &options = csv_data.options;

	if (!options.suffix.empty()) {
		writer.WriteRawString(options.suffix);
	} else if (writer.WrittenAnything()) {
		writer.WriteRawString(writer.writer_options.newline);
	}
	global_state.Close();
}

//===--------------------------------------------------------------------===//
// Execution Mode
//===--------------------------------------------------------------------===//
CopyFunctionExecutionMode WriteCSVExecutionMode(bool preserve_insertion_order, bool supports_batch_index) {
	if (!preserve_insertion_order) {
		return CopyFunctionExecutionMode::PARALLEL_COPY_TO_FILE;
	}
	if (supports_batch_index) {
		return CopyFunctionExecutionMode::BATCH_COPY_TO_FILE;
	}
	return CopyFunctionExecutionMode::REGULAR_COPY_TO_FILE;
}
//===--------------------------------------------------------------------===//
// Prepare Batch
//===--------------------------------------------------------------------===//
struct WriteCSVBatchData : public PreparedBatchData {
	explicit WriteCSVBatchData(unique_ptr<CSVWriterState> writer_state) : writer_local_state(std::move(writer_state)) {
	}

	//! The thread-local buffer to write data into
	unique_ptr<CSVWriterState> writer_local_state;
};

unique_ptr<PreparedBatchData> WriteCSVPrepareBatch(ClientContext &context, FunctionData &bind_data,
                                                   GlobalFunctionData &gstate,
                                                   unique_ptr<ColumnDataCollection> collection) {
	auto &csv_data = bind_data.Cast<WriteCSVData>();

	// create the cast chunk with VARCHAR types
	vector<LogicalType> types;
	types.resize(csv_data.options.name_list.size(), LogicalType::VARCHAR);
	DataChunk cast_chunk;
	cast_chunk.Initialize(Allocator::Get(context), types);

	auto &original_types = collection->Types();
	auto expressions = CreateCastExpressions(csv_data, context, csv_data.options.name_list, original_types);
	ExpressionExecutor executor(context, expressions);
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();

	// write CSV chunks to the batch data
	auto local_writer_state = global_state.GetLocalState(context, NextPowerOfTwo(collection->SizeInBytes()));
	auto batch = make_uniq<WriteCSVBatchData>(std::move(local_writer_state));
	for (auto &chunk : collection->Chunks()) {
		WriteCSVChunkInternal(global_state.GetWriter(), *batch->writer_local_state, cast_chunk, chunk, executor);
	}
	return std::move(batch);
}

//===--------------------------------------------------------------------===//
// Flush Batch
//===--------------------------------------------------------------------===//
void WriteCSVFlushBatch(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                        PreparedBatchData &batch) {
	auto &csv_batch = batch.Cast<WriteCSVBatchData>();
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();
	global_state.Flush(*csv_batch.writer_local_state);
	global_state.StoreLocalState(std::move(csv_batch.writer_local_state));
}

//===--------------------------------------------------------------------===//
// File Size Bytes
//===--------------------------------------------------------------------===//
idx_t WriteCSVFileSizeBytes(GlobalFunctionData &gstate) {
	auto &global_state = gstate.Cast<GlobalWriteCSVData>();
	return global_state.FileSize();
}

void CSVCopyFunction::RegisterFunction(BuiltinFunctions &set) {
	CopyFunction info("csv");
	info.copy_to_bind = WriteCSVBind;
	info.copy_options = CSVListCopyOptions;
	info.copy_to_initialize_local = WriteCSVInitializeLocal;
	info.copy_to_initialize_global = WriteCSVInitializeGlobal;
	info.copy_to_sink = WriteCSVSink;
	info.copy_to_combine = WriteCSVCombine;
	info.copy_to_finalize = WriteCSVFinalize;
	info.execution_mode = WriteCSVExecutionMode;

	info.prepare_batch = WriteCSVPrepareBatch;
	info.flush_batch = WriteCSVFlushBatch;
	info.file_size_bytes = WriteCSVFileSizeBytes;

	info.copy_from_bind = TableFunctionMultiFileWrapper::MultiFileBindCopy;
	info.copy_from_function = ReadCSVTableFunction::GetFunction();

	info.extension = "csv";

	set.AddFunction(info);
}

} // namespace duckdb
