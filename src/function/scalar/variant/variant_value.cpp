#include "duckdb/common/types/variant_iterator.hpp"
#include "duckdb/function/scalar/variant_functions.hpp"
#include "duckdb/function/scalar/variant_path_function.hpp"
#include "duckdb/common/types/value.hpp"

namespace duckdb {

static void ValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	VariantPathFunction::Execute<string_t>(args, state, result, VariantValue::GetValue);
}

static void ValueManyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	VariantPathFunction::Execute<string_t>(args, state, result, VariantValue::GetValue);
}

static void GetValueFunctionsInternal(ScalarFunctionSet &set, const LogicalType &input_type) {
	ScalarFunction index_fun({}, LogicalType::VARCHAR, ValueFunction, VariantBindUtils::VariantPathBind, nullptr,
	                         JSONFunctionLocalState::Init);
	index_fun.GetSignature().AddParameter("variant", LogicalType::VARIANT).AddParameter("index", LogicalType::BIGINT);
	index_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(index_fun);

	ScalarFunction path_fun({}, LogicalType::VARCHAR, ValueFunction, VariantBindUtils::VariantPathBind, nullptr,
	                        JSONFunctionLocalState::Init);
	path_fun.GetSignature().AddParameter("variant", input_type).AddParameter("path", LogicalType::VARCHAR);
	path_fun.SetResolveTypesCallback(JSONReadFunctionData::ResolveTypes);
	set.AddFunction(path_fun);
    
	ScalarFunction many_fun({}, LogicalType::LIST(LogicalType::VARCHAR), ValueManyFunction,
	                        JSONReadManyFunctionData::Bind, nullptr, JSONFunctionLocalState::Init);
	many_fun.GetSignature()
	    .AddParameter("variant", input_type)
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	set.AddFunction(many_fun);
}

ScalarFunctionSet VariantValueFun::GetValueFunction() {
	// The value function is just like the extract function but returns NULL if the JSON is not a scalar value
	ScalarFunctionSet set("variant_value");
	GetValueFunctionsInternal(set, LogicalType::VARCHAR);
	GetValueFunctionsInternal(set, LogicalType::JSON());
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

