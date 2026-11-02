#include "duckdb/logging/log_storage.hpp"
#include "duckdb/function/table_function.hpp"

#include "duckdb/common/csv_writer.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/function/table/read_csv.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/logging/logging.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/operator/string_cast.hpp"
#include "duckdb/execution/operator/csv_scanner/sniffer/csv_sniffer.hpp"
#include "duckdb/common/printer.hpp"

#include <complex>

namespace duckdb {

vector<LogicalType> LogStorage::GetSchema(LoggingTargetTable table) {
	switch (table) {
	case LoggingTargetTable::ALL_LOGS: {
		auto all_logs = GetSchema(LoggingTargetTable::LOG_CONTEXTS);
		auto log_entries = GetSchema(LoggingTargetTable::LOG_ENTRIES);
		all_logs.insert(all_logs.end(), log_entries.begin() + 1, log_entries.end());
		return all_logs;
	}
	case LoggingTargetTable::LOG_ENTRIES:
		return {
		    LogicalType::UBIGINT,      // context_id
		    LogicalType::TIMESTAMP_TZ, // timestamp
		    LogicalType::VARCHAR,      // log_type
		    LogicalType::VARCHAR,      // level
		    LogicalType::VARCHAR,      // message
		};
	case LoggingTargetTable::LOG_CONTEXTS:
		return {
		    LogicalType::UBIGINT, // context_id
		    LogicalType::VARCHAR, // scope
		    LogicalType::UBIGINT, // connection_id
		    LogicalType::UBIGINT, // transaction_id
		    LogicalType::UBIGINT, // query_id
		    LogicalType::UBIGINT, // thread
		};
	default:
		throw NotImplementedException("Unknown logging target table");
	}
}

vector<Identifier> LogStorage::GetColumnNames(LoggingTargetTable table) {
	switch (table) {
	case LoggingTargetTable::ALL_LOGS: {
		auto all_logs = GetColumnNames(LoggingTargetTable::LOG_CONTEXTS);
		auto log_entries = GetColumnNames(LoggingTargetTable::LOG_ENTRIES);
		all_logs.insert(all_logs.end(), log_entries.begin() + 1, log_entries.end());
		return all_logs;
	}
	case LoggingTargetTable::LOG_ENTRIES:
		return {"context_id", "timestamp", "type", "log_level", "message"};
	case LoggingTargetTable::LOG_CONTEXTS:
		return {
		    "context_id", "scope", "connection_id", "transaction_id", "query_id", "thread_id",
		};
	default:
		throw NotImplementedException("Unknown logging target table");
	}
}

unique_ptr<LogStorageScanState> LogStorage::CreateScanState(LoggingTargetTable table) const {
	throw NotImplementedException("Not implemented for this LogStorage: CreateScanEntriesState");
}
bool LogStorage::Scan(LogStorageScanState &state, DataChunk &result) const {
	throw NotImplementedException("Not implemented for this LogStorage: ScanEntries");
}
void LogStorage::InitializeScan(LogStorageScanState &state) const {
	throw NotImplementedException("Not implemented for this LogStorage: InitializeScanEntries");
}
optional_idx LogStorage::GetScanRowCount(LoggingTargetTable table) const {
	return optional_idx();
}
void LogStorage::Truncate() {
	throw NotImplementedException("Not implemented for this LogStorage: TruncateLogStorage");
}

void LogStorage::UpdateConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config) {
	if (!config.empty()) {
		throw InvalidInputException("Log storage '%s' does not support passing configuration", GetStorageName());
	}
}

unique_ptr<TableRef> LogStorage::BindReplace(ClientContext &context, TableFunctionBindInput &input,
                                             LoggingTargetTable table) {
	return nullptr;
}

CSVLogStorage::~CSVLogStorage() {
}

