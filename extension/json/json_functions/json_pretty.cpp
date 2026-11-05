#include "json_executors.hpp"

namespace duckdb {

//! Pretty Print a given JSON Document
optional<string_t> PrettyPrint(yyjson_val *val, yyjson_alc *alc, Vector &) {
	D_ASSERT(alc);
	size_t len_size_t;
	auto data = yyjson_val_write_opts(val, JSONCommon::WRITE_PRETTY_FLAG, alc, &len_size_t, nullptr);
	idx_t len = len_size_t;
	return string_t(data, len);
}

static void PrettyPrintFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto json_type = args.data[0].GetType();
	D_ASSERT(json_type == LogicalType::VARCHAR || json_type == LogicalType::JSON());

	JSONExecutors::UnaryExecute<string_t>(args, state, result, PrettyPrint);
}

static void GetPrettyPrintFunctionInternal(ScalarFunctionSet &set, const LogicalType &json) {
	ScalarFunction fun("json_pretty", {}, LogicalType::VARCHAR, PrettyPrintFunction, nullptr, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddParameter("json", json);
	set.AddFunction(fun);
}

ScalarFunctionSet JSONFunctions::GetPrettyPrintFunction() {
	ScalarFunctionSet set("json_pretty");
	GetPrettyPrintFunctionInternal(set, LogicalType::JSON());
	return set;
}

} // namespace duckdb
