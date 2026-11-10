#include "core_functions/scalar/generic_functions.hpp"

namespace duckdb {

static void HashFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	args.Hash(result);
}

ScalarFunction HashFun::GetFunction() {
	auto hash_fun = ScalarFunction({}, LogicalType::HASH, HashFunction);
	hash_fun.GetSignature().AddParameter("value", LogicalType::ANY);
	hash_fun.GetSignature().AddArgs("args", LogicalType::ANY);
	hash_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// hash the arguments after their combinable collations
	hash_fun.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	return hash_fun;
}

} // namespace duckdb