CSVLogStorage::CSVLogStorage(DatabaseInstance &db, bool normalize, idx_t buffer_size)
    : BufferingLogStorage(db, buffer_size, normalize) {
	reader_options = make_uniq<CSVReaderOptions>();
	writer_options = make_uniq<CSVWriterOptions>(*reader_options);

	reader_options->dialect_options.state_machine_options.escape = '\"';
	reader_options->dialect_options.state_machine_options.quote = '\"';
	reader_options->dialect_options.state_machine_options.delimiter = CSVOption<string>("\t");

	ResetCastChunk();
}

void BufferingLogStorage::UpdateConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config) {
	lock_guard<mutex> lck(lock);
	return UpdateConfigInternal(db, config);
}

bool BufferingLogStorage::IsEnabled(LoggingTargetTable table) {
	lock_guard<mutex> lck(lock);
	return IsEnabledInternal(table);
}

bool BufferingLogStorage::IsEnabledInternal(LoggingTargetTable table) {
	if (normalize_contexts) {
		return table == LoggingTargetTable::LOG_CONTEXTS || table == LoggingTargetTable::LOG_ENTRIES;
	}

	return table == LoggingTargetTable::ALL_LOGS;
}

idx_t BufferingLogStorage::GetBufferLimit() const {
	return buffer_limit;
}

void CSVLogStorage::ExecuteCast(LoggingTargetTable table, DataChunk &chunk) {
	// Reset the cast buffer before use
	cast_buffers[table]->Reset();

	auto &cast_buffer = *cast_buffers[table];
	idx_t count = chunk.size();

	// Do default casts
	for (idx_t i = 0; i < chunk.data.size(); i++) {
		VectorOperations::DefaultCast(chunk.data[i], cast_buffer.data[i], count, false);
	}
}

void CSVLogStorage::ResetAllBuffers() {
	BufferingLogStorage::ResetAllBuffers();
	ResetCastChunk();
}

void CSVLogStorage::UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config) {
	auto config_copy = config;

	bool changed_writer_settings = false;

	vector<string> to_remove;
	for (const auto &it : config_copy) {
		auto key = StringUtil::Lower(it.first);
		if (key == "delim") {
			changed_writer_settings = true;
			reader_options->dialect_options.state_machine_options.delimiter = CSVOption<string>(it.second.ToString());
			to_remove.push_back(it.first);
		}
	}

	if (changed_writer_settings) {
		for (auto &writer : writers) {
			SetWriterConfigs(*writer.second, GetColumnNames(writer.first));
		}
	}

	for (const auto &it : to_remove) {
		config_copy.erase(it);
	}

	return BufferingLogStorage::UpdateConfigInternal(db, config_copy);
}

void CSVLogStorage::RegisterWriter(LoggingTargetTable table, unique_ptr<CSVWriter> writer) {
	writers[table] = std::move(writer);
}

CSVWriter &CSVLogStorage::GetWriter(LoggingTargetTable table) {
	return *writers[table];
}

void CSVLogStorage::InitializeCastChunk(LoggingTargetTable table) {
	cast_buffers[table] = make_uniq<DataChunk>();

	vector<LogicalType> types;
	types.resize(GetSchema(table).size(), LogicalType::VARCHAR);
	idx_t buffer_size = MaxValue<idx_t>(GetBufferLimit(), 1);
	cast_buffers[table]->Initialize(Allocator::DefaultAllocator(), types, buffer_size);
}

void CSVLogStorage::ResetCastChunk() {
	InitializeCastChunk(LoggingTargetTable::LOG_ENTRIES);
	InitializeCastChunk(LoggingTargetTable::LOG_CONTEXTS);
	InitializeCastChunk(LoggingTargetTable::ALL_LOGS);
}

void CSVLogStorage::SetWriterConfigs(CSVWriter &writer, vector<Identifier> column_names) {
	writer.options = *reader_options;
	writer.writer_options = *writer_options;

	// Update the config with the column names since that is different per schema
	writer.options.name_list = std::move(column_names);
	writer.options.force_quote = vector<bool>(writer.options.name_list.size(), false);
}

CSVReaderOptions &CSVLogStorage::GetCSVReaderOptions() {
	return *reader_options;
}

