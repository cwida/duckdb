//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_optional_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {

OptionalFilterFunctionData::OptionalFilterFunctionData(unique_ptr<Expression> child_filter_expr_p)
    : child_filter_expr(std::move(child_filter_expr_p)) {
}

unique_ptr<FunctionData> OptionalFilterFunctionData::Copy() const {
	return make_uniq<OptionalFilterFunctionData>(child_filter_expr ? child_filter_expr->Copy() : nullptr);
}

bool OptionalFilterFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<OptionalFilterFunctionData>();
	if (!child_filter_expr && !other.child_filter_expr) {
		return true;
	}
	if (!child_filter_expr || !other.child_filter_expr) {
		return false;
	}
	return child_filter_expr->Equals(*other.child_filter_expr);
}

static void OptionalFilterSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                    const BoundScalarFunction &function) {
	if (!bind_data) {
		return;
	}
	auto &data = bind_data->Cast<OptionalFilterFunctionData>();
	serializer.WritePropertyWithDefault<unique_ptr<Expression>>(200, "child_filter_expr", data.child_filter_expr);
}

static unique_ptr<FunctionData> OptionalFilterDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	auto child_filter_expr = deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(200, "child_filter_expr");
	return make_uniq<OptionalFilterFunctionData>(std::move(child_filter_expr));
}

static void OptionalFilterFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	SetAllTrue(args, result);
}

static idx_t OptionalFilterSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                                  optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	return SetAllTrueSelection(args.size(), sel, true_sel, false_sel);
}

ScalarFunction OptionalFilterScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, OptionalFilterFunction, TableFilterFunctions::Bind);
	func.GetSignature().GetParameter(0).SetName("col");
	func.SetSelectCallback(OptionalFilterSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(OptionalFilterScalarFun::FilterPrune);
	func.SetSerializeCallback(OptionalFilterSerialize);
	func.SetDeserializeCallback(OptionalFilterDeserialize);
	return func;
}

FilterPropagateResult OptionalFilterScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<OptionalFilterFunctionData>();
	if (!data.child_filter_expr) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return ExpressionFilter::CheckExpressionStatistics(*data.child_filter_expr, *column_stats);
}

string OptionalFilterScalarFun::ToString(const string &child_filter_string) {
	return FormatOptionalFilterString(child_filter_string);
}

ScalarFunction TableFilterOptionalFun::GetFunction() {
	return OptionalFilterScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
