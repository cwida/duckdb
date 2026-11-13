#include "json_executors.hpp"

namespace duckdb {

static inline optional<string_t> GetType(yyjson_val *val, yyjson_alc *, Vector &) {
	return JSONCommon::ValTypeToStringT(val);
}

static void UnaryTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::UnaryExecute<string_t>(args, state, result, GetType);
}

static void BinaryTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<string_t>(args, state, result, GetType);
}

static void ManyTypeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<string_t>(args, state, result, GetType);
}

static void GetTypeFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction unary_fun({}, LogicalType::VARCHAR, UnaryTypeFunction, nullptr, nullptr,
	                         JSONFunctionLocalState::Init);
	unary_fun.GetSignature().AddParameter("json", input_type);
	set.AddFunction(unary_fun);
	ScalarFunction path_fun({}, LogicalType::VARCHAR, BinaryTypeFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	path_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::VARCHAR), ManyTypeFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetTypeFunction() {
	ScalarFunctionSet set("json_type");
	GetTypeFunctionsInternal(set, LogicalType::VARCHAR);
	GetTypeFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameterCount() == 1 && sig.GetParameter(0).GetType().IsJSONType()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

} // namespace duckdb