CSVWriterOptions &CSVLogStorage::GetCSVWriterOptions() {
	return *writer_options;
}

void CSVLogStorage::FlushChunk(LoggingTargetTable table, DataChunk &chunk) {
	BeforeFlush(table, chunk);

	// Execute the cast
	ExecuteCast(table, chunk);

	// Write the chunk to the CSVWriter
	writers[table]->WriteChunk(*cast_buffers[table]);
	writers[table]->Flush();

	// Call child class to implement any post flushing behaviour (e.g. calling sync)
	AfterFlush(table, chunk);

	// Reset the cast buffer
	cast_buffers[table]->Reset();
}

void BufferingLogStorage::UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config) {
	for (const auto &it : config) {
		if (StringUtil::CIEquals(it.first, "buffer_size")) {
			buffer_limit = it.second.GetValue<uint64_t>();
			ResetAllBuffers();
		} else if (StringUtil::CIEquals(it.first, "only_flush_on_full_buffer")) {
			// This is a debug option used during testing. It disables the manual
			only_flush_on_full_buffer = it.second.GetValue<bool>();
		} else if (StringUtil::CIEquals(it.first, "normalize")) {
			throw InternalException("'normalize' setting should be handled in child class");
		} else {
			throw InvalidInputException("Unrecognized log storage config option for storage: '%s': '%s'",
			                            GetStorageName(), it.first);
		}
	}
}

void StdOutLogStorage::StdOutWriteStream::WriteData(const_data_ptr_t buffer, idx_t write_size) {
	string data(const_char_ptr_cast(buffer), NumericCast<size_t>(write_size));
	Printer::RawPrint(OutputStream::STREAM_STDOUT, data);
	Printer::Flush(OutputStream::STREAM_STDOUT);
}

StdOutLogStorage::StdOutLogStorage(DatabaseInstance &db) : CSVLogStorage(db, false, 1) {
	// StdOutLogStorage is denormalized only
	auto target_table = LoggingTargetTable::ALL_LOGS;

	// Set storage specific defaults
	GetCSVWriterOptions().newline_writing_mode = CSVNewLineMode::WRITE_AFTER;
	GetCSVReaderOptions().dialect_options.state_machine_options.delimiter = CSVOption<string>("\t");

	// Create and configure writer
	auto writer = make_uniq<CSVWriter>(stdout_stream, GetColumnNames(target_table), false);
	SetWriterConfigs(*writer, GetColumnNames(target_table));

	RegisterWriter(target_table, std::move(writer));
}

StdOutLogStorage::~StdOutLogStorage() {
}

FileLogStorage::FileLogStorage(DatabaseInstance &db_p) : CSVLogStorage(db_p, true, STANDARD_VECTOR_SIZE), db(db_p) {
	tables[LoggingTargetTable::ALL_LOGS] = TableWriter();
	tables[LoggingTargetTable::LOG_CONTEXTS] = TableWriter();
	tables[LoggingTargetTable::LOG_ENTRIES] = TableWriter();

	// Set storage specific defaults
	GetCSVWriterOptions().newline_writing_mode = CSVNewLineMode::WRITE_BEFORE;
	GetCSVReaderOptions().dialect_options.state_machine_options.delimiter = CSVOption<string>(",");
}

FileLogStorage::~FileLogStorage() {
}

void FileLogStorage::InitializeFile(DatabaseInstance &db, LoggingTargetTable table) {
	auto &table_writer = tables[table];

	// reset the files writer, we may be re-initializing it here and otherwise we hold 2 handles to the same file
	table_writer.file_writer.reset();

	// (re)initialize the file writer
	table_writer.file_writer = InitializeFileWriter(db, table_writer.path);
	auto file_writer = table_writer.file_writer.get();

	// Create CSV writer that writes to file
	auto column_names = GetColumnNames(table);

	// Configure writer
	auto csv_writer = make_uniq<CSVWriter>(*file_writer, column_names, false);
	SetWriterConfigs(*csv_writer, column_names);
	bool should_write_header = file_writer->handle->GetFileSize() == 0;

	// We write the header only if the file was empty: when appending to the file we don't
	csv_writer->options.dialect_options.header = {should_write_header, true};

	// Initialize the writer, this writes out the header if required
	csv_writer->Initialize();

	// Needed to ensure we correctly start with a newline
	csv_writer->SetWrittenAnything(true);

	RegisterWriter(table, std::move(csv_writer));

	// Ensures that the file is fully initialized when this function returns
	file_writer->Sync();

	table_writer.initialized = true;
}

