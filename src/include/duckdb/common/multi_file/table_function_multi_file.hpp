//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/multi_file/table_function_multi_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/once_flag.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// How a single-file table function takes part in a multi-file scan
//===--------------------------------------------------------------------===//
//! What the multi-file scan tells the bind of the single-file function that reads one of its files - see
//! TableFunctionBindInput::multi_file_input. The file itself is the input of the bind: see GetFile
struct TableFunctionFileBindInput {
	//! (Optional) The schema this bind is expected to produce, when the schema of the scan was already determined -
	//! the bind should read the file using this schema instead of determining a schema of its own
	optional_ptr<const vector<Identifier>> expected_names;
	optional_ptr<const vector<LogicalType>> expected_types;
	//! (Optional) The bind data that determined the schema above, when it came from another bind of this same
	//! function. This lets the bind read the file exactly the way the schema was determined, rather than deriving
	//! that from the names and types alone
	optional_ptr<const FunctionData> expected_bind_data;
	//! The options of the scan. They tell the bind how its file is combined with the other files of the scan - e.g.
	//! whether their columns are unified by name, in which case a type that could not be determined should be
	//! reported as SQLNULL so the other files can determine it
	optional_ptr<const MultiFileOptions> multi_file_options;
	//! Whether the scan reads several files. Options that describe the schema then describe the scan rather than
	//! this one file, so the bind should not hold this file to them exactly
	bool multi_file_scan = false;
	//! Whether the file is only bound to determine the schema of the scan - it is not read with the resulting bind
	//! data, so the bind should not keep resources around for scanning it
	bool schema_only = false;
	//! (Optional) The bind data an earlier bind of this same file produced, when it was bound before to determine
	//! the schema of the scan. The bind can take whatever it read from the file from there again
	optional_ptr<const FunctionData> file_bind_data;

	bool HasExpectedSchema() const {
		return expected_names && expected_types;
	}
	//! The multi-file input of a bind - an empty one when the function is not bound as part of a multi-file scan
	static const TableFunctionFileBindInput &Get(const TableFunctionBindInput &input) {
		static const TableFunctionFileBindInput EMPTY;
		return input.multi_file_input ? *input.multi_file_input : EMPTY;
	}
	//! The file a single-file function reads: its first input, which is the path of the file or a file struct
	//! that also holds the options to open the file with (e.g. its size or encryption key) - see
	//! OpenFileInfo::FromValue. A multi-file scan passes every file it reads this way
	static OpenFileInfo GetFile(const TableFunctionBindInput &input) {
		auto &file = input.inputs[0];
		auto type = file.type().id();
		if (type != LogicalTypeId::VARCHAR && type != LogicalTypeId::STRUCT && type != LogicalTypeId::VARIANT) {
			throw BinderException("%s reads a single file - given as a VARCHAR path or as a file struct, not as %s",
			                      input.table_function.GetName(), file.type().ToString());
		}
		return OpenFileInfo::FromValue(file, input.table_function.GetName());
	}
};

//! What the multi-file scan tells the initialization of the single-file function that reads one of its files - see
//! TableFunctionInitInput::multi_file_input
struct TableFunctionFileInitInput {
	//! (Optional) The types the columns must be produced as, when they differ from the types the function bound to.
	//! Only set for functions that declare "supports_cast_map" - the function converts to these types while reading,
	//! rather than having the conversion applied to its output
	optional_ptr<const unordered_map<column_t, LogicalType>> cast_map;
	//! (Optional) The index each of the filters has in the scan - a function that keeps state per filter across the
	//! files of a scan (like an adaptive filter order) identifies them by these
	optional_ptr<const vector<MultiFileGlobalIndex>> filter_global_indices;
	//! (Optional) Expressions the function must evaluate on the columns of its file before the filters are applied -
	//! used when a filter could not be expressed in the types the file stores
	optional_ptr<const unordered_map<ProjectionIndex, BaseFileReaderExpression>> expression_map;
	//! (Optional) The rows that were deleted from this file, which the function must not produce. The scan keeps
	//! ownership of the filter - it outlives the scan the function initializes. It is used (and updated) while the
	//! file is read, so it is not part of what makes this input const
	mutable optional_ptr<DeleteFilter> deletion_filter;
	//! (Optional) The virtual columns among the column indexes, as a map of the index they are projected in to the
	//! virtual column id wanted there. A virtual column gets an index of its own, past the columns the function bound
	optional_ptr<const unordered_map<column_t, column_t>> virtual_columns;
	//! The index of the file this function reads within the scan, and the number of files the scan reads in total.
	//! TableFunctionInitInput::op is the operator all those files are read for
	optional_idx file_index;
	idx_t file_count = 1;

