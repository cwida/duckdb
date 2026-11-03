#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

namespace duckdb::capiv2 {

class CV2AggregateFunctionData final : public FunctionData {
public:
	// The function this call binds to. All V2 aggregates share the same C++ callbacks, so the bind data
	// must tell them apart, or different aggregates over the same arguments are merged.
	const AggregateFunctionInfo *function = nullptr;
	// The bind data set by the user
	shared_ptr<CV2UserData> handle;

	auto Copy() const -> unique_ptr<FunctionData> override {
		auto copy = make_uniq<CV2AggregateFunctionData>();
		copy->function = function;
		copy->handle = handle;
		return std::move(copy);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		const auto &other_data = other.Cast<CV2AggregateFunctionData>();
		if (function != other_data.function) {
			return false;
		}
		if (!handle && !other_data.handle) {
			return true;
		}
		return handle && other_data.handle && handle->Equals(*other_data.handle);
	}
};

class CV2AggregateBindInfo {
public:
	BindAggregateFunctionInput *in_input = nullptr;
};

static auto Convert(duckdb_v2_aggregate_function_bind_info_handle info) -> CV2AggregateBindInfo * {
	return reinterpret_cast<CV2AggregateBindInfo *>(info);
}
static auto Convert(CV2AggregateBindInfo *info) -> duckdb_v2_aggregate_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_bind_info_handle>(info);
}

class CV2AggregateSizeInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	idx_t out_size = 0;
};

static auto Convert(duckdb_v2_aggregate_function_size_info_handle info) -> CV2AggregateSizeInfo * {
	return reinterpret_cast<CV2AggregateSizeInfo *>(info);
}
static auto Convert(CV2AggregateSizeInfo *info) -> duckdb_v2_aggregate_function_size_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_size_info_handle>(info);
}

class CV2AggregateInitInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	void **states = nullptr;
	idx_t state_count = 0;
};

static auto Convert(duckdb_v2_aggregate_function_init_info_handle info) -> CV2AggregateInitInfo * {
	return reinterpret_cast<CV2AggregateInitInfo *>(info);
}
static auto Convert(CV2AggregateInitInfo *info) -> duckdb_v2_aggregate_function_init_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_init_info_handle>(info);
}

class CV2AggregateUpdateInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	Vector *inputs = nullptr;
	idx_t input_count = 0;
	//! Tells the four parts of the argument list apart
	const BoundAggregateFunction *function = nullptr;
	idx_t row_count = 0;
	void **states = nullptr;
};

static auto Convert(duckdb_v2_aggregate_function_update_info_handle info) -> CV2AggregateUpdateInfo * {
	return reinterpret_cast<CV2AggregateUpdateInfo *>(info);
}
static auto Convert(CV2AggregateUpdateInfo *info) -> duckdb_v2_aggregate_function_update_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_update_info_handle>(info);
}

class CV2AggregateCombineInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	void **sources = nullptr;
	void **targets = nullptr;
	idx_t state_count = 0;
};

static auto Convert(duckdb_v2_aggregate_function_combine_info_handle info) -> CV2AggregateCombineInfo * {
	return reinterpret_cast<CV2AggregateCombineInfo *>(info);
}
static auto Convert(CV2AggregateCombineInfo *info) -> duckdb_v2_aggregate_function_combine_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_combine_info_handle>(info);
}

class CV2AggregateFinalizeInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	void **states = nullptr;
	idx_t state_count = 0;
	Vector *result = nullptr;
	idx_t result_offset = 0;
};

static auto Convert(duckdb_v2_aggregate_function_finalize_info_handle info) -> CV2AggregateFinalizeInfo * {
	return reinterpret_cast<CV2AggregateFinalizeInfo *>(info);
}
static auto Convert(CV2AggregateFinalizeInfo *info) -> duckdb_v2_aggregate_function_finalize_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_finalize_info_handle>(info);
}

class CV2AggregateDestroyInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	void **states = nullptr;
	idx_t state_count = 0;
};

static auto Convert(duckdb_v2_aggregate_function_destroy_info_handle info) -> CV2AggregateDestroyInfo * {
	return reinterpret_cast<CV2AggregateDestroyInfo *>(info);
}
static auto Convert(CV2AggregateDestroyInfo *info) -> duckdb_v2_aggregate_function_destroy_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_destroy_info_handle>(info);
}