unique_ptr<BufferedFileWriter> FileLogStorage::InitializeFileWriter(DatabaseInstance &db, const string &path) {
	auto &fs = db.GetFileSystem();

	// Create parent directories if non existent
	auto pos = path.find_last_of("/\\");
	if (pos != path.npos) {
		fs.CreateDirectoriesRecursive(path.substr(0, pos));
	}

	FileOpenFlags flags;
	if (!fs.FileExists(path)) {
		flags = FileFlags::FILE_FLAGS_DISABLE_LOGGING | FileFlags::FILE_FLAGS_WRITE |
		        FileFlags::FILE_FLAGS_FILE_CREATE_NEW | FileCompressionType::UNCOMPRESSED;
	} else {
		flags = FileFlags::FILE_FLAGS_DISABLE_LOGGING | FileFlags::FILE_FLAGS_WRITE | FileFlags::FILE_FLAGS_APPEND;
	}

	return make_uniq<BufferedFileWriter>(fs, path, flags);
}

void FileLogStorage::Truncate() {
	lock_guard<mutex> lck(lock);

	// Reset buffers
	ResetAllBuffers();

	for (const auto &it : tables) {
		auto &file_writer = it.second.file_writer;
		if (!file_writer) {
			continue;
		}
		// Truncate the file writer
		file_writer->Truncate(0);
		auto &writer = GetWriter(it.first);
		// Reset writer and header option, then re-initialize
		writer.Reset(nullptr);
		writer.options.dialect_options.header = CSVOption<bool>(true);
		writer.Initialize(true);
	}
}

void FileLogStorage::BeforeFlush(LoggingTargetTable table, DataChunk &) {
	// Lazily initialize the files
	Initialize(table);
}

void FileLogStorage::AfterFlush(LoggingTargetTable table, DataChunk &) {
	tables[table].file_writer->Sync();
}

void FileLogStorage::Initialize(LoggingTargetTable table) {
	auto &table_writer = tables[table];
	if (!table_writer.initialized) {
		if (table_writer.path.empty()) {
			throw InvalidConfigurationException("Failed to initialize file log storage table, path wasn't set");
		}

		InitializeFile(db, table);
	}
}

void FileLogStorage::SetPaths(const string &base_path) {
	for (auto &it : tables) {
		it.second.path.clear();
	}

	LocalFileSystem fs;
	if (normalize_contexts) {
		tables[LoggingTargetTable::LOG_CONTEXTS].path = fs.JoinPath(base_path, "duckdb_log_contexts.csv");
		tables[LoggingTargetTable::LOG_ENTRIES].path = fs.JoinPath(base_path, "duckdb_log_entries.csv");
	} else {
		if (StringUtil::EndsWith(base_path, ".csv")) {
			tables[LoggingTargetTable::ALL_LOGS].path = base_path;
		} else {
			tables[LoggingTargetTable::LOG_ENTRIES].path = fs.JoinPath(base_path, "duckdb_log_entries.csv");
		}
	}
}

