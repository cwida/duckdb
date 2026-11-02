#include "duckdb/common/vector/array_vector.hpp"
#include "core_functions/scalar/array_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/storage/statistics/array_stats.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

void ArrayValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto array_type = result.GetType();

	D_ASSERT(array_type.id() == LogicalTypeId::ARRAY);
	D_ASSERT(args.ColumnCount() == ArrayType::GetSize(array_type));

	auto &child_type = ArrayType::GetChildType(array_type);

	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	for (idx_t i = 0; i < args.ColumnCount(); i++) {
		if (args.data[i].GetVectorType() != VectorType::CONSTANT_VECTOR) {
			result.SetVectorType(VectorType::FLAT_VECTOR);
		}
	}

	auto num_rows = args.size();
	auto num_columns = args.ColumnCount();

	auto &child = ArrayVector::GetChildMutable(result);

	if (num_columns > 1) {
		// Ensure that the child has a validity mask of the correct size
		// The SetValue call below expects the validity mask to be initialized
		auto &child_validity = FlatVector::ValidityMutable(child);
		child_validity.Resize(num_rows * num_columns);
	}

	for (idx_t i = 0; i < num_rows; i++) {
		for (idx_t j = 0; j < num_columns; j++) {
			auto val = args.GetValue(j, i).DefaultCastAs(child_type);
			child.SetValue((i * num_columns) + j, val);
		}
	}

	result.Verify();
}

void ArrayValueResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw InvalidInputException("array_value requires at least one argument");
	}

	// construct return type
	LogicalType child_type = arguments[0]->GetReturnType();
	for (idx_t i = 1; i < arguments.size(); i++) {
		child_type = LogicalType::MaxLogicalType(context, child_type, arguments[i]->GetReturnType());
	}

	if (arguments.size() > ArrayType::MAX_ARRAY_SIZE) {
		throw OutOfRangeException("Array size exceeds maximum allowed size");
	}

	// Cast all arguments to the common child type so that execution and statistics see matching types.
	auto &function_args = bound_function.GetArguments();
	function_args.clear();
	function_args.reserve(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		function_args.push_back(child_type);
	}

	bound_function.SetReturnType(LogicalType::ARRAY(child_type, arguments.size()));
}

unique_ptr<BaseStatistics> ArrayValueStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto list_stats = ArrayStats::CreateEmpty(expr.GetReturnType());
	auto &list_child_stats = ArrayStats::GetChildStats(list_stats);
	for (idx_t i = 0; i < child_stats.size(); i++) {
		list_child_stats.Merge(child_stats[i]);
	}
	list_stats.SetHasNoNullFast();
	return list_stats.ToUnique();
}

} // namespace

ScalarFunction ArrayValueFun::GetFunction() {
	// the arguments and return types are actually set in the binder function
	ScalarFunction fun("array_value", {}, LogicalTypeId::ARRAY, ArrayValueFunction, VariableReturnBindData::Bind,
	                   ArrayValueStats);
	fun.SetResolveTypesCallback(ArrayValueResolveTypes);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// casting the arguments to the common child type can fail, e.g. for arrays of different sizes
	fun.SetFallible();
	return fun;
}

} // namespace duckdb
