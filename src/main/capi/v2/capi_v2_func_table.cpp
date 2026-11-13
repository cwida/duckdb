#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/table_function_catalog_entry.hpp"
#include "duckdb/common/multi_file/multi_file_data.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/function/partition_stats.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parser/parsed_data/create_table_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <algorithm>

namespace duckdb::capiv2 {

class CV2TableFunctionInfo;

struct CV2TableAcceptedFilterBatch {
	vector<ColumnIndex> column_ids;
	vector<unique_ptr<Expression>> filters;

	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty(100, "column_ids", column_ids);
		serializer.WriteProperty(101, "filters", filters);
	}

	static unique_ptr<CV2TableAcceptedFilterBatch> Deserialize(Deserializer &deserializer) {
		auto result = make_uniq<CV2TableAcceptedFilterBatch>();
		deserializer.ReadProperty(100, "column_ids", result->column_ids);
		deserializer.ReadProperty(101, "filters", result->filters);
		return result;
	}
};

//! The bind data of a bound call site. Besides the user's own bind data it carries a borrowed pointer back to the
//! function info: the progress hook only receives the bind data, so it is its only route to the callbacks. The
//! registered function owns the info and outlives every call site bound against it.
class CV2TableFunctionData final : public FunctionData {
public:
	shared_ptr<CV2UserData> handle;
	optional_ptr<const CV2TableFunctionInfo> info;

	// The static estimate a bind callback may set via table_function_bind_set_cardinality.
	idx_t cardinality = 0;
	bool cardinality_is_exact = false;
	bool cardinality_set = false;

	vector<LogicalType> column_types;
	vector<Identifier> column_names;
	//! Retained here because function data is deserialized before LogicalGet's invocation parameters.
	vector<Value> parameters;
	named_argument_map_t named_parameters;
	//! Accepted predicates with their pre-pruning column mapping, replayed on rebind.
	vector<shared_ptr<const CV2TableAcceptedFilterBatch>> accepted_filters;

	auto Copy() const -> unique_ptr<FunctionData> override {
		return make_uniq<CV2TableFunctionData>(*this);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		const auto &other_data = other.Cast<CV2TableFunctionData>();
		return handle && other_data.handle && handle->Equals(*other_data.handle);
	}
};

class CV2TableGlobalState final : public GlobalTableFunctionState {
public:
	auto MaxThreads() const -> idx_t override {
		return max_threads;
	}

	CV2UserData handle;
	idx_t max_threads = 1;

	// The declared column each vector of the output chunk stands for, kept here so the exec callback can ask.
	vector<column_t> scan_columns;
};

class CV2TableLocalState final : public LocalTableFunctionState {
public:
	CV2UserData handle;
	//! Whether the caller claims the batches of this thread (see TableFunction::claim_batch)
	bool claimed_externally = false;
	//! Whether this thread has claimed a batch it has not exhausted yet
	bool batch_claimed = false;
	//! Written into by the exec callback when the scan requests no columns, as the function produces its rows through
	//! its first column
	unique_ptr<DataChunk> row_chunk;

	// Requested partition columns as declared columns, fixed for the lifetime of the local state's pipeline.
	const vector<column_t> &PartitionColumns(const OperatorPartitionInfo &partition_info,
	                                         const vector<column_t> &scan_columns) {
		if (!partition_columns_resolved) {
			for (auto scan_position : partition_info.partition_columns) {
				D_ASSERT(scan_position < scan_columns.size());
				partition_columns.push_back(scan_columns[scan_position]);
			}
			partition_columns_resolved = true;
		}
		D_ASSERT(partition_columns.size() == partition_info.partition_columns.size());
		return partition_columns;
	}

	void CheckPartitionData(const Identifier &function_name, const OperatorPartitionData &data) {
		if (last_batch_index.IsValid()) {
			auto last_index = last_batch_index.GetIndex();
			// The engine would otherwise raise an InternalException, which invalidates the database.
			if (data.batch_index < last_index) {
				throw InvalidInputException(
				    "The partition data callback of table function %s reported batch index %llu after "
				    "previously reporting %llu on the same thread; the batch index must not decrease.",
				    function_name, data.batch_index, last_index);
			}
			// The engine only reads the values when the batch index changes; a changed value would silently be ignored.
			if (data.batch_index == last_index) {
				D_ASSERT(data.partition_data.size() == last_partition_values.size());
				for (idx_t i = 0; i < last_partition_values.size(); i++) {
					if (Value::NotDistinctFrom(data.partition_data[i].min_val, last_partition_values[i])) {
						continue;
					}
					throw InvalidInputException(
					    "The partition data callback of table function %s reported a different partitioning "
					    "column value at index %llu without changing the batch index.",
					    function_name, i);
				}
				return;
			}
		}
		last_batch_index = data.batch_index;
		last_partition_values.clear();
		for (auto &entry : data.partition_data) {
			last_partition_values.push_back(entry.min_val);
		}
	}

private:
	bool partition_columns_resolved = false;
	vector<column_t> partition_columns;
	optional_idx last_batch_index;
	vector<Value> last_partition_values;
};

class CV2TableBindInfo {
public:
	vector<LogicalType> out_column_types;
	vector<Identifier> out_column_names;
	idx_t out_cardinality = 0;
	bool out_cardinality_is_exact = false;
	bool out_cardinality_set = false;
	OrderPreservationType out_order_preservation = OrderPreservationType::INSERTION_ORDER;
};

static auto Convert(duckdb_v2_table_function_bind_info_handle info) -> CV2TableBindInfo * {
	return reinterpret_cast<CV2TableBindInfo *>(info);
}
static auto Convert(CV2TableBindInfo *info) -> duckdb_v2_table_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_bind_info_handle>(info);
}

class CV2TableInitGlobalInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	const vector<column_t> *in_scan_columns = nullptr;

	duckdb_v2_opaque out_global_state = {};
	idx_t out_max_threads = 1;
};

static auto Convert(duckdb_v2_table_function_init_global_info_handle info) -> CV2TableInitGlobalInfo * {
	return reinterpret_cast<CV2TableInitGlobalInfo *>(info);
}
static auto Convert(CV2TableInitGlobalInfo *info) -> duckdb_v2_table_function_init_global_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_init_global_info_handle>(info);
}

class CV2TableInitLocalInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;
	const vector<column_t> *in_scan_columns = nullptr;

	duckdb_v2_opaque out_local_state = {};
};

static auto Convert(duckdb_v2_table_function_init_local_info_handle info) -> CV2TableInitLocalInfo * {
	return reinterpret_cast<CV2TableInitLocalInfo *>(info);
}
static auto Convert(CV2TableInitLocalInfo *info) -> duckdb_v2_table_function_init_local_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_init_local_info_handle>(info);
}

class CV2TableExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;
	void *in_local_state = nullptr;
	const vector<column_t> *in_scan_columns = nullptr;

	DataChunk *output = nullptr;
};

