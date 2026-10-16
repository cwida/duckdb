#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/iterator.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/parallel/async_result.hpp"
#include "duckdb/parallel/scan_read_ahead.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/storage/storage_index.hpp"
#include "duckdb/storage/table_io_manager.hpp"
#include "duckdb/storage/table/data_table_info.hpp"
#include "duckdb/storage/table/scan_state.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/main/profiler/profiling_node.hpp"

namespace duckdb {

//! A pre-claimed scan assignment whose I/O is scheduled on the async pool ahead of decoding
struct TableScanJobState {
	//! Rows of the assignment, counted as scanned by the progress once a thread decodes the job
	idx_t rows = 0;
	//! The scan state the job's I/O and decoding operate on
	unique_ptr<TableScanState> scan_state;
};
using TableScanJob = ScanReadAheadJobWrapper<TableScanJobState>;

//! Create the read-ahead driver, returns null when read-ahead is disabled or prefetching is not beneficial
static unique_ptr<ScanReadAhead> CreateTableScanReadAhead(ClientContext &context, DataTable &storage) {
	if (!storage.GetTableIOManager().GetBlockManagerForRowData().Prefetch()) {
		return nullptr;
	}
	return ScanReadAhead::Create(context);
}

//! Tracks how many rows of a scan assignment have been consumed, for reporting progress
struct ScanAssignmentProgress {
	//! Rows of the current assignment
	idx_t assignment_rows = 0;
	//! Rows of the current assignment that were counted as scanned
	idx_t reported_rows = 0;

	void Start(idx_t rows) {
		assignment_rows = rows;
		reported_rows = 0;
	}
	//! Returns the rows consumed since the last update
	idx_t Update(const TableScanState &scan_state) {
		idx_t remaining = 0;
		if (scan_state.table_state.GetRowGroup()) {
			remaining = scan_state.table_state.RemainingAssignmentRows();
		} else if (scan_state.local_state.GetRowGroup()) {
			remaining = scan_state.local_state.RemainingAssignmentRows();
		}
		auto consumed = assignment_rows - MinValue<idx_t>(remaining, assignment_rows);
		return Report(consumed);
	}
	//! Returns the rows of the assignment that were not yet counted
	idx_t Finish() {
		return Report(assignment_rows);
	}

private:
	idx_t Report(idx_t consumed) {
		if (consumed <= reported_rows) {
			return 0;
		}
		auto delta = consumed - reported_rows;
		reported_rows = consumed;
		return delta;
	}
};

//! The scan metrics already handed to the profiler, so that each metrics call only reports what is new
struct TableScanMetricsLocalState : public LocalTableFunctionState {
	idx_t reported_rows_scanned = 0;
	idx_t reported_row_groups_scanned = 0;
};

struct TableScanLocalState : public TableScanMetricsLocalState {
	//! The current position in the scan.
	TableScanState scan_state;
	//! The DataChunk containing all read columns.
	//! This includes filter columns, which are immediately removed.
	DataChunk all_columns;
	//! The read-ahead job currently being decoded
	unique_ptr<TableScanJob> job;
	//! Rows scanned by finished read-ahead jobs, folded in when their scan state is recycled
	idx_t job_rows_scanned = 0;
	//! Consumed rows of the assignment (or read-ahead job) currently being scanned
	ScanAssignmentProgress assignment_progress;

	idx_t row_groups_scanned = 0;
};

struct IndexScanLocalState : public TableScanMetricsLocalState {
	//! The batch index, which determines the offset in the row ID vector.
	idx_t batch_index;
	//! The DataChunk containing all read columns.
	//! This includes filter columns, which are immediately removed.
	DataChunk all_columns;
	//! The row fetch state.
	ColumnFetchState fetch_state;
	//! The current position in the local storage scan.
	TableScanState scan_state;
	//! The column IDs of the local storage scan.
	vector<StorageIndex> column_ids;
	bool in_charge_of_final_stretch {false};
	idx_t rows_scanned = 0;
};

class TableScanGlobalState : public GlobalTableFunctionState {
public:
	TableScanGlobalState(ClientContext &context, const FunctionData *bind_data_p) {
		D_ASSERT(bind_data_p);
		auto &bind_data = bind_data_p->Cast<TableScanBindData>();
		auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
		auto &storage = duck_table.GetStorage();
		max_threads = storage.MaxThreads(context);
		total_row_groups_to_scan = storage.GetRowGroupCountWithLocalStorage(context);
	}

	//! The maximum number of threads for this table scan.
	idx_t max_threads;
	//! The total number of row groups available to this table scan.
	idx_t total_row_groups_to_scan;
	//! The projected columns of this table scan.
	vector<idx_t> projection_ids;
	//! The types of all scanned columns.
	vector<LogicalType> scanned_types;

public:
	virtual unique_ptr<LocalTableFunctionState> InitLocalState(ExecutionContext &context,
	                                                           TableFunctionInitInput &input) = 0;
	virtual void TableScanFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) = 0;
	virtual double TableScanProgress(ClientContext &context, const FunctionData *bind_data_p) const = 0;
	virtual OperatorPartitionData TableScanGetPartitionData(ClientContext &context,
	                                                        TableFunctionGetPartitionInput &input) = 0;
	virtual idx_t TableScanRowsScanned(LocalTableFunctionState &state) = 0;
	virtual idx_t TableScanRowGroupsScanned(LocalTableFunctionState &state) = 0;

	idx_t MaxThreads() const override {
		return max_threads;
	}
	bool CanRemoveFilterColumns() const {
		return !projection_ids.empty();
	}
};

