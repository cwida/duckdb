#include "core_functions/scalar/debug_functions.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/enum_util.hpp"

namespace duckdb {

static void VectorTypeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	auto data = ConstantVector::GetData<string_t>(result);
	auto &heap = StringVector::GetStringHeap(result);
	data[0] = heap.AddString(EnumUtil::ToString(input.data[0].GetVectorType()));
}

ScalarFunction VectorTypeFun::GetFunction() {
	auto vector_type_fun = ScalarFunction({}, LogicalType::VARCHAR, VectorTypeFunction);
	vector_type_fun.GetSignature().AddParameter("col", LogicalType::ANY);
	vector_type_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	vector_type_fun.SetStability(FunctionStability::VOLATILE);
	return vector_type_fun;
}

} // namespace duckdb
