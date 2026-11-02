//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/multi_file/multi_file_states.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/condition_variable.hpp"
#include "duckdb/common/multi_file/multi_file_options.hpp"
#include "duckdb/common/multi_file/base_file_reader.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/windows_undefs.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/common/table_column.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/parallel/scan_read_ahead.hpp"

namespace duckdb {
struct MultiFileReader;
struct MultiFileReaderInterface;

//! The bind data for the multi-file reader, obtained through MultiFileReader::BindReader
struct MultiFileReaderBindData {
	//! The (global) column id of the filename column (if any)
	optional_idx filename_idx;
	//! The (global) column id of the file_row_number column (if any) - it is read from the row number virtual column
	optional_idx file_row_number_idx;
	//! The set of hive partitioning indexes (if any)
	vector<HivePartitioningIndex> hive_partitioning_indexes;
	//! (optional) The schema set by the multi file reader
	vector<MultiFileColumnDefinition> schema;
	//! The method used to map local -> global columns
	MultiFileColumnMappingMode mapping = MultiFileColumnMappingMode::BY_NAME;

	DUCKDB_API void Serialize(Serializer &serializer) const;
	DUCKDB_API static MultiFileReaderBindData Deserialize(Deserializer &deserializer);
};

//! Global state for MultiFileReads
struct MultiFileReaderGlobalState {
	MultiFileReaderGlobalState(vector<LogicalType> extra_columns_p, optional_ptr<const MultiFileList> file_list_p,
	                           bool supports_local_extra_columns_p = false)
	    : extra_columns(std::move(extra_columns_p)), file_list(file_list_p),
	      supports_local_extra_columns(supports_local_extra_columns_p) {};
	virtual ~MultiFileReaderGlobalState();

	//! extra columns that will be produced during scanning
	const vector<LogicalType> extra_columns;
	// the file list driving the current scan
	const optional_ptr<const MultiFileList> file_list;
	//! Whether individual readers can add extra columns during InitializeReader
	const bool supports_local_extra_columns;

	//! Indicates that the MultiFileReader has added columns to be scanned that are not in the projection
	bool RequiresExtraColumns() const {
		return !extra_columns.empty() || supports_local_extra_columns;
	}

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
};

struct MultiFileBindData : public TableFunctionData {
	~MultiFileBindData() override;

	unique_ptr<TableFunctionData> bind_data;
	shared_ptr<MultiFileList> file_list;
	unique_ptr<MultiFileReader> multi_file_reader;
	unique_ptr<MultiFileReaderInterface> interface;
	vector<MultiFileColumnDefinition> columns;
	MultiFileReaderBindData reader_bind;
	MultiFileOptions file_options;
	vector<LogicalType> types;
	vector<Identifier> names;
	virtual_column_map_t virtual_columns;
	//! Table column names - set when using COPY tbl FROM file.parquet
	vector<string> table_columns;
	shared_ptr<BaseFileReader> initial_reader;
	// The union readers are created (when the union_by_name option is on) during binding
	vector<shared_ptr<BaseUnionData>> union_readers;

	void Initialize(shared_ptr<BaseFileReader> reader) {
		initial_reader = std::move(reader);
	}
	void Initialize(ClientContext &, BaseUnionData &union_data) {
		Initialize(std::move(union_data.reader));
	}
	bool SupportStatementCache() const override {
		return false;
	}

	unique_ptr<FunctionData> Copy() const override;

	//! Whether an output column is supplied by a filename, row-number or Hive partition role.
	DUCKDB_API bool IsGeneratedColumn(idx_t column_idx) const;
	//! Match a physical column to an output column without a generated override or conversion (including collations).
	DUCKDB_API bool MatchesPhysicalColumn(idx_t column_idx, const Identifier &physical_name,
	                                      const LogicalType &physical_type) const;
	//! Whether all physical roots have the output names and types at the same positions, without generated roles.
	DUCKDB_API bool HasIdentityMapping(const vector<MultiFileColumnDefinition> &physical_columns) const;
};

//! Per-file data for the multi file reader
struct MultiFileReaderData {
	// Create data for an unopened file
	explicit MultiFileReaderData(const OpenFileInfo &file_to_be_opened)
	    : reader(nullptr), file_state(MultiFileFileState::UNOPENED), file_mutex(make_uniq<mutex>()),
	      file_to_be_opened(file_to_be_opened) {
	}
	// Create data for an existing reader
	explicit MultiFileReaderData(shared_ptr<BaseFileReader> reader_p)
	    : reader(std::move(reader_p)), file_state(MultiFileFileState::OPEN), file_mutex(make_uniq<mutex>()) {
	}
	// Create data for an existing reader
	explicit MultiFileReaderData(shared_ptr<BaseUnionData> union_data_p) : file_mutex(make_uniq<mutex>()) {
		if (union_data_p->reader) {
			reader = std::move(union_data_p->reader);
			file_state = MultiFileFileState::OPEN;
		} else {
			union_data = std::move(union_data_p);
			file_state = MultiFileFileState::UNOPENED;
		}
	}

