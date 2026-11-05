//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/logging/log_storage.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/logging/logging.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/serializer/write_stream.hpp"
#include "duckdb/common/serializer/buffered_file_writer.hpp"

#include "duckdb/common/types/column/column_data_scan_states.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {
struct TableFunctionBindInput;
struct RegisteredLoggingContext;
class ColumnDataCollection;
struct ColumnDataScanState;
class MemoryStream;
struct LogStorageConfig;
class CSVWriter;
struct CSVWriterState;
class BufferedFileWriter;
struct CSVWriterOptions;
struct CSVReaderOptions;

//! Logging storage can store entries normalized or denormalized. This enum describes what a single table/file/etc
//! contains
enum class LoggingTargetTable : uint8_t {
	ALL_LOGS,     // Denormalized: log entries consisting of both the full log entry and the context
	LOG_ENTRIES,  // Normalized: contains only the log entries and a context_id
	LOG_CONTEXTS, // Normalized: contains only the log contexts
};

class LogStorageScanState {
public:
	explicit LogStorageScanState(LoggingTargetTable table_p) : table(table_p) {
	}
	virtual ~LogStorageScanState() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	LoggingTargetTable table;
};

// Interface for Log Storage
class LogStorage {
public:
	DUCKDB_API explicit LogStorage() {
	}
	DUCKDB_API virtual ~LogStorage() = default;

	virtual const string GetStorageName() = 0;

	static vector<LogicalType> GetSchema(LoggingTargetTable table);
	static vector<Identifier> GetColumnNames(LoggingTargetTable table);

	//! WRITING
	DUCKDB_API virtual void WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type,
	                                      const string &log_message, const RegisteredLoggingContext &context) = 0;
	DUCKDB_API virtual void WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) = 0;
	DUCKDB_API virtual void FlushAll() = 0;
	DUCKDB_API virtual void Flush(LoggingTargetTable table) = 0;
	DUCKDB_API virtual void Truncate();
	DUCKDB_API virtual bool IsEnabled(LoggingTargetTable table) = 0;

	//! READING (OPTIONAL)
	DUCKDB_API virtual bool CanScan(LoggingTargetTable table) {
		return false;
	}
	// Reading interface 1: basic single-threaded scan
	DUCKDB_API virtual unique_ptr<LogStorageScanState> CreateScanState(LoggingTargetTable table) const;
	DUCKDB_API virtual bool Scan(LogStorageScanState &state, DataChunk &result) const;
	DUCKDB_API virtual void InitializeScan(LogStorageScanState &state) const;
	//! The number of rows a scan of the table produces, if known (used for progress reporting)
	DUCKDB_API virtual optional_idx GetScanRowCount(LoggingTargetTable table) const;

	// Reading interface 2: using bind_replace
	DUCKDB_API virtual unique_ptr<TableRef> BindReplace(ClientContext &context, TableFunctionBindInput &input,
	                                                    LoggingTargetTable table);

	//! CONFIGURATION
	DUCKDB_API virtual void UpdateConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config);
};

//! The buffering Log storage implements a buffering mechanism around the Base LogStorage class. It implements some
//! general features that most log storages will need.
class BufferingLogStorage : public LogStorage {
public:
	explicit BufferingLogStorage(DatabaseInstance &db_p, idx_t buffer_size, bool normalize);
	~BufferingLogStorage() override;

	/// (Partially) Implements  LogStorage API

	//! Write out the entry to the buffers
	void WriteLogEntry(timestamp_t timestamp, LogLevel level, const string &log_type, const string &log_message,
	                   const RegisteredLoggingContext &context) final;
	//! Write out the chunk to the buffers
	void WriteLogEntries(DataChunk &chunk, const RegisteredLoggingContext &context) final;
	//! Flushes buffers for all tables
	void FlushAll() final;
	//! Flushes buffer for a specific table
	void Flush(LoggingTargetTable table) final;
	//! Truncates log storage: both buffers and persistent storage (if applicable)
	void Truncate() override;
	//! Apply a new log storage configuration
	void UpdateConfig(DatabaseInstance &db, case_insensitive_map_t<Value> &config) override;
	//! Returns whether the table is enabled for this storage
	bool IsEnabled(LoggingTargetTable table) override;

protected:
	/// Interface to child classes

