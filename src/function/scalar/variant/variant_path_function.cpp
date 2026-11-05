#include "duckdb/function/scalar/variant_path_function.hpp"

namespace duckdb {

ScalarFunctionSet VariantPathFunction::CreateFunctionSet(const Identifier &name, const scalar_function_t &function,
                                                         const LogicalType &return_type, const bool path_optional,
                                                         const init_local_state_t init_state) {
	ScalarFunctionSet fun_set(name);

	if (path_optional) {
		ScalarFunction no_path_fun({}, return_type, function, VariantBindUtils::VariantPathBind, nullptr);
		no_path_fun.GetSignature().AddParameter("input_variant", LogicalType::VARIANT());
		fun_set.AddFunction(std::move(no_path_fun));
	}

	ScalarFunction path_fun({}, return_type, function, VariantBindUtils::VariantPathBind, nullptr, init_state);
	path_fun.GetSignature()
	    .AddParameter("input_variant", LogicalType::VARIANT())
	    .AddParameter("path", LogicalType::VARCHAR);
	fun_set.AddFunction(std::move(path_fun));

	ScalarFunction path_list_fun({}, LogicalType::LIST(return_type), function, VariantBindUtils::VariantPathBind,
	                             nullptr, init_state);
	path_list_fun.GetSignature()
	    .AddParameter("input_variant", LogicalType::VARIANT())
	    .AddParameter("path", LogicalType::LIST(LogicalType::VARCHAR));
	fun_set.AddFunction(std::move(path_list_fun));

	return fun_set;
}

} // namespace duckdb
