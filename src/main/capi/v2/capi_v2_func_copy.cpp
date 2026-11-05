#include "duckdb/common/optional_idx.hpp"
#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/parser/parsed_data/create_copy_function_info.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <algorithm>

namespace duckdb::capiv2 {

class CV2CopyFunctionInfo final : public CopyFunctionInfo {
public:
	// COPY ... TO
	duckdb_v2_copy_to_bind_callback_fn to_bind_cb = nullptr;
	duckdb_v2_copy_to_batch_size_callback_fn to_batch_size_cb = nullptr;
	duckdb_v2_copy_to_init_callback_fn to_init_cb = nullptr;
	duckdb_v2_copy_to_batch_callback_fn to_batch_cb = nullptr;
	duckdb_v2_copy_to_flush_callback_fn to_flush_cb = nullptr;
	duckdb_v2_copy_to_finalize_callback_fn to_finalize_cb = nullptr;
	duckdb_v2_copy_to_statistics_callback_fn to_statistics_cb = nullptr;
	// COPY ... FROM
	duckdb_v2_copy_from_bind_callback_fn from_bind_cb = nullptr;
	duckdb_v2_copy_from_init_global_callback_fn from_init_global_cb = nullptr;
	duckdb_v2_copy_from_init_local_callback_fn from_init_local_cb = nullptr;
	duckdb_v2_copy_from_exec_callback_fn from_exec_cb = nullptr;
	duckdb_v2_copy_from_progress_callback_fn from_progress_cb = nullptr;

	shared_ptr<CV2UserData> user_data = nullptr;

	bool HasCopyTo() const {
		return to_bind_cb || to_batch_size_cb || to_init_cb || to_batch_cb || to_flush_cb || to_finalize_cb ||
		       to_statistics_cb;
	}
	bool HasCopyFrom() const {
		return from_bind_cb || from_init_global_cb || from_init_local_cb || from_exec_cb || from_progress_cb;
	}
};

//! The reader table function behind the COPY FROM side only receives itself in its bind input, so it carries the
//! function info along to reach the callbacks.
class CV2CopyFromTableInfo final : public TableFunctionInfo {
public:
	explicit CV2CopyFromTableInfo(shared_ptr<CopyFunctionInfo> info) : info(std::move(info)) {
	}
	shared_ptr<CopyFunctionInfo> info;
};

//! The bind data of a bound COPY statement, in either direction. Besides the user's own bind data it shares
//! ownership of the function info: the later hooks only receive the bind data, so it is their only route to the
//! callbacks.
class CV2CopyFunctionData final : public FunctionData {
public:
	shared_ptr<CV2UserData> handle;
	shared_ptr<CopyFunctionInfo> info;

	// The static estimate a COPY FROM bind callback may set via copy_from_bind_set_cardinality.
	idx_t cardinality = 0;
	bool cardinality_is_exact = false;
	bool cardinality_set = false;

	auto Copy() const -> unique_ptr<FunctionData> override {
		auto copy = make_uniq<CV2CopyFunctionData>();
		copy->handle = handle;
		copy->info = info;
		copy->cardinality = cardinality;
		copy->cardinality_is_exact = cardinality_is_exact;
		copy->cardinality_set = cardinality_set;
		return std::move(copy);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		const auto &other_data = other.Cast<CV2CopyFunctionData>();
		return handle && other_data.handle && handle->Equals(*other_data.handle);
	}
};

class CV2CopyToGlobalState final : public GlobalFunctionData {
public:
	CV2UserData handle;
	//! Where the statistics of the file are reported, when the statement asks for them
	optional_ptr<CopyFunctionFileStatistics> written_statistics;
	//! Serializes the flushes of callers that sink into the file directly (see CV2CopyToSink)
	mutex flush_lock;
};

//! The local state of callers that sink into a file directly rather than through the batch interface
class CV2CopyToLocalState final : public LocalFunctionData {};

class CV2CopyToBatchData final : public PreparedBatchData {
public:
	CV2UserData handle;
};

class CV2CopyFromGlobalState final : public GlobalTableFunctionState {
public:
	auto MaxThreads() const -> idx_t override {
		return max_threads;
	}

	CV2UserData handle;
	idx_t max_threads = 1;
};

class CV2CopyFromLocalState final : public LocalTableFunctionState {
public:
	CV2UserData handle;
};

//! The statement's options, snapshotted for the duration of a bind callback and ordered by name so that indices
//! are predictable.
using CV2CopyOptionList = vector<std::pair<const Identifier *, const vector<Value> *>>;

static auto CV2CopyCollectOptions(const identifier_map_t<vector<Value>> &options) -> CV2CopyOptionList {
	CV2CopyOptionList result;
	for (auto &entry : options) {
		result.emplace_back(&entry.first, &entry.second);
	}
	std::sort(result.begin(), result.end(),
	          [](const CV2CopyOptionList::value_type &a, const CV2CopyOptionList::value_type &b) {
		          return StringUtil::Lower(a.first->GetIdentifierName()) <
		                 StringUtil::Lower(b.first->GetIdentifierName());
	          });
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO callback info
//----------------------------------------------------------------------------------------------------------------------

class CV2CopyToBindInfo {
public:
	void *in_user_data = nullptr;
	const string *in_file_path = nullptr;
	const vector<Identifier> *in_names = nullptr;
	const vector<LogicalType> *in_types = nullptr;
	CV2CopyOptionList in_options;

	shared_ptr<CV2UserData> out_bind_data;
};

static auto Convert(duckdb_v2_copy_to_bind_info_handle info) -> CV2CopyToBindInfo * {
	return reinterpret_cast<CV2CopyToBindInfo *>(info);
}
static auto Convert(CV2CopyToBindInfo *info) -> duckdb_v2_copy_to_bind_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_bind_info_handle>(info);
}

class CV2CopyToBatchSizeInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	optional_idx out_target;
};