	//! Invoked whenever buffers are full to flush to storage
	virtual void FlushChunk(LoggingTargetTable table, DataChunk &chunk) = 0;
	//! This method is called in a chained way down the class hierarchy. This allows each class to interpret its own
	//! part of the config. Unhandled config values that are left over will result in an error
	virtual void UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config);
	//! ResetAllBuffers will clear all buffered data. To be overridden by child classes to ensure their buffers are
	//! flushed too
	virtual void ResetAllBuffers();

	/// Helper methods

	//! Flushes all tables
	void FlushAllInternal();
	//! Flushes one of the tables
	void FlushInternal(LoggingTargetTable table);
	//! Whether a specific table is available in the log storage
	bool IsEnabledInternal(LoggingTargetTable table);

	idx_t GetBufferLimit() const;

	//! lock to be used by this class and child classes to ensure thread safety TODO: maybe remove and delegate
	//! thread-safety to LogManager?
	mutable mutex lock;
	//! Switches between using false = use LoggingTargetTable::ALL_LOGS, true = use LoggingTargetTable::LOG_ENTRIES +
	//! LoggingTargetTable::CONTEXTS
	bool normalize_contexts = true;

private:
	//! Resets the log buffers
	void ResetLogBuffers();
	//! Write out a logging context
	void WriteLoggingContext(const RegisteredLoggingContext &context);

	//! The currently registered RegisteredLoggingContext's
	unordered_set<idx_t> registered_contexts;
	//! Configuration for buffering
	idx_t buffer_limit = 0;
	//! Debug option for testing buffering behaviour
	bool only_flush_on_full_buffer = false;
	//! The buffers used for each table
	map<LoggingTargetTable, unique_ptr<DataChunk>> buffers;
	//! This flag is set whenever a new context_is written to the entry buffer. It means that the next flush of
	//! LoggingTargetTable::LOG_ENTRIES also requires a flush of LoggingTargetTable::LOG_CONTEXTS
	bool flush_contexts_on_next_entry_flush = false;
};

//! The CSVLogStorage implements an additional layer on the BufferingLogStorage which will handle converting the log
//! entries and contexts to CSV lines. It provides functionality to write log data in CSV format with automatic type
//! casting and configuration of CSV writers. This class serves as a base for both file-based and stdout-based CSV
//! logging.
class CSVLogStorage : public BufferingLogStorage {
public:
	explicit CSVLogStorage(DatabaseInstance &db, bool normalize, idx_t buffer_size);
	~CSVLogStorage() override;

protected:
	/// Implement the BufferingLogStorage interface

	//! Flushes the Chunk to the CSV writers
	void FlushChunk(LoggingTargetTable table, DataChunk &chunk) final;
	//! Resets all buffers and state
	void ResetAllBuffers() override;
	//! Implements CSVLogStorage specific config handling
	void UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config) override;

	/// Interface to child classes

	//! Hooks for child class to run code pre-flush
	virtual void BeforeFlush(LoggingTargetTable table, DataChunk &chunk) {};
	virtual void AfterFlush(LoggingTargetTable table, DataChunk &chunk) {};

	/// Helper functions

	//! To be called by child classed to register the CSV writers used to write to.
	void RegisterWriter(LoggingTargetTable table, unique_ptr<CSVWriter> writer);
	//! Returns the writer for a table
	CSVWriter &GetWriter(LoggingTargetTable table);
	//! Configure a CSV writer by initializing its settings with the `writer_options` and `reader_options` settings
	void SetWriterConfigs(CSVWriter &Writer, vector<Identifier> column_names);
	//! Allows child classes to manipulate options
	CSVWriterOptions &GetCSVWriterOptions();
	//! Allows child classes to manipulate options
	CSVReaderOptions &GetCSVReaderOptions();

private:
	//! Perform the cast (does not reset input chunk!)
	void ExecuteCast(LoggingTargetTable table, DataChunk &chunk);
	//! Reset the Cast chunks
	void ResetCastChunk();
	//! Initialize the cast chunks
	void InitializeCastChunk(LoggingTargetTable table);

	//! The cast buffers used to cast from the original types to the VARCHAR types ready to write to CSV format
	map<LoggingTargetTable, unique_ptr<DataChunk>> cast_buffers;
	//! The writers to be registered by child classes
	map<LoggingTargetTable, unique_ptr<CSVWriter>> writers;

	//! CSV Options to initialize the CSVWriters with. TODO: cleanup, this is now a little bit of a mixed bag of
	//! settings
	unique_ptr<CSVWriterOptions> writer_options;
	unique_ptr<CSVReaderOptions> reader_options;
};

//! Implements a stdout-based log storage using log lines written in CSV format to allow for easy parsing of the log
//! messages Note that this only supports denormalized logging since there is only 1 practical output stream.
class StdOutLogStorage : public CSVLogStorage {
public:
	explicit StdOutLogStorage(DatabaseInstance &db);
	~StdOutLogStorage() override;

