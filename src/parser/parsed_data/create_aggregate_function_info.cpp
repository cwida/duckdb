#include "duckdb/parser/parsed_data/create_aggregate_function_info.hpp"

namespace duckdb {

CreateAggregateFunctionInfo::CreateAggregateFunctionInfo(AggregateFunction function)
    : CreateFunctionInfo(CatalogType::AGGREGATE_FUNCTION_ENTRY), functions(function.name) {
	SetFunctionName(function.name);
	function.GetSignature().Verify();
	functions.AddFunction(std::move(function));
	internal = true;
}

CreateAggregateFunctionInfo::CreateAggregateFunctionInfo(AggregateFunctionSet set)
    : CreateFunctionInfo(CatalogType::AGGREGATE_FUNCTION_ENTRY), functions(std::move(set)) {
	SetFunctionName(functions.name);
	for (auto &func : functions.functions) {
		func->GetSignature().Verify();
	}
	functions.ApplyToFunctions([&](AggregateFunction &func) { func.name = functions.name; });
	internal = true;
}

unique_ptr<CreateInfo> CreateAggregateFunctionInfo::Copy() const {
	auto result = make_uniq<CreateAggregateFunctionInfo>(functions);
	CopyFunctionProperties(*result);
	return std::move(result);
}

} // namespace duckdb