	//! The multi-file input of an initialization - an empty one when the function is not read as part of a
	//! multi-file scan
	static const TableFunctionFileInitInput &Get(const TableFunctionInitInput &input) {
		static const TableFunctionFileInitInput EMPTY;
		return input.multi_file_input ? *input.multi_file_input : EMPTY;
	}
};

//! Input for combining the schemas of several files that were bound individually into one schema
struct TableFunctionCombineSchemaInput {
	TableFunctionCombineSchemaInput(const vector<reference<const FunctionData>> &bind_data_p, bool union_by_name_p)
	    : bind_data(bind_data_p), union_by_name(union_by_name_p) {
	}

	//! The bind data of each of the files whose schemas are being combined - in file order
	const vector<reference<const FunctionData>> &bind_data;
	//! Whether the schemas are combined because of union_by_name - the files are then expected to have different
	//! columns, which are unified by name. Otherwise the files are expected to have the same columns, and the
	//! schemas are combined to determine the schema of the scan more accurately
	bool union_by_name;
};

//! Claims the next batch for the given local state - returns false when there is nothing left to scan.
//! A function that implements this is scanned one batch at a time, rather than being run until it returns an empty
//! chunk. This lets the scan tell the batches apart, so that batches scanned in parallel can be put back in order
typedef bool (*table_function_claim_batch_t)(ClientContext &context, TableFunctionInput &input);
//! Prepares the batch a local state has just claimed - called without the lock of the scan held, so that preparing
//! batches is done in parallel. Only meaningful together with table_function_claim_batch_t
typedef void (*table_function_prepare_batch_t)(ClientContext &context, TableFunctionInput &input);
//! Called when a local state will not scan any more batches - lets the function release the resources of the batch
//! it scanned last. The counterpart of table_function_claim_batch_t
typedef void (*table_function_finish_batch_t)(ClientContext &context, TableFunctionInput &input);
//! Whether the scan of this function can be driven by read-ahead - batches are then claimed and have their I/O
//! scheduled ahead of being scanned. Only meaningful together with table_function_claim_batch_t
//! Bind data can be absent when the schema is supplied without opening a file, e.g. by a table format.
typedef bool (*table_function_supports_read_ahead_t)(optional_ptr<const FunctionData> bind_data);
//! Schedules the I/O needed by the batch a local state has claimed, so it can be loaded before it is scanned
typedef AsyncResult (*table_function_schedule_io_t)(ClientContext &context, TableFunctionInput &input);
//! Called on the read-ahead pool once the scan of this function has been initialized, before any batch is claimed.
//! Lets the function pre-open the resources its scan needs, so that claiming a batch does no I/O
typedef void (*table_function_prepare_read_ahead_t)(ClientContext &context, TableFunctionInput &input);
//! Combines the schemas of several individually bound files into one. The names and types are pre-filled with the
//! schemas of the files combined by name - the function can replace or adjust them. Returns the bind data describing
//! the combined schema, which is then handed to the bind of every file that is read - or nullptr when the files must
//! be bound individually and reconciled with the combined schema
typedef unique_ptr<FunctionData> (*table_function_combine_schema_t)(ClientContext &context,
                                                                    TableFunctionCombineSchemaInput &input,
                                                                    vector<LogicalType> &return_types,
                                                                    vector<Identifier> &names);
//! The columns of the file a function reads, with their nested structure and identifiers. The files of a scan are
//! mapped onto one another with these, so reporting only names and types is not enough
typedef vector<MultiFileColumnDefinition> (*table_function_get_file_columns_t)(ClientContext &context,
                                                                               const FunctionData &bind_data);