	const string GetStorageName() override {
		return "StdOutLogStorage";
	}

private:
	class StdOutWriteStream : public WriteStream {
		void WriteData(const_data_ptr_t buffer, idx_t write_size) override;
	};

	StdOutWriteStream stdout_stream;
};

//! FileLogStorage implements a file-based logging system in CSV format.
//! It implements CSVLogStorage to provide persistent log storage in CSV files. The FileLogStorage can operate in
//! normalized (separate files for entries and contexts) or denormalized mode (single file)
class FileLogStorage : public CSVLogStorage {
public:
	explicit FileLogStorage(DatabaseInstance &db);
	~FileLogStorage() override;

	const string GetStorageName() override {
		return "FileLogStorage";
	}

	/// Implement LogStorage interface

	//! Truncates the csv files
	void Truncate() override;
	//! Bind replace function to scan the different tables
	unique_ptr<TableRef> BindReplace(ClientContext &context, TableFunctionBindInput &input,
	                                 LoggingTargetTable table) override;

protected:
	/// Implement CSVLogStorage interface

	//! Handles the config related to the FileLogStorage
	void UpdateConfigInternal(DatabaseInstance &db, case_insensitive_map_t<Value> &config) override;
	//! Lazily initializes the CSV files before first flush
	void BeforeFlush(LoggingTargetTable table, DataChunk &chunk) override;
	//! Calls `Sync` on the FileWriters to ensure a LogStorage Flush is flushed to disk immediately
	void AfterFlush(LoggingTargetTable table, DataChunk &chunk) override;

private:
	//! Initialize the csv file for `table`
	void InitializeFile(DatabaseInstance &db, LoggingTargetTable table);
	//! Initialize the filewriter to be passed to the CSVWriter
	static unique_ptr<BufferedFileWriter> InitializeFileWriter(DatabaseInstance &db, const string &path);
	//! Ensures the table is initialized, used in lazy initialization. If already initialized this will NOP
	void Initialize(LoggingTargetTable table);
	//! Internal helper function to handle the BindReplace generation
	unique_ptr<TableRef> BindReplaceInternal(ClientContext &context, TableFunctionBindInput &input, const string &path,
	                                         const string &select_clause, const string &csv_columns);

	//! DB reference to get the DB filesystem
	DatabaseInstance &db;

	//! Writer for a table
	struct TableWriter {
		//! Passed as WriteStreams to the CSVWriter in the base class
		unique_ptr<BufferedFileWriter> file_writer;
		//! Path to initialize the file_writer from
		string path;
		//! Whether the file_writer has been (lazily) initialized
		bool initialized = false;
	};

	//! The table info per table
	map<LoggingTargetTable, TableWriter> tables;

	//! Base path to generate the file paths from
	string base_path;

private:
	void SetPaths(const string &base_path);
};

//! State for scanning the in memory buffers
class InMemoryLogStorageScanState : public LogStorageScanState {
public:
	explicit InMemoryLogStorageScanState(LoggingTargetTable table);
	~InMemoryLogStorageScanState() override;

	ColumnDataScanState scan_state;
};

//! The InMemoryLogStorage implements a log storage that is backed by ColumnDataCollection's. It only support normalized
//! mode and support a basic single-threaded scan. TODO: improve?
class InMemoryLogStorage : public BufferingLogStorage {
public:
	explicit InMemoryLogStorage(DatabaseInstance &db);
	~InMemoryLogStorage() override;

	const string GetStorageName() override {
		return "InMemoryLogStorage";
	}

	//! Implement LogStorage Single-threaded scan interface
	bool CanScan(LoggingTargetTable table) override;
	unique_ptr<LogStorageScanState> CreateScanState(LoggingTargetTable table) const override;
	bool Scan(LogStorageScanState &state, DataChunk &result) const override;
	void InitializeScan(LogStorageScanState &state) const override;
	optional_idx GetScanRowCount(LoggingTargetTable table) const override;

protected:
	/// Implement BufferingLogStorage interface

	//! Flushes a chunk to the corresponding ColumnDataCollection
	void FlushChunk(LoggingTargetTable table, DataChunk &chunk) override;
	//! Resets the all ColumnDataCollection's
	void ResetAllBuffers() override;

private:
	//! Helper function to get the buffer
	ColumnDataCollection &GetBuffer(LoggingTargetTable table) const;

	map<LoggingTargetTable, unique_ptr<ColumnDataCollection>> log_storage_buffers;
};

} // namespace duckdb
