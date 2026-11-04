#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb::capiv2 {

class CV2FunctionData final : public FunctionData {
public:
	// The bind data set by the user
	shared_ptr<CV2UserData> handle;

	auto Copy() const -> unique_ptr<FunctionData> override {
		auto copy = make_uniq<CV2FunctionData>();
		copy->handle = handle;
		return std::move(copy);
	}

	auto Equals(const FunctionData &other) const -> bool override {
		const auto &other_data = other.Cast<CV2FunctionData>();
		return handle && other_data.handle && handle->Equals(*other_data.handle);
	}
};

class CV2FunctionLocalState final : public FunctionLocalState {
public:
	CV2UserData handle;
};

static constexpr const char *SCALAR_BIND_DATA_ERROR =
    "Scalar functions set the bind data through duckdb_v2_scalar_function_bind_set_bind_data";

//! The arguments of a scalar call before they are cast to the parameter types - the bind data cannot be set
class CV2ResolveTypesBindInfo final : public CV2FunctionBindInfo {
public:
	CV2ResolveTypesBindInfo(const BoundScalarFunction &function, void *user_data,
	                        const ResolveScalarFunctionTypesInput &input)
	    : CV2FunctionBindInfo(function, user_data), input(input) {
		D_ASSERT(GetArgCount() == input.GetArgumentCount());
		bind_data_error = SCALAR_BIND_DATA_ERROR;
	}

	LogicalType GetArgType(idx_t index) const override {
		return input.GetArgumentType(index);
	}
	Value GetArgValue(idx_t index) const override {
		return input.GetConstant(index);
	}

private:
	const ResolveScalarFunctionTypesInput &input;
};

class CV2BindInfo {
public:
	//! The bind data the callback set, or nullptr
	shared_ptr<CV2UserData> out_bind_data;
};

static auto Convert(duckdb_v2_scalar_function_bind_info_handle info) -> CV2BindInfo * {
	return reinterpret_cast<CV2BindInfo *>(info);
}
static auto Convert(CV2BindInfo *info) -> duckdb_v2_scalar_function_bind_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_bind_info_handle>(info);
}

class CV2ResolveTypesInfo {
public:
	explicit CV2ResolveTypesInfo(BoundScalarFunction &bound_function) : bound_function(bound_function) {
	}

	BoundScalarFunction &bound_function;
};

static auto Convert(duckdb_v2_scalar_function_resolve_types_info_handle info) -> CV2ResolveTypesInfo * {
	return reinterpret_cast<CV2ResolveTypesInfo *>(info);
}
static auto Convert(CV2ResolveTypesInfo *info) -> duckdb_v2_scalar_function_resolve_types_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_resolve_types_info_handle>(info);
}

class CV2InitInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	duckdb_v2_opaque out_init_data = {};
};

static auto Convert(duckdb_v2_scalar_function_init_info_handle info) -> CV2InitInfo * {
	return reinterpret_cast<CV2InitInfo *>(info);
}
static auto Convert(CV2InitInfo *info) -> duckdb_v2_scalar_function_init_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_init_info_handle>(info);
}

class CV2ExecInfo {
public:
	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;
	void *in_init_data = nullptr;

	DataChunk *input;
	//! Tells the four parts of the argument list apart
	const BoundScalarFunction *function = nullptr;
	Vector *result;
};

static auto Convert(duckdb_v2_scalar_function_exec_info_handle info) -> CV2ExecInfo * {
	return reinterpret_cast<CV2ExecInfo *>(info);
}
static auto Convert(CV2ExecInfo *info) -> duckdb_v2_scalar_function_exec_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_exec_info_handle>(info);
}

class CV2ScalarStatsInfo {
public:
	CV2ScalarStatsInfo(const BoundScalarFunction &function, BaseStatistics &result)
	    : function(function), result_stats(CV2Stats::Writable(result)) {
	}

	void *in_user_data = nullptr;
	void *in_bind_data = nullptr;

	//! Tells the four parts of the argument list apart
	const BoundScalarFunction &function;
	vector<CV2Stats> arg_stats;
	CV2Stats result_stats;
};

static auto Convert(duckdb_v2_scalar_function_stats_info_handle info) -> CV2ScalarStatsInfo * {
	return reinterpret_cast<CV2ScalarStatsInfo *>(info);
}
static auto Convert(CV2ScalarStatsInfo *info) -> duckdb_v2_scalar_function_stats_info_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_stats_info_handle>(info);
}