//! The options of a wrapped single-file table function - the named parameters that are forwarded to it as-is
class TableFunctionFileReaderOptions : public BaseFileReaderOptions {
public:
	named_argument_map_t named_parameters;
	//! The schema the files are expected to produce - set by COPY, which takes its columns from the target table
	vector<Identifier> expected_names;
	vector<LogicalType> expected_types;
	//! The bind data that determined the schema above, if it came from binding a file of this same scan
	shared_ptr<FunctionData> schema_bind_data;
	//! Whether the scan reads several files
	bool multi_file_scan = false;
};

//! Bind data of a multi-file function that wraps a single-file table function
struct TableFunctionMultiFileData : public TableFunctionData {
	TableFunctionFileReaderOptions options;
	//! The cardinality estimate of the first file, kept here because its reader can be released or pruned
	optional_idx cardinality;

	//! Whether the schema every file is read with is known upfront - because it was combined from several files,
	//! taken from the file the schema was determined on, or given by COPY from the target table
	bool HasExpectedSchema() const {
		return !options.expected_names.empty();
	}

	unique_ptr<FunctionData> Copy() const override {
		auto result = make_uniq<TableFunctionMultiFileData>();
		result->options = options;
		result->cardinality = cardinality;
		return std::move(result);
	}
};

//! Settings of a multi-file wrapper - how the wrapped single-file function is exposed as a multi-file function
struct TableFunctionMultiFileSettings {
	//! How the file paths passed to the function are globbed
	FileGlobInput glob_input = FileGlobOptions::DISALLOW_EMPTY;
	//! How the files are referred to in error messages (e.g. "Parquet", "JSON") - defaults to the function name
	string reader_type;
	//! How many files are sampled by default to determine the schema
	idx_t maximum_sample_files = 1;
	//! The named parameter of the wrapped function that sets how many files are sampled (if it has one)
	Identifier sample_files_parameter;
	//! Whether the schemas of the sampled files are combined into a union of their columns - files are then allowed
	//! to be missing columns of the combined schema
	bool sampled_schema_is_union = true;

	//! How the wrapped function takes part in the scan - all optional, see the typedefs above
	table_function_claim_batch_t claim_batch = nullptr;
	table_function_prepare_batch_t prepare_batch = nullptr;
	table_function_finish_batch_t finish_batch = nullptr;
	table_function_supports_read_ahead_t supports_read_ahead = nullptr;
	table_function_schedule_io_t schedule_io = nullptr;
	table_function_prepare_read_ahead_t prepare_read_ahead = nullptr;
	table_function_combine_schema_t combine_schema = nullptr;
	table_function_get_file_columns_t get_file_columns = nullptr;
	//! Whether one local state may be used to scan several files in turn - the scan then keeps the state it created
	//! rather than making a new one per file, so that what the function learns while reading a file (like the order
	//! its filters are best applied in) carries over to the next
	bool reuses_local_state = false;
	//! Whether the function can produce columns as a different type than it bound them - see
	//! TableFunctionFileInitInput::cast_map. The function then reports the schema of its own file, and still
	//! produces the types of the scan
	bool supports_cast_map = false;
	//! Whether the function counts the bytes it scans, reported as operator.bytes_scanned of the scan
	bool reports_bytes_scanned = false;
};

//! The function info of a multi-file wrapper - holds the single-file function that is wrapped
struct TableFunctionMultiFileInfo : public TableFunctionInfo {
	TableFunctionMultiFileInfo(TableFunction function_p, TableFunctionMultiFileSettings settings_p)
	    : function(std::move(function_p)), settings(std::move(settings_p)) {
	}

	TableFunction function;
	TableFunctionMultiFileSettings settings;
};

//! Union data of a wrapped single-file table function
class TableFunctionUnionData : public BaseUnionData {
public:
	explicit TableFunctionUnionData(OpenFileInfo file) : BaseUnionData(std::move(file)) {
	}

	optional_idx cardinality;
	//! The bind data of the wrapped function for this file. It is used to combine the schemas of several files, and
	//! kept afterwards so that the bind that reads the file can reuse what this one read from it
	shared_ptr<FunctionData> bind_data;
	//! The statistics callback of the wrapped function, which reads the statistics of this file from its bind data
	table_statistics_t statistics = nullptr;

	optional_idx TryGetCardinalityEstimate() const override {
		return cardinality;
	}
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const Identifier &name) override;
};