class DuckIndexScanState : public TableScanGlobalState {
public:
	DuckIndexScanState(ClientContext &context, const FunctionData *bind_data_p, unsafe_vector<row_t> &&row_ids_p)
	    : TableScanGlobalState(context, bind_data_p), next_batch_index(0), row_ids(std::move(row_ids_p)),
	      finished_first_phase(row_ids.empty()), started_last_phase(false) {
		// Tasks claim STANDARD_VECTOR_SIZE row IDs at a time, and one more task can scan the transaction-local rows.
		auto &duck_table = bind_data_p->Cast<TableScanBindData>().table.Cast<DuckTableEntry>();
		auto local_table = LocalStorage::Get(context, duck_table.catalog).GetStorage(duck_table.GetStorage());
		idx_t useful_threads = (row_ids.size() + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
		if (local_table && local_table->GetCollection().GetTotalRows() > 0) {
			useful_threads++;
		}
		max_threads = MinValue<idx_t>(max_threads, MaxValue<idx_t>(useful_threads, 1));
	}

	//! The batch index of the next Sink.
	//! Also determines the offset of the next chunk. I.e., offset = next_batch_index * STANDARD_VECTOR_SIZE.
	atomic<idx_t> next_batch_index;
	//! Finalized before construction and only read by Fetch tasks.
	unsafe_vector<row_t> row_ids;
	//! The column IDs of the to-be-scanned columns.
	vector<StorageIndex> column_ids;
	//! True, if no more row IDs must be scanned.
	bool finished_first_phase;
	bool started_last_phase;
	//! Synchronize changes to the global index scan state.
	mutex index_scan_lock;
	//! Keep ART rowids and row-group trees paired while rowid-shifting index vacuum can run.
	unique_ptr<StorageLockKey> vacuum_lock;

public:
	unique_ptr<LocalTableFunctionState> InitLocalState(ExecutionContext &context,
	                                                   TableFunctionInitInput &input) override {
		auto l_state = make_uniq<IndexScanLocalState>();
		if (input.CanRemoveFilterColumns()) {
			l_state->all_columns.Initialize(context.client, scanned_types);
		}
		l_state->scan_state.options.force_fetch_row = Settings::Get<DebugForceFetchRowSetting>(context.client);

		// Initialize the local storage scan.
		auto &bind_data = input.bind_data->Cast<TableScanBindData>();
		auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
		auto &storage = duck_table.GetStorage();
		auto &local_storage = LocalStorage::Get(context.client, duck_table.catalog);

		for (const auto &col_idx : input.column_indexes) {
			l_state->column_ids.push_back(bind_data.table.GetStorageIndex(col_idx));
		}
		l_state->scan_state.Initialize(l_state->column_ids, context.client, input.filters.get());
		local_storage.InitializeScan(storage, l_state->scan_state.local_state, input.filters);
		return std::move(l_state);
	}

	void TableScanFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) override {
		auto &bind_data = data_p.bind_data->Cast<TableScanBindData>();
		auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
		auto &tx = DuckTransaction::Get(context, duck_table.catalog);
		auto &storage = duck_table.GetStorage();
		auto &l_state = data_p.local_state->Cast<IndexScanLocalState>();

		enum class ExecutionPhase { NONE = 0, STORAGE = 1, LOCAL_STORAGE = 2 };

		// We might need to loop back, so while (true)
		while (true) {
			idx_t scan_count = 0;
			idx_t offset = 0;

			// Phase selection
			auto phase_to_be_performed = ExecutionPhase::NONE;
			{
				// Synchronize changes to the shared global state.
				lock_guard<mutex> l(index_scan_lock);
				if (!finished_first_phase) {
					l_state.batch_index = next_batch_index;
					next_batch_index++;

					offset = l_state.batch_index * STANDARD_VECTOR_SIZE;
					auto remaining = row_ids.size() - offset;
					scan_count = remaining <= STANDARD_VECTOR_SIZE ? remaining : STANDARD_VECTOR_SIZE;
					finished_first_phase = remaining <= STANDARD_VECTOR_SIZE ? true : false;
					phase_to_be_performed = ExecutionPhase::STORAGE;
				} else if (!started_last_phase) {
					// First thread to get last phase, great, set l_state's in_charge_of_final_stretch, so same thread
					// will be on again
					started_last_phase = true;
					l_state.in_charge_of_final_stretch = true;
					// The local rows form their own batch after all row ID batches.
					l_state.batch_index = next_batch_index;
					phase_to_be_performed = ExecutionPhase::LOCAL_STORAGE;
				} else if (l_state.in_charge_of_final_stretch) {
					phase_to_be_performed = ExecutionPhase::LOCAL_STORAGE;
				}
			}

			switch (phase_to_be_performed) {
			case ExecutionPhase::NONE: {
				// No work to be picked up
				return;
			}
			case ExecutionPhase::STORAGE: {
				// Scan (in parallel) storage
				auto row_id_data = reinterpret_cast<data_ptr_t>(row_ids.data() + offset);
				Vector local_vector(LogicalType::ROW_TYPE, row_id_data, scan_count);

				if (CanRemoveFilterColumns()) {
					l_state.all_columns.Reset();
					storage.Fetch(tx, l_state.all_columns, column_ids, local_vector, scan_count, l_state.fetch_state);
					output.ReferenceColumns(l_state.all_columns, projection_ids);
				} else {
					storage.Fetch(tx, output, column_ids, local_vector, scan_count, l_state.fetch_state);
				}

				l_state.rows_scanned += scan_count;

				if (output.size() == 0) {
					if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
						// We can avoid looping, and just return as appropriate
						data_p.async_result = AsyncResultType::HAVE_MORE_OUTPUT;
						return;
					}

					// output is empty, loop back, since there might be results to be picked up from LOCAL_STORAGE phase
					continue;
				}
				return;
			}
			case ExecutionPhase::LOCAL_STORAGE: {
				// Scan (sequentially, always same logical thread) local_storage
				auto &local_storage = LocalStorage::Get(tx);
				{
					if (CanRemoveFilterColumns()) {
						l_state.all_columns.Reset();
						local_storage.Scan(l_state.scan_state.local_state, column_ids, l_state.all_columns);
						output.ReferenceColumns(l_state.all_columns, projection_ids);
					} else {
						local_storage.Scan(l_state.scan_state.local_state, column_ids, output);
					}
					l_state.rows_scanned += output.size();
				}
				return;
			}
			}
		}
	}

	double TableScanProgress(ClientContext &context, const FunctionData *bind_data_p) const override {
		if (row_ids.empty()) {
			return 100;
		}
		auto scanned_rows = next_batch_index * STANDARD_VECTOR_SIZE;
		auto percentage = 100 * (static_cast<double>(scanned_rows) / static_cast<double>(row_ids.size()));
		return percentage > 100 ? 100 : percentage;
	}

	OperatorPartitionData TableScanGetPartitionData(ClientContext &context,
	                                                TableFunctionGetPartitionInput &input) override {
		auto &l_state = input.local_state->Cast<IndexScanLocalState>();
		return OperatorPartitionData(l_state.batch_index);
	}

	idx_t TableScanRowsScanned(LocalTableFunctionState &state) override {
		auto &l_state = state.Cast<IndexScanLocalState>();
		return l_state.rows_scanned;
	}

	idx_t TableScanRowGroupsScanned(LocalTableFunctionState &) override {
		return 0;
	}
};