static auto Convert(duckdb_v2_copy_to_batch_size_info_handle info) -> CV2CopyToBatchSizeInfo * {
	return reinterpret_cast<CV2CopyToBatchSizeInfo *>(info);
}
static auto Convert(CV2CopyToBatchSizeInfo *info) -> duckdb_v2_copy_to_batch_size_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_batch_size_info_handle>(info);
}

class CV2CopyToInitInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	const string *in_file_path = nullptr;

	duckdb_v2_opaque out_init_data = {};
};

static auto Convert(duckdb_v2_copy_to_init_info_handle info) -> CV2CopyToInitInfo * {
	return reinterpret_cast<CV2CopyToInitInfo *>(info);
}
static auto Convert(CV2CopyToInitInfo *info) -> duckdb_v2_copy_to_init_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_init_info_handle>(info);
}

class CV2CopyToBatchInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;
	// The batch, until the callback takes it; whatever is left is destroyed with the args.
	unique_ptr<ColumnDataCollection> in_collection;

	duckdb_v2_opaque out_batch_data = {};
};

static auto Convert(duckdb_v2_copy_to_batch_info_handle info) -> CV2CopyToBatchInfo * {
	return reinterpret_cast<CV2CopyToBatchInfo *>(info);
}
static auto Convert(CV2CopyToBatchInfo *info) -> duckdb_v2_copy_to_batch_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_batch_info_handle>(info);
}

class CV2CopyToFlushInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;
	void *in_batch_data = nullptr;
};

static auto Convert(duckdb_v2_copy_to_flush_info_handle info) -> CV2CopyToFlushInfo * {
	return reinterpret_cast<CV2CopyToFlushInfo *>(info);
}
static auto Convert(CV2CopyToFlushInfo *info) -> duckdb_v2_copy_to_flush_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_flush_info_handle>(info);
}

class CV2CopyToFinalizeInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;
};

static auto Convert(duckdb_v2_copy_to_finalize_info_handle info) -> CV2CopyToFinalizeInfo * {
	return reinterpret_cast<CV2CopyToFinalizeInfo *>(info);
}
static auto Convert(CV2CopyToFinalizeInfo *info) -> duckdb_v2_copy_to_finalize_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_finalize_info_handle>(info);
}

class CV2CopyToStatisticsInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;

	idx_t out_row_count = 0;
	idx_t out_file_size_bytes = 0;
};

static auto Convert(duckdb_v2_copy_to_statistics_info_handle info) -> CV2CopyToStatisticsInfo * {
	return reinterpret_cast<CV2CopyToStatisticsInfo *>(info);
}
static auto Convert(CV2CopyToStatisticsInfo *info) -> duckdb_v2_copy_to_statistics_info_handle {
	return reinterpret_cast<duckdb_v2_copy_to_statistics_info_handle>(info);
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM callback info
//----------------------------------------------------------------------------------------------------------------------

class CV2CopyFromBindInfo {
public:
	void *in_user_data = nullptr;
	const string *in_file_path = nullptr;
	const vector<Identifier> *in_names = nullptr;
	const vector<LogicalType> *in_types = nullptr;
	CV2CopyOptionList in_options;

	shared_ptr<CV2UserData> out_bind_data;
	idx_t out_cardinality = 0;
	bool out_cardinality_is_exact = false;
	bool out_cardinality_set = false;
};

static auto Convert(duckdb_v2_copy_from_bind_info_handle info) -> CV2CopyFromBindInfo * {
	return reinterpret_cast<CV2CopyFromBindInfo *>(info);
}
static auto Convert(CV2CopyFromBindInfo *info) -> duckdb_v2_copy_from_bind_info_handle {
	return reinterpret_cast<duckdb_v2_copy_from_bind_info_handle>(info);
}

class CV2CopyFromInitGlobalInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	duckdb_v2_opaque out_global_state = {};
	idx_t out_max_threads = 1;
};

static auto Convert(duckdb_v2_copy_from_init_global_info_handle info) -> CV2CopyFromInitGlobalInfo * {
	return reinterpret_cast<CV2CopyFromInitGlobalInfo *>(info);
}
static auto Convert(CV2CopyFromInitGlobalInfo *info) -> duckdb_v2_copy_from_init_global_info_handle {
	return reinterpret_cast<duckdb_v2_copy_from_init_global_info_handle>(info);
}

class CV2CopyFromInitLocalInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;

	duckdb_v2_opaque out_local_state = {};
};