//! Reads a single file by binding and executing the wrapped table function over that file
class TableFunctionFileReader : public BaseFileReader {
public:
	TableFunctionFileReader(TableFunction function, OpenFileInfo file, named_argument_map_t named_parameters,
	                        TableFunctionMultiFileSettings settings);
	~TableFunctionFileReader() override;

public:
	string GetReaderType() const override;
	bool UseCastMap() const override;
	shared_ptr<BaseUnionData> GetUnionData(idx_t file_idx) override;
	unique_ptr<BaseStatistics> GetStatistics(ClientContext &context, const Identifier &name) override;
	unique_ptr<BaseStatistics> GetVirtualColumnStatistics(ClientContext &context, column_t virtual_column_id) override;
	void AddVirtualColumn(column_t virtual_column_id) override;
	void PrepareReader(ClientContext &context, GlobalTableFunctionState &gstate) override;
	void PrepareReadAhead(ClientContext &context, GlobalTableFunctionState &gstate) override;
	void FinishFile(ClientContext &context, GlobalTableFunctionState &gstate) override;
	bool TryInitializeScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	void PrepareScan(ClientContext &context, GlobalTableFunctionState &gstate,
	                 LocalTableFunctionState &lstate) override;
	AsyncResult ScheduleIO(ClientContext &context, GlobalTableFunctionState &gstate,
	                       LocalTableFunctionState &lstate) override;
	AsyncResult Scan(ClientContext &context, GlobalTableFunctionState &gstate, LocalTableFunctionState &lstate,
	                 DataChunk &chunk) override;
	double GetProgressInFile(ClientContext &context) override;
	InsertionOrderPreservingMap<Value> GetMetadata() const override;

	//! Release the resources the given local state holds for the batch it scanned last
	void FinishBatch(ClientContext &context, LocalTableFunctionState &local_state);
	//! Bind the wrapped table function over this file - this sets up the columns of the reader.
	//! When the schema of the scan is known upfront, the file is bound against that schema
	//! When schema_only is set the file is bound only to determine the schema of the scan and is not read with the
	//! resulting bind data. file_bind_data is what an earlier bind of this same file produced, if there was one
	void BindFunction(ClientContext &context, const TableFunctionFileReaderOptions &options,
	                  const MultiFileOptions &file_options, bool schema_only = false,
	                  optional_ptr<const FunctionData> file_bind_data = nullptr);
	//! The cardinality of this file (if the wrapped function can provide one)
	optional_idx GetCardinality() const {
		return cardinality;
	}
	//! The number of threads the wrapped function can use to scan this file
	optional_idx MaxThreads(ClientContext &context, GlobalTableFunctionState &gstate);
	//! Collect what the wrapped function counted for this file since it was last asked into the state of the scan
	void CollectMetrics(ClientContext &context, GlobalTableFunctionState &gstate);

public:
	//! The wrapped single-file table function
	TableFunction function;
	//! The bind data of the wrapped function for this file
	shared_ptr<FunctionData> bind_data;
	//! The names/types the wrapped function bound to for this file
	vector<Identifier> names;
	vector<LogicalType> types;
	//! The cardinality estimate of the wrapped function for this file (if it has one)
	optional_idx cardinality;
	//! The virtual columns that are read, as a map of the index they are projected in to their virtual column id
	unordered_map<column_t, column_t> virtual_columns;
	//! The key-value metadata the wrapped function reported for this file
	InsertionOrderPreservingMap<Value> metadata;
	//! The operator this file is scanned for, and the number of files that scan reads
	optional_ptr<const PhysicalOperator> scan_op;
	idx_t scan_file_count = 1;

public:
	//! The state of the wrapped function for this file, if it has been initialized
	optional_ptr<GlobalTableFunctionState> GetFunctionState() {
		return global_state.get();
	}
	//! The wrapped single-file function
	const TableFunction &GetFunction() const {
		return function;
	}

private:
	//! Take the operator and file count of the scan this file is read for from its state
	void SetScanState(GlobalTableFunctionState &gstate);
	//! The input the wrapped function is initialized with - "file_input" receives what the scan adds to it
	TableFunctionInitInput GetInitInput(TableFunctionFileInitInput &file_input) const;
	//! Initialize the global state of the wrapped function (if it has not been initialized yet)
	void InitializeFunctionState(ClientContext &context);

private:
	//! The named parameters that are passed on to the wrapped function
	named_argument_map_t named_parameters;
	//! How the wrapped function takes part in the scan
	TableFunctionMultiFileSettings settings;
	//! Initializes the global state once per file reader
	OnceFlag initialize_once;
	//! The global state of the wrapped function - shared by all threads scanning this file
	unique_ptr<GlobalTableFunctionState> global_state;
	//! The number of row groups to scan of this file that has been collected into the state of the scan
	idx_t collected_file_total = 0;
	//! Set when the wrapped function has no local state - only a single thread can scan the file in that case
	atomic<bool> file_is_assigned;
	//! Set when the wrapped function has emitted its last chunk for this file
	atomic<bool> exhausted;
};