class DuckTableScanState : public TableScanGlobalState {
public:
	DuckTableScanState(ClientContext &context, const FunctionData *bind_data_p)
	    : TableScanGlobalState(context, bind_data_p), bind_data(bind_data_p->Cast<TableScanBindData>()),
	      duck_table(bind_data.table.Cast<DuckTableEntry>()), tx(DuckTransaction::Get(context, duck_table.catalog)),
	      storage(duck_table.GetStorage()), total_rows(storage.GetTotalRows()) {
	}

public:
	ParallelTableScanState state;
	unique_ptr<ScanReadAhead> read_ahead;

private:
	const TableScanBindData &bind_data;
	DuckTableEntry &duck_table;
	DuckTransaction &tx;
	DataTable &storage;
	const idx_t total_rows;
	//! Scan initialization info retained for creating scan states
	vector<StorageIndex> storage_ids;
	optional_ptr<TableFilterSet> filters;
	optional_ptr<SampleOptions> sample_options;
	//! Guards assignment claims, keeping job batch indexes dense in claim order
	mutex read_ahead_lock;
	//! Batch index assigned to the next produced job
	idx_t next_job_index = 0;
	ScanStatePool<TableScanState> state_pool;
	//! Rows of claimed assignments that have been consumed by a scan
	atomic<idx_t> scanned_rows {0};

public:
	//! Retains the scan initialization info shared by all scan states of this scan
	void InitializeScanInfo(TableFunctionInitInput &input) {
		for (auto &col : input.column_indexes) {
			storage_ids.push_back(bind_data.table.GetStorageIndex(col));
		}
		filters = input.filters;
		sample_options = input.sample_options;
	}

	//! Shared scan-state setup for this table scan
	void InitializeScanState(ClientContext &context, TableScanState &scan_state) const {
		if (bind_data.order_options) {
			scan_state.table_state.reorderer =
			    make_uniq<RowGroupReorderer>(*bind_data.order_options, TransactionData(tx));
			scan_state.local_state.reorderer =
			    make_uniq<RowGroupReorderer>(*bind_data.order_options, TransactionData(tx));
		}
		scan_state.Initialize(storage_ids, context, filters, sample_options, total_rows);
		scan_state.options.force_fetch_row = Settings::Get<DebugForceFetchRowSetting>(context);
	}

	unique_ptr<LocalTableFunctionState> InitLocalState(ExecutionContext &context,
	                                                   TableFunctionInitInput &input) override {
		auto l_state = make_uniq<TableScanLocalState>();
		InitializeScanState(context.client, l_state->scan_state);

		if (!read_ahead) {
			ClaimAssignment(context.client, *l_state);
		}
		if (input.CanRemoveFilterColumns()) {
			l_state->all_columns.Initialize(context.client, scanned_types);
		}
		return std::move(l_state);
	}

	//! Claims the next assignment into the thread's own scan state, returns false when none are left
	bool ClaimAssignment(ClientContext &context, TableScanLocalState &l_state) {
		scanned_rows.fetch_add(l_state.assignment_progress.Finish(), std::memory_order_relaxed);
		auto rows = storage.NextParallelScan(context, state, l_state.scan_state);
		if (!rows.IsValid()) {
			return false;
		}
		l_state.assignment_progress.Start(rows.GetIndex());
		l_state.row_groups_scanned++;
		return true;
	}

	//! Counts the rows of the current assignment that were consumed by the scan
	void UpdateScanProgress(TableScanLocalState &l_state, const TableScanState &scan_state) {
		scanned_rows.fetch_add(l_state.assignment_progress.Update(scan_state), std::memory_order_relaxed);
	}

	//! How TableScanFunc's loop proceeds after a persistent scan iteration
	enum class PersistentScanResult { YIELD, NEXT_VECTOR, EXHAUSTED };

	//! Emits a scanned chunk into the output, scanning into all_columns first when filter columns are removed
	template <class FUNC>
	void EmitChunk(TableScanLocalState &l_state, DataChunk &output, FUNC &&scan) {
		if (!CanRemoveFilterColumns()) {
			scan(output);
			return;
		}
		l_state.all_columns.Reset();
		scan(l_state.all_columns);
		output.ReferenceColumns(l_state.all_columns, projection_ids);
	}

	//! Prepares the next vector, schedules its I/O and decodes it, draining local storage when exhausted
	PersistentScanResult ScanPersistentStorage(ClientContext &context, TableFunctionInput &data_p,
	                                           TableScanLocalState &l_state, TableScanState &scan_state,
	                                           DataChunk &output) {
		// persistent storage phase, prepare the next vector and schedule its I/O before decoding
		auto &table_state = scan_state.table_state;
		vector<unique_ptr<AsyncTask>> io_tasks;
		if (!table_state.PrepareScanIO(tx, io_tasks)) {
			// we are done, scan drains any claimed local storage rows
			EmitChunk(l_state, output, [&](DataChunk &chunk) { storage.Scan(tx, chunk, scan_state); });
			return PersistentScanResult::EXHAUSTED;
		}
		auto io_result = AsyncResult::FromTasks(std::move(io_tasks), TaskSchedulerType::ASYNC);
		// on resume the prepared vector is decoded without registering I/O again
		if (io_result.GetResultType() == AsyncResultType::BLOCKED && data_p.HandleBlocked(context, io_result)) {
			return PersistentScanResult::YIELD;
		}
		EmitChunk(l_state, output, [&](DataChunk &chunk) { table_state.ProcessPreparedScan(tx, chunk); });
		if (output.size() > 0) {
			return PersistentScanResult::YIELD;
		}
		// the prepared vector was filtered out entirely, go the next vector
		context.InterruptCheck();
		return PersistentScanResult::NEXT_VECTOR;
	}

	//! Claims the next assignment as a job and registers its I/O, returns null when there are no more assignments
	unique_ptr<TableScanJob> ProduceJob(ClientContext &context, vector<unique_ptr<AsyncTask>> &io_tasks) {
		auto job = make_uniq<TableScanJob>();
		// jobs recycle finished scan states, create a fresh one when none was available
		job->scan_state = state_pool.TryPop();
		if (!job->scan_state) {
			job->scan_state = make_uniq<TableScanState>();
			InitializeScanState(context, *job->scan_state);
		}
		{
			// only the claim and its index need the lock, the per-column setup runs outside it
			lock_guard<mutex> guard(read_ahead_lock);
			const auto rows = storage.NextParallelScan(context, state, *job->scan_state, false);
			if (!rows.IsValid()) {
				return nullptr;
			}
			job->rows = rows.GetIndex();
			job->batch_index = next_job_index++;
		}
		job->scan_state->InitializeColumnScans();
		// preparing the first vector skips the leading vectors the zonemaps or sampling reject before registering I/O
		job->scan_state->table_state.PrepareScanIO(tx, io_tasks, true);
		return job;
	}