class CV2ScalarFunctionInfo : public ScalarFunctionInfo {
public:
	duckdb_v2_scalar_function_resolve_types_callback_fn resolve_types_cb = nullptr;
	duckdb_v2_scalar_function_bind_callback_fn bind_cb = nullptr;
	duckdb_v2_scalar_function_init_callback_fn init_cb = nullptr;
	duckdb_v2_scalar_function_exec_callback_fn exec_cb = nullptr;
	duckdb_v2_scalar_function_stats_callback_fn stats_cb = nullptr;
	shared_ptr<CV2UserData> user_data = nullptr;
};

static auto CV2ScalarResolveTypes(ResolveScalarFunctionTypesInput &input) -> void {
	auto &bound_function = input.GetBoundFunction();
	const auto &info = bound_function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	CV2ResolveTypesBindInfo bind_info(bound_function, info.user_data ? info.user_data->GetData() : nullptr, input);
	CV2ResolveTypesInfo result_info(bound_function);

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.GetClientContext());
	info.resolve_types_cb(Convert(&bind_info), Convert(&result_info), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2ScalarBind(BindScalarFunctionInput &input) -> unique_ptr<FunctionData> {
	const auto &info = input.GetBoundFunction().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto &bound_function = input.GetBoundFunction();
	CV2ExpressionBindInfo bind_info(bound_function, info.user_data ? info.user_data->GetData() : nullptr, input);
	bind_info.bind_data_error = SCALAR_BIND_DATA_ERROR;
	CV2BindInfo result_info;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(input.GetClientContext());
	info.bind_cb(Convert(&bind_info), Convert(&result_info), Convert(&callback_context), &err_ptr);

	unique_ptr<FunctionData> result = nullptr;
	if (result_info.out_bind_data) {
		auto set_result = make_uniq<CV2FunctionData>();
		set_result->handle = std::move(result_info.out_bind_data);
		result = std::move(set_result);
	}

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result;
}

static auto CV2ScalarInit(ExpressionState &state, const BoundFunctionExpression &expr, FunctionData *bind_data)
    -> unique_ptr<FunctionLocalState> {
	const auto &info = expr.Function().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto user_bind_data = bind_data ? bind_data->Cast<CV2FunctionData>().handle : nullptr;

	CV2InitInfo args = {};
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.in_bind_data = user_bind_data ? user_bind_data->GetData() : nullptr;

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);
	CV2CallbackContext callback_context(state.GetContext());
	info.init_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	unique_ptr<FunctionLocalState> result = nullptr;
	if (args.out_init_data.ptr) {
		auto set_result = make_uniq<CV2FunctionLocalState>();
		set_result->handle = CV2UserData(args.out_init_data.ptr, args.out_init_data.destroy, args.out_init_data.equals);
		result = std::move(set_result);
	}

	// Throw after setting the state, so that it is destroyed even if we error.

	if (err.HasError()) {
		err.ThrowAsException();
	}

	return result;
}

static auto CV2ScalarExec(DataChunk &input, ExpressionState &state, Vector &result) -> void {
	auto &expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = expr.Function().GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	CV2ExecInfo args;
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	args.input = &input;
	args.function = &expr.Function();
	args.result = &result;

	// Setup bind data (if provided)
	if (auto bind_ptr = expr.BindInfo().get()) {
		const auto &bind_data = bind_ptr->Cast<CV2FunctionData>();
		args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	}

	// Setup local state (if provided)
	if (auto state_ptr = ExecuteFunctionState::GetFunctionState(state)) {
		const auto &state_data = state_ptr->Cast<CV2FunctionLocalState>();
		args.in_init_data = state_data.handle.GetData();
	}

	CV2ErrorInfo err = {};
	auto err_ptr = Convert(&err);

	CV2CallbackContext callback_context(state.GetContext());
	info.exec_cb(Convert(&args), Convert(&callback_context), &err_ptr);

	if (err.HasError()) {
		err.ThrowAsException();
	}
}