	//! Currently opened reader for the file
	shared_ptr<BaseFileReader> reader;
	//! The file reader after we have started all scans to the file
	weak_ptr<BaseFileReader> closed_reader;
	//! Flag to indicate the file is being opened
	MultiFileFileState file_state;
	//! Mutexes to wait for the file when it is being opened
	unique_ptr<mutex> file_mutex;
	//! Options for opening the file
	shared_ptr<BaseUnionData> union_data;
	//! The constants that should be applied at the various positions
	MultiFileConstantMap constant_map;
	//! The set of expressions that should be evaluated to obtain the final result
	vector<unique_ptr<Expression>> expressions;
	//! Extra columns required by FinalizeChunk for this file, appended after any global extra columns
	//! Is only allowed to be populated when MultiFileReaderGlobalState::supports_local_extra_columns is set
	vector<LogicalType> extra_columns;

	vector<LogicalType> GetExtraColumns(const MultiFileReaderGlobalState &global_state) const {
		auto result = global_state.extra_columns;
		result.insert(result.end(), extra_columns.begin(), extra_columns.end());
		return result;
	}

	//! (only set when file_state is UNOPENED) the file to be opened
	OpenFileInfo file_to_be_opened;
};

struct MultiFileGlobalState : public GlobalTableFunctionState {
	explicit MultiFileGlobalState(MultiFileList &file_list_p);
	explicit MultiFileGlobalState(unique_ptr<MultiFileList> owned_file_list_p);
	~MultiFileGlobalState() override;

	//! The file list to scan
	MultiFileList &file_list;
	//! The scan over the file_list
	MultiFileListScanData file_list_scan;
	//! Owned multi file list - if filters have been dynamically pushed into the reader
	unique_ptr<MultiFileList> owned_file_list;
	//! Reader state
	unique_ptr<MultiFileReaderGlobalState> multi_file_reader_state;
	//! Lock
	mutable mutex lock;
	//! Signal to other threads that a file failed to open, letting every thread abort.
	//! Atomic because a cancelled file open settles it while the scheduling thread may hold the lock.
	atomic<bool> error_opening_file {false};
	//! Signalled when an async file open settles, waking the threads waiting for the front file
	condition_variable async_open_settled;

	//! Index of file currently up for scanning
	atomic<idx_t> file_index;
	//! Number of files that were actually opened by the scan
	atomic<idx_t> files_opened = 0;
	//! Index of the lowest file we know we have completely read
	mutable idx_t completed_file_index = 0;
	//! The current set of readers
	vector<unique_ptr<MultiFileReaderData>> readers;

	idx_t batch_index = 0;

	idx_t max_threads = 1;
	vector<idx_t> projection_ids;
	vector<LogicalType> scanned_types;
	vector<ColumnIndex> column_indexes;
	optional_ptr<TableFilterSet> filters;

	unique_ptr<GlobalTableFunctionState> global_state;

	unique_ptr<ScanReadAhead> read_ahead;
	ScanStatePool<LocalTableFunctionState> state_pool;

	optional_ptr<const PhysicalOperator> op;

	idx_t MaxThreads() const override {
		return max_threads;
	}

	bool CanRemoveColumns() const {
		return !projection_ids.empty();
	}
};

//! Lifecycle of the job a scanning thread currently holds
enum class MultiFileJobState : uint8_t {
	NONE,     //! no job claimed
	SCHEDULE, //! I/O still needs scheduling
	WAIT_IO,  //! parked until the job's scheduled I/O completes
	DECODE    //! job ready to decode
};

//! Outcome of decoding the current scan job
enum class MultiFileDecodeResult : uint8_t {
	CONTINUE,         //! keep looping
	RETURN_TO_CALLER, //! return from the scan (a chunk was emitted, or the operator parked on async I/O)
	JOB_FINISHED      //! job is done
};

//! Outcome of claiming the next scan job from the current file
enum class MultiFileClaimResult : uint8_t {
	CLAIMED,   //! the current file's next unit of work (e.g. a parquet row group)
	EXHAUSTED, //! the scan is exhausted
	WAIT_OPEN  //! the current file is still being opened
};

//! A single, independently schedulable unit of scan work (e.g. one Parquet row group of one file)
struct MultiFileScanJobState {
	//! The reader producing this job
	shared_ptr<BaseFileReader> reader;
	//! Per-file data for the reader
	optional_ptr<MultiFileReaderData> reader_data;
	//! Index of the file this job belongs to
	idx_t file_index = DConstants::INVALID_INDEX;
	//! The scan state the job's I/O and decoding operate on, declared last so it is destroyed before the reader
	unique_ptr<LocalTableFunctionState> scan_state;
};

struct MultiFileLocalState : public LocalTableFunctionState {
public:
	explicit MultiFileLocalState(ClientContext &context) : executor(context) {
	}

public:
	//! The job currently being scanned by this thread
	unique_ptr<ScanReadAheadJobWrapper<MultiFileScanJobState>> job;
	//! Job's state
	MultiFileJobState job_state = MultiFileJobState::NONE;
	//! The chunk written to by the reader, handed to FinalizeChunk to transform to the global schema
	DataChunk scan_chunk;
	//! Set when the previous Scan() returned BLOCKED, so the next Scan() preserves the partial chunk
	bool resuming_blocked_scan = false;
	//! The file index that scan_chunk is initialized for.
	idx_t scan_chunk_file_index = DConstants::INVALID_INDEX;
	//! The executor to transform scan_chunk into the final result with FinalizeChunk
	ExpressionExecutor executor;
	//! Number of rows scanned by this thread (for profiling)
	idx_t rows_scanned = 0;
	//! FinalizeScan may have no job, here's a special batch index for it
	optional_idx finalize_batch_index;
};

} // namespace duckdb