//! A MultiFileReaderInterface that turns a table function reading a single file into a multi-file table function.
//! The wrapped function must accept the file as its only positional parameter: as ANY, to receive the file together
//! with the options to open it with (see TableFunctionFileBindInput::GetFile), or as a VARCHAR path - all of its named
//! parameters are forwarded to it, and all multi-file options (union_by_name, hive partitioning, filename, ...) are
//! provided by the multi-file framework on top of it.
class TableFunctionMultiFileWrapper : public MultiFileReaderInterface {
public:
	TableFunctionMultiFileWrapper(TableFunction function, TableFunctionMultiFileSettings settings);

public:
	//! Wrap a single-file table function into a multi-file table function.
	//! The wrapped function and the settings are kept in the result's "function_info", where the default bind reads
	//! them. A format whose function is bound through a TableFunction other than this one - because a caller copies
	//! it and puts info of its own in that slot, as DuckLake, Iceberg and Delta do with the parquet scan - passes a
	//! "bind" of its own here instead, built on MultiFileBindWith. No info is then stored, leaving the slot free.
	static TableFunction CreateFunction(TableFunction single_file_function, Identifier name,
	                                    TableFunctionMultiFileSettings settings = TableFunctionMultiFileSettings(),
	                                    table_function_bind_t bind = nullptr);
	//! Wrap a single-file table function into a multi-file table function set (VARCHAR and LIST(VARCHAR) variants)
	static TableFunctionSet
	CreateFunctionSet(TableFunction single_file_function, Identifier name,
	                  TableFunctionMultiFileSettings settings = TableFunctionMultiFileSettings(),
	                  table_function_bind_t bind = nullptr);

	//! Bind a multi-file scan over the given single-file function. Use this to build a "bind" of your own when the
	//! function may be bound through a TableFunction that is not the one this wrapper created - the wrapped
	//! function is then taken from here rather than from the function being bound
	static unique_ptr<FunctionData> MultiFileBindWith(ClientContext &context, TableFunctionBindInput &input,
	                                                  vector<LogicalType> &return_types, vector<Identifier> &names,
	                                                  TableFunction single_file_function,
	                                                  TableFunctionMultiFileSettings settings);

	//! Binds a COPY ... FROM over the wrapped function - assign this to CopyFunction::copy_from_bind, together with
	//! the wrapped multi-file function as CopyFunction::copy_from_function
	static unique_ptr<FunctionData> MultiFileBindCopy(ClientContext &context, CopyFromFunctionBindInput &input,
	                                                  vector<Identifier> &expected_names,
	                                                  vector<LogicalType> &expected_types);

	//! Binds a COPY ... FROM over the given single-file function. Use this to build a "copy_from_bind" of your own
	//! when the multi-file function carries no info of ours - the counterpart of MultiFileBindWith
	static unique_ptr<FunctionData> MultiFileBindCopyWith(ClientContext &context, CopyFromFunctionBindInput &input,
	                                                      vector<Identifier> &expected_names,
	                                                      vector<LogicalType> &expected_types,
	                                                      TableFunction single_file_function,
	                                                      TableFunctionMultiFileSettings settings);

	//! The row groups of a scan that reads a single file, as the wrapped function describes them - the files of a
	//! scan over several files have not been opened at this point, so their row groups are unknown
	static vector<PartitionStatistics> GetPartitionStats(ClientContext &context, GetPartitionStatsInput &input);

