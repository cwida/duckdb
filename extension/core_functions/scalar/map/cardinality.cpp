#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"

namespace duckdb {

// the number of entries of a MAP or LIST (both are stored as a list of entries)
static void CardinalityFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &map = args.data[0];
	auto entries = map.Values<list_entry_t>();

	auto result_data = FlatVector::Writer<uint64_t>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		auto entry = entries[row];
		if (!entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		result_data.WriteValue(entries.GetValueUnsafe(row).length);
	}
}

static unique_ptr<FunctionData> CardinalityBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("Cardinality must have exactly one arguments");
	}

	auto &context = input.GetClientContext();
	switch (arguments[0]->GetReturnType().id()) {
	case LogicalTypeId::MAP:
	case LogicalTypeId::LIST:
		break;
	case LogicalTypeId::ARRAY: {
		// a fixed-size array is read as a list
		auto target_type = LogicalType::LIST(ArrayType::GetChildType(arguments[0]->GetReturnType()));
		arguments[0] = BoundCastExpression::AddCastToType(context, std::move(arguments[0]), target_type);
		break;
	}
	default:
		throw BinderException("Cardinality can only operate on MAPs, LISTs and ARRAYs");
	}

	bound_function.SetReturnType(LogicalType::UBIGINT);
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

ScalarFunction CardinalityFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::UBIGINT, CardinalityFunction, CardinalityBind);
	fun.GetSignature().AddParameter("map", LogicalType::ANY);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::DEFAULT_NULL_HANDLING);
	return fun;
}

} // namespace duckdb