static auto Convert(duckdb_v2_copy_from_init_local_info_handle info) -> CV2CopyFromInitLocalInfo * {
	return reinterpret_cast<CV2CopyFromInitLocalInfo *>(info);
}
static auto Convert(CV2CopyFromInitLocalInfo *info) -> duckdb_v2_copy_from_init_local_info_handle {
	return reinterpret_cast<duckdb_v2_copy_from_init_local_info_handle>(info);
}

class CV2CopyFromExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;
	void *in_local_state = nullptr;

	DataChunk *output = nullptr;
};

static auto Convert(duckdb_v2_copy_from_exec_info_handle info) -> CV2CopyFromExecInfo * {
	return reinterpret_cast<CV2CopyFromExecInfo *>(info);
}
static auto Convert(CV2CopyFromExecInfo *info) -> duckdb_v2_copy_from_exec_info_handle {
	return reinterpret_cast<duckdb_v2_copy_from_exec_info_handle>(info);
}

class CV2CopyFromProgressInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_global_state = nullptr;

	double out_progress = 0.0;
};

static auto Convert(duckdb_v2_copy_from_progress_info_handle info) -> CV2CopyFromProgressInfo * {
	return reinterpret_cast<CV2CopyFromProgressInfo *>(info);
}
static auto Convert(CV2CopyFromProgressInfo *info) -> duckdb_v2_copy_from_progress_info_handle {
	return reinterpret_cast<duckdb_v2_copy_from_progress_info_handle>(info);
}

//----------------------------------------------------------------------------------------------------------------------
// Shared helpers
//----------------------------------------------------------------------------------------------------------------------

static auto GetFunctionInfo(const FunctionData &bind_data) -> const CV2CopyFunctionInfo & {
	return bind_data.Cast<CV2CopyFunctionData>().info->Cast<CV2CopyFunctionInfo>();
}

static auto GetUserData(const CV2CopyFunctionInfo &info) -> void * {
	return info.user_data ? info.user_data->GetData() : nullptr;
}

static auto GetUserBindData(const FunctionData &bind_data) -> void * {
	const auto &handle = bind_data.Cast<CV2CopyFunctionData>().handle;
	return handle ? handle->GetData() : nullptr;
}

