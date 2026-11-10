#include "duckdb/common/limits.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

bool VectorOperations::TryCast(CastFunctionSet &set, GetCastFunctionInput &input, Vector &source, Vector &result,
                               idx_t count, string *error_message, bool strict, const bool nullify_parent) {
	auto cast_function = set.GetCastFunction(source.GetType(), result.GetType(), input);
	unique_ptr<FunctionLocalState> local_state;
	if (cast_function.HasInitLocalState()) {
		CastLocalStateParameters lparameters(input.context, cast_function.GetCastData());
		local_state = cast_function.InitLocalState(lparameters);
	}
	CastParameters parameters(cast_function.GetCastData().get(), strict, error_message, local_state.get(),
	                          nullify_parent);
	return cast_function.Cast(source, result, count, parameters);
}

bool VectorOperations::DefaultTryCast(Vector &source, Vector &result, idx_t count, string *error_message, bool strict) {
	CastFunctionSet set;
	GetCastFunctionInput input;
	return VectorOperations::TryCast(set, input, source, result, count, error_message, strict);
}

void VectorOperations::DefaultCast(Vector &source, Vector &result, idx_t count, bool strict) {
	VectorOperations::DefaultTryCast(source, result, count, nullptr, strict);
}

bool VectorOperations::TryCast(ClientContext &context, Vector &source, Vector &result, idx_t count,
                               string *error_message, bool strict, const bool nullify_parent) {
	auto &config = DBConfig::GetConfig(context);
	auto &set = config.GetCastFunctions();
	GetCastFunctionInput get_input(context);
	return VectorOperations::TryCast(set, get_input, source, result, count, error_message, strict, nullify_parent);
}

bool VectorOperations::TryCast(Context &context, Vector &source, Vector &result, idx_t count, string *error_message,
                               bool strict, const bool nullify_parent) {
	GetCastFunctionInput get_input(context);
	return VectorOperations::TryCast(CastFunctionSet::Get(context), get_input, source, result, count, error_message,
	                                 strict, nullify_parent);
}

void VectorOperations::Cast(Context &context, Vector &source, Vector &result, idx_t count, bool strict) {
	VectorOperations::TryCast(context, source, result, count, nullptr, strict);
}

void VectorOperations::Cast(ClientContext &context, Vector &source, Vector &result, idx_t count, bool strict) {
	VectorOperations::TryCast(context, source, result, count, nullptr, strict);
}

} // namespace duckdb
