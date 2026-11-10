#include "duckdb/main/capi_v2/capi_v2_internal.hpp"
#include "duckdb/main/capi_v2/capi_v2_function_internal.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb::capiv2 {

//! A cast is a pair of scalar functions of one parameter. These are the calls of them that a bound cast executes.
class CV2CastBoundData final : public BoundCastData {
public:
	CV2CastBoundData(shared_ptr<Expression> call, shared_ptr<Expression> try_call)
	    : call(std::move(call)), try_call(std::move(try_call)) {
	}

	auto Copy() const -> unique_ptr<BoundCastData> override {
		return make_uniq<CV2CastBoundData>(call, try_call);
	}

public:
	shared_ptr<Expression> call;
	shared_ptr<Expression> try_call;
};

//! The per-cast info the engine hands to the bind function
class CV2CastBindFunctionInfo final : public BindCastInfo {
public:
	CV2CastBindFunctionInfo(ScalarFunction function, ScalarFunction try_function)
	    : function(std::move(function)), try_function(std::move(try_function)) {
	}

	ScalarFunction function;
	ScalarFunction try_function;
};

//! Holds the executor that runs the call, so that the state of the scalar function lives as long as the cast does
class CV2CastLocalState final : public FunctionLocalState {
public:
	unique_ptr<CV2CastExecutor> executor;
	unique_ptr<CV2CastExecutor> try_executor;
};

static auto MakeExecutor(optional_ptr<ClientContext> context, const Expression &call, bool is_try)
    -> unique_ptr<CV2CastExecutor> {
	// Without a client context (e.g. while an index is built) the scalar function runs with a NULL context
	if (context) {
		return make_uniq<CV2CastExecutor>(*context, call, is_try);
	}
	return make_uniq<CV2CastExecutor>(call, is_try);
}

static auto CV2CastInitLocalState(CastLocalStateParameters &parameters) -> unique_ptr<FunctionLocalState> {
	auto state = make_uniq<CV2CastLocalState>();
	auto &bound_data = parameters.cast_data->Cast<CV2CastBoundData>();
	state->executor = MakeExecutor(parameters.context, *bound_data.call, false);
	state->try_executor = MakeExecutor(parameters.context, *bound_data.try_call, true);
	return std::move(state);
}

static auto CV2CastExec(Vector &input, Vector &output, idx_t count, CastParameters &parameters) -> bool {
	const auto &bound_data = parameters.cast_data->Cast<CV2CastBoundData>();

	// A try cast is exactly the case where the engine handed us a slot to write a message into instead of aborting.
	const auto is_try_cast = parameters.error_message != nullptr;

	unique_ptr<CV2CastExecutor> temporary;
	optional_ptr<CV2CastExecutor> executor;
	if (parameters.local_state) {
		auto &local_state = parameters.local_state->Cast<CV2CastLocalState>();
		executor = is_try_cast ? local_state.try_executor.get() : local_state.executor.get();
	} else {
		temporary = MakeExecutor(nullptr, is_try_cast ? *bound_data.try_call : *bound_data.call, is_try_cast);
		executor = temporary.get();
	}
	executor->error.clear();

	DataChunk chunk;
	chunk.InitializeEmpty(vector<LogicalType> {input.GetType()});
	chunk.data[0].Reference(input);
	chunk.SetCardinality(count);
	{
		CV2ActiveCastScope scope(*executor);
		executor->ExecuteExpression(chunk, output);
	}

	if (!executor->error.empty()) {
		// The engine discards the message and keeps whatever the function left in the output vector.
		HandleCastError::AssignError(executor->error, parameters);
		return false;
	}
	return true;
}

static auto BindCall(BindCastInput &input, const ScalarFunction &function, const LogicalType &source,
                     const LogicalType &target) -> unique_ptr<Expression> {
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(source, 0));

	unique_ptr<Expression> call;
	if (input.context) {
		call = FunctionBinder(*input.context).BindScalarFunction(function, std::move(children));
	} else {
		// The bind callback needs a client context: without one, the call is made as it is
		call = make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(children), nullptr);
	}
	if (call->GetReturnType() != target) {
		throw InvalidInputException("Cast function %s returns %s, not the %s it casts to", function.GetName(),
		                            call->GetReturnType().ToString(), target.ToString());
	}
	return call;
}