	//! Only present to satisfy MultiFileFunction - the wrapper builds its interface from the function info instead
	static unique_ptr<MultiFileReaderInterface> CreateInterface(ClientContext &context);

public:
	void InitializeFileOptions(MultiFileOptions &file_options) override;
	bool ReportsBytesScanned() const override {
		return settings.reports_bytes_scanned;
	}
	unique_ptr<BaseFileReaderOptions> InitializeOptions(ClientContext &context,
	                                                    optional_ptr<TableFunctionInfo> info) override;
	bool ParseCopyOption(ClientContext &context, const Identifier &key, const vector<Value> &values,
	                     BaseFileReaderOptions &options, vector<Identifier> &expected_names,
	                     vector<LogicalType> &expected_types) override;
	bool ParseOption(ClientContext &context, const Identifier &key, const Value &val, MultiFileOptions &file_options,
	                 BaseFileReaderOptions &options) override;
	void FinalizeCopyBind(ClientContext &context, BaseFileReaderOptions &options,
	                      const vector<Identifier> &expected_names, const vector<LogicalType> &expected_types) override;
	unique_ptr<TableFunctionData> InitializeBindData(MultiFileBindData &multi_file_data,
	                                                 unique_ptr<BaseFileReaderOptions> options) override;
	optional_idx MaxThreads(ClientContext &context, const MultiFileBindData &bind_data,
	                        const MultiFileGlobalState &global_state, FileExpandResult expand_result) override;
	void BindReader(ClientContext &context, vector<LogicalType> &return_types, vector<Identifier> &names,
	                MultiFileBindData &bind_data) override;
	void CombineSchemas(ClientContext &context, const vector<shared_ptr<BaseUnionData>> &union_data, bool union_by_name,
	                    vector<LogicalType> &return_types, vector<Identifier> &names) override;
	void FinalizeBindData(MultiFileBindData &multi_file_data) override;
	void GetVirtualColumns(ClientContext &context, MultiFileBindData &bind_data, virtual_column_map_t &result) override;
	unique_ptr<GlobalTableFunctionState> InitializeGlobalState(ClientContext &context, MultiFileBindData &bind_data,
	                                                           MultiFileGlobalState &global_state) override;
	unique_ptr<LocalTableFunctionState> InitializeLocalState(ClientContext &context,
	                                                         GlobalTableFunctionState &global_state) override;
	void FinishReading(ClientContext &context, GlobalTableFunctionState &global_state,
	                   LocalTableFunctionState &local_state) override;
	bool SupportsReadAhead(const MultiFileBindData &bind_data) const override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        BaseUnionData &union_data, const MultiFileBindData &bind_data) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, GlobalTableFunctionState &gstate,
	                                        const OpenFileInfo &file, idx_t file_idx,
	                                        const MultiFileBindData &bind_data) override;
	shared_ptr<BaseFileReader> CreateReader(ClientContext &context, const OpenFileInfo &file,
	                                        BaseFileReaderOptions &options,
	                                        const MultiFileOptions &file_options) override;
	unique_ptr<NodeStatistics> GetCardinality(ClientContext &context, const MultiFileBindData &bind_data,
	                                          idx_t file_count) override;
	unique_ptr<MultiFileReaderInterface> Copy() override;
	FileGlobInput GetGlobInput() override;

private:
	//! Parse a named parameter of the wrapped function - returns false if the function has no such parameter
	bool ParseNamedParameter(const Identifier &key, const Value &val, TableFunctionFileReaderOptions &options) const;
	//! The name and type of the wrapped function's option of that name or alias - a keyword-only parameter or an
	//! option its "**kwargs" declares - or empty if it declares none
	optional<pair<Identifier, LogicalType>> GetDeclaredOption(const Identifier &key) const;
	//! Release the per-file bind data that is only needed while combining schemas
	static void ReleaseBindData(const vector<shared_ptr<BaseUnionData>> &union_data);

public:
	//! The single-file table function that is wrapped
	TableFunction function;
	//! How the wrapped function is exposed as a multi-file function
	TableFunctionMultiFileSettings settings;
	//! The schema obtained by combining the schemas of the files, and the bind data describing it (if any).
	//! "schema_combined" distinguishes "no combining happened" from "combining produced an empty schema"
	bool schema_combined = false;
	vector<Identifier> combined_names;
	vector<LogicalType> combined_types;
	shared_ptr<FunctionData> combined_bind_data;
};

} // namespace duckdb