	//! Drops a settled job's row group when a filter set since it was produced prunes it
	static void RecheckJob(TableScanLocalState &l_state) {
		if (!l_state.job->scan_state->table_state.RecheckPreparedRowGroup()) {
			// the row group is dropped without being scanned
			l_state.row_groups_scanned--;
		}
	}

	// Try to return control to the executor instead of looping back within the scan.
	static bool TryYieldControl(TableFunctionInput &data_p) {
		if (data_p.results_execution_mode != AsyncResultsExecutionMode::TASK_EXECUTOR) {
			return false;
		}
		data_p.async_result = AsyncResultType::HAVE_MORE_OUTPUT;
		return true;
	}

	//! Decodes read-ahead jobs, returns true when the caller must yield, false once every assignment is consumed
	bool ScanWithReadAhead(ClientContext &context, TableFunctionInput &data_p, TableScanLocalState &l_state,
	                       DataChunk &output) {
		while (true) {
			if (l_state.job && l_state.job->io_completion) {
				// resuming after parking, the job's I/O has completed
				read_ahead->WaitForJob(*l_state.job);
				RecheckJob(l_state);
			}
			if (!l_state.job) {
				unique_ptr<ScanReadAheadJob> claimed;
				auto acquired = read_ahead->AcquireJob(
				    context, data_p,
				    [&](vector<unique_ptr<AsyncTask>> &io_tasks) { return ProduceJob(context, io_tasks); }, claimed);
				if (acquired == ScanReadAheadAcquire::EXHAUSTED) {
					return false;
				}
				l_state.job = unique_ptr_cast<ScanReadAheadJob, TableScanJob>(std::move(claimed));
				l_state.row_groups_scanned++;
				l_state.assignment_progress.Start(l_state.job->rows);
				if (acquired == ScanReadAheadAcquire::PARKED) {
					return true;
				}
				RecheckJob(l_state);
			}
			auto &job_scan = *l_state.job->scan_state;
			// the job's I/O was registered when it was produced, so no I/O is scheduled here
			auto scan_result = ScanPersistentStorage(context, data_p, l_state, job_scan, output);
			UpdateScanProgress(l_state, job_scan);
			switch (scan_result) {
			case PersistentScanResult::YIELD:
				return true;
			case PersistentScanResult::NEXT_VECTOR:
				// the prepared vector was filtered out, we got to return control before scanning the next one
				if (TryYieldControl(data_p)) {
					return true;
				}
				continue;
			case PersistentScanResult::EXHAUSTED:
				break;
			}
			if (output.size() > 0) {
				return true;
			}
			// the job is exhausted, fold its scan counters into this thread and recycle its state
			scanned_rows.fetch_add(l_state.assignment_progress.Finish(), std::memory_order_relaxed);
			l_state.job_rows_scanned += job_scan.RowsScanned();
			job_scan.table_state.rows_scanned = 0;
			job_scan.local_state.rows_scanned = 0;
			// a pooled state outlives the pipeline, it must not keep blocks pinned that a sink may still convert
			job_scan.ReleasePins();
			state_pool.Push(std::move(l_state.job->scan_state));
			l_state.job.reset();
			if (TryYieldControl(data_p)) {
				return true;
			}
		}
	}

	void TableScanFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) override {
		auto &l_state = data_p.local_state->Cast<TableScanLocalState>();
		l_state.scan_state.options.force_fetch_row = Settings::Get<DebugForceFetchRowSetting>(context);

#ifdef DUCKDB_DEBUG_ASYNC_SINK_SOURCE
		{
			AsyncResult test_result;
			if (AsyncResult::TryGenerateTestResult(test_result) && data_p.HandleBlocked(context, test_result)) {
				return;
			}
		}
#endif

		do {
			if (bind_data.is_create_index) {
				storage.CreateIndexScan(l_state.scan_state, output);
				UpdateScanProgress(l_state, l_state.scan_state);
			} else if (read_ahead) {
				if (!ScanWithReadAhead(context, data_p, l_state, output) &&
				    data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
					// every assignment went to a read-ahead job, the thread's own scan state never claims one
					data_p.async_result = AsyncResultType::FINISHED;
				}
				return;
			} else {
				auto scan_result = ScanPersistentStorage(context, data_p, l_state, l_state.scan_state, output);
				UpdateScanProgress(l_state, l_state.scan_state);
				switch (scan_result) {
				case PersistentScanResult::YIELD:
					return;
				case PersistentScanResult::NEXT_VECTOR:
					continue;
				case PersistentScanResult::EXHAUSTED:
					break;
				}
			}
			if (output.size() > 0) {
				return;
			}

			const bool claimed = ClaimAssignment(context, l_state);
			if (data_p.results_execution_mode == AsyncResultsExecutionMode::TASK_EXECUTOR) {
				// We can avoid looping, and just return as appropriate
				data_p.async_result = claimed ? AsyncResultType::HAVE_MORE_OUTPUT : AsyncResultType::FINISHED;
				return;
			}
			if (!claimed) {
				return;
			}

			// Before looping back, check if we are interrupted
			context.InterruptCheck();
		} while (true);
	}

	double TableScanProgress(ClientContext &context, const FunctionData *bind_data_p) const override {
		// The table is empty or smaller than the standard vector size.
		if (total_rows == 0) {
			return 100;
		}

		// rows count once they are consumed by a scan, or when their assignment is skipped entirely
		idx_t done_rows = scanned_rows.load();
		done_rows += state.scan_state.skipped_rows.load();
		done_rows += state.local_state.skipped_rows.load();
		auto percentage = 100 * (static_cast<double>(done_rows) / static_cast<double>(total_rows));
		if (percentage > 100) {
			// If the last chunk has fewer elements than STANDARD_VECTOR_SIZE, and if our percentage is over 100,
			// then we finished this table.
			return 100;
		}
		return percentage;
	}

	OperatorPartitionData TableScanGetPartitionData(ClientContext &context,
	                                                TableFunctionGetPartitionInput &input) override {
		auto &l_state = input.local_state->Cast<TableScanLocalState>();
		// with read-ahead the assignment being decoded lives in the claimed job's scan state
		auto &scan_state = l_state.job ? *l_state.job->scan_state : l_state.scan_state;
		if (scan_state.table_state.GetRowGroup()) {
			return OperatorPartitionData(scan_state.table_state.batch_index);
		}
		if (scan_state.local_state.GetRowGroup()) {
			return OperatorPartitionData(scan_state.table_state.batch_index + scan_state.local_state.batch_index);
		}
		return OperatorPartitionData(0);
	}

	idx_t TableScanRowsScanned(LocalTableFunctionState &state) override {
		const auto &l_state = state.Cast<TableScanLocalState>();
		auto result = l_state.scan_state.RowsScanned() + l_state.job_rows_scanned;
		if (l_state.job) {
			result += l_state.job->scan_state->RowsScanned();
		}
		return result;
	}

	idx_t TableScanRowGroupsScanned(LocalTableFunctionState &state) override {
		auto &l_state = state.Cast<TableScanLocalState>();
		return l_state.row_groups_scanned;
	}
};