static auto CV2ScalarStats(ClientContext &context, FunctionStatisticsInput &input) -> unique_ptr<BaseStatistics> {
	const auto &function = input.expr.Function();
	const auto &info = function.GetExtraFunctionInfo().Cast<CV2ScalarFunctionInfo>();

	auto result = BaseStatistics::CreateUnknown(input.expr.GetReturnType());
	CV2ScalarStatsInfo args(function, result);
	args.in_user_data = info.user_data ? info.user_data->GetData() : nullptr;
	if (input.bind_data) {
		const auto &bind_data = input.bind_data->Cast<CV2FunctionData>();
		args.in_bind_data = bind_data.handle ? bind_data.handle->GetData() : nullptr;
	}
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

class CV2ScalarFunction {
public:
	explicit CV2ScalarFunction(DatabaseInstance &db) : db(db) {
		// The exec callback's error slot makes failure part of the API contract, so the function defaults to fallible:
		// a non-fallible function turns any execution error reported through the slot into an internal error.
		properties.SetFallible();
	}

	//! Validates the configuration and builds the scalar function to register on `target`.
	ScalarFunction Build(DatabaseInstance &target) {
		CheckRegistrationTarget(db, target, "scalar function");
		if (name.empty()) {
			throw InvalidInputException("Function name cannot be empty.");
		}
		if (!info.exec_cb) {
			throw InvalidInputException("Exec callback must be set for the function.");
		}

		const auto &return_type = signature.GetReturnType();

		// ANY is allowed as a placeholder return type only when a resolve types callback is present to resolve the
		// actual type.
		if (return_type.id() == LogicalTypeId::ANY) {
			if (info.resolve_types_cb == nullptr) {
				throw InvalidInputException(
				    "An ANY return type requires a resolve types callback to set the concrete return type.");
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

		ScalarFunction function(name, {}, return_type, CV2ScalarExec);
		function.SetProperties(properties);
		function.GetSignature() = signature;

		if (info.resolve_types_cb) {
			function.SetResolveTypesCallback(CV2ScalarResolveTypes);
		}
		if (info.bind_cb) {
			function.SetBindCallback(CV2ScalarBind);
		}
		if (info.init_cb) {
			function.SetInitStateCallback(CV2ScalarInit);
		}
		if (info.stats_cb) {
			function.SetStatisticsCallback(CV2ScalarStats);
		}
		function.SetExtraFunctionInfo<CV2ScalarFunctionInfo>(std::move(info));

		return function;
	}

public:
	//! The database it was created for: the only one it can be registered on.
	DatabaseInstance &db;
	FunctionSignature signature;
	CV2FunctionDocs docs;
	CV2ScalarFunctionInfo info;
	Identifier name;
	FunctionProperties properties;
};

static auto Convert(duckdb_v2_scalar_function_handle func) -> CV2ScalarFunction * {
	return reinterpret_cast<CV2ScalarFunction *>(func);
}
static auto Convert(CV2ScalarFunction *func) -> duckdb_v2_scalar_function_handle {
	return reinterpret_cast<duckdb_v2_scalar_function_handle>(func);
}

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_scalar_function_create(duckdb_v2_factory_handle factory,
                                                 duckdb_v2_scalar_function_handle *function,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(function);
	*function = nullptr;
	return WithErrorHandler(err, [&]() {
		auto result = duckdb::make_uniq<CV2ScalarFunction>(Convert(factory)->GetDatabase());
		*function = Convert(result.release());
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_name(duckdb_v2_scalar_function_handle function, const duckdb_v2_str *name,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(name);
	return WithErrorHandler(err, [&]() { Convert(function)->name = duckdb::Identifier(ConvertIdentifierName(name)); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_get_docs(duckdb_v2_scalar_function_handle function,
                                                   duckdb_v2_function_docs_handle *docs,
                                                   duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(docs);
	return WithErrorHandler(err, [&]() { *docs = Convert(&Convert(function)->docs); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_get_signature(duckdb_v2_scalar_function_handle function,
                                                        duckdb_v2_function_signature_handle *sig,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(sig);
	return WithErrorHandler(err, [&]() { *sig = Convert(&Convert(function)->signature); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_user_data(duckdb_v2_scalar_function_handle function,
                                                        duckdb_v2_opaque *user_data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(user_data);
	return WithErrorHandler(err, [&]() {
		Convert(function)->info.user_data =
		    duckdb::make_shared_ptr<CV2UserData>(user_data->ptr, user_data->destroy, user_data->equals);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_property(duckdb_v2_scalar_function_handle function,
                                                       DUCKDB_V2_FUNCTION_PROPERTY_KEY key,
                                                       DUCKDB_V2_FUNCTION_PROPERTY_VALUE value,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { SetScalarFunctionProperty(Convert(function)->properties, key, value); });
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_set_resolve_types_callback(duckdb_v2_scalar_function_handle function,
                                                     duckdb_v2_scalar_function_resolve_types_callback_fn callback,
                                                     duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.resolve_types_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_bind_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_bind_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.bind_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_init_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_init_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.init_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_exec_callback(duckdb_v2_scalar_function_handle function,
                                                            duckdb_v2_scalar_function_exec_callback_fn callback,
                                                            duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.exec_cb = callback; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_set_stats_callback(duckdb_v2_scalar_function_handle function,
                                                             duckdb_v2_scalar_function_stats_callback_fn callback,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() { Convert(function)->info.stats_cb = callback; });
}

DUCKDB_V2_ERROR
duckdb_v2_scalar_function_resolve_types_set_return_type(duckdb_v2_scalar_function_resolve_types_info_handle info,
                                                        duckdb_v2_logical_type_handle return_type,
                                                        duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(return_type);
	return WithErrorHandler(err, [&]() {
		auto type = Convert(return_type);
		Convert(info)->bound_function.SetReturnType(*type);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_bind_set_bind_data(duckdb_v2_scalar_function_bind_info_handle info,
                                                             duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() {
		Convert(info)->out_bind_data =
		    data->ptr ? duckdb::make_shared_ptr<CV2UserData>(data->ptr, data->destroy, data->equals) : nullptr;
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_get_user_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_get_bind_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_init_set_init_data(duckdb_v2_scalar_function_init_info_handle info,
                                                             duckdb_v2_opaque *data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);

	return WithErrorHandler(err, [&]() { Convert(info)->out_init_data = *data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_user_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_bind_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_init_data(duckdb_v2_scalar_function_exec_info_handle info,
                                                             void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);

	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_init_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_row_count(duckdb_v2_scalar_function_exec_info_handle info,
                                                             idx_t *count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(count);

	return WithErrorHandler(err, [&]() { *count = Convert(info)->input->size(); });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_arg_count(duckdb_v2_scalar_function_exec_info_handle info,
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

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_arg(duckdb_v2_scalar_function_exec_info_handle info, idx_t index,
                                                       duckdb_v2_vector_handle *vector,
                                                       duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto &exec_info = *Convert(info);
		if (index >= exec_info.input->ColumnCount()) {
			throw duckdb::InvalidInputException("Index out of bounds in duckdb_v2_scalar_function_exec_get_arg");
		}
		*vector = Convert(&exec_info.input->data[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_exec_get_result(duckdb_v2_scalar_function_exec_info_handle info,
                                                          duckdb_v2_vector_handle *vector,
                                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(vector);

	return WithErrorHandler(err, [&]() {
		auto &exec_info = *Convert(info);
		*vector = Convert(exec_info.result);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_user_data(duckdb_v2_scalar_function_stats_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_user_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_bind_data(duckdb_v2_scalar_function_stats_info_handle info,
                                                              void **data, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(data);
	return WithErrorHandler(err, [&]() { *data = Convert(info)->in_bind_data; });
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_arg_count(duckdb_v2_scalar_function_stats_info_handle info,
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

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_arg_stats(duckdb_v2_scalar_function_stats_info_handle info,
                                                              idx_t index, duckdb_v2_stats_handle *stats,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() {
		auto &stats_info = *Convert(info);
		if (index >= stats_info.arg_stats.size()) {
			throw duckdb::InvalidInputException("Index out of bounds in duckdb_v2_scalar_function_stats_get_arg_stats");
		}
		*stats = Convert(&stats_info.arg_stats[index]);
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_stats_get_result_stats(duckdb_v2_scalar_function_stats_info_handle info,
                                                                 duckdb_v2_stats_handle *stats,
                                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(info);
	DUCKDB_CHECK_ARG(stats);
	return WithErrorHandler(err, [&]() { *stats = Convert(&Convert(info)->result_stats); });
}

DUCKDB_V2_ERROR duckdb_v2_connection_register_scalar_function(duckdb_v2_connection_handle conn,
                                                              duckdb_v2_scalar_function_handle function,
                                                              duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &context = *Convert(conn)->context;
		context.RunFunctionInTransaction([&]() {
			auto &self = *Convert(function);
			auto info = duckdb::CreateScalarFunctionInfo(self.Build(*context.db));
			self.docs.AddTo(self.signature, info);
			info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
			auto &catalog = duckdb::Catalog::GetSystemCatalog(context);
			catalog.CreateFunction(context, info);
		});
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_scalar_function(duckdb_v2_extension_handle extension,
                                                             duckdb_v2_scalar_function_handle function,
                                                             duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	return WithErrorHandler(err, [&]() {
		auto &self = *Convert(function);
		auto &loader = GetExtensionLoader(extension);
		duckdb::CreateScalarFunctionInfo info(self.Build(loader.GetDatabaseInstance()));
		self.docs.AddTo(self.signature, info);
		info.on_conflict = duckdb::OnCreateConflict::ALTER_ON_CONFLICT;
		loader.RegisterFunction(std::move(info));
	});
}

DUCKDB_V2_ERROR duckdb_v2_scalar_function_destroy(duckdb_v2_scalar_function_handle *function) {
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