static auto Convert(duckdb_v2_table_function_exec_info_handle info) -> CV2TableExecInfo * {
	return reinterpret_cast<CV2TableExecInfo *>(info);
}
static auto Convert(CV2TableExecInfo *info) -> duckdb_v2_table_function_exec_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_exec_info_handle>(info);
}

class CV2TableProgressInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;

	double out_progress = 0.0;
};

static auto Convert(duckdb_v2_table_function_progress_info_handle info) -> CV2TableProgressInfo * {
	return reinterpret_cast<CV2TableProgressInfo *>(info);
}
static auto Convert(CV2TableProgressInfo *info) -> duckdb_v2_table_function_progress_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_progress_info_handle>(info);
}

class CV2TableClaimBatchInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;
	void *in_local_state = nullptr;

	bool out_claimed = false;
};

static auto Convert(duckdb_v2_table_function_claim_batch_info_handle info) -> CV2TableClaimBatchInfo * {
	return reinterpret_cast<CV2TableClaimBatchInfo *>(info);
}
static auto Convert(CV2TableClaimBatchInfo *info) -> duckdb_v2_table_function_claim_batch_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_claim_batch_info_handle>(info);
}

class CV2TableFilterPushdownInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	// The predicates the optimizer offers, borrowed from the engine for the duration of the callback.
	const vector<unique_ptr<Expression>> *in_filters = nullptr;
	// What the column references inside the predicates index: the scan's column list at optimization time.
	const vector<ColumnIndex> *in_column_ids = nullptr;

	vector<bool> out_handled;
};

static auto Convert(duckdb_v2_table_function_filter_pushdown_info_handle info) -> CV2TableFilterPushdownInfo * {
	return reinterpret_cast<CV2TableFilterPushdownInfo *>(info);
}
static auto Convert(CV2TableFilterPushdownInfo *info) -> duckdb_v2_table_function_filter_pushdown_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_filter_pushdown_info_handle>(info);
}

class CV2TablePartitionDataInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;
	void *in_local_state = nullptr;
	const OperatorPartitionInfo *in_partition_info = nullptr;
	const vector<column_t> *in_partition_columns = nullptr;
	const vector<LogicalType> *in_column_types = nullptr;

	optional_idx out_batch_index;
	vector<bool> out_partition_value_set;
	vector<Value> out_partition_values;
};

static auto Convert(duckdb_v2_table_function_partition_data_info_handle info) -> CV2TablePartitionDataInfo * {
	return reinterpret_cast<CV2TablePartitionDataInfo *>(info);
}
static auto Convert(CV2TablePartitionDataInfo *info) -> duckdb_v2_table_function_partition_data_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_partition_data_info_handle>(info);
}

class CV2TableStatsInfo {
public:
	explicit CV2TableStatsInfo(BaseStatistics &result) : result_stats(CV2Stats::Writable(result)) {
	}

	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	idx_t in_column_index = 0;

	CV2Stats result_stats;
};

static auto Convert(duckdb_v2_table_function_stats_info_handle info) -> CV2TableStatsInfo * {
	return reinterpret_cast<CV2TableStatsInfo *>(info);
}
static auto Convert(CV2TableStatsInfo *info) -> duckdb_v2_table_function_stats_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_stats_info_handle>(info);
}

class CV2TablePartitioningInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	const vector<column_t> *in_partition_ids = nullptr;

	// Not calling the setter is not a caller error: it means "not partitioned", the conservative default.
	TablePartitionInfo out_partition_info = TablePartitionInfo::NOT_PARTITIONED;
};

static auto Convert(duckdb_v2_table_function_partitioning_info_handle info) -> CV2TablePartitioningInfo * {
	return reinterpret_cast<CV2TablePartitioningInfo *>(info);
}
static auto Convert(CV2TablePartitioningInfo *info) -> duckdb_v2_table_function_partitioning_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_partitioning_info_handle>(info);
}

static_assert(static_cast<int>(TablePartitionInfo::NOT_PARTITIONED) == DUCKDB_V2_TABLE_PARTITION_INFO_NOT_PARTITIONED,
              "TablePartitionInfo::NOT_PARTITIONED mismatch");
static_assert(static_cast<int>(TablePartitionInfo::SINGLE_VALUE_PARTITIONS) ==
                  DUCKDB_V2_TABLE_PARTITION_INFO_SINGLE_VALUE_PARTITIONS,
              "TablePartitionInfo::SINGLE_VALUE_PARTITIONS mismatch");
static_assert(static_cast<int>(TablePartitionInfo::OVERLAPPING_PARTITIONS) ==
                  DUCKDB_V2_TABLE_PARTITION_INFO_OVERLAPPING_PARTITIONS,
              "TablePartitionInfo::OVERLAPPING_PARTITIONS mismatch");
static_assert(static_cast<int>(TablePartitionInfo::DISJOINT_PARTITIONS) ==
                  DUCKDB_V2_TABLE_PARTITION_INFO_DISJOINT_PARTITIONS,
              "TablePartitionInfo::DISJOINT_PARTITIONS mismatch");

static auto CV2ConvertPartitionInfo(DUCKDB_V2_TABLE_PARTITION_INFO value) -> TablePartitionInfo {
	if (static_cast<uint32_t>(value) > DUCKDB_V2_TABLE_PARTITION_INFO_DISJOINT_PARTITIONS) {
		throw duckdb::InvalidInputException(
		    "Invalid value in duckdb_v2_table_function_partitioning_set_partition_info");
	}
	return static_cast<TablePartitionInfo>(value);
}

static_assert(static_cast<int>(OrderPreservationType::NO_ORDER) == DUCKDB_V2_ORDER_PRESERVATION_NO_ORDER,
              "OrderPreservationType::NO_ORDER mismatch");
static_assert(static_cast<int>(OrderPreservationType::INSERTION_ORDER) == DUCKDB_V2_ORDER_PRESERVATION_INSERTION_ORDER,
              "OrderPreservationType::INSERTION_ORDER mismatch");
static_assert(static_cast<int>(OrderPreservationType::FIXED_ORDER) == DUCKDB_V2_ORDER_PRESERVATION_FIXED_ORDER,
              "OrderPreservationType::FIXED_ORDER mismatch");

static auto CV2ConvertOrderPreservation(DUCKDB_V2_ORDER_PRESERVATION value) -> OrderPreservationType {
	if (static_cast<uint32_t>(value) > DUCKDB_V2_ORDER_PRESERVATION_FIXED_ORDER) {
		throw duckdb::InvalidInputException("Invalid value in duckdb_v2_table_function_bind_set_order_preservation");
	}
	return static_cast<OrderPreservationType>(value);
}