static unique_ptr<LocalTableFunctionState> TableScanInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                                              GlobalTableFunctionState *g_state) {
	auto &cast_g_state = g_state->Cast<TableScanGlobalState>();
	return cast_g_state.InitLocalState(context, input);
}

unique_ptr<GlobalTableFunctionState> DuckTableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input,
                                                             DataTable &storage, const TableScanBindData &bind_data) {
	auto g_state = make_uniq<DuckTableScanState>(context, input.bind_data.get());
	if (bind_data.order_options) {
		auto transaction = TransactionData(DuckTransaction::Get(context, storage.GetAttached()));
		g_state->state.scan_state.reorderer = make_uniq<RowGroupReorderer>(*bind_data.order_options, transaction);
		g_state->state.local_state.reorderer = make_uniq<RowGroupReorderer>(*bind_data.order_options, transaction);
	}
	if (bind_data.partitions_to_scan) {
		g_state->state.scan_state.partitions_to_scan = bind_data.partitions_to_scan.get();
	}

	// Check if row_number column is requested and initialize row_number_base
	for (idx_t i = 0; i < input.column_ids.size(); i++) {
		if (input.column_ids[i] == COLUMN_IDENTIFIER_ROW_NUMBER) {
			g_state->state.scan_state.row_number_base = 0;
			break;
		}
	}
	storage.InitializeParallelScan(context, g_state->state, input.column_indexes);
	g_state->InitializeScanInfo(input);
	const bool repeatable_percentage_sample =
	    input.sample_options && input.sample_options->repeatable && input.sample_options->is_percentage;
	if (!bind_data.is_create_index && !repeatable_percentage_sample) {
		g_state->read_ahead = CreateTableScanReadAhead(context, storage);
	}
	if (!input.CanRemoveFilterColumns()) {
		return std::move(g_state);
	}

	g_state->projection_ids = input.projection_ids;
	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	const auto &columns = duck_table.GetColumns();
	for (const auto &col_idx : input.column_indexes) {
		if (col_idx.IsRowIdColumn() || col_idx.IsRowNumberColumn()) {
			g_state->scanned_types.emplace_back(LogicalType::ROW_TYPE);
		} else if (col_idx.HasType()) {
			g_state->scanned_types.push_back(col_idx.GetScanType());
		} else {
			g_state->scanned_types.push_back(columns.GetColumn(col_idx.ToLogical()).Type());
		}
	}
	return std::move(g_state);
}

unique_ptr<GlobalTableFunctionState> DuckIndexScanInitGlobal(ClientContext &context, TableFunctionInitInput &input,
                                                             const TableScanBindData &bind_data,
                                                             unsafe_vector<row_t> &&row_ids,
                                                             unique_ptr<StorageLockKey> vacuum_lock) {
	auto g_state = make_uniq<DuckIndexScanState>(context, input.bind_data.get(), std::move(row_ids));
	g_state->vacuum_lock = std::move(vacuum_lock);

	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	if (input.CanRemoveFilterColumns()) {
		g_state->projection_ids = input.projection_ids;
	}

	const auto &columns = duck_table.GetColumns();
	for (const auto &col_idx : input.column_indexes) {
		g_state->column_ids.push_back(bind_data.table.GetStorageIndex(col_idx));
		if (col_idx.IsRowIdColumn()) {
			g_state->scanned_types.emplace_back(LogicalType::ROW_TYPE);
		} else if (col_idx.HasType()) {
			g_state->scanned_types.emplace_back(col_idx.GetScanType());
		} else {
			g_state->scanned_types.push_back(columns.GetColumn(col_idx.ToLogical()).Type());
		}
	}

	// Const-cast to indicate an index scan.
	// We need this information in the bind data so that we can access it during ANALYZE.
	auto &no_const_bind_data = bind_data.CastNoConst<TableScanBindData>();
	no_const_bind_data.is_index_scan = true;

	return std::move(g_state);
}

struct ComparisonCondition {
	ExpressionType type;
	Value constant;
};

static bool CollectValuesAndComparisonsFromExpression(const Expression &expr, value_set_t &in_values,
                                                      vector<ComparisonCondition> &comparisons) {
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR &&
	    expr.GetExpressionType() == ExpressionType::COMPARE_IN) {
		auto &op = expr.Cast<BoundOperatorExpression>();
		if (op.GetChildren().empty() || op.GetChildren()[0]->GetExpressionClass() != ExpressionClass::BOUND_REF) {
			return false;
		}
		for (idx_t i = 1; i < op.GetChildren().size(); i++) {
			if (op.GetChildren()[i]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				return false;
			}
			auto &value = op.GetChildren()[i]->Cast<BoundConstantExpression>().GetValue();
			if (!value.IsNull()) {
				in_values.insert(value);
			}
		}
		return true;
	}
	if (BoundComparisonExpression::IsComparison(expr)) {
		auto &comp = expr.Cast<BoundFunctionExpression>();
		Value val;
		auto &left = BoundComparisonExpression::Left(comp);
		auto &right = BoundComparisonExpression::Right(comp);
		bool left_is_ref = left.GetExpressionClass() == ExpressionClass::BOUND_REF;
		bool right_is_ref = right.GetExpressionClass() == ExpressionClass::BOUND_REF;
		if (right.GetExpressionType() == ExpressionType::VALUE_CONSTANT && left_is_ref) {
			val = right.Cast<BoundConstantExpression>().GetValue();
		} else if (left.GetExpressionType() == ExpressionType::VALUE_CONSTANT && right_is_ref) {
			val = left.Cast<BoundConstantExpression>().GetValue();
		} else {
			return false;
		}
		if (val.IsNull()) {
			return false;
		}
		if (comp.GetExpressionType() == ExpressionType::COMPARE_EQUAL) {
			in_values.insert(val);
		}
		comparisons.push_back({comp.GetExpressionType(), std::move(val)});
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	    expr.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr.Cast<BoundConjunctionExpression>();
		for (auto &child : conj.GetChildren()) {
			if (!CollectValuesAndComparisonsFromExpression(*child, in_values, comparisons)) {
				return false;
			}
		}
		return true;
	}
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &func = expr.Cast<BoundFunctionExpression>();
		if (ExpressionFilter::IsRootOptionalExpression(func)) {
			auto optional_child = ExpressionFilter::GetOptionalFilterChild(func);
			return !optional_child ||
			       CollectValuesAndComparisonsFromExpression(*optional_child, in_values, comparisons);
		}
		if (TableFilterFunctions::IsTableFilterFunction(func.Function())) {
			return true;
		}
	}
	return false;
}

