#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"
#include "duckdb/parser/parsed_data/alter_scalar_function_info.hpp"

namespace duckdb {

CreateScalarFunctionInfo::CreateScalarFunctionInfo(ScalarFunction function)
    : CreateFunctionInfo(CatalogType::SCALAR_FUNCTION_ENTRY), functions(function.name) {
	SetFunctionName(function.name);
	function.GetSignature().Verify();
	functions.AddFunction(std::move(function));
	internal = true;
}
CreateScalarFunctionInfo::CreateScalarFunctionInfo(ScalarFunctionSet set)
    : CreateFunctionInfo(CatalogType::SCALAR_FUNCTION_ENTRY), functions(std::move(set)) {
	SetFunctionName(functions.name);
	for (auto &func : functions.functions) {
		func->GetSignature().Verify();
	}
	functions.ApplyToFunctions([&](ScalarFunction &func) { func.name = functions.name; });
	internal = true;
}

unique_ptr<CreateInfo> CreateScalarFunctionInfo::Copy() const {
	ScalarFunctionSet set {GetFunctionName()};
	set.functions = functions.functions;
	auto result = make_uniq<CreateScalarFunctionInfo>(std::move(set));
	CopyFunctionProperties(*result);
	return std::move(result);
}

unique_ptr<AlterInfo> CreateScalarFunctionInfo::GetAlterInfo() const {
	return make_uniq_base<AlterInfo, AddScalarFunctionOverloadInfo>(
	    AlterEntryData(GetQualifiedName(), OnEntryNotFound::RETURN_NULL),
	    unique_ptr_cast<CreateInfo, CreateScalarFunctionInfo>(Copy()));
}

} // namespace duckdb
