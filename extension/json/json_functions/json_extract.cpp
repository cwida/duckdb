#include "json_executors.hpp"

namespace duckdb {

static inline optional<string_t> ExtractFromVal(yyjson_val *val, yyjson_alc *alc, Vector &) {
	return JSONCommon::WriteVal<yyjson_val>(val, alc);
}

static inline optional<string_t> ExtractStringFromVal(yyjson_val *val, yyjson_alc *alc, Vector &) {
	switch (yyjson_get_tag(val)) {
	case YYJSON_TYPE_NULL | YYJSON_SUBTYPE_NONE:
		return {};
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NOESC:
	case YYJSON_TYPE_STR | YYJSON_SUBTYPE_NONE:
		return string_t(unsafe_yyjson_get_str(val), unsafe_yyjson_get_len(val));
	default:
		return JSONCommon::WriteVal<yyjson_val>(val, alc);
	}
}

static void ExtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<string_t>(args, state, result, ExtractFromVal);
}

static void ExtractManyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<string_t>(args, state, result, ExtractFromVal);
}

static void ExtractStringFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::BinaryExecute<string_t>(args, state, result, ExtractStringFromVal);
}

static void ExtractStringManyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	JSONExecutors::ExecuteMany<string_t>(args, state, result, ExtractStringFromVal);
}

static void GetExtractFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction index_fun({}, LogicalType::JSON(), ExtractFunction, JSONReadFunctionData::Bind, nullptr,
	                         JSONFunctionLocalState::Init);
	index_fun.GetSignature().AddParameter("json", input_type).AddParameter("index", LogicalType::BIGINT);
	index_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	index_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(index_fun);
	ScalarFunction path_fun({}, LogicalType::JSON(), ExtractFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	path_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	path_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::JSON()), ExtractManyFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetExtractFunction() {
	// Generic extract function
	ScalarFunctionSet set("json_extract");
	GetExtractFunctionsInternal(set, LogicalType::VARCHAR);
	GetExtractFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameter(0).GetType().IsJSONType() && sig.GetParameter(1).GetType().IsNumeric()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

static void GetExtractStringFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction index_fun({}, LogicalType::VARCHAR, ExtractStringFunction, JSONReadFunctionData::Bind, nullptr,
	                         JSONFunctionLocalState::Init);
	index_fun.GetSignature().AddParameter("json", input_type).AddParameter("index", LogicalType::BIGINT);
	set.AddFunction(index_fun);
	ScalarFunction path_fun({}, LogicalType::VARCHAR, ExtractStringFunction, JSONReadFunctionData::Bind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("json", input_type).AddParameter("path", LogicalType::VARCHAR);
	set.AddFunction(path_fun);
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::VARCHAR), ExtractStringManyFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("json", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet JSONFunctions::GetExtractStringFunction() {
	// String extract function
	ScalarFunctionSet set("json_extract_string");
	GetExtractStringFunctionsInternal(set, LogicalType::VARCHAR);
	GetExtractStringFunctionsInternal(set, LogicalType::JSON());
	set.ApplyToFunctions([](ScalarFunction &func) {
		const auto &sig = func.GetSignature();
		if (sig.GetParameter(0).GetType().IsJSONType() && sig.GetParameter(1).GetType().IsNumeric()) {
			return;
		}
		func.SetFallible();
	});
	return set;
}

} // namespace duckdb