static auto MakeBindData(const shared_ptr<CopyFunctionInfo> &info, shared_ptr<CV2UserData> handle)
    -> unique_ptr<CV2CopyFunctionData> {
	auto result = make_uniq<CV2CopyFunctionData>();
	result->info = info;
	result->handle = std::move(handle);
	return result;
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO hooks
//----------------------------------------------------------------------------------------------------------------------

static auto CV2CopyToBind(ClientContext &context, CopyFunctionBindInput &input, const vector<Identifier> &names,
                          const vector<LogicalType> &sql_types) -> unique_ptr<FunctionData> {
	if (!input.function_info) {
		// the bind reaches the callbacks through the function info, which a caller that binds the copy function
		// itself has to pass along
		throw InvalidInputException("The COPY TO bind of a copy function was called without its function info");
	}
	const auto &info = input.function_info->Cast<CV2CopyFunctionInfo>();

	CV2CopyToBindInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_file_path = &input.info.file_path;
	args.in_names = &names;
	args.in_types = &sql_types;
	args.in_options = CV2CopyCollectOptions(input.info.options);

	// The bind callback is optional: without one, the statement carries no bind data.
	CV2ErrorInfo err = {};
	if (info.to_bind_cb) {
		auto err_ptr = Convert(&err);
		CV2CallbackContext callback_context(context);
		info.to_bind_cb(Convert(&args), Convert(&callback_context), &err_ptr);
	}

	auto result = MakeBindData(input.function_info, std::move(args.out_bind_data));
	if (err.HasError()) {
		err.ThrowAsException();
	}
	return std::move(result);
}

static auto CV2CopyToBatchSize(ClientContext &context, FunctionData &bind_data) -> optional_idx {
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyToBatchSizeInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.to_batch_size_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
	if (args.out_target.IsValid() && args.out_target == 0) {
		throw InvalidInputException("The batch size callback must set a target greater than 0.");
	}
	return args.out_target;
}

static auto CV2CopyToInitGlobal(ClientContext &context, FunctionData &bind_data, const string &file_path)
    -> unique_ptr<GlobalFunctionData> {
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyToInitInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	args.in_file_path = &file_path;

	// The init callback is optional: without one, the file carries no init data.
	CV2ErrorInfo err = {};
	if (info.to_init_cb) {
		auto err_ptr = Convert(&err);
		CV2CallbackContext callback_context(context);
		info.to_init_cb(Convert(&args), Convert(&callback_context), &err_ptr);
	}

	auto result = make_uniq<CV2CopyToGlobalState>();
	if (args.out_init_data.ptr) {
		result->handle = CV2UserData(args.out_init_data.ptr, args.out_init_data.destroy, args.out_init_data.equals);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(result);
}

static auto CV2CopyToPrepareBatch(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                  unique_ptr<ColumnDataCollection> collection) -> unique_ptr<PreparedBatchData> {
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyToBatchInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	args.in_init_data = gstate.Cast<CV2CopyToGlobalState>().handle.GetData();
	args.in_collection = std::move(collection);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.to_batch_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	auto result = make_uniq<CV2CopyToBatchData>();
	if (args.out_batch_data.ptr) {
		result->handle = CV2UserData(args.out_batch_data.ptr, args.out_batch_data.destroy, args.out_batch_data.equals);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(result);
}

static auto CV2CopyToFlushBatch(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                PreparedBatchData &batch) -> void {
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyToFlushInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	args.in_init_data = gstate.Cast<CV2CopyToGlobalState>().handle.GetData();
	args.in_batch_data = batch.Cast<CV2CopyToBatchData>().handle.GetData();

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.to_flush_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2CopyToReportStatistics(ClientContext &context, FunctionData &bind_data, CV2CopyToGlobalState &gstate)
    -> void {
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyToStatisticsInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	args.in_init_data = gstate.handle.GetData();

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.to_statistics_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
	gstate.written_statistics->row_count = args.out_row_count;
	gstate.written_statistics->file_size_bytes = args.out_file_size_bytes;
}

static auto CV2CopyToFinalize(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate_p) -> void {
	const auto &info = GetFunctionInfo(bind_data);
	auto &gstate = gstate_p.Cast<CV2CopyToGlobalState>();

	// Always wired, as the engine requires a finalize hook; the user's callback is optional.
	if (info.to_finalize_cb) {
		CV2CopyToFinalizeInfo args = {};
		args.in_user_data = GetUserData(info);
		args.in_bind_data = GetUserBindData(bind_data);
		args.in_init_data = gstate.handle.GetData();

		CV2ErrorInfo err = {};
		auto err_ptr = Convert(&err);
		CV2CallbackContext callback_context(context);
		info.to_finalize_cb(Convert(&args), Convert(&callback_context), &err_ptr);

		if (err.HasError()) {
			err.ThrowAsException();
		}
	}
	// the statistics describe the finished file, so they are reported once it is finalized
	if (gstate.written_statistics) {
		CV2CopyToReportStatistics(context, bind_data, gstate);
	}
}

//! The engine asks where to report the statistics of a file right after creating it, before anything is written to it
static auto CV2CopyToGetWrittenStatistics(ClientContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                                          CopyFunctionFileStatistics &statistics) -> void {
	gstate.Cast<CV2CopyToGlobalState>().written_statistics = &statistics;
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO sink interface
//----------------------------------------------------------------------------------------------------------------------
// COPY TO itself drives the function through its batch interface. These hooks serve callers that write a file through
// the copy function directly, sinking chunks into it one at a time - every chunk is written as a batch of its own.

static auto CV2CopyToInitLocal(ExecutionContext &context, FunctionData &bind_data) -> unique_ptr<LocalFunctionData> {
	return make_uniq<CV2CopyToLocalState>();
}

static auto CV2CopyToSink(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                          LocalFunctionData &lstate, DataChunk &input) -> void {
	if (input.size() == 0) {
		return;
	}
	auto batch = make_uniq<ColumnDataCollection>(context.client, input.GetTypes());
	batch->Append(input);
	auto prepared = CV2CopyToPrepareBatch(context.client, bind_data, gstate, std::move(batch));
	// batches are never flushed concurrently for the same file
	lock_guard<mutex> guard(gstate.Cast<CV2CopyToGlobalState>().flush_lock);
	CV2CopyToFlushBatch(context.client, bind_data, gstate, *prepared);
}

static auto CV2CopyToCombine(ExecutionContext &context, FunctionData &bind_data, GlobalFunctionData &gstate,
                             LocalFunctionData &lstate) -> void {
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM hooks
//----------------------------------------------------------------------------------------------------------------------

static auto CV2CopyFromBind(ClientContext &context, CopyFromFunctionBindInput &input,
                            vector<Identifier> &expected_names, vector<LogicalType> &expected_types)
    -> unique_ptr<FunctionData> {
	const auto &function_info = input.tf.function_info->Cast<CV2CopyFromTableInfo>().info;
	const auto &info = function_info->Cast<CV2CopyFunctionInfo>();

	CV2CopyFromBindInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_file_path = &input.info.file_path;
	args.in_names = &expected_names;
	args.in_types = &expected_types;
	args.in_options = CV2CopyCollectOptions(input.info.options);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.from_bind_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	auto result = MakeBindData(function_info, std::move(args.out_bind_data));
	result->cardinality = args.out_cardinality;
	result->cardinality_is_exact = args.out_cardinality_is_exact;
	result->cardinality_set = args.out_cardinality_set;
	if (err.HasError()) {
		err.ThrowAsException();
	}
	return std::move(result);
}

static auto CV2CopyFromInitGlobal(ClientContext &context, TableFunctionInitInput &input)
    -> unique_ptr<GlobalTableFunctionState> {
	const auto &bind_data = *input.bind_data;
	const auto &info = GetFunctionInfo(bind_data);

	// Always produced, even without a callback: it carries the thread count the read runs with.
	auto result = make_uniq<CV2CopyFromGlobalState>();
	if (!info.from_init_global_cb) {
		return std::move(result);
	}

	CV2CopyFromInitGlobalInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.from_init_global_cb(Convert(&args), Convert(&callback_context), &err_ptr);

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

static auto CV2CopyFromInitLocal(ExecutionContext &context, TableFunctionInitInput &input,
                                 GlobalTableFunctionState *global_state) -> unique_ptr<LocalTableFunctionState> {
	const auto &bind_data = *input.bind_data;
	const auto &info = GetFunctionInfo(bind_data);

	if (!info.from_init_local_cb) {
		return nullptr;
	}

	CV2CopyFromInitLocalInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	if (global_state) {
		args.in_global_state = global_state->Cast<CV2CopyFromGlobalState>().handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context.client);
	info.from_init_local_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	unique_ptr<LocalTableFunctionState> result = nullptr;
	if (args.out_local_state.ptr) {
		auto set_result = make_uniq<CV2CopyFromLocalState>();
		set_result->handle =
		    CV2UserData(args.out_local_state.ptr, args.out_local_state.destroy, args.out_local_state.equals);
		result = std::move(set_result);
	}

	// Throw after taking ownership of the state, so that it is destroyed even if we error.
	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result;
}

static auto CV2CopyFromExec(ClientContext &context, TableFunctionInput &input, DataChunk &output) -> void {
	const auto &bind_data = *input.bind_data;
	const auto &info = GetFunctionInfo(bind_data);

	CV2CopyFromExecInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(bind_data);
	if (input.global_state) {
		args.in_global_state = input.global_state->Cast<CV2CopyFromGlobalState>().handle.GetData();
	}
	if (input.local_state) {
		args.in_local_state = input.local_state->Cast<CV2CopyFromLocalState>().handle.GetData();
	}
	args.output = &output;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.from_exec_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	// The vector write API sizes per vector, while the engine reads the chunk's cardinality. Callers typically size
	// only the first vector and write the rest through direct buffer writes, so take the first vector's size as the
	// batch's row count and propagate it to the others. The target table always has at least one column.
	output.SetChildCardinality(output.data[0].size());
}

static auto CV2CopyFromCardinality(ClientContext &context, const FunctionData *bind_data)
    -> unique_ptr<NodeStatistics> {
	const auto &data = bind_data->Cast<CV2CopyFunctionData>();
	if (!data.cardinality_set) {
		// No estimate at all: the optimizer falls back on its own defaults.
		return nullptr;
	}
	// An exact estimate also pins the maximum; otherwise only the estimate is known.
	if (data.cardinality_is_exact) {
		return make_uniq<NodeStatistics>(data.cardinality, data.cardinality);
	}
	return make_uniq<NodeStatistics>(data.cardinality);
}

static auto CV2CopyFromProgress(ClientContext &context, const FunctionData *bind_data,
                                const GlobalTableFunctionState *global_state) -> double {
	const auto &info = GetFunctionInfo(*bind_data);

	CV2CopyFromProgressInfo args = {};
	args.in_user_data = GetUserData(info);
	args.in_bind_data = GetUserBindData(*bind_data);
	if (global_state) {
		// The callback runs concurrently with the read, so the state it reads is the shared one, unsynchronized.
		args.in_global_state = global_state->Cast<CV2CopyFromGlobalState>().handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.from_progress_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	// The API contract is a 0.0-1.0 fraction, the engine reads a 0-100 percentage.
	return MinValue<double>(MaxValue<double>(args.out_progress, 0.0), 1.0) * 100.0;
}

//----------------------------------------------------------------------------------------------------------------------
// Function handle
//----------------------------------------------------------------------------------------------------------------------

class CV2CopyFunction {
public:
	explicit CV2CopyFunction(DatabaseInstance &db) : db(db) {
	}

	//! Validates the configuration and builds the copy function to register on `target`.
	CopyFunction Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "copy function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		const bool has_copy_to = info.HasCopyTo();
		const bool has_copy_from = info.HasCopyFrom();
		if (!has_copy_to && !has_copy_from) {
			throw InvalidInputException(
			    "At least one of the COPY TO and COPY FROM sides must be set for the function.");
		}
		if (has_copy_to) {
			if (!info.to_batch_cb) {
				throw InvalidInputException("Batch callback must be set for the COPY TO side of the function.");
			}
			if (!info.to_flush_cb) {
				throw InvalidInputException("Flush callback must be set for the COPY TO side of the function.");
			}
		}
		if (has_copy_from) {
			if (!info.from_bind_cb) {
				throw InvalidInputException("Bind callback must be set for the COPY FROM side of the function.");
			}
			if (!info.from_exec_cb) {
				throw InvalidInputException("Exec callback must be set for the COPY FROM side of the function.");
			}
		}

		auto function_info = make_shared_ptr<CV2CopyFunctionInfo>(std::move(info));

		CopyFunction function(name);
		if (has_copy_to) {
			function.copy_to_bind = CV2CopyToBind;
			function.copy_to_initialize_global = CV2CopyToInitGlobal;
			function.prepare_batch = CV2CopyToPrepareBatch;
			function.flush_batch = CV2CopyToFlushBatch;
			function.copy_to_finalize = CV2CopyToFinalize;
			function.copy_to_initialize_local = CV2CopyToInitLocal;
			function.copy_to_sink = CV2CopyToSink;
			function.copy_to_combine = CV2CopyToCombine;
			if (function_info->to_batch_size_cb) {
				function.desired_batch_size = CV2CopyToBatchSize;
			}
			if (function_info->to_statistics_cb) {
				function.copy_to_get_written_statistics = CV2CopyToGetWrittenStatistics;
			}
		}
		if (has_copy_from) {
			// The engine reads through a table function whose own bind is skipped in favour of copy_from_bind.
			TableFunction reader(name, {}, CV2CopyFromExec, nullptr, CV2CopyFromInitGlobal, CV2CopyFromInitLocal);
			// Always wired: it serves the static estimate a bind callback may set, which is not known at registration.
			reader.cardinality = CV2CopyFromCardinality;
			if (function_info->from_progress_cb) {
				reader.table_scan_progress = CV2CopyFromProgress;
			}
			reader.function_info = make_shared_ptr<CV2CopyFromTableInfo>(function_info);
			function.copy_from_bind = CV2CopyFromBind;
			function.copy_from_function = std::move(reader);
		}
		function.function_info = std::move(function_info);
		function.extension = extension;

		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	CV2CopyFunctionInfo info;
	Identifier name;
	string extension;
};

static auto Convert(duckdb_v2_copy_function_handle func) -> CV2CopyFunction * {
	return reinterpret_cast<CV2CopyFunction *>(func);
}
static auto Convert(CV2CopyFunction *func) -> duckdb_v2_copy_function_handle {
	return reinterpret_cast<duckdb_v2_copy_function_handle>(func);
}

//! The column and option accessors of the two bind infos share their bodies.
template <class INFO>
static auto GetColumnType(INFO &args, idx_t index, const char *function) -> duckdb_v2_logical_type_handle {
	const auto &types = *args.in_types;
	if (index >= types.size()) {
		throw InvalidInputException("Index out of bounds in %s", function);
	}
	return Convert(types[index]);
}

template <class INFO>
static auto GetColumnName(INFO &args, idx_t index, const char *function) -> duckdb_v2_identifier_t {
	const auto &names = *args.in_names;
	if (index >= names.size()) {
		throw InvalidInputException("Index out of bounds in %s", function);
	}
	return Convert(names[index]);
}

template <class INFO>
static auto GetOption(INFO &args, idx_t index, const char *function) -> const CV2CopyOptionList::value_type & {
	if (index >= args.in_options.size()) {
		throw InvalidInputException("Index out of bounds in %s", function);
	}
	return args.in_options[index];
}

//! One value per option: a bare option reads as true, a single value as itself, and a list as a tuple of its
//! elements, i.e. an unnamed struct.
template <class INFO>
static auto GetOptionValue(INFO &args, idx_t index, const char *function) -> duckdb_v2_value_handle {
	const auto &values = *GetOption(args, index, function).second;
	if (values.empty()) {
		return Convert(new Value(Value::BOOLEAN(true)));
	}
	if (values.size() == 1) {
		return Convert(new Value(values[0]));
	}
	child_list_t<Value> elements;
	for (auto &value : values) {
		elements.emplace_back(string(), value);
	}
	return Convert(new Value(Value::STRUCT(std::move(elements))));
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_copy_function_create(duckdb_v2_factory_handle factory,
                                               duckdb_v2_copy_function_handle *function,
                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2CopyFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_function_set_name(duckdb_v2_copy_function_handle function, const duckdb_v2_str *name,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_function_set_file_extension(duckdb_v2_copy_function_handle function,
                                                           const duckdb_v2_str *extension,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(extension);
	return WithErrorHandler(err, [&]() {
		duckdb::string value(extension->ptr, extension->len);
		if (!value.empty() && value[0] == '.') {
			throw duckdb::InvalidInputException("The file extension of a copy function is set without its leading dot, "
			                                    "e.g. \"parquet\" rather than \".parquet\".");
		}
		Convert(function)->extension = std::move(value);
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_function_set_user_data(duckdb_v2_copy_function_handle function, duckdb_v2_opaque *data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals);
	});
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO callbacks
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_bind_callback(duckdb_v2_copy_function_handle function,
                                                    duckdb_v2_copy_to_bind_callback_fn callback,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_batch_size_callback(duckdb_v2_copy_function_handle function,
                                                          duckdb_v2_copy_to_batch_size_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_batch_size_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_init_callback(duckdb_v2_copy_function_handle function,
                                                    duckdb_v2_copy_to_init_callback_fn callback,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_init_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_batch_callback(duckdb_v2_copy_function_handle function,
                                                     duckdb_v2_copy_to_batch_callback_fn callback,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_batch_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_flush_callback(duckdb_v2_copy_function_handle function,
                                                     duckdb_v2_copy_to_flush_callback_fn callback,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_flush_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_finalize_callback(duckdb_v2_copy_function_handle function,
                                                        duckdb_v2_copy_to_finalize_callback_fn callback,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_finalize_cb = callback; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO bind
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_user_data(duckdb_v2_copy_to_bind_info_handle info, void **data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_set_bind_data(duckdb_v2_copy_to_bind_info_handle info, duckdb_v2_opaque *data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_file_path(duckdb_v2_copy_to_bind_info_handle info, duckdb_v2_str *path,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(path);
	return WithErrorHandler(err, [&]() { *path = Convert(*Convert(info)->in_file_path); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_column_count(duckdb_v2_copy_to_bind_info_handle info, idx_t *count,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_types->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_column_type(duckdb_v2_copy_to_bind_info_handle info, idx_t index,
                                                       duckdb_v2_logical_type_handle *type,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(
	    err, [&]() { *type = GetColumnType(*Convert(info), index, "duckdb_v2_copy_to_bind_get_column_type"); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_column_name(duckdb_v2_copy_to_bind_info_handle info, idx_t index,
                                                       duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(
	    err, [&]() { *name = GetColumnName(*Convert(info), index, "duckdb_v2_copy_to_bind_get_column_name"); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_option_count(duckdb_v2_copy_to_bind_info_handle info, idx_t *count,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_options.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_option_name(duckdb_v2_copy_to_bind_info_handle info, idx_t index,
                                                       duckdb_v2_identifier_t *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		*name = Convert(*GetOption(*Convert(info), index, "duckdb_v2_copy_to_bind_get_option_name").first);
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_bind_get_option_value(duckdb_v2_copy_to_bind_info_handle info, idx_t index,
                                                        duckdb_v2_value_handle *value,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *value = GetOptionValue(*Convert(info), index, "duckdb_v2_copy_to_bind_get_option_value"); });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO batch size
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_size_get_user_data(duckdb_v2_copy_to_batch_size_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_size_get_bind_data(duckdb_v2_copy_to_batch_size_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_size_set_target(duckdb_v2_copy_to_batch_size_info_handle info, idx_t rows,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_target = rows; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO init
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_init_get_user_data(duckdb_v2_copy_to_init_info_handle info, void **data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_init_get_bind_data(duckdb_v2_copy_to_init_info_handle info, void **data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_init_get_file_path(duckdb_v2_copy_to_init_info_handle info, duckdb_v2_str *path,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(path);
	return WithErrorHandler(err, [&]() { *path = Convert(*Convert(info)->in_file_path); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_init_set_init_data(duckdb_v2_copy_to_init_info_handle info, duckdb_v2_opaque *data,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_init_data = *data; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO batch
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_get_user_data(duckdb_v2_copy_to_batch_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_get_bind_data(duckdb_v2_copy_to_batch_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_get_init_data(duckdb_v2_copy_to_batch_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_take_input(duckdb_v2_copy_to_batch_info_handle info,
                                                   duckdb_v2_column_data_collection_handle *collection,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(collection);
	*collection = nullptr;
	return WithErrorHandler(err, [&]() {
		auto &batch_info = *Convert(info);
		if (!batch_info.in_collection) {
			throw duckdb::InvalidInputException("The batch input was already taken.");
		}
		*collection = Convert(batch_info.in_collection.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_batch_set_batch_data(duckdb_v2_copy_to_batch_info_handle info, duckdb_v2_opaque *data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_batch_data = *data; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO flush
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_flush_get_user_data(duckdb_v2_copy_to_flush_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_flush_get_bind_data(duckdb_v2_copy_to_flush_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_flush_get_init_data(duckdb_v2_copy_to_flush_info_handle info, void **data,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_flush_get_batch_data(duckdb_v2_copy_to_flush_info_handle info, void **data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_batch_data; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY TO finalize
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_finalize_get_user_data(duckdb_v2_copy_to_finalize_info_handle info, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_finalize_get_bind_data(duckdb_v2_copy_to_finalize_info_handle info, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_finalize_get_init_data(duckdb_v2_copy_to_finalize_info_handle info, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM callbacks
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_set_bind_callback(duckdb_v2_copy_function_handle function,
                                                      duckdb_v2_copy_from_bind_callback_fn callback,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.from_bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_set_init_global_callback(duckdb_v2_copy_function_handle function,
                                                             duckdb_v2_copy_from_init_global_callback_fn callback,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.from_init_global_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_set_init_local_callback(duckdb_v2_copy_function_handle function,
                                                            duckdb_v2_copy_from_init_local_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.from_init_local_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_set_exec_callback(duckdb_v2_copy_function_handle function,
                                                      duckdb_v2_copy_from_exec_callback_fn callback,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.from_exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_set_progress_callback(duckdb_v2_copy_function_handle function,
                                                          duckdb_v2_copy_from_progress_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.from_progress_cb = callback; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM bind
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_user_data(duckdb_v2_copy_from_bind_info_handle info, void **data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_set_bind_data(duckdb_v2_copy_from_bind_info_handle info,
                                                       duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_file_path(duckdb_v2_copy_from_bind_info_handle info, duckdb_v2_str *path,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(path);
	return WithErrorHandler(err, [&]() { *path = Convert(*Convert(info)->in_file_path); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_column_count(duckdb_v2_copy_from_bind_info_handle info, idx_t *count,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_types->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_column_type(duckdb_v2_copy_from_bind_info_handle info, idx_t index,
                                                         duckdb_v2_logical_type_handle *type,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(type);
	*type = nullptr;
	return WithErrorHandler(
	    err, [&]() { *type = GetColumnType(*Convert(info), index, "duckdb_v2_copy_from_bind_get_column_type"); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_column_name(duckdb_v2_copy_from_bind_info_handle info, idx_t index,
                                                         duckdb_v2_identifier_t *name,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(
	    err, [&]() { *name = GetColumnName(*Convert(info), index, "duckdb_v2_copy_from_bind_get_column_name"); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_option_count(duckdb_v2_copy_from_bind_info_handle info, idx_t *count,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->in_options.size(); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_option_name(duckdb_v2_copy_from_bind_info_handle info, idx_t index,
                                                         duckdb_v2_identifier_t *name,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() {
		*name = Convert(*GetOption(*Convert(info), index, "duckdb_v2_copy_from_bind_get_option_name").first);
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_get_option_value(duckdb_v2_copy_from_bind_info_handle info, idx_t index,
                                                          duckdb_v2_value_handle *value,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(value);
	*value = nullptr;
	return WithErrorHandler(
	    err, [&]() { *value = GetOptionValue(*Convert(info), index, "duckdb_v2_copy_from_bind_get_option_value"); });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_bind_set_cardinality(duckdb_v2_copy_from_bind_info_handle info, idx_t cardinality,
                                                         bool is_exact, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &bind_info = *Convert(info);
		bind_info.out_cardinality = cardinality;
		bind_info.out_cardinality_is_exact = is_exact;
		bind_info.out_cardinality_set = true;
	});
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM init global
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_global_get_user_data(duckdb_v2_copy_from_init_global_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_global_get_bind_data(duckdb_v2_copy_from_init_global_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_global_set_global_state(duckdb_v2_copy_from_init_global_info_handle info,
                                                                 duckdb_v2_opaque *data,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_global_state = *data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_global_set_max_threads(duckdb_v2_copy_from_init_global_info_handle info,
                                                                idx_t max_threads, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		if (max_threads == 0) {
			throw duckdb::InvalidInputException("The maximum number of threads must be at least 1");
		}
		Convert(info)->out_max_threads = max_threads;
	});
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM init local
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_local_get_user_data(duckdb_v2_copy_from_init_local_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_local_get_bind_data(duckdb_v2_copy_from_init_local_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_local_get_global_state(duckdb_v2_copy_from_init_local_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_init_local_set_local_state(duckdb_v2_copy_from_init_local_info_handle info,
                                                               duckdb_v2_opaque *data,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { Convert(info)->out_local_state = *data; });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM exec
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_exec_get_user_data(duckdb_v2_copy_from_exec_info_handle info, void **data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_exec_get_bind_data(duckdb_v2_copy_from_exec_info_handle info, void **data,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_exec_get_global_state(duckdb_v2_copy_from_exec_info_handle info, void **data,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_exec_get_local_state(duckdb_v2_copy_from_exec_info_handle info, void **data,
                                                         duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_local_state; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_exec_get_output_chunk(duckdb_v2_copy_from_exec_info_handle info,
                                                          duckdb_v2_data_chunk_handle *chunk,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(chunk);
	return WithErrorHandler(err, [&]() { *chunk = Convert(Convert(info)->output); });
}

//----------------------------------------------------------------------------------------------------------------------
// COPY FROM progress
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_from_progress_get_user_data(duckdb_v2_copy_from_progress_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_progress_get_bind_data(duckdb_v2_copy_from_progress_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_progress_get_global_state(duckdb_v2_copy_from_progress_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_global_state; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_from_progress_set_progress(duckdb_v2_copy_from_progress_info_handle info,
                                                          double progress, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_progress = progress; });
}

//----------------------------------------------------------------------------------------------------------------------
// Register / destroy
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_connection_register_copy_function(duckdb_v2_connection_handle conn,
                                                            duckdb_v2_copy_function_handle function,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto info = duckdb::CreateCopyFunctionInfo(Convert(function)->Build(*context.db));
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			catalog.CreateCopyFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_copy_function(duckdb_v2_extension_handle extension,
                                                           duckdb_v2_copy_function_handle function,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		loader.RegisterFunction(Convert(function)->Build(loader.GetDatabaseInstance()));
	});
}

DUCKDB_V2_ERROR duckdb_v2_copy_function_destroy(duckdb_v2_copy_function_handle *function) {
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
// COPY TO Statistics
//----------------------------------------------------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_copy_to_set_statistics_callback(duckdb_v2_copy_function_handle function,
                                                          duckdb_v2_copy_to_statistics_callback_fn callback,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.to_statistics_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_statistics_get_user_data(duckdb_v2_copy_to_statistics_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_statistics_get_bind_data(duckdb_v2_copy_to_statistics_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_statistics_get_init_data(duckdb_v2_copy_to_statistics_info_handle info, void **data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_statistics_set_row_count(duckdb_v2_copy_to_statistics_info_handle info,
                                                           idx_t row_count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_row_count = row_count; });
}

DUCKDB_V2_ERROR duckdb_v2_copy_to_statistics_set_file_size(duckdb_v2_copy_to_statistics_info_handle info,
                                                           idx_t file_size_bytes, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_file_size_bytes = file_size_bytes; });
}