void FileLogStorage::UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config) {
	auto config_copy = config;

	string new_path;
	bool normalize_contexts_new_value = normalize_contexts;
	bool normalize_set_explicitly = false;
	bool require_reinitializing_files = false;

	vector<string> to_remove;
	for (const auto &it : config_copy) {
		auto key = StringUtil::Lower(it.first);
		if (key == "path") {
			auto path_value = it.second.ToString();
			//! We implicitly set normalize to false when a path ending in .csv is specified
			if (!normalize_set_explicitly && StringUtil::EndsWith(path_value, ".csv")) {
				normalize_contexts_new_value = false;
			}
			new_path = path_value;
			to_remove.push_back(it.first);
		} else if (key == "normalize") {
			normalize_set_explicitly = true;
			normalize_contexts_new_value = it.second.GetValue<bool>();
			to_remove.push_back(it.first);
		} else if (key == "delim") {
			require_reinitializing_files = true;
		}
	}

	if (StringUtil::EndsWith(new_path, ".csv") && normalize_contexts_new_value) {
		throw InvalidConfigurationException(
		    "Can not set path to '%s' while normalize is true. Normalize will make DuckDB write multiple log files to "
		    "more efficiently store log entries. Please specify a directory path instead of a csv file path, or set "
		    "normalize to false.",
		    new_path);
	}

	// If any writer is initialized, we flush first:
	// - when switching between normalized and denormalized, this is necessary since we are changing the buffer schema
	// - when simply changing the path, it avoids writing log entries written before this change to end up in the new
	// file
	bool initialized = false;
	for (auto &it : tables) {
		initialized |= it.second.initialized;
	}
	if (initialized) {
		FlushAllInternal();
	}

	require_reinitializing_files |= normalize_contexts != normalize_contexts_new_value;
	normalize_contexts = normalize_contexts_new_value;

	// Reset the buffers to ensure they have the correct schema
	if (require_reinitializing_files) {
		ResetAllBuffers();

		// Mark tables as uninitialized
		for (auto &table : tables) {
			table.second.initialized = false;
		}
	}

	// Apply any path change
	if (new_path != base_path) {
		base_path = new_path;
		SetPaths(new_path);
	}

	for (const auto &it : to_remove) {
		config_copy.erase(it);
	}

	CSVLogStorage::UpdateConfigInternal(db, config_copy);
}

unique_ptr<TableRef> FileLogStorage::BindReplaceInternal(ClientContext &context, TableFunctionBindInput &input,
                                                         const string &path, const string &select_clause,
                                                         const string &csv_columns) {
	string sub_query_string =
	    StringUtil::Format("%s FROM read_csv_auto(%s, columns={%s})", select_clause, SQLString(path), csv_columns);

	Parser parser(context);
	parser.ParseQuery(sub_query_string);
	auto select_stmt = unique_ptr_cast<SQLStatement, SelectStatement>(std::move(parser.statements[0]));

	return duckdb::make_uniq<SubqueryRef>(std::move(select_stmt));
}

unique_ptr<TableRef> FileLogStorage::BindReplace(ClientContext &context, TableFunctionBindInput &input,
                                                 LoggingTargetTable table) {
	lock_guard<mutex> lck(lock);

	// We only allow scanning enabled tables
	if (!IsEnabledInternal(table)) {
		return nullptr;
	}

	// We start by flushing the table to ensure we scan the latest version
	FlushInternal(table);

	if (normalize_contexts && table == LoggingTargetTable::ALL_LOGS) {
		throw InvalidConfigurationException("Can not scan ALL_LOGS table when logs are normalized");
	}
	if (!normalize_contexts && table != LoggingTargetTable::ALL_LOGS) {
		throw InvalidConfigurationException("Can only scan ALL_LOGS table when logs are normalized");
	}

	string select = "SELECT *";
	string path = tables[table].path;

	string columns;
	if (table == LoggingTargetTable::LOG_ENTRIES) {
		columns = "'context_id': 'UBIGINT', 'timestamp': 'TIMESTAMP WITH TIME ZONE', 'type': 'VARCHAR', 'log_level': "
		          "'VARCHAR' , "
		          "'message': 'VARCHAR'";
	} else if (table == LoggingTargetTable::LOG_CONTEXTS) {
		columns = "'context_id': 'UBIGINT', 'scope': 'VARCHAR', 'connection_id': 'UBIGINT', 'transaction_id': "
		          "'UBIGINT', 'query_id': 'UBIGINT', 'thread_id': 'UBIGINT'";
	} else if (table == LoggingTargetTable::ALL_LOGS) {
		select = "SELECT context_id, scope, connection_id, transaction_id, query_id, thread_id, timestamp, type, "
		         "log_level, message ";
		columns = "'context_id': 'UBIGINT', 'scope': 'VARCHAR', 'connection_id': 'UBIGINT', 'transaction_id': "
		          "'UBIGINT', 'query_id': 'UBIGINT', 'thread_id': 'UBIGINT', 'timestamp': 'TIMESTAMP WITH TIME ZONE', "
		          "'type': "
		          "'VARCHAR', 'log_level': 'VARCHAR' , 'message': 'VARCHAR'";
	} else {
		throw InternalException("Invalid logging target table");
	}

	return BindReplaceInternal(context, input, path, select, columns);
}