//! Check if a value qualifies against all extracted comparison conditions.
static bool ValueQualifies(const Value &value, const vector<ComparisonCondition> &comparisons) {
	for (auto &comp : comparisons) {
		bool passes;
		switch (comp.type) {
		case ExpressionType::COMPARE_EQUAL:
			passes = ValueOperations::Equals(value, comp.constant);
			break;
		case ExpressionType::COMPARE_NOTEQUAL:
			passes = ValueOperations::NotEquals(value, comp.constant);
			break;
		case ExpressionType::COMPARE_GREATERTHAN:
			passes = ValueOperations::GreaterThan(value, comp.constant);
			break;
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			passes = ValueOperations::GreaterThanEquals(value, comp.constant);
			break;
		case ExpressionType::COMPARE_LESSTHAN:
			passes = ValueOperations::LessThan(value, comp.constant);
			break;
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
			passes = ValueOperations::LessThanEquals(value, comp.constant);
			break;
		case ExpressionType::COMPARE_DISTINCT_FROM:
			passes = ValueOperations::DistinctFrom(value, comp.constant);
			break;
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			passes = ValueOperations::NotDistinctFrom(value, comp.constant);
			break;
		default:
			return true;
		}
		if (!passes) {
			return false;
		}
	}
	return true;
}

static bool ExtractValuesFromExpression(const Expression &expr, const LogicalType &key_type, DataChunk &key_columns) {
	value_set_t in_values;
	vector<ComparisonCondition> comparisons;
	if (!CollectValuesAndComparisonsFromExpression(expr, in_values, comparisons) || in_values.empty()) {
		return false;
	}
	key_columns.Initialize(Allocator::DefaultAllocator(), {key_type}, in_values.size());
	for (const auto &value : in_values) {
		if (!ValueQualifies(value, comparisons)) {
			continue;
		}
		if (value.type() != key_type) {
			return false;
		}
		key_columns.data[0].Append(value);
	}
	return key_columns.size() > 0;
}

static unique_ptr<IndexScanState> TryInitializeIndexScan(const IndexReadHandle<ART> &art, const Expression &index_expr,
                                                         const ColumnDefinition &col, const TableFilter &filter,
                                                         ProjectionIndex storage_index) {
	auto &expr_filter = ExpressionFilter::GetExpressionFilter(filter, "TryInitializeIndexScan");
	ColumnBinding binding(TableIndex(0), storage_index);
	BoundColumnRefExpression bound_ref(col.Name(), col.Type(), binding);

	auto key_columns = make_uniq<DataChunk>();
	if (!ExtractValuesFromExpression(*expr_filter.expr, col.Type(), *key_columns)) {
		auto filter_expr = expr_filter.ToExpression(bound_ref);
		return art->TryInitializeScan(index_expr, *filter_expr);
	}
	if (key_columns->size() == 1) {
		auto filter_expr =
		    BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, bound_ref.Copy(),
		                                      make_uniq<BoundConstantExpression>(key_columns->GetValue(0, 0)));
		return art->TryInitializeScan(index_expr, *filter_expr);
	}

	if (!bound_ref.Equals(index_expr)) {
		return nullptr;
	}
	return art->InitializeBatchScan(std::move(key_columns));
}

bool TryScanIndex(const IndexReadHandle<ART> &art, const ColumnList &column_list, TableFunctionInitInput &input,
                  TableFilterSet &filter_set, RowIdVectorOutput &row_ids) {
	row_ids.Reset();
	// FIXME: No support for index scans on compound ARTs.
	// See note above on multi-filter support.
	if (art->UnboundExpressionCount() > 1) {
		return false;
	}

	auto index_expr = art->CopyUnboundExpression(0);
	auto indexed_columns = art->GetColumnIds();

	// NOTE: We do not push down multi-column filters, e.g., 42 = a + b.
	if (indexed_columns.size() != 1) {
		return false;
	}

	// Get ART column. GetColumnIds returns physical column IDs, which skip generated columns.
	auto &col = column_list.GetColumn(PhysicalIndex(indexed_columns[0]));

	// The indexes of the filters match input.column_indexes, which are: i -> column_index.
	// Try to find a filter on the ART column.
	ProjectionIndex storage_index;
	for (idx_t i = 0; i < input.column_indexes.size(); i++) {
		if (input.column_indexes[i].ToLogical() == col.Logical()) {
			storage_index = ProjectionIndex(i);
			break;
		}
	}

	// No filter matches the ART column.
	if (!storage_index.IsValid()) {
		return false;
	}

	// A bound column reference in an unbound index expression is an ordinal into indexed_columns, which is not the
	// column's position in this scan. Rebind the references to the ART column's position in the scan input.
	ExpressionIterator::EnumerateExpression(index_expr, [&](Expression &expr) {
		if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
			return;
		}
		expr.Cast<BoundColumnRefExpression>().BindingMutable().column_index = storage_index;
	});

	// Try to find a matching filter for the column.
	auto filter = filter_set.TryGetFilterByColumnIndex(storage_index);
	if (!filter) {
		return false;
	}

	auto scan_state = TryInitializeIndexScan(art, *index_expr, col, *filter, storage_index);
	if (!scan_state) {
		return false;
	}
	if (!art->Scan(*scan_state, row_ids)) {
		return false;
	}
	for (const auto delta : {IndexDeltaType::DELETED_ROWS_IN_USE, IndexDeltaType::ADDED_DATA_DURING_CHECKPOINT}) {
		auto delta_index = art.FindDelta(delta);
		if (delta_index && !delta_index->Scan(*scan_state, row_ids)) {
			return false;
		}
	}
	return true;
}

