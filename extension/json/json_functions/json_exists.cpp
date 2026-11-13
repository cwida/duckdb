#include "json_executors.hpp"

namespace duckdb {

static inline optional<bool> JSONExists(yyjson_val *val, yyjson_alc *, Vector &) {
	return val != nullptr;
}

static void BinaryExistsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<bool, false>(args, state, result, JSONExists);
}

static void ManyExistsFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<bool, false>(args, state, result, JSONExists);
}

static void GetExistsFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction single_fun({}, LogicalType::BOOLEAN, BinaryExistsFunction, JSONReadFunctionData::Bind, nullptr,
	                          JSONFunctionLocalState::Init);
	single_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	single_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(single_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::BOOLEAN), ManyExistsFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetExistsFunction() {
	ScalarFunctionSet set("json_exists");
	GetExistsFunctionsInternal(set, LogicalType::VARCHAR);
	GetExistsFunctionsInternal(set, LogicalType::JSON());
	set.SetFallible();
	return set;
}

} // namespace duckdb