BufferingLogStorage::BufferingLogStorage(DatabaseInstance &db_p, idx_t buffer_size, bool normalize)
    : normalize_contexts(normalize), buffer_limit(buffer_size) {
	ResetLogBuffers();
}

void BufferingLogStorage::ResetLogBuffers() {
	idx_t buffer_size = MaxValue<idx_t>(buffer_limit, 1);
	// initialize the new buffers before replacing the old ones - initializing allocates, and a buffer that is
	// replaced but not initialized has no columns, so every later write to it indexes out of bounds
	if (normalize_contexts) {
		auto log_entries = make_uniq<DataChunk>();
		auto log_contexts = make_uniq<DataChunk>();
		log_entries->Initialize(Allocator::DefaultAllocator(), GetSchema(LoggingTargetTable::LOG_ENTRIES), buffer_size);
		log_contexts->Initialize(Allocator::DefaultAllocator(), GetSchema(LoggingTargetTable::LOG_CONTEXTS),
		                         buffer_size);
		buffers[LoggingTargetTable::LOG_ENTRIES] = std::move(log_entries);
		buffers[LoggingTargetTable::LOG_CONTEXTS] = std::move(log_contexts);
	} else {
		auto all_logs = make_uniq<DataChunk>();
		all_logs->Initialize(Allocator::DefaultAllocator(), GetSchema(LoggingTargetTable::ALL_LOGS), buffer_size);
		buffers[LoggingTargetTable::ALL_LOGS] = std::move(all_logs);
	}
	registered_contexts.clear();
}

void BufferingLogStorage::ResetAllBuffers() {
	ResetLogBuffers();
}

InMemoryLogStorageScanState::InMemoryLogStorageScanState(LoggingTargetTable table) : LogStorageScanState(table) {
}
InMemoryLogStorageScanState::~InMemoryLogStorageScanState() {
}

InMemoryLogStorage::InMemoryLogStorage(DatabaseInstance &db_p) : BufferingLogStorage(db_p, STANDARD_VECTOR_SIZE, true) {
	log_storage_buffers[LoggingTargetTable::LOG_ENTRIES] =
	    make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), GetSchema(LoggingTargetTable::LOG_ENTRIES));
	log_storage_buffers[LoggingTargetTable::LOG_CONTEXTS] =
	    make_uniq<ColumnDataCollection>(Allocator::DefaultAllocator(), GetSchema(LoggingTargetTable::LOG_CONTEXTS));
}

void InMemoryLogStorage::ResetAllBuffers() {
	BufferingLogStorage::ResetAllBuffers();

	for (const auto &buffer : log_storage_buffers) {
		buffer.second->Reset();
	}
}

ColumnDataCollection &InMemoryLogStorage::GetBuffer(LoggingTargetTable table) const {
	auto res = log_storage_buffers.find(table);
	if (res == log_storage_buffers.end()) {
		throw InternalException("Failed to find table");
	}
	return *res->second;
}

InMemoryLogStorage::~InMemoryLogStorage() {
}

BufferingLogStorage::~BufferingLogStorage() {
}