class CV2AggregateStatsInfo {
public:
	CV2AggregateStatsInfo(const BoundAggregateFunction &function, BaseStatistics &result)
	    : function(function), result_stats(CV2Stats::Writable(result)) {
	}

	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	//! Tells the four parts of the argument list apart
	const BoundAggregateFunction &function;
	vector<CV2Stats> arg_stats;
	CV2Stats result_stats;
};

static auto Convert(duckdb_v2_aggregate_function_stats_info_handle info) -> CV2AggregateStatsInfo * {
	return reinterpret_cast<CV2AggregateStatsInfo *>(info);
}
static auto Convert(CV2AggregateStatsInfo *info) -> duckdb_v2_aggregate_function_stats_info_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_stats_info_handle>(info);
}

class CV2AggregateFunctionInfo : public AggregateFunctionInfo {
public:
	duckdb_v2_aggregate_function_bind_callback_fn bind_cb = nullptr;
	duckdb_v2_aggregate_function_size_callback_fn size_cb = nullptr;
	duckdb_v2_aggregate_function_init_callback_fn init_cb = nullptr;
	duckdb_v2_aggregate_function_update_callback_fn update_cb = nullptr;
	duckdb_v2_aggregate_function_combine_callback_fn combine_cb = nullptr;
	duckdb_v2_aggregate_function_finalize_callback_fn finalize_cb = nullptr;
	duckdb_v2_aggregate_function_destroy_callback_fn destroy_cb = nullptr;
	duckdb_v2_aggregate_function_stats_callback_fn stats_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto GetUserBindData(const FunctionData *bind_data) -> void * {
	if (!bind_data) {
		return nullptr;
	}
	const auto &handle = bind_data->Cast<CV2AggregateFunctionData>().handle;
	return handle ? handle->GetData() : nullptr;
}

static auto CV2AggregateBind(BindAggregateFunctionInput &input) -> unique_ptr<FunctionData> {
	const auto &info = input.GetBoundFunction().GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	auto result = make_uniq<CV2AggregateFunctionData>();
	result->function = &info;
	if (!info.bind_cb) {
		return std::move(result);
	}

	auto &bound_function = input.GetBoundFunction();
	CV2ExpressionBindInfo bind_info(bound_function, info.user_data ? info.user_data->GetData() : nullptr, input);
	CV2AggregateBindInfo result_info = {};
	result_info.in_input = &input;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.GetClientContext());
	info.bind_cb(Convert(&bind_info), Convert(&result_info), Convert(&callback_context), &err_ptr);