class CV2TableFunctionInfo : public TableFunctionInfo {
public:
	duckdb_v2_table_function_bind_callback_fn bind_cb = nullptr;
	duckdb_v2_table_function_init_global_callback_fn init_global_cb = nullptr;
	duckdb_v2_table_function_init_local_callback_fn init_local_cb = nullptr;
	duckdb_v2_table_function_exec_callback_fn exec_cb = nullptr;
	duckdb_v2_table_function_progress_callback_fn progress_cb = nullptr;
	duckdb_v2_table_function_filter_pushdown_callback_fn filter_pushdown_cb = nullptr;
	duckdb_v2_table_function_partition_data_callback_fn partition_data_cb = nullptr;
	duckdb_v2_table_function_partitioning_callback_fn partitioning_cb = nullptr;
	duckdb_v2_table_function_claim_batch_callback_fn claim_batch_cb = nullptr;
	duckdb_v2_table_function_get_bind_info_callback_fn get_bind_info_cb = nullptr;
	duckdb_v2_table_function_stats_callback_fn stats_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
	bool projection_pushdown = false;

	Identifier name;
};

//! What the "get bind info" callback describes a bound call with
class CV2TableGetBindInfoInfo {
public:
	explicit CV2TableGetBindInfoInfo(const CV2TableFunctionData &bind_data_p) : bind_data(bind_data_p) {
	}

	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	//! The bound call - its result columns are the columns identifiers are attached to
	const CV2TableFunctionData &bind_data;

	//! What the callback reports
	BindInfo out_bind_info {ScanType::EXTERNAL};
};

static auto Convert(duckdb_v2_table_function_get_bind_info_info_handle info) -> CV2TableGetBindInfoInfo * {
	return reinterpret_cast<CV2TableGetBindInfoInfo *>(info);
}
static auto Convert(CV2TableGetBindInfoInfo *info) -> duckdb_v2_table_function_get_bind_info_info_handle {
	return reinterpret_cast<duckdb_v2_table_function_get_bind_info_info_handle>(info);
}

//! Asks the function to describe a bound call: e.g. the identifiers of its columns
static auto CV2TableGetBindInfo(TableFunctionGetBindInfoInput &input) -> BindInfo {
	if (!input.bind_data) {
		return BindInfo(ScanType::EXTERNAL);
	}
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	CV2TableGetBindInfoInfo args(bind_data);
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.context);
	info.get_bind_info_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(args.out_bind_info);
}

static auto CV2TableBind(ClientContext &context, TableFunctionBindInput &input, vector<LogicalType> &return_types,
                         vector<Identifier> &names) -> unique_ptr<FunctionData> {
	const auto &info = input.info->Cast<CV2TableFunctionInfo>();

	// the binder places every argument in its signature slot, defaults included, so the call reads them as-is
	CV2ConstantBindInfo bind_info(input.table_function, input.table_function.GetSignature(),
	                              info.user_data ? info.user_data->GetData() : nullptr, input.inputs,
	                              input.named_parameters);
	CV2TableBindInfo args = {};

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.bind_cb(Convert(&bind_info), Convert(&args), Convert(&callback_context), &err_ptr);

	// Take ownership of whatever the callback set before reporting an error, so it is destroyed either way.
	auto result = make_uniq<CV2TableFunctionData>();
	result->info = &info;
	result->handle = std::move(bind_info.out_bind_data);
	result->cardinality = args.out_cardinality;
	result->cardinality_is_exact = args.out_cardinality_is_exact;
	result->cardinality_set = args.out_cardinality_set;

	if (err.HasError()) {
		err.ThrowAsException();
	}

	if (args.out_column_types.empty()) {
		throw InvalidInputException("The bind callback of table function %s did not declare any result columns.",
		                            input.table_function.GetName());
	}

	// The binder hands each call site its own copy of the function, so this does not leak into other calls.
	input.table_function.order_preservation_type = args.out_order_preservation;

	result->column_types = args.out_column_types;
	result->column_names = args.out_column_names;
	result->parameters = input.inputs;
	result->named_parameters = input.named_parameters;
	return_types = std::move(args.out_column_types);
	names = std::move(args.out_column_names);
	return std::move(result);
}

//! The declared column behind each vector of the output chunk. With projection pushdown the engine sizes the chunk
//! to the columns it asked for; without it the chunk always holds every declared column, whatever the engine asked.
static auto CV2TableScanColumns(const CV2TableFunctionData &bind_data, const TableFunctionInitInput &input)
    -> vector<column_t> {
	if (bind_data.info->projection_pushdown) {
		if (input.column_ids.empty()) {
			// the scan only produces rows (e.g. for count(*)) - the function produces them through its first column
			return {0};
		}
		return input.column_ids;
	}
	vector<column_t> columns;
	for (idx_t i = 0; i < bind_data.column_types.size(); i++) {
		columns.push_back(i);
	}
	return columns;
}

static auto CV2TableInitGlobal(ClientContext &context, TableFunctionInitInput &input)
    -> unique_ptr<GlobalTableFunctionState> {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	// Always produced, even without a callback: it carries the thread count and column list the scan runs with.
	auto result = make_uniq<CV2TableGlobalState>();
	result->scan_columns = CV2TableScanColumns(bind_data, input);
	if (!info.init_global_cb) {
		return std::move(result);
	}

	CV2TableInitGlobalInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	args.in_scan_columns = &result->scan_columns;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.init_global_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (args.out_global_state.ptr) {
		result->handle =
		    CV2UserData(args.out_global_state.ptr, args.out_global_state.destroy, args.out_global_state.equals);
	}
	result->max_threads = args.out_max_threads;

	// Throw after taking ownership of the state, so that it is destroyed even if we error.
	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(result);
}

static auto CV2TableInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                              GlobalTableFunctionState *global_state) -> unique_ptr<LocalTableFunctionState> {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	// Always produced, even without a callback: get_partition_data keeps its per-thread guard state here.
	auto result = make_uniq<CV2TableLocalState>();
	if (!info.init_local_cb) {
		return std::move(result);
	}

	auto scan_columns = CV2TableScanColumns(bind_data, input);

	CV2TableInitLocalInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	args.in_scan_columns = &scan_columns;
	if (global_state) {
		args.in_global_state = global_state->Cast<CV2TableGlobalState>().handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context.client);
	info.init_local_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (args.out_local_state.ptr) {
		result->handle =
		    CV2UserData(args.out_local_state.ptr, args.out_local_state.destroy, args.out_local_state.equals);
	}

	// Throw after taking ownership of the state, so that it is destroyed even if we error.
	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(result);
}

//! Calls the claim batch callback for the local state of the given input - returns whether a batch was claimed
static auto CV2TableClaimNextBatch(ClientContext &context, TableFunctionInput &input) -> bool {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;
	auto &local_state = input.local_state->Cast<CV2TableLocalState>();

	CV2TableClaimBatchInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	if (input.global_state) {
		args.in_global_state = input.global_state->Cast<CV2TableGlobalState>().handle.GetData();
	}
	args.in_local_state = local_state.handle.GetData();

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.claim_batch_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
	local_state.batch_claimed = args.out_claimed;
	return args.out_claimed;
}

//! The caller claims the batches of this local state itself - every exec then scans the claimed batch only
static auto CV2TableClaimBatch(ClientContext &context, TableFunctionInput &input) -> bool {
	input.local_state->Cast<CV2TableLocalState>().claimed_externally = true;
	return CV2TableClaimNextBatch(context, input);
}

static auto CV2TableExecBatch(ClientContext &context, TableFunctionInput &input, DataChunk &output) -> void;