static void WriteLoggingContextsToChunk(DataChunk &chunk, const RegisteredLoggingContext &context, idx_t &col) {
	auto size = chunk.size();

	auto context_id_data = FlatVector::GetDataMutable<idx_t>(chunk.data[col++]);
	context_id_data[size] = context.context_id;

	auto context_scope_data = FlatVector::GetDataMutable<string_t>(chunk.data[col]);
	context_scope_data[size] = StringVector::AddString(chunk.data[col++], EnumUtil::ToString(context.context.scope));

	if (context.context.connection_id.IsValid()) {
		auto client_context_data = FlatVector::GetDataMutable<idx_t>(chunk.data[col++]);
		client_context_data[size] = context.context.connection_id.GetIndex();
	} else {
		FlatVector::ValidityMutable(chunk.data[col++]).SetInvalid(size);
	}
	if (context.context.transaction_id.IsValid()) {
		auto client_context_data = FlatVector::GetDataMutable<idx_t>(chunk.data[col++]);
		client_context_data[size] = context.context.transaction_id.GetIndex();
	} else {
		FlatVector::ValidityMutable(chunk.data[col++]).SetInvalid(size);
	}
	if (context.context.query_id.IsValid()) {
		auto client_context_data = FlatVector::GetDataMutable<idx_t>(chunk.data[col++]);
		client_context_data[size] = context.context.query_id.GetIndex();
	} else {
		FlatVector::ValidityMutable(chunk.data[col++]).SetInvalid(size);
	}

	if (context.context.thread_id.IsValid()) {
		auto thread_data = FlatVector::GetDataMutable<idx_t>(chunk.data[col++]);
		thread_data[size] = context.context.thread_id.GetIndex();
	} else {
		FlatVector::ValidityMutable(chunk.data[col++]).SetInvalid(size);
	}

	chunk.SetChildCardinality(size + 1);
}

void BufferingLogStorage::WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type,
                                        const string &log_message, const RegisteredLoggingContext &context) {
	unique_lock<mutex> lck(lock);

	auto &log_entries_buffer =
	    normalize_contexts ? buffers[LoggingTargetTable::LOG_ENTRIES] : buffers[LoggingTargetTable::ALL_LOGS];

	auto size = log_entries_buffer->size();
	if (size >= buffer_limit && buffer_limit != 0) {
		throw InternalException("Log buffer limit exceeded: code should have flushed before");
	}

	if (registered_contexts.find(context.context_id) == registered_contexts.end()) {
		WriteLoggingContext(context);
		// New context_id: we should flush both contexts and entries next time LOG_ENTRIES gets flushed
		flush_contexts_on_next_entry_flush = true;
	}

	idx_t col = 0;

	if (normalize_contexts) {
		auto context_id_data = FlatVector::GetDataMutable<idx_t>(log_entries_buffer->data[col++]);
		context_id_data[size] = context.context_id;
	} else {
		WriteLoggingContextsToChunk(*log_entries_buffer, context, col);
	}

	auto timestamp_data = FlatVector::GetDataMutable<timestamp_t>(log_entries_buffer->data[col++]);
	timestamp_data[size] = timestamp;

	auto type_data = FlatVector::GetDataMutable<string_t>(log_entries_buffer->data[col]);
	type_data[size] = StringVector::AddString(log_entries_buffer->data[col++], log_type);

	auto level_data = FlatVector::GetDataMutable<string_t>(log_entries_buffer->data[col]);
	level_data[size] = StringVector::AddString(log_entries_buffer->data[col++],
	                                           EnumUtil::ToString(level)); // TODO: do cast on write out

	auto message_data = FlatVector::GetDataMutable<string_t>(log_entries_buffer->data[col]);
	message_data[size] = StringVector::AddString(log_entries_buffer->data[col++], log_message);

	log_entries_buffer->SetChildCardinality(size + 1);

	if (size + 1 >= buffer_limit) {
		if (normalize_contexts) {
			// Flush all entries
			FlushInternal(LoggingTargetTable::LOG_ENTRIES);
			// Flush contexts if required
			if (flush_contexts_on_next_entry_flush) {
				FlushInternal(LoggingTargetTable::LOG_CONTEXTS);
				flush_contexts_on_next_entry_flush = false;
			}
		} else {
			FlushInternal(LoggingTargetTable::ALL_LOGS);
		}
	}
}

