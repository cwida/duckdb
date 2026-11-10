#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

static void StructPackFunction(DataChunk &args, ExpressionState &state, Vector &result) {
#ifdef DEBUG
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<VariableReturnBindData>();
	// this should never happen if the binder below is sane
	D_ASSERT(args.ColumnCount() == StructType::GetChildTypes(info.stype).size());
#endif
	if (args.ColumnCount() == 0) {
		// empty struct: no children to reference, the value is a single non-null constant
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, false);
		return;
	}
	bool all_const = true;
	auto &child_entries = StructVector::GetEntries(result);
	idx_t children_size = 0;
	for (idx_t i = 0; i < args.ColumnCount(); i++) {
		if (args.data[i].GetVectorType() != VectorType::CONSTANT_VECTOR) {
			all_const = false;
		}
		// same holds for this
		child_entries[i].Reference(args.data[i]);
		children_size = MaxValue<idx_t>(children_size, child_entries[i].size());
	}
	// set only the struct buffer's type/size - do not propagate to children
	// since children reference external vectors (args) that may have incompatible buffer types.
	// match the parent size to the (already-set) child vector size, not to the chunk cardinality - those can
	// differ when the caller is collapsing all-constant inputs to a single argument row.
	result.BufferMutable().SetVectorTypeOnly(all_const ? VectorType::CONSTANT_VECTOR : VectorType::FLAT_VECTOR);
	result.BufferMutable().SetVectorSizeOnly(children_size);
	result.Verify();
}

template <bool IS_STRUCT_PACK>
static unique_ptr<FunctionData> StructPackBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &args = input.GetArguments();

	if (IS_STRUCT_PACK) {
		child_list_t<LogicalType> fields;
		for (idx_t i = 0; i < args.size(); i++) {
			fields.emplace_back(input.GetArgumentName(i), args[i]->GetReturnType());
		}
		bound_function.SetReturnType(LogicalType::STRUCT(std::move(fields)));
	} else {
		vector<LogicalType> fields;
		for (idx_t i = 0; i < args.size(); i++) {
			fields.push_back(args[i]->GetReturnType());
		}
		bound_function.SetReturnType(LogicalType::TUPLE(std::move(fields)));
	}
	return make_uniq<VariableReturnBindData>(bound_function.GetReturnType());
}

static unique_ptr<BaseStatistics> StructPackStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto struct_stats = StructStats::CreateUnknown(expr.GetReturnType());
	struct_stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
	for (idx_t i = 0; i < child_stats.size(); i++) {
		StructStats::SetChildStats(struct_stats, i, child_stats[i]);
	}
	return struct_stats.ToUnique();
}

template <bool IS_STRUCT_PACK>
static unique_ptr<ParsedExpression> StructPackUnbind(FunctionUnbindInput &input) {
	auto &expression = input.expression;
	vector<FunctionArgument> arguments;
	for (idx_t i = 0; i < input.children.size(); i++) {
		auto name = IS_STRUCT_PACK ? StructType::GetChildName(expression.GetReturnType(), i) : Identifier();
		arguments.emplace_back(std::move(name), std::move(input.children[i]));
	}
	return make_uniq<FunctionExpression>(expression.Function().GetDefinition()->GetQualifiedName(),
	                                     std::move(arguments));
}

template <bool IS_STRUCT_PACK>
static ScalarFunction GetStructPackFunction() {
	ScalarFunction fun(IS_STRUCT_PACK ? "struct_pack" : "row", {},
	                   IS_STRUCT_PACK ? LogicalTypeId::STRUCT : LogicalTypeId::TUPLE, StructPackFunction,
	                   StructPackBind<IS_STRUCT_PACK>, StructPackStats);

	if (IS_STRUCT_PACK) {
		// struct_pack derives its field names from argument aliases, so the binder must capture argument expression
		// aliases as named-argument names. This also preserves the legacy behavior of allowing positional arguments
		// after named ones (the positional arguments simply take their expression's name as the field name).
		fun.GetSignature().AddKwargs("kwargs", LogicalType::ANY);
		fun.SetCaptureArgumentAliases(true);
	} else {
		// row produces an unnamed TUPLE, so it ignores the names of its arguments
		fun.GetSignature().AddArgs("args", LogicalType::ANY);
	}

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(VariableReturnBindData::Serialize);
	fun.SetDeserializeCallback(VariableReturnBindData::Deserialize);
	fun.SetUnbindCallback(StructPackUnbind<IS_STRUCT_PACK>);
	return fun;
}

ScalarFunction StructPackFun::GetFunction() {
	return GetStructPackFunction<true>();
}

ScalarFunction RowFun::GetFunction() {
	return GetStructPackFunction<false>();
}

} // namespace duckdb
