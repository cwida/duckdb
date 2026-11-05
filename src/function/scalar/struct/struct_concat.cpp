#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

static void StructConcatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &result_cols = StructVector::GetEntries(result);
	idx_t offset = 0;
	for (const auto &arg : args.data) {
		const auto &child_cols = StructVector::GetEntries(arg);
		for (auto &child_col : child_cols) {
			result_cols[offset++].Reference(child_col);
		}
	}
	D_ASSERT(offset == result_cols.size());
}

static unique_ptr<FunctionData> StructConcatBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// collect names and deconflict, construct return type
	if (arguments.empty()) {
		throw InvalidInputException("struct_concat: At least one argument is required");
	}

	child_list_t<LogicalType> combined_children;
	identifier_set_t name_set;

	bool has_unnamed = false;

	for (idx_t arg_idx = 0; arg_idx < arguments.size(); arg_idx++) {
		const auto &arg = arguments[arg_idx];

		if (arg->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}

		if (!StructType::IsStruct(arg->GetReturnType())) {
			throw InvalidInputException("struct_concat: Argument at position \"%d\" is not a STRUCT", arg_idx + 1);
		}

		const auto &child_types = StructType::GetChildTypes(arg->GetReturnType());
		for (const auto &child : child_types) {
			if (!child.first.empty()) {
				auto it = name_set.find(child.first);
				if (it != name_set.end()) {
					if (it->GetIdentifierName() == child.first.GetIdentifierName()) {
						throw InvalidInputException("struct_concat: Arguments contain duplicate STRUCT entry %s",
						                            child.first);
					}
					throw InvalidInputException(
					    "struct_concat: Arguments contain case-insensitive duplicate STRUCT entry %s and %s",
					    child.first, *it);
				}
				name_set.insert(child.first);
			} else {
				has_unnamed = true;
			}
			combined_children.push_back(child);
		}
	}

	if (has_unnamed && !name_set.empty()) {
		throw InvalidInputException("struct_concat: Cannot mix named and unnamed STRUCTs");
	}

	// all-unnamed inputs produce an unnamed TUPLE, otherwise a named STRUCT
	if (has_unnamed) {
		bound_function.SetReturnType(LogicalType::TUPLE(combined_children));
	} else {
		bound_function.SetReturnType(LogicalType::STRUCT(combined_children));
	}
	return nullptr;
}

static unique_ptr<BaseStatistics> StructConcatStats(ClientContext &context, FunctionStatisticsInput &input) {
	const auto &expr = input.expr;

	auto &arg_stats = input.child_stats;
	auto &arg_exprs = input.expr.GetChildren();

	auto struct_stats = StructStats::CreateUnknown(expr.GetReturnType());
	struct_stats.Set(StatsInfo::CANNOT_HAVE_NULL_VALUES);
	idx_t struct_index = 0;

	for (idx_t arg_idx = 0; arg_idx < arg_exprs.size(); arg_idx++) {
		auto &arg_stat = arg_stats[arg_idx];
		auto &arg_type = arg_exprs[arg_idx]->GetReturnType();
		for (idx_t child_idx = 0; child_idx < StructType::GetChildCount(arg_type); child_idx++) {
			auto &child_stat = StructStats::GetChildStats(arg_stat, child_idx);
			StructStats::SetChildStats(struct_stats, struct_index++, child_stat);
		}
	}
	return struct_stats.ToUnique();
}

ScalarFunction StructConcatFun::GetFunction() {
	ScalarFunction fun("struct_concat", {}, LogicalTypeId::STRUCT, StructConcatFunction, StructConcatBind,
	                   StructConcatStats);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