static auto CV2TableExec(ClientContext &context, TableFunctionInput &input, DataChunk &output) -> void {
	const auto &info = *input.bind_data->Cast<CV2TableFunctionData>().info;
	if (!info.claim_batch_cb) {
		CV2TableExecBatch(context, input, output);
		return;
	}
	auto &local_state = input.local_state->Cast<CV2TableLocalState>();
	if (local_state.claimed_externally) {
		// an empty chunk ends the batch, after which the caller claims the next one
		CV2TableExecBatch(context, input, output);
		return;
	}
	// nobody claims the batches for us - move on to the next batch whenever the current one is exhausted
	while (true) {
		if (!local_state.batch_claimed && !CV2TableClaimNextBatch(context, input)) {
			return;
		}
		CV2TableExecBatch(context, input, output);
		if (output.size() > 0) {
			return;
		}
		local_state.batch_claimed = false;
	}
}

static auto CV2TableExecBatch(ClientContext &context, TableFunctionInput &input, DataChunk &output) -> void {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	CV2TableExecInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	if (input.global_state) {
		auto &global_state = input.global_state->Cast<CV2TableGlobalState>();
		args.in_global_state = global_state.handle.GetData();
		args.in_scan_columns = &global_state.scan_columns;
	}
	auto &local_state = input.local_state->Cast<CV2TableLocalState>();
	args.in_local_state = local_state.handle.GetData();
	reference<DataChunk> target(output);
	if (output.ColumnCount() == 0) {
		// the scan requests no columns (e.g. for count(*)), only rows - the function writes them into its first column
		if (!local_state.row_chunk) {
			local_state.row_chunk = make_uniq<DataChunk>();
			local_state.row_chunk->Initialize(context, {bind_data.column_types[0]});
		}
		local_state.row_chunk->Reset();
		target = *local_state.row_chunk;
	}
	args.output = &target.get();

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.exec_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	// The vector write API sizes per vector, while the engine reads the chunk's cardinality. Callers typically size
	// only the first vector and write the rest through direct buffer writes, so take the first vector's size as the
	// batch's row count and propagate it to the others. A table function always has at least one result column.
	auto row_count = target.get().data[0].size();
	if (output.ColumnCount() == 0) {
		output.SetCardinalityUnsafe(row_count);
		return;
	}
	output.SetChildCardinality(row_count);
}

//! An exact estimate also pins the maximum; otherwise only the estimate is known.
static auto CV2TableMakeStatistics(idx_t cardinality, bool is_exact) -> unique_ptr<NodeStatistics> {
	if (is_exact) {
		return make_uniq<NodeStatistics>(cardinality, cardinality);
	}
	return make_uniq<NodeStatistics>(cardinality);
}

static auto CV2TableCardinality(ClientContext &context, const FunctionData *bind_data) -> unique_ptr<NodeStatistics> {
	const auto &data = bind_data->Cast<CV2TableFunctionData>();
	if (!data.cardinality_set) {
		// No estimate at all: the optimizer falls back on its own defaults.
		return nullptr;
	}
	return CV2TableMakeStatistics(data.cardinality, data.cardinality_is_exact);
}

static auto CV2TableStats(ClientContext &context, TableFunctionGetStatisticsInput &input)
    -> unique_ptr<BaseStatistics> {
	const auto &data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *data.info;

	// The callback only describes whole declared columns, not virtual columns or fields extracted from a column
	const auto &column_index = input.column_index;
	if (!column_index.HasPrimaryIndex() || column_index.HasChildren() || column_index.IsVirtualColumn() ||
	    column_index.GetPrimaryIndex() >= data.column_types.size()) {
		return nullptr;
	}

	auto result = BaseStatistics::CreateUnknown(data.column_types[column_index.GetPrimaryIndex()]);
	CV2TableStatsInfo args(result);
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = data.handle ? data.handle->GetData() : nullptr;
	args.in_column_index = column_index.GetPrimaryIndex();

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.stats_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result.ToUnique();
}

static auto CV2TableProgress(ClientContext &context, const FunctionData *bind_data,
                             const GlobalTableFunctionState *global_state) -> double {
	const auto &data = bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *data.info;

	CV2TableProgressInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = data.handle ? data.handle->GetData() : nullptr;
	if (global_state) {
		// The callback runs concurrently with the scan, so the state it reads is the shared one, unsynchronized.
		args.in_global_state = global_state->Cast<CV2TableGlobalState>().handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.progress_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	// The API contract is a 0.0-1.0 fraction, the engine reads a 0-100 percentage.
	return MinValue<double>(MaxValue<double>(args.out_progress, 0.0), 1.0) * 100.0;
}

static auto CV2TableOfferFilters(ClientContext &context, const CV2TableFunctionData &bind_data,
                                 const vector<unique_ptr<Expression>> &filters, const vector<ColumnIndex> &column_ids)
    -> vector<bool> {
	const auto &info = *bind_data.info;

	CV2TableFilterPushdownInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	args.in_filters = &filters;
	args.in_column_ids = &column_ids;
	args.out_handled.resize(filters.size(), false);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.filter_pushdown_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
	return std::move(args.out_handled);
}

//! The optimizer hands over the predicates it would otherwise evaluate above the scan; whatever the callback accepts
//! is removed from the list, and the engine keeps applying the rest itself.
static auto CV2TableFilterPushdown(ClientContext &context, LogicalGet &get, FunctionData *bind_data_p,
                                   vector<unique_ptr<Expression>> &filters) -> void {
	if (filters.empty()) {
		return;
	}
	auto &bind_data = bind_data_p->Cast<CV2TableFunctionData>();
	const auto &column_ids = get.GetColumnIds();
	auto handled = CV2TableOfferFilters(context, bind_data, filters, column_ids);
	vector<unique_ptr<Expression>> accepted, remaining;
	for (idx_t i = 0; i < filters.size(); i++) {
		if (handled[i]) {
			accepted.push_back(std::move(filters[i]));
		} else {
			remaining.push_back(std::move(filters[i]));
		}
	}
	if (!accepted.empty()) {
		bind_data.accepted_filters.push_back(make_shared_ptr<CV2TableAcceptedFilterBatch>(
		    CV2TableAcceptedFilterBatch {column_ids, std::move(accepted)}));
	}
	filters = std::move(remaining);
}

static auto CV2TableReplayAcceptedFilters(ClientContext &context, const CV2TableFunctionData &bind_data) -> void {
	for (auto &batch : bind_data.accepted_filters) {
		auto handled = CV2TableOfferFilters(context, bind_data, batch->filters, batch->column_ids);
		if (!std::all_of(handled.begin(), handled.end(), [](bool accepted) { return accepted; })) {
			throw InvalidInputException("The filter pushdown callback of table function \"%s\" did not accept a "
			                            "previously accepted predicate when rebinding.",
			                            bind_data.info->name.GetIdentifierName());
		}
	}
}

static auto CV2TableSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
                              const BoundTableFunction &) -> void {
	const auto &bind_data = bind_data_p->Cast<CV2TableFunctionData>();
	serializer.WriteProperty(100, "parameters", bind_data.parameters);
	serializer.WriteProperty(101, "named_parameters", bind_data.named_parameters);
	serializer.WriteProperty(102, "column_types", bind_data.column_types);
	serializer.WriteProperty(103, "column_names", bind_data.column_names);
	serializer.WriteProperty(104, "accepted_filters", bind_data.accepted_filters);
}