unique_ptr<GlobalTableFunctionState> TableScanInitGlobal(ClientContext &context, TableFunctionInitInput &input) {
	D_ASSERT(input.bind_data);

	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();

	// Can't index scan without filters.
	if (!input.filters) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}

	// Only scan specific partitions
	if (bind_data.partitions_to_scan) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}

	// The index scan does not apply the sample.
	if (input.sample_options) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}

	auto &filter_set = *input.filters;

	// FIXME: We currently only support scanning one ART with one filter.
	// If multiple filters exist, i.e., a = 11 AND b = 24, we need to
	// 1.	1.1. Find + scan one ART for a = 11.
	//		1.2. Find + scan one ART for b = 24.
	//		1.3. Return the intersecting row IDs.
	// 2. (Reorder and) scan a single ART with a compound key of (a, b).
	if (filter_set.FilterCount() != 1) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}

	auto &info = storage.GetDataTableInfo();
	auto &indexes = info->GetIndexes();
	if (indexes.Empty()) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}

	auto scan_percentage = Settings::Get<IndexScanPercentageSetting>(context);
	auto scan_max_count = Settings::Get<IndexScanMaxCountSetting>(context);

	auto total_rows = storage.GetTotalRows();
	auto total_rows_from_percentage = LossyNumericCast<idx_t>(double(total_rows) * scan_percentage);
	auto max_count = MaxValue(scan_max_count, total_rows_from_percentage);

	auto &column_list = duck_table.GetColumns();
	bool index_scan = false;
	RowIdVectorOutput row_ids(max_count);

	info->BindIndexes(context, ART::TYPE_NAME);

	// Exclude rowid-shifting vacuum from the ART probe until the index scan finishes: collected rowids must be
	// fetched against the matching row-group tree. Falling back to a table scan releases the lock on return.
	unique_ptr<StorageLockKey> vacuum_lock;
	auto &attached = storage.GetAttached();
	const bool indexed_vacuum_may_move_rowids = attached.GetVacuumRebuildIndexThreshold() > 0 ||
	                                            StorageCompatibility::FromDatabase(attached).CanPersistRowIdGaps();
	if (indexed_vacuum_may_move_rowids) {
		vacuum_lock = DuckTransactionManager::Get(attached).SharedVacuumLock();
	}

	for (auto entry : indexes.IndexEntries()) {
		if (entry->GetBindState() != IndexBindState::BOUND || entry->GetIndexType() != ART::TYPE_NAME) {
			continue;
		}
		{
			auto index = entry->GetReadHandle<ART>();
			index_scan = TryScanIndex(index, column_list, input, filter_set, row_ids);
		}
		if (index_scan) {
			// found an index - break
			break;
		}
	}

	if (!index_scan) {
		return DuckTableScanInitGlobal(context, input, storage, bind_data);
	}
	return DuckIndexScanInitGlobal(context, input, bind_data, row_ids.TakeRows(), std::move(vacuum_lock));
}

static unique_ptr<BaseStatistics> TableScanStatistics(ClientContext &context, TableFunctionGetStatisticsInput &input) {
	auto &column_id = input.column_index;
	if (column_id.IsRowIdColumn() || column_id.IsRowNumberColumn()) {
		return nullptr;
	}

	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	auto &column = duck_table.GetColumn(LogicalIndex(column_id.GetPrimaryIndex()));
	if (column.Generated()) {
		return nullptr;
	}

	auto storage_index = duck_table.GetStorageIndex(column_id);
	return duck_table.GetStatistics(context, storage_index);
}

static void TableScanFunc(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &g_state = data_p.global_state->Cast<TableScanGlobalState>();
	g_state.TableScanFunc(context, data_p, output);
}

double TableScanProgress(ClientContext &context, const FunctionData *bind_data_p,
                         const GlobalTableFunctionState *g_state_p) {
	auto &g_state = g_state_p->Cast<TableScanGlobalState>();
	return g_state.TableScanProgress(context, bind_data_p);
}

OperatorPartitionData TableScanGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input) {
	if (input.partition_info.RequiresPartitionColumns()) {
		throw InternalException("TableScan::GetPartitionData: partition columns not supported");
	}

	auto &g_state = input.global_state->Cast<TableScanGlobalState>();
	return g_state.TableScanGetPartitionData(context, input);
}

vector<PartitionStatistics> TableScanGetPartitionStats(ClientContext &context, GetPartitionStatsInput &input) {
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	auto &storage = duck_table.GetStorage();
	return storage.GetPartitionStats(context);
}

optional_ptr<TableCatalogEntry> TableScanGetTableEntry(optional_ptr<const FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	return &bind_data.table;
}

void TableScanDependency(LogicalDependencyList &entries, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	entries.AddDependency(bind_data.table);
}

unique_ptr<NodeStatistics> TableScanCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	auto &duck_table = bind_data.table.Cast<DuckTableEntry>();
	auto &local_storage = LocalStorage::Get(context, duck_table.catalog);
	auto &storage = duck_table.GetStorage();
	idx_t table_rows = storage.GetTotalRows();
	idx_t estimated_cardinality = table_rows + local_storage.AddedRows(duck_table.GetStorage());
	return make_uniq<NodeStatistics>(estimated_cardinality, estimated_cardinality);
}

//! The part of a running count that has not been reported yet
static idx_t TakeUnreported(idx_t count, idx_t &reported) {
	auto result = count >= reported ? count - reported : count;
	reported = count;
	return result;
}

void TableScanGetMetrics(TableFunctionGetMetricsInput &input) {
	auto &gstate = input.global_state->Cast<TableScanGlobalState>();
	auto &local_state = *input.local_state;
	auto &reported = local_state.Cast<TableScanMetricsLocalState>();
	input.operator_metrics.rows_scanned +=
	    TakeUnreported(gstate.TableScanRowsScanned(local_state), reported.reported_rows_scanned);
	input.operator_metrics.row_groups_scanned +=
	    TakeUnreported(gstate.TableScanRowGroupsScanned(local_state), reported.reported_row_groups_scanned);
	input.operator_metrics.total_row_groups_to_scan = gstate.total_row_groups_to_scan;
}

InsertionOrderPreservingMap<string> TableScanToString(TableFunctionToStringInput &input) {
	InsertionOrderPreservingMap<string> result;
	auto &bind_data = input.bind_data->Cast<TableScanBindData>();
	result["Table"] = bind_data.table.schema.GetQualifiedName(bind_data.table.name)
	                      .ToString(QualifiedNameToStringMode::HIDE_DEFAULT_SCHEMA);
	result["Type"] = bind_data.is_index_scan ? "Index Scan" : "Sequential Scan";
	return result;
}

static void TableScanSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                               const BoundTableFunction &function) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	// the catalog/schema/name are only the innermost qualification - "qualified_name" carries the full (possibly
	// nested) schema path
	serializer.WriteProperty(100, "catalog", bind_data.table.schema.catalog.GetName());
	serializer.WriteProperty(101, "schema", bind_data.table.schema.name);
	serializer.WriteProperty(102, "table", bind_data.table.name);
	serializer.WriteProperty(103, "is_index_scan", bind_data.is_index_scan);
	serializer.WriteProperty(104, "is_create_index", bind_data.is_create_index);
	serializer.WritePropertyWithDefault(105, "result_ids", unsafe_vector<row_t>());
	serializer.WritePropertyWithDefault<QualifiedName>(
	    106, "qualified_name", bind_data.table.schema.GetQualifiedName(bind_data.table.name), QualifiedName());
}

