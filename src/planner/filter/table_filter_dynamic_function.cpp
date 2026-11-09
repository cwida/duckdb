//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_dynamic_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

namespace duckdb {

DynamicFilterData::DynamicFilterData(ExpressionType comparison_type_p, Value constant_p)
    : comparison_type(comparison_type_p), constant(std::move(constant_p)) {
}

unique_ptr<Expression> DynamicFilterData::ToExpression(const Expression &column) const {
	annotated_shared_lock<annotated_shared_mutex> guard(lock);
	return BoundComparisonExpression::Create(comparison_type, column.Copy(),
	                                         make_uniq<BoundConstantExpression>(constant));
}

bool DynamicFilterData::CompareValue(ExpressionType comparison_type, const Value &constant, const Value &value) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
		return ValueOperations::Equals(value, constant);
	case ExpressionType::COMPARE_NOTEQUAL:
		return ValueOperations::NotEquals(value, constant);
	case ExpressionType::COMPARE_GREATERTHAN:
		return ValueOperations::GreaterThan(value, constant);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return ValueOperations::GreaterThanEquals(value, constant);
	case ExpressionType::COMPARE_LESSTHAN:
		return ValueOperations::LessThan(value, constant);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return ValueOperations::LessThanEquals(value, constant);
	default:
		throw InternalException("Unknown comparison type for DynamicFilter: " + EnumUtil::ToString(comparison_type));
	}
}

FilterPropagateResult DynamicFilterData::CheckStatistics(const BaseStatistics &stats, ExpressionType comparison_type,
                                                         const Value &constant) {
	if (!stats.CanHaveNoNull()) {
		return comparison_type == ExpressionType::COMPARE_DISTINCT_FROM ? FilterPropagateResult::FILTER_ALWAYS_TRUE
		                                                                : FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	switch (constant.type().InternalType()) {
	case PhysicalType::UINT8:
	case PhysicalType::UINT16:
	case PhysicalType::UINT32:
	case PhysicalType::UINT64:
	case PhysicalType::UINT128:
	case PhysicalType::INT8:
	case PhysicalType::INT16:
	case PhysicalType::INT32:
	case PhysicalType::INT64:
	case PhysicalType::INT128:
	case PhysicalType::FLOAT:
	case PhysicalType::DOUBLE:
		return NumericStats::CheckZonemap(stats, comparison_type, array_ptr<const Value>(constant));
	case PhysicalType::VARCHAR:
		if (stats.GetStatsType() == StatisticsType::STRING_STATS) {
			return StringStats::CheckZonemap(stats, comparison_type, array_ptr<const Value>(constant));
		}
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	default:
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
}

DynamicFilterFunctionData::DynamicFilterFunctionData(shared_ptr<DynamicFilterData> filter_data_p)
    : filter_data(std::move(filter_data_p)) {
}

unique_ptr<FunctionData> DynamicFilterFunctionData::Copy() const {
	return make_uniq<DynamicFilterFunctionData>(filter_data);
}

bool DynamicFilterFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<DynamicFilterFunctionData>();
	return filter_data.get() == other.filter_data.get();
}

static idx_t SelectDynamicFilter(Vector &input, ExpressionType comparison_type, const Value &constant,
                                 SelectionVector &result_sel, idx_t count) {
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);

	idx_t approved_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			continue;
		}
		auto value = input.GetValue(idx);
		if (!DynamicFilterData::CompareValue(comparison_type, constant, value)) {
			continue;
		}
		result_sel.set_index(approved_count++, i);
	}
	return approved_count;
}

static idx_t DynamicFilterSelect(DataChunk &args, ExpressionState &state, optional_ptr<const SelectionVector> sel,
                                 optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &func_data = func_expr.BindInfo()->Cast<DynamicFilterFunctionData>();
	auto count = args.size();
	if (!func_data.filter_data || !func_data.filter_data->initialized.load()) {
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}

	ExpressionType comparison_type;
	Value constant;
	{
		annotated_shared_lock<annotated_shared_mutex> lock(func_data.filter_data->lock);
		comparison_type = func_data.filter_data->comparison_type;
		constant = func_data.filter_data->constant;
	}

	SelectionVector temp_true;
	auto &result_true_sel = GetFilterResultSelection(count, sel, true_sel, temp_true);
	auto approved_count = SelectDynamicFilter(args.data[0], comparison_type, constant, result_true_sel, count);
	return TranslateSelection(count, sel, result_true_sel, approved_count, true_sel, false_sel);
}

ScalarFunction DynamicFilterScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, nullptr, TableFilterFunctions::Bind);
	func.GetSignature().GetParameter(0).SetName("col");
	func.SetSelectCallback(DynamicFilterSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(DynamicFilterScalarFun::FilterPrune);
	func.SetSerializeCallback(TableFilterFunctionSerialize);
	func.SetDeserializeCallback(TableFilterFunctionDeserialize);
	return func;
}

FilterPropagateResult DynamicFilterScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<DynamicFilterFunctionData>();
	if (!data.filter_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	annotated_shared_lock<annotated_shared_mutex> lock(data.filter_data->lock);
	if (!data.filter_data->initialized) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return DynamicFilterData::CheckStatistics(*column_stats, data.filter_data->comparison_type,
	                                          data.filter_data->constant);
}

string DynamicFilterScalarFun::ToString(const string &column_name, bool has_filter_data) {
	if (has_filter_data) {
		return "Dynamic Filter (" + column_name + ")";
	}
	return "Dynamic Filter";
}

ScalarFunction TableFilterDynamicFun::GetFunction() {
	return DynamicFilterScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