static auto CV2TableDeserialize(Deserializer &deserializer, BoundTableFunction &function) -> unique_ptr<FunctionData> {
	auto parameters = deserializer.ReadProperty<vector<Value>>(100, "parameters");
	auto named_parameters = deserializer.ReadProperty<named_argument_map_t>(101, "named_parameters");
	auto column_types = deserializer.ReadProperty<vector<LogicalType>>(102, "column_types");
	auto column_names = deserializer.ReadProperty<vector<Identifier>>(103, "column_names");
	auto accepted_filters =
	    deserializer.ReadProperty<vector<shared_ptr<const CV2TableAcceptedFilterBatch>>>(104, "accepted_filters");
	auto &context = deserializer.Get<ClientContext &>();
	vector<LogicalType> input_types, return_types;
	vector<Identifier> input_names, return_names;
	TableFunctionRef ref;
	TableFunctionBindInput input(parameters, named_parameters, input_types, input_names, function.function_info.get(),
	                             nullptr, function, ref, nullptr);
	auto result = CV2TableBind(context, input, return_types, return_names);
	auto &bind_data = result->Cast<CV2TableFunctionData>();
	if (return_types != column_types || return_names != column_names) {
		throw SerializationException("Table function \"%s\" returned a different schema when rebinding",
		                             bind_data.info->name.GetIdentifierName());
	}
	bind_data.accepted_filters = std::move(accepted_filters);
	CV2TableReplayAcceptedFilters(context, bind_data);
	return result;
}

//! Batch index is required on every call, even when unrequested: the pipeline range-checks it regardless.
static auto CV2TakePartitionData(CV2TablePartitionDataInfo &args, const Identifier &function_name)
    -> OperatorPartitionData {
	if (!args.out_batch_index.IsValid()) {
		throw InvalidInputException("The partition data callback of table function %s did not set a batch index.",
		                            function_name);
	}
	OperatorPartitionData result(args.out_batch_index.GetIndex());
	result.partition_data.reserve(args.out_partition_values.size());
	for (idx_t i = 0; i < args.out_partition_values.size(); i++) {
		if (!args.out_partition_value_set[i]) {
			throw InvalidInputException("The partition data callback of table function %s did not set the "
			                            "partitioning column value at index %llu.",
			                            function_name, i);
		}
		result.partition_data.emplace_back(std::move(args.out_partition_values[i]));
	}
	return result;
}

static auto CV2TableGetPartitionData(ClientContext &context, TableFunctionGetPartitionInput &input)
    -> OperatorPartitionData {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	auto &global_state = input.global_state->Cast<CV2TableGlobalState>();
	auto &local_state = input.local_state->Cast<CV2TableLocalState>();

	CV2TablePartitionDataInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	args.in_global_state = global_state.handle.GetData();
	args.in_local_state = local_state.handle.GetData();
	args.in_partition_info = &input.partition_info;
	// The planner resolves these to scan positions, unlike get_partition_info's, which it resolves to declared
	// columns; the callback is promised declared columns in both.
	args.in_partition_columns = &local_state.PartitionColumns(input.partition_info, global_state.scan_columns);
	args.in_column_types = &bind_data.column_types;
	args.out_partition_value_set.resize(args.in_partition_columns->size(), false);
	args.out_partition_values.resize(args.in_partition_columns->size());

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.partition_data_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	auto result = CV2TakePartitionData(args, info.name);
	local_state.CheckPartitionData(info.name, result);
	return result;
}

//! Runs at plan time and must be deterministic: the optimizer may call it for a plan shape it later discards.
static auto CV2TableGetPartitionInfo(ClientContext &context, TableFunctionPartitionInput &input) -> TablePartitionInfo {
	const auto &bind_data = input.bind_data->Cast<CV2TableFunctionData>();
	const auto &info = *bind_data.info;

	CV2TablePartitioningInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	args.in_partition_ids = &input.partition_ids;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.partitioning_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return args.out_partition_info;
}

class CV2TableFunction {
public:
	explicit CV2TableFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the table function to register on `target`.
	TableFunction Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "table function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (!info.bind_cb) {
			throw InvalidInputException("Bind callback must be set for the function.");
		}
		if (!info.exec_cb) {
			throw InvalidInputException("Exec callback must be set for the function.");
		}
		// The engine calls get_partition_data without a null check once get_partition_info claims partitions.
		if (info.partitioning_cb && !info.partition_data_cb) {
			throw InvalidInputException("Partition data callback must be set when the partitioning callback is set.");
		}
		// A table function declares the columns it returns from its bind callback, not through a return type.
		if (signature.GetReturnType().id() != LogicalTypeId::INVALID) {
			throw InvalidInputException("A table function signature cannot set a return type.");
		}

		signature.Verify();

		// A table function holds a signature of its own, so the declared one is handed over as-is
		TableFunction function(name, {}, CV2TableExec, CV2TableBind, CV2TableInitGlobal, CV2TableInitLocal);
		function.GetSignature() = signature;

		// Always wired: it serves the estimate a bind callback may set, which is not known at registration. It
		// reports no estimate when the bind callback sets none.
		function.cardinality = CV2TableCardinality;

		function.SetSerializeCallback(CV2TableSerialize);
		function.SetDeserializeCallback(CV2TableDeserialize);
		if (info.progress_cb) {
			function.table_scan_progress = CV2TableProgress;
		}
		if (info.filter_pushdown_cb) {
			function.pushdown_complex_filter = CV2TableFilterPushdown;
		}
		if (info.partition_data_cb) {
			function.get_partition_data = CV2TableGetPartitionData;
		}
		if (info.partitioning_cb) {
			function.get_partition_info = CV2TableGetPartitionInfo;
		}
		// The extended variant: setting the plain one would turn off pushing field extraction into the scan
		if (info.stats_cb) {
			function.statistics_extended = CV2TableStats;
		}
		function.projection_pushdown = info.projection_pushdown;
		if (info.get_bind_info_cb) {
			function.get_bind_info = CV2TableGetBindInfo;
		}

		info.name = name;
		function.function_info = make_shared_ptr<CV2TableFunctionInfo>(std::move(info));

		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2TableFunctionInfo info;
	Identifier name;
};

static auto Convert(duckdb_v2_table_function_handle func) -> CV2TableFunction * {
	return reinterpret_cast<CV2TableFunction *>(func);
}
static auto Convert(CV2TableFunction *func) -> duckdb_v2_table_function_handle {
	return reinterpret_cast<duckdb_v2_table_function_handle>(func);
}