	if (bind_info.out_bind_data) {
		result->handle = std::move(bind_info.out_bind_data);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return std::move(result);
}

static auto CV2AggregateSize(AggregateStateInput &input) -> idx_t {
	const auto &info = input.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	CV2AggregateSizeInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(input.bind_data.get());

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.size_cb(Convert(&args), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return args.out_size;
}

static auto CV2AggregateInit(AggregateStateInput &input, data_ptr_t *states, idx_t count) -> void {
	const auto &info = input.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	CV2AggregateInitInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(input.bind_data.get());
	args.states = reinterpret_cast<void **>(states);
	args.state_count = count;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.init_cb(Convert(&args), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2AggregateUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &state,
                               idx_t count) -> void {
	const auto &info = aggr_input_data.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	// the callback expects one state per row - flatten a reference, as callers may hold on to the state buffer
	auto states = Vector::Ref(state);
	if (states.GetVectorType() != VectorType::FLAT_VECTOR) {
		states.Flatten(*FlatVector::IncrementalSelectionVector(), count);
	}

	CV2AggregateUpdateInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(aggr_input_data.bind_data.get());
	args.inputs = inputs;
	args.input_count = input_count;
	args.function = &aggr_input_data.function;
	args.row_count = count;
	args.states = FlatVector::GetDataMutableUnsafe<void *>(states);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.update_cb(Convert(&args), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2AggregateCombine(Vector &state, Vector &combined, AggregateInputData &aggr_input_data, idx_t count)
    -> void {
	const auto &info = aggr_input_data.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	state.Flatten();

	CV2AggregateCombineInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(aggr_input_data.bind_data.get());
	args.sources = FlatVector::GetDataMutableUnsafe<void *>(state);
	args.targets = FlatVector::GetDataMutableUnsafe<void *>(combined);
	args.state_count = count;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.combine_cb(Convert(&args), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2AggregateFinalize(Vector &state, AggregateFinalizeInputData &finalize_input_data, Vector &result,
                                 idx_t count, idx_t offset) -> void {
	const auto &info = finalize_input_data.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	state.Flatten();

	CV2AggregateFinalizeInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(finalize_input_data.bind_data.get());
	args.states = FlatVector::GetDataMutableUnsafe<void *>(state);
	args.state_count = count;
	args.result = &result;
	args.result_offset = offset;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.finalize_cb(Convert(&args), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2AggregateDestroy(Vector &state, AggregateInputData &aggr_input_data, idx_t count) -> void {
	const auto &info = aggr_input_data.function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	state.Flatten();

	CV2AggregateDestroyInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(aggr_input_data.bind_data.get());
	args.states = FlatVector::GetDataMutableUnsafe<void *>(state);
	args.state_count = count;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	info.destroy_cb(Convert(&args), &err_ptr);

	// The destructor path may not throw: an error reported through the slot is dropped.
}

static auto CV2AggregateStats(ClientContext &context, BoundAggregateExpression &expr, AggregateStatisticsInput &input)
    -> unique_ptr<BaseStatistics> {
	const auto &function = expr.Function();
	const auto &info = function.GetExtraFunctionInfo().Cast<CV2AggregateFunctionInfo>();

	auto result = BaseStatistics::CreateUnknown(expr.GetReturnType());
	CV2AggregateStatsInfo args(function, result);
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = GetUserBindData(input.bind_data.get());
	args.arg_stats.reserve(input.child_stats.size());
	for (const auto &child_stats : input.child_stats) {
		args.arg_stats.emplace_back(child_stats);
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(context);
	info.stats_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result.ToUnique();
}

class CV2AggregateFunction {
public:
	explicit CV2AggregateFunction(DatabaseInstance &db) : db(db) {
		// The callbacks' error slots make failure part of the API contract, so the function defaults to fallible:
		// a non-fallible function turns any execution error reported through the slot into an internal error.
		properties.SetFallible();
	}

	//! Validates the configuration and builds the aggregate function to register on `target`.
	AggregateFunction Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "aggregate function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (!info.size_cb) {
			throw InvalidInputException("Size callback must be set for the function.");
		}
		if (!info.init_cb) {
			throw InvalidInputException("Init callback must be set for the function.");
		}
		if (!info.update_cb) {
			throw InvalidInputException("Update callback must be set for the function.");
		}
		if (!info.combine_cb) {
			throw InvalidInputException("Combine callback must be set for the function.");
		}
		if (!info.finalize_cb) {
			throw InvalidInputException("Finalize callback must be set for the function.");
		}

		const auto &return_type = signature.GetReturnType();

		// ANY is allowed as a placeholder return type only when a bind callback is present to resolve the actual type.
		if (return_type.id() == LogicalTypeId::ANY) {
			if (info.bind_cb == nullptr) {
				throw InvalidInputException(
				    "An ANY return type requires a bind callback to set the concrete return type.");
			}
		} else {
			if (return_type.id() == LogicalTypeId::INVALID) {
				throw InvalidInputException("Return type must be set for the function.");
			}
			if (!return_type.IsComplete()) {
				throw InvalidInputException("Return type must be a fully defined concrete type");
			}
		}

		signature.Verify();

		AggregateFunction function(name, {}, return_type, CV2AggregateSize, CV2AggregateInit, CV2AggregateUpdate,
		                           CV2AggregateCombine, CV2AggregateFinalize,
		                           FunctionNullHandling::DEFAULT_NULL_HANDLING);
		function.SetProperties(properties);
		function.GetSignature() = signature;

		// Always bound, even without a user bind callback: the bind data identifies the function (see
		// CV2AggregateFunctionData).
		function.SetBindCallback(CV2AggregateBind);
		if (info.destroy_cb) {
			function.SetStateDestructorCallback(CV2AggregateDestroy);
		}
		if (info.stats_cb) {
			function.SetStatisticsCallback(CV2AggregateStats);
		}
		function.SetExtraFunctionInfo<CV2AggregateFunctionInfo>(std::move(info));

		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2AggregateFunctionInfo info;
	Identifier name;
	AggregateFunctionProperties properties;
};

static auto Convert(duckdb_v2_aggregate_function_handle func) -> CV2AggregateFunction * {
	return reinterpret_cast<CV2AggregateFunction *>(func);
}
static auto Convert(CV2AggregateFunction *func) -> duckdb_v2_aggregate_function_handle {
	return reinterpret_cast<duckdb_v2_aggregate_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_create(duckdb_v2_factory_handle factory,
                                                    duckdb_v2_aggregate_function_handle *function,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2AggregateFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_name(duckdb_v2_aggregate_function_handle function,
                                                      const duckdb_v2_str *name, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_get_docs(duckdb_v2_aggregate_function_handle function,
                                                      duckdb_v2_function_docs_handle *docs,
                                                      duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_get_signature(duckdb_v2_aggregate_function_handle function,
                                                           duckdb_v2_function_signature_handle *sig,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_user_data(duckdb_v2_aggregate_function_handle function,
                                                           duckdb_v2_opaque *user_data,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_property(duckdb_v2_aggregate_function_handle function,
                                                          DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                          DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { SetAggregateFunctionProperty(Convert(function)->properties, key, value); });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_bind_callback(duckdb_v2_aggregate_function_handle function,
                                                               duckdb_v2_aggregate_function_bind_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_size_callback(duckdb_v2_aggregate_function_handle function,
                                                               duckdb_v2_aggregate_function_size_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.size_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_init_callback(duckdb_v2_aggregate_function_handle function,
                                                               duckdb_v2_aggregate_function_init_callback_fn callback,
                                                               duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.init_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_set_update_callback(duckdb_v2_aggregate_function_handle function,
                                                 duckdb_v2_aggregate_function_update_callback_fn callback,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.update_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_set_combine_callback(duckdb_v2_aggregate_function_handle function,
                                                  duckdb_v2_aggregate_function_combine_callback_fn callback,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.combine_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_set_finalize_callback(duckdb_v2_aggregate_function_handle function,
                                                   duckdb_v2_aggregate_function_finalize_callback_fn callback,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.finalize_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_set_destroy_callback(duckdb_v2_aggregate_function_handle function,
                                                  duckdb_v2_aggregate_function_destroy_callback_fn callback,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.destroy_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_set_stats_callback(duckdb_v2_aggregate_function_handle function,
                                                                duckdb_v2_aggregate_function_stats_callback_fn callback,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.stats_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_bind_set_return_type(duckdb_v2_aggregate_function_bind_info_handle info,
                                                                  duckdb_v2_logical_type_handle return_type,
                                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(return_type);
	return WithErrorHandler(err, [&]() {
		auto type = Convert(return_type);
		Convert(info)->in_input->GetBoundFunction().SetReturnType(*type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_size_get_user_data(duckdb_v2_aggregate_function_size_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_size_get_bind_data(duckdb_v2_aggregate_function_size_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_size_set_state_size(duckdb_v2_aggregate_function_size_info_handle info,
                                                                 idx_t size, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() { Convert(info)->out_size = size; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_init_get_user_data(duckdb_v2_aggregate_function_init_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_init_get_bind_data(duckdb_v2_aggregate_function_init_info_handle info,
                                                                void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_init_get_state_count(duckdb_v2_aggregate_function_init_info_handle info,
                                                                  idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->state_count; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_init_get_states(duckdb_v2_aggregate_function_init_info_handle info,
                                                             void ***states, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->states; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_user_data(duckdb_v2_aggregate_function_update_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_bind_data(duckdb_v2_aggregate_function_update_info_handle info,
                                                                  void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_row_count(duckdb_v2_aggregate_function_update_info_handle info,
                                                                  idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->row_count; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_arg_count(duckdb_v2_aggregate_function_update_info_handle info,
                                                                  idx_t *positional_fixed, idx_t *positional_variadic,
                                                                  idx_t *named_fixed, idx_t *named_variadic,
                                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &function = *Convert(info)->function;
		if (positional_fixed) {
			*positional_fixed = function.GetStandardArgumentCount();
		}
		if (positional_variadic) {
			*positional_variadic = function.GetVarArgsCount();
		}
		if (named_fixed) {
			*named_fixed = function.GetKeywordOnlyArgumentCount();
		}
		if (named_variadic) {
			*named_variadic = function.GetKwargsCount();
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_arg(duckdb_v2_aggregate_function_update_info_handle info,
                                                            idx_t index, duckdb_v2_vector_handle *vector,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto &update_info = *Convert(info);
		if (index >= update_info.input_count) {
			throw duckdb::InvalidInputException("Index out of bounds in duckdb_v2_aggregate_function_update_get_arg");
		}
		*vector = Convert(&update_info.inputs[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_update_get_states(duckdb_v2_aggregate_function_update_info_handle info,
                                                               void ***states, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->states; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_combine_get_user_data(duckdb_v2_aggregate_function_combine_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_combine_get_bind_data(duckdb_v2_aggregate_function_combine_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_combine_get_state_count(duckdb_v2_aggregate_function_combine_info_handle info,
                                                     idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->state_count; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_combine_get_sources(duckdb_v2_aggregate_function_combine_info_handle info,
                                                                 void ***states, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->sources; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_combine_get_targets(duckdb_v2_aggregate_function_combine_info_handle info,
                                                                 void ***states, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->targets; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_user_data(duckdb_v2_aggregate_function_finalize_info_handle info, void **data,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_bind_data(duckdb_v2_aggregate_function_finalize_info_handle info, void **data,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_state_count(duckdb_v2_aggregate_function_finalize_info_handle info,
                                                      idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->state_count; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_states(duckdb_v2_aggregate_function_finalize_info_handle info, void ***states,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->states; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_result(duckdb_v2_aggregate_function_finalize_info_handle info,
                                                 duckdb_v2_vector_handle *vector, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() { *vector = Convert(Convert(info)->result); });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_finalize_get_result_offset(duckdb_v2_aggregate_function_finalize_info_handle info,
                                                        idx_t *offset, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(offset);
	return WithErrorHandler(err, [&]() { *offset = Convert(info)->result_offset; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_destroy_get_user_data(duckdb_v2_aggregate_function_destroy_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_destroy_get_bind_data(duckdb_v2_aggregate_function_destroy_info_handle info, void **data,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR
duckdb_v2_aggregate_function_destroy_get_state_count(duckdb_v2_aggregate_function_destroy_info_handle info,
                                                     idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);
	return WithErrorHandler(err, [&]() { *count = Convert(info)->state_count; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_destroy_get_states(duckdb_v2_aggregate_function_destroy_info_handle info,
                                                                void ***states, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(states);
	return WithErrorHandler(err, [&]() { *states = Convert(info)->states; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_stats_get_user_data(duckdb_v2_aggregate_function_stats_info_handle info,
                                                                 void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_stats_get_bind_data(duckdb_v2_aggregate_function_stats_info_handle info,
                                                                 void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_stats_get_arg_count(duckdb_v2_aggregate_function_stats_info_handle info,
                                                                 idx_t *positional_fixed, idx_t *positional_variadic,
                                                                 idx_t *named_fixed, idx_t *named_variadic,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	return WithErrorHandler(err, [&]() {
		auto &function = Convert(info)->function;
		if (positional_fixed) {
			*positional_fixed = function.GetStandardArgumentCount();
		}
		if (positional_variadic) {
			*positional_variadic = function.GetVarArgsCount();
		}
		if (named_fixed) {
			*named_fixed = function.GetKeywordOnlyArgumentCount();
		}
		if (named_variadic) {
			*named_variadic = function.GetKwargsCount();
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_stats_get_arg_stats(duckdb_v2_aggregate_function_stats_info_handle info,
                                                                 idx_t index, duckdb_v2_stats_handle *stats,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &stats_info = *Convert(info);
		if (index >= stats_info.arg_stats.size()) {
			throw duckdb::InvalidInputException(
			    "Index out of bounds in duckdb_v2_aggregate_function_stats_get_arg_stats");
		}
		*stats = Convert(&stats_info.arg_stats[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_stats_get_result_stats(duckdb_v2_aggregate_function_stats_info_handle info,
                                                                    duckdb_v2_stats_handle *stats,
                                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() { *stats = Convert(&Convert(info)->result_stats); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_aggregate_function(duckdb_v2_connection_handle conn,
                                                                 duckdb_v2_aggregate_function_handle function,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &self = *Convert(function);
			auto info = duckdb::CreateAggregateFunctionInfo(self.Build(*context.db));
			self.docs.AddTo(self.signature, info);
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			catalog.CreateFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_aggregate_function(duckdb_v2_extension_handle extension,
                                                                duckdb_v2_aggregate_function_handle function,
                                                                duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		duckdb::CreateAggregateFunctionInfo info(self.Build(loader.GetDatabaseInstance()));
		self.docs.AddTo(self.signature, info);
		info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_aggregate_function_destroy(duckdb_v2_aggregate_function_handle *function) {
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