void BufferingLogStorage::WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) {
	throw NotImplementedException("BufferingLogStorage::WriteLogEntries(DataChunk &chunk) not implemented");
}

void BufferingLogStorage::FlushAll() {
	unique_lock<mutex> lck(lock);
	if (!only_flush_on_full_buffer) {
		FlushAllInternal();
	}
}

void BufferingLogStorage::Flush(LoggingTargetTable table) {
	unique_lock<mutex> lck(lock);
	if (!only_flush_on_full_buffer) {
		FlushInternal(table);
	}
}

void BufferingLogStorage::Truncate() {
	unique_lock<mutex> lck(lock);
	ResetAllBuffers();
}

void InMemoryLogStorage::FlushChunk(LoggingTargetTable table, DataChunk &chunk) {
	D_ASSERT(table == LoggingTargetTable::LOG_ENTRIES || table == LoggingTargetTable::LOG_CONTEXTS);
	log_storage_buffers[table]->Append(chunk);
}

void BufferingLogStorage::FlushAllInternal() {
	if (normalize_contexts) {
		FlushInternal(LoggingTargetTable::LOG_ENTRIES);
		FlushInternal(LoggingTargetTable::LOG_CONTEXTS);
	} else {
		FlushInternal(LoggingTargetTable::ALL_LOGS);
	}
}

void BufferingLogStorage::FlushInternal(LoggingTargetTable table) {
	if (!IsEnabledInternal(table)) {
		throw InvalidConfigurationException("Cannot flush disabled logging target");
	}
	FlushChunk(table, *buffers[table]);
	buffers[table]->Reset();
}

void BufferingLogStorage::WriteLoggingContext(const RegisteredLoggingContext &context) {
	registered_contexts.insert(context.context_id);

	// If we don't normalize the contexts they are written out on every log entry
	if (!normalize_contexts) {
		return;
	}

	idx_t col = 0;

	auto &log_contexts_buffer = buffers[LoggingTargetTable::LOG_CONTEXTS];

	if (log_contexts_buffer->size() + 1 > buffer_limit) {
		FlushInternal(LoggingTargetTable::LOG_CONTEXTS);
	}

	WriteLoggingContextsToChunk(*log_contexts_buffer, context, col);
}

bool InMemoryLogStorage::CanScan(LoggingTargetTable table) {
	unique_lock<mutex> lck(lock);
	return IsEnabledInternal(table);
}

unique_ptr<LogStorageScanState> InMemoryLogStorage::CreateScanState(LoggingTargetTable table) const {
	return make_uniq<InMemoryLogStorageScanState>(table);
}

bool InMemoryLogStorage::Scan(LogStorageScanState &state, DataChunk &result) const {
	unique_lock<mutex> lck(lock);
	auto &in_mem_scan_state = state.Cast<InMemoryLogStorageScanState>();
	return GetBuffer(in_mem_scan_state.table).Scan(in_mem_scan_state.scan_state, result);
}

optional_idx InMemoryLogStorage::GetScanRowCount(LoggingTargetTable table) const {
	unique_lock<mutex> lck(lock);
	return GetBuffer(table).Count();
}

void InMemoryLogStorage::InitializeScan(LogStorageScanState &state) const {
	unique_lock<mutex> lck(lock);
	auto &in_mem_scan_state = state.Cast<InMemoryLogStorageScanState>();
	GetBuffer(in_mem_scan_state.table)
	    .InitializeScan(in_mem_scan_state.scan_state, ColumnDataScanProperties::DISALLOW_ZERO_COPY);
}

} // namespace duckdb