//----------------------------------------------------------------------------------------------------------------------
// Multi-file functions
//----------------------------------------------------------------------------------------------------------------------
//! The overloads of a table function catalog entry, copied so they outlive the transaction that read them.
static auto CopyTableFunctions(optional_ptr<CatalogEntry> entry) -> unique_ptr<TableFunctionSet> {
	if (!entry) {
		return nullptr;
	}
	return make_uniq<TableFunctionSet>(entry->Cast<TableFunctionCatalogEntry>().functions);
}

class CV2MultiFileFunction {
public:
	explicit CV2MultiFileFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the function set to register on `target`, wrapping the overloads of
	//! the single-file function registered there.
	TableFunctionSet Build(DatabaseInstance &target, unique_ptr<TableFunctionSet> single_file_overloads) {
		CheckRegistrationTarget(db, target, "multi-file function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (single_file_function.empty()) {
			throw InvalidInputException("The single-file function must be set for the multi-file function.");
		}
		TableFunctionMultiFileSettings settings;
		settings.reader_type = reader_type;
		if (!file_extension.empty()) {
			settings.glob_input = FileGlobInput(FileGlobOptions::FALLBACK_GLOB, file_extension);
		}
		auto single_file = SelectSingleFileFunction(std::move(single_file_overloads));
		if (single_file.function == CV2TableExec && single_file.function_info) {
			// the multi-file scan claims the batches of a C API function that scans one batch at a time
			if (single_file.function_info->Cast<CV2TableFunctionInfo>().claim_batch_cb) {
				settings.claim_batch = CV2TableClaimBatch;
			}
		}
		auto function = TableFunctionMultiFileWrapper::CreateFunctionSet(std::move(single_file), name, settings);
		return function;
	}

private:
	//! The overload of the single-file function that takes the path of the file to read
	TableFunction SelectSingleFileFunction(unique_ptr<TableFunctionSet> overloads) const {
		if (!overloads) {
			throw InvalidInputException("Table function %s to read single files with does not exist.",
			                            single_file_function);
		}
		for (auto &function : overloads->functions) {
			auto &signature = function->GetSignature();
			if (signature.GetRequiredParameterCount() != 1 || !signature.GetParameter(0).AcceptsPosition()) {
				continue;
			}
			auto &type = signature.GetParameter(0).GetType();
			if (type == LogicalType::VARCHAR || type.id() == LogicalTypeId::ANY) {
				return *function;
			}
		}
		throw InvalidInputException(
		    "Table function %s cannot read single files: it must take the file to read as its only positional "
		    "parameter, of type ANY (or VARCHAR).",
		    single_file_function);
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	Identifier name;
	Identifier single_file_function;
	string reader_type;
	string file_extension;
};

static auto Convert(duckdb_v2_multi_file_function_handle func) -> CV2MultiFileFunction * {
	return reinterpret_cast<CV2MultiFileFunction *>(func);
}
static auto Convert(CV2MultiFileFunction *func) -> duckdb_v2_multi_file_function_handle {
	return reinterpret_cast<duckdb_v2_multi_file_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_table_function_create(duckdb_v2_factory_handle factory,
                                                duckdb_v2_table_function_handle *function,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2TableFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_name(duckdb_v2_table_function_handle function, const duckdb_v2_str *name,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_get_docs(duckdb_v2_table_function_handle function,
                                                  duckdb_v2_function_docs_handle *docs,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_get_signature(duckdb_v2_table_function_handle function,
                                                       duckdb_v2_function_signature_handle *sig,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_user_data(duckdb_v2_table_function_handle function, duckdb_v2_opaque *data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_bind_callback(duckdb_v2_table_function_handle function,
                                                           duckdb_v2_table_function_bind_callback_fn callback,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.bind_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_init_global_callback(duckdb_v2_table_function_handle function,
                                                  duckdb_v2_table_function_init_global_callback_fn callback,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.init_global_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_init_local_callback(duckdb_v2_table_function_handle function,
                                                 duckdb_v2_table_function_init_local_callback_fn callback,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.init_local_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_exec_callback(duckdb_v2_table_function_handle function,
                                                           duckdb_v2_table_function_exec_callback_fn callback,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_progress_callback(duckdb_v2_table_function_handle function,
                                                               duckdb_v2_table_function_progress_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.progress_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_projection_pushdown(duckdb_v2_table_function_handle function, bool enable,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.projection_pushdown = enable; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_filter_pushdown_callback(duckdb_v2_table_function_handle function,
                                                      duckdb_v2_table_function_filter_pushdown_callback_fn callback,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.filter_pushdown_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_partition_data_callback(duckdb_v2_table_function_handle function,
                                                     duckdb_v2_table_function_partition_data_callback_fn callback,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.partition_data_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_partitioning_callback(duckdb_v2_table_function_handle function,
                                                   duckdb_v2_table_function_partitioning_callback_fn callback,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.partitioning_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_set_stats_callback(duckdb_v2_table_function_handle function,
                                                            duckdb_v2_table_function_stats_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.stats_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_bind_add_result_column(duckdb_v2_table_function_bind_info_handle info,
                                                                const duckdb_v2_identifier_t *name,
                                                                duckdb_v2_logical_type_handle type,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	DUCKDB_CHECK_ARG(type);
	return WithErrorHandler(err, [&]() {
		auto column_type_ref = Convert(type);
		const auto &column_type = *column_type_ref;
		// A result column carries data, so a wildcard or otherwise incomplete type has no meaning here.
		if (!column_type.IsComplete()) {
			throw duckdb::InvalidInputException("Result column type must be a fully defined concrete type");
		}
		auto &bind_info = *Convert(info);
		bind_info.out_column_names.emplace_back(ConvertIdentifierName(name));
		bind_info.out_column_types.push_back(column_type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_bind_set_cardinality(duckdb_v2_table_function_bind_info_handle info,
                                                              idx_t cardinality, bool is_exact,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		bind_info.out_cardinality = cardinality;
		bind_info.out_cardinality_is_exact = is_exact;
		bind_info.out_cardinality_set = true;
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_bind_set_order_preservation(duckdb_v2_table_function_bind_info_handle info,
                                                                     DUCKDB_V2_ORDER_PRESERVATION order,
                                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_order_preservation = CV2ConvertOrderPreservation(order); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_get_user_data(duckdb_v2_table_function_init_global_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_get_bind_data(duckdb_v2_table_function_init_global_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_set_global_state(duckdb_v2_table_function_init_global_info_handle info,
                                                      duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_global_state = *data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_set_max_threads(duckdb_v2_table_function_init_global_info_handle info,
                                                     idx_t max_threads, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		if (max_threads == 0) {
			throw duckdb::InvalidInputException("The maximum number of threads must be at least 1");
		}
		Convert(info)->out_max_threads = max_threads;
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_get_column_count(duckdb_v2_table_function_init_global_info_handle info,
                                                      idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_scan_columns->size(); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_global_get_column_index(duckdb_v2_table_function_init_global_info_handle info,
                                                      idx_t index, idx_t *column_index,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_scan_columns;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_init_global_get_column_index");
		}
		*column_index = columns[index];
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_init_local_get_user_data(duckdb_v2_table_function_init_local_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_init_local_get_bind_data(duckdb_v2_table_function_init_local_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_local_get_global_state(duckdb_v2_table_function_init_local_info_handle info, void **data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_local_set_local_state(duckdb_v2_table_function_init_local_info_handle info,
                                                    duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_local_state = *data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_local_get_column_count(duckdb_v2_table_function_init_local_info_handle info, idx_t *count,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_scan_columns->size(); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_init_local_get_column_index(duckdb_v2_table_function_init_local_info_handle info, idx_t index,
                                                     idx_t *column_index, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_scan_columns;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_init_local_get_column_index");
		}
		*column_index = columns[index];
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_user_data(duckdb_v2_table_function_exec_info_handle info, void **data,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_bind_data(duckdb_v2_table_function_exec_info_handle info, void **data,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_global_state(duckdb_v2_table_function_exec_info_handle info,
                                                               void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_local_state(duckdb_v2_table_function_exec_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_local_state; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_output_chunk(duckdb_v2_table_function_exec_info_handle info,
                                                               duckdb_v2_data_chunk_handle *chunk,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(chunk);
	return WithErrorHandler(err, [&]() { *chunk = Convert(Convert(info)->output); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_column_count(duckdb_v2_table_function_exec_info_handle info,
                                                               idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_scan_columns->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_exec_get_column_index(duckdb_v2_table_function_exec_info_handle info,
                                                               idx_t index, idx_t *column_index,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_scan_columns;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_exec_get_column_index");
		}
		*column_index = columns[index];
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_progress_get_user_data(duckdb_v2_table_function_progress_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_progress_get_bind_data(duckdb_v2_table_function_progress_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_progress_get_global_state(duckdb_v2_table_function_progress_info_handle info,
                                                                   void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_progress_set_progress(duckdb_v2_table_function_progress_info_handle info,
                                                               double progress, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_progress = progress; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_user_data(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                       void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_bind_data(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                       void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_filter_count(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                          idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_filters->size(); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_filter(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                    idx_t index, duckdb_v2_expression_handle *filter,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(filter);
	*filter = nullptr;
	return WithErrorHandler(err, [&]() {
		const auto &filters = *Convert(info)->in_filters;
		if (index >= filters.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_filter_pushdown_get_filter");
		}
		*filter = Convert(filters[index].get());
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_accept(duckdb_v2_table_function_filter_pushdown_info_handle info, idx_t index,
                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &handled = Convert(info)->out_handled;
		if (index >= handled.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_filter_pushdown_accept");
		}
		handled[index] = true;
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_column_count(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                          idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_column_ids->size(); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_filter_pushdown_get_column_index(duckdb_v2_table_function_filter_pushdown_info_handle info,
                                                          idx_t index, idx_t *column_index,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_column_ids;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_filter_pushdown_get_column_index");
		}
		// A function registered here declares no virtual columns, so every entry is a declared column.
		*column_index = columns[index].GetPrimaryIndex();
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_get_user_data(duckdb_v2_table_function_partition_data_info_handle info,
                                                      void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_get_bind_data(duckdb_v2_table_function_partition_data_info_handle info,
                                                      void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_get_global_state(duckdb_v2_table_function_partition_data_info_handle info,
                                                         void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_get_local_state(duckdb_v2_table_function_partition_data_info_handle info,
                                                        void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_local_state; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_requires_batch_index(duckdb_v2_table_function_partition_data_info_handle info,
                                                             bool *required, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(required);
	return WithErrorHandler(err, [&]() { *required = Convert(info)->in_partition_info->RequiresBatchIndex(); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_partition_data_requires_partition_columns(
    duckdb_v2_table_function_partition_data_info_handle info, bool *required, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(required);
	return WithErrorHandler(err, [&]() { *required = Convert(info)->in_partition_info->RequiresPartitionColumns(); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_partition_data_get_partition_column_count(
    duckdb_v2_table_function_partition_data_info_handle info, idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_partition_columns->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_partition_data_get_partition_column_index(
    duckdb_v2_table_function_partition_data_info_handle info, idx_t index, idx_t *column_index,
    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_partition_columns;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_partition_data_get_partition_column_index");
		}
		*column_index = columns[index];
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_set_batch_index(duckdb_v2_table_function_partition_data_info_handle info,
                                                        idx_t batch_index, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		// ToPipelinePartitionData adds base + 1 and throws an InternalException from BATCH_INCREMENT - 1 on.
		if (batch_index >= duckdb::PipelineBuildState::BATCH_INCREMENT - 2) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_table_function_partition_data_set_batch_index: batch_index %llu must be less than "
			    "%llu",
			    batch_index, duckdb::PipelineBuildState::BATCH_INCREMENT - 2);
		}
		Convert(info)->out_batch_index = batch_index;
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partition_data_set_partition_value(duckdb_v2_table_function_partition_data_info_handle info,
                                                            idx_t index, duckdb_v2_value_handle value,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		if (index >= args.out_partition_values.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_partition_data_set_partition_value");
		}
		const auto &expected_type = (*args.in_column_types)[(*args.in_partition_columns)[index]];
		const auto &actual = *Convert(value);
		if (actual.type() != expected_type) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_table_function_partition_data_set_partition_value: expected type %s for the "
			    "partitioning column at index %llu, got %s",
			    expected_type.ToString(), index, actual.type().ToString());
		}
		args.out_partition_values[index] = actual;
		args.out_partition_value_set[index] = true;
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partitioning_get_user_data(duckdb_v2_table_function_partitioning_info_handle info, void **data,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partitioning_get_bind_data(duckdb_v2_table_function_partitioning_info_handle info, void **data,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partitioning_get_partition_column_count(duckdb_v2_table_function_partitioning_info_handle info,
                                                                 idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_partition_ids->size(); });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partitioning_get_partition_column_index(duckdb_v2_table_function_partitioning_info_handle info,
                                                                 idx_t index, idx_t *column_index,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() {
		const auto &columns = *Convert(info)->in_partition_ids;
		if (index >= columns.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_partitioning_get_partition_column_index");
		}
		*column_index = columns[index];
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_partitioning_set_partition_info(duckdb_v2_table_function_partitioning_info_handle info,
                                                         DUCKDB_V2_TABLE_PARTITION_INFO partition_info,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err,
	                        [&]() { Convert(info)->out_partition_info = CV2ConvertPartitionInfo(partition_info); });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_stats_get_user_data(duckdb_v2_table_function_stats_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_stats_get_bind_data(duckdb_v2_table_function_stats_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_stats_get_column_index(duckdb_v2_table_function_stats_info_handle info,
                                                                idx_t *column_index, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(column_index);
	return WithErrorHandler(err, [&]() { *column_index = Convert(info)->in_column_index; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_stats_get_result_stats(duckdb_v2_table_function_stats_info_handle info,
                                                                duckdb_v2_stats_handle *stats,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() { *stats = Convert(&Convert(info)->result_stats); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_table_function(duckdb_v2_connection_handle conn,
                                                             duckdb_v2_table_function_handle function,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &self = *Convert(function);
			auto info = duckdb::CreateTableFunctionInfo(self.Build(*context.db));
			self.docs.AddTo(self.signature, info);
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			// CreateFunction, unlike CreateTableFunction, adds the overloads to an existing function.
			catalog.CreateFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_table_function(duckdb_v2_extension_handle extension,
                                                            duckdb_v2_table_function_handle function,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		duckdb::CreateTableFunctionInfo info(self.Build(loader.GetDatabaseInstance()));
		self.docs.AddTo(self.signature, info);
		info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_table_function_destroy(duckdb_v2_table_function_handle *function) {
	return WithErrorHandler(nullptr, [&]() {
		if (!function) {
			return;
		}
		if (*function) {
			delete Convert(*function);
			*function = nullptr;
		}
	});
}

//----------------------------------------------------------------------------------------------------------------------
// Batch claiming
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_claim_batch_callback(duckdb_v2_table_function_handle function,
                                                  duckdb_v2_table_function_claim_batch_callback_fn callback,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.claim_batch_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_claim_batch_get_user_data(duckdb_v2_table_function_claim_batch_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_claim_batch_get_bind_data(duckdb_v2_table_function_claim_batch_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_claim_batch_get_global_state(duckdb_v2_table_function_claim_batch_info_handle info,
                                                      void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_claim_batch_get_local_state(duckdb_v2_table_function_claim_batch_info_handle info, void **data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_local_state; });
}

DUCKDB_V2_ERROR duckdb_v2_table_function_claim_batch_set_claimed(duckdb_v2_table_function_claim_batch_info_handle info,
                                                                 bool claimed, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_claimed = claimed; });
}

//----------------------------------------------------------------------------------------------------------------------
// File readers
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR
duckdb_v2_table_function_set_get_bind_info_callback(duckdb_v2_table_function_handle function,
                                                    duckdb_v2_table_function_get_bind_info_callback_fn callback,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.get_bind_info_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_get_bind_info_get_user_data(duckdb_v2_table_function_get_bind_info_info_handle info,
                                                     void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_get_bind_info_get_bind_data(duckdb_v2_table_function_get_bind_info_info_handle info,
                                                     void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_get_bind_info_set_column_identifier(duckdb_v2_table_function_get_bind_info_info_handle info,
                                                             idx_t column_index, const idx_t *child_path,
                                                             idx_t child_path_length, duckdb_v2_value_handle identifier,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(identifier);
	return WithErrorHandler(err, [&]() {
		auto &args = *Convert(info);
		auto &bind_data = args.bind_data;
		if (column_index >= bind_data.column_types.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_table_function_get_bind_info_set_column_identifier: result column "
			    "%llu has not been declared",
			    column_index);
		}
		if (child_path_length > 0 && !child_path) {
			throw duckdb::InvalidInputException("duckdb_v2_table_function_get_bind_info_set_column_identifier: the "
			                                    "child path cannot be null unless it is empty");
		}
		const auto &value = *Convert(identifier);
		const auto identifier_type = value.type().id();
		if (value.IsNull() ||
		    (identifier_type != duckdb::LogicalTypeId::INTEGER && identifier_type != duckdb::LogicalTypeId::VARCHAR)) {
			throw duckdb::InvalidInputException("duckdb_v2_table_function_get_bind_info_set_column_identifier: an "
			                                    "identifier must be a non-NULL INTEGER or VARCHAR, got %s",
			                                    value.ToString());
		}
		duckdb::vector<duckdb::idx_t> path(child_path, child_path + child_path_length);
		// verify the path addresses a nested field of the column, so a mistake is reported where it is made
		auto definition = duckdb::MultiFileColumnDefinition::CreateNested(bind_data.column_names[column_index],
		                                                                  bind_data.column_types[column_index]);
		definition.ResolveChildPath(path);

		auto &identifiers = args.out_bind_info.column_identifiers;
		for (auto &entry : identifiers) {
			if (entry.column_index == column_index && entry.child_path == path) {
				entry.identifier = value;
				return;
			}
		}
		identifiers.push_back({column_index, std::move(path), value});
	});
}

DUCKDB_V2_ERROR
duckdb_v2_table_function_get_bind_info_set_option(duckdb_v2_table_function_get_bind_info_info_handle info,
                                                  duckdb_v2_str *key, duckdb_v2_value_handle value,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(key);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(
	    err, [&]() { Convert(info)->out_bind_info.options[duckdb::string(Convert(*key))] = *Convert(value); });
}

//----------------------------------------------------------------------------------------------------------------------
// Multi-file functions
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_create(duckdb_v2_factory_handle factory,
                                                     duckdb_v2_multi_file_function_handle *function,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2MultiFileFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_set_name(duckdb_v2_multi_file_function_handle function,
                                                       duckdb_v2_str *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(Convert(*name)); });
}

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_set_single_file_function(duckdb_v2_multi_file_function_handle function,
                                                                       duckdb_v2_str *name,
                                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err,
	                        [&]() { Convert(function)->single_file_function = duckdb::Identifier(Convert(*name)); });
}

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_set_reader_type(duckdb_v2_multi_file_function_handle function,
                                                              duckdb_v2_str *reader_type,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(reader_type);
	return WithErrorHandler(err, [&]() { Convert(function)->reader_type = duckdb::string(Convert(*reader_type)); });
}

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_set_file_extension(duckdb_v2_multi_file_function_handle function,
                                                                 duckdb_v2_str *extension,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(extension);
	return WithErrorHandler(err, [&]() {
		auto file_extension = duckdb::string(Convert(*extension));
		if (duckdb::StringUtil::StartsWith(file_extension, ".")) {
			throw duckdb::InvalidInputException("The file extension \"%s\" must be given without a leading dot",
			                                    file_extension);
		}
		Convert(function)->file_extension = std::move(file_extension);
	});
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_multi_file_function(duckdb_v2_connection_handle conn,
                                                                  duckdb_v2_multi_file_function_handle function,
                                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			auto single_file = catalog.GetEntry(context, duckdb::CatalogType::TABLE_FUNCTION_ENTRY,
			                                    duckdb::Identifier::DefaultSchema(), self.single_file_function,
			                                    duckdb::OnEntryNotFound::RETURN_NULL);
			duckdb::CreateTableFunctionInfo info(self.Build(*context.db, CopyTableFunctions(single_file)));
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			// CreateFunction, unlike CreateTableFunction, adds the overloads to an existing function.
			catalog.CreateFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_multi_file_function(duckdb_v2_extension_handle extension,
                                                                 duckdb_v2_multi_file_function_handle function,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		auto single_file = CopyTableFunctions(loader.TryGetTableFunction(self.single_file_function));
		loader.RegisterFunction(self.Build(loader.GetDatabaseInstance(), std::move(single_file)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_multi_file_function_destroy(duckdb_v2_multi_file_function_handle *function) {
	return WithErrorHandler(nullptr, [&]() {
		if (!function) {
			return;
		}
		if (*function) {
			delete Convert(*function);
			*function = nullptr;
		}
	});
}