static unique_ptr<FunctionData> TableScanDeserialize(Deserializer &deserializer, BoundTableFunction &function) {
	auto catalog = deserializer.ReadProperty<Identifier>(100, "catalog");
	auto schema = deserializer.ReadProperty<Identifier>(101, "schema");
	auto table = deserializer.ReadProperty<Identifier>(102, "table");
	auto is_index_scan = deserializer.ReadProperty<bool>(103, "is_index_scan");
	auto is_create_index = deserializer.ReadProperty<bool>(104, "is_create_index");
	deserializer.ReadDeletedProperty<unsafe_vector<row_t>>(105, "result_ids");
	// plans written before nested schema support only have the innermost qualification
	auto qualified_name =
	    deserializer.ReadPropertyWithExplicitDefault<QualifiedName>(106, "qualified_name", QualifiedName());
	if (qualified_name.Path().empty()) {
		qualified_name = QualifiedName(catalog, schema, table);
	}
	auto &catalog_entry = Catalog::GetEntry<TableCatalogEntry>(deserializer.Get<ClientContext &>(), qualified_name);
	if (catalog_entry.type != CatalogType::TABLE_ENTRY) {
		throw SerializationException("Cant find table for %s.%s", schema, table);
	}
	auto result = make_uniq<TableScanBindData>(catalog_entry.Cast<DuckTableEntry>());
	result->is_index_scan = is_index_scan;
	result->is_create_index = is_create_index;
	return std::move(result);
}

static bool TableSupportsPushdownExtract(const FunctionData &bind_data_ref, const LogicalIndex &column_idx) {
	auto &bind_data = bind_data_ref.Cast<TableScanBindData>();
	auto &column = bind_data.table.GetColumn(column_idx);
	if (column.Generated()) {
		return false;
	}
	auto column_type = column.GetType();
	return column_type.id() == LogicalTypeId::STRUCT || column_type.id() == LogicalTypeId::VARIANT;
}

bool TableScanPushdownExpression(ClientContext &context, const LogicalGet &get, Expression &expr) {
	return true;
}

virtual_column_map_t TableScanGetVirtualColumns(ClientContext &context, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	return bind_data.table.GetVirtualColumns();
}

vector<column_t> TableScanGetRowIdColumns(ClientContext &context, optional_ptr<FunctionData> bind_data) {
	vector<column_t> result;
	result.emplace_back(COLUMN_IDENTIFIER_ROW_ID);
	return result;
}

void SetScanOrder(unique_ptr<RowGroupOrderOptions> order_options, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	bind_data.order_options = std::move(order_options);
}

void SetPartitionsToScan(vector<idx_t> partition_indices, optional_ptr<FunctionData> bind_data_p) {
	auto &bind_data = bind_data_p->Cast<TableScanBindData>();
	bind_data.partitions_to_scan = make_uniq<unordered_set<idx_t>>(partition_indices.begin(), partition_indices.end());
}

static string TableScanToSQLGuard(const LogicalGet &get, bool has_input) {
	auto &data = get.bind_data->Cast<TableScanBindData>();
	if (has_input) {
		return "input_child";
	}
	if (get.ordinality_idx.IsValid()) {
		return "ordinality";
	}
	if (!get.scan_partition_indices.empty()) {
		return "scan_partitions";
	}
	if (data.is_create_index) {
		return "create_index";
	}
	if (data.partitions_to_scan) {
		return "bound_scan_partitions";
	}
	const bool has_filters =
	    get.table_filters.HasFilters() || get.table_filters.HasMultiColumnFilters() || get.dynamic_filters;
	for (auto &index : get.GetColumnIds()) {
		if (index.IsRowNumberColumn() && has_filters) {
			return "row_number_with_filters";
		}
		if (index.IsVirtualColumn() && !index.IsRowIdColumn() && !index.IsRowNumberColumn()) {
			return "virtual_column";
		}
		if (!index.IsVirtualColumn()) {
			auto &definition = data.table.GetColumn(index.ToLogical());
			if (!index.IsPushdownExtract() && definition.Type() != get.GetColumnType(index)) {
				return "column_type";
			}
		}
	}
	return string();
}

static TableFunctionToSQLResult TableScanToSQL(ClientContext &, const LogicalGet &get) {
	auto guard = TableScanToSQLGuard(get, !get.children.empty());
	if (!guard.empty()) {
		return {nullptr, std::move(guard)};
	}
	auto table = make_uniq<BaseTableRef>();
	auto entry = get.GetTable();
	table->SetQualifiedName(entry->schema.GetQualifiedName(entry->name));
	return {std::move(table), {}};
}

TableFunction TableScanFunction::GetFunction() {
	TableFunction scan_function("seq_scan", {}, TableScanFunc);
	scan_function.init_local = TableScanInitLocal;
	scan_function.init_global = TableScanInitGlobal;
	scan_function.statistics_extended = TableScanStatistics;
	scan_function.dependency = TableScanDependency;
	scan_function.cardinality = TableScanCardinality;
	scan_function.get_metrics = TableScanGetMetrics;
	scan_function.pushdown_complex_filter = nullptr;
	scan_function.to_string = TableScanToString;
	scan_function.to_sql = TableScanToSQL;
	scan_function.table_scan_progress = TableScanProgress;
	scan_function.get_partition_data = TableScanGetPartitionData;
	scan_function.get_partition_stats = TableScanGetPartitionStats;
	scan_function.get_table_entry = TableScanGetTableEntry;
	scan_function.projection_pushdown = true;
	scan_function.filter_pushdown = true;
	scan_function.filter_prune = true;
	scan_function.sampling_pushdown = true;
	scan_function.late_materialization = true;
	scan_function.serialize = TableScanSerialize;
	scan_function.deserialize = TableScanDeserialize;
	scan_function.pushdown_expression = TableScanPushdownExpression;
	scan_function.get_virtual_columns = TableScanGetVirtualColumns;
	scan_function.get_row_id_columns = TableScanGetRowIdColumns;
	scan_function.set_scan_order = SetScanOrder;
	scan_function.set_partitions_to_scan = SetPartitionsToScan;
	scan_function.supports_pushdown_extract = TableSupportsPushdownExtract;
	return scan_function;
}

void TableScanFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet table_scan_set("seq_scan");
	table_scan_set.AddFunction(GetFunction());
	set.AddFunction(std::move(table_scan_set));
}

void BuiltinFunctions::RegisterTableScanFunctions() {
	TableScanFunction::RegisterFunction(*this);
}

} // namespace duckdb