static auto CV2CastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) -> BoundCastInfo {
	D_ASSERT(input.info);
	const auto &info = input.info->Cast<CV2CastBindFunctionInfo>();

	auto call = BindCall(input, info.function, source, target);
	auto try_call = BindCall(input, info.try_function, source, target);

	// A cast is only as stable as its least stable function
	auto stability = FunctionStability::CONSISTENT;
	for (auto &bound : {call.get(), try_call.get()}) {
		if (bound->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
			continue;
		}
		auto function_stability = bound->Cast<BoundFunctionExpression>().Function().GetStability();
		if (function_stability == FunctionStability::VOLATILE || stability == FunctionStability::VOLATILE) {
			stability = FunctionStability::VOLATILE;
		} else if (function_stability == FunctionStability::CONSISTENT_WITHIN_QUERY) {
			stability = function_stability;
		}
	}
	BoundCastInfo cast_info(CV2CastExec,
	                        make_uniq<CV2CastBoundData>(shared_ptr<Expression>(std::move(call)),
	                                                    shared_ptr<Expression>(std::move(try_call))),
	                        CV2CastInitLocalState);
	cast_info.SetStability(stability);
	return cast_info;
}

//! The registration of a cast made from a scalar function
struct CV2CastRegistration {
	LogicalType source_type;
	LogicalType target_type;
	shared_ptr<BindCastInfo> info;

	CV2CastRegistration(duckdb_v2_scalar_function_handle function, duckdb_v2_scalar_function_handle try_function,
	                    DatabaseInstance &target) {
		auto scalar_function = BuildScalarFunctionForCast(function, target);
		auto try_scalar_function = BuildScalarFunctionForCast(try_function, target);
		auto &signature = scalar_function.GetSignature();
		auto &try_signature = try_scalar_function.GetSignature();
		source_type = signature.GetParameter(0).GetType();
		target_type = signature.GetReturnType();
		if (try_signature.GetParameter(0).GetType() != source_type || try_signature.GetReturnType() != target_type) {
			throw InvalidInputException("The try function of a cast must have the same parameter and return type as "
			                            "the cast function: %s -> %s, not %s -> %s",
			                            source_type.ToString(), target_type.ToString(),
			                            try_signature.GetParameter(0).GetType().ToString(),
			                            try_signature.GetReturnType().ToString());
		}
		info = make_shared_ptr<CV2CastBindFunctionInfo>(std::move(scalar_function), std::move(try_scalar_function));
	}
};

} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public Functions
//----------------------------------------------------------------------------------------------------------------------

using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_connection_register_cast(duckdb_v2_connection_handle conn,
                                                   duckdb_v2_scalar_function_handle function,
                                                   duckdb_v2_scalar_function_handle try_function,
                                                   int64_t implicit_cast_cost, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(conn);
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(try_function);
	return WithErrorHandler(err, [&]() {
		auto &db = *Convert(conn)->context->db;
		CV2CastRegistration registration(function, try_function, db);
		auto &casts = duckdb::DBConfig::GetConfig(db).GetCastFunctions();
		casts.RegisterCastFunction(registration.source_type, registration.target_type, CV2CastBind, registration.info,
		                           implicit_cast_cost);
	});
}

DUCKDB_V2_ERROR duckdb_v2_extension_register_cast(duckdb_v2_extension_handle extension,
                                                  duckdb_v2_scalar_function_handle function,
                                                  duckdb_v2_scalar_function_handle try_function,
                                                  int64_t implicit_cast_cost, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(extension);
	DUCKDB_CHECK_ARG(function);
	DUCKDB_CHECK_ARG(try_function);
	return WithErrorHandler(err, [&]() {
		auto &loader = GetExtensionLoader(extension);
		CV2CastRegistration registration(function, try_function, loader.GetDatabaseInstance());
		loader.RegisterCastFunction(registration.source_type, registration.target_type, CV2CastBind, registration.info,
		                            implicit_cast_cost);
	});
}
