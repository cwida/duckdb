//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_selectivity_optional_function.cpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/filter/table_filter_function_helpers.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

namespace duckdb {

SelectivityOptionalFilterState::SelectivityStats::SelectivityStats(const idx_t n_vectors_to_check,
                                                                   const float selectivity_threshold)
    : n_vectors_to_check(n_vectors_to_check), selectivity_threshold(selectivity_threshold), tuples_accepted(0),
      tuples_processed(0), vectors_processed(0), status(FilterStatus::ACTIVE), pause_multiplier(0) {
}

void SelectivityOptionalFilterState::SelectivityStats::Update(idx_t accepted, idx_t processed) {
	vectors_processed++;
	tuples_accepted += accepted;
	tuples_processed += processed;

	static constexpr idx_t VECTOR_PAUSE = 10;
	D_ASSERT(n_vectors_to_check < VECTOR_PAUSE);
	if (vectors_processed == MaxValue<idx_t>(pause_multiplier, 1) * VECTOR_PAUSE) {
		vectors_processed = 0;
		tuples_accepted = 0;
		tuples_processed = 0;
		status = FilterStatus::ACTIVE;
	} else if (status == FilterStatus::ACTIVE && vectors_processed >= n_vectors_to_check) {
		// pause the filter if we processed enough vectors and the selectivity is too high
		if (GetSelectivity() >= selectivity_threshold) {
			status = FilterStatus::PAUSED_DUE_TO_HIGH_SELECTIVITY;
			pause_multiplier++; // increase the pause duration
		} else {
			pause_multiplier = 0; // selective enough, reset the pause duration
		}
	}
}

bool SelectivityOptionalFilterState::SelectivityStats::IsActive() const {
	return status == FilterStatus::ACTIVE;
}
double SelectivityOptionalFilterState::SelectivityStats::GetSelectivity() const {
	if (tuples_processed == 0) {
		return 0.0;
	}
	return static_cast<double>(tuples_accepted) / static_cast<double>(tuples_processed);
}

SelectivityOptionalFilterFunctionData::SelectivityOptionalFilterFunctionData(unique_ptr<Expression> child_filter_expr_p,
                                                                             float selectivity_threshold_p,
                                                                             idx_t n_vectors_to_check_p)
    : child_filter_expr(std::move(child_filter_expr_p)), selectivity_threshold(selectivity_threshold_p),
      n_vectors_to_check(n_vectors_to_check_p) {
}

unique_ptr<FunctionData> SelectivityOptionalFilterFunctionData::Copy() const {
	return make_uniq<SelectivityOptionalFilterFunctionData>(child_filter_expr ? child_filter_expr->Copy() : nullptr,
	                                                        selectivity_threshold, n_vectors_to_check);
}

bool SelectivityOptionalFilterFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<SelectivityOptionalFilterFunctionData>();
	if (selectivity_threshold != other.selectivity_threshold || n_vectors_to_check != other.n_vectors_to_check) {
		return false;
	}
	if (!child_filter_expr && !other.child_filter_expr) {
		return true;
	}
	if (!child_filter_expr || !other.child_filter_expr) {
		return false;
	}
	return child_filter_expr->Equals(*other.child_filter_expr);
}

static void SelectivityOptionalFilterSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                               const BoundScalarFunction &function) {
	if (!bind_data) {
		return;
	}
	auto &data = bind_data->Cast<SelectivityOptionalFilterFunctionData>();
	serializer.WritePropertyWithDefault<unique_ptr<Expression>>(200, "child_filter_expr", data.child_filter_expr);
	serializer.WritePropertyWithDefault<float>(201, "selectivity_threshold", data.selectivity_threshold);
	serializer.WritePropertyWithDefault<idx_t>(202, "n_vectors_to_check", data.n_vectors_to_check);
}

static unique_ptr<FunctionData> SelectivityOptionalFilterDeserialize(Deserializer &deserializer,
                                                                     BoundScalarFunction &function) {
	auto child_filter_expr = deserializer.ReadPropertyWithDefault<unique_ptr<Expression>>(200, "child_filter_expr");
	auto selectivity_threshold =
	    deserializer.ReadPropertyWithExplicitDefault<float>(201, "selectivity_threshold", 0.5f);
	auto n_vectors_to_check = deserializer.ReadPropertyWithExplicitDefault<idx_t>(202, "n_vectors_to_check", idx_t(6));
	return make_uniq<SelectivityOptionalFilterFunctionData>(std::move(child_filter_expr), selectivity_threshold,
	                                                        n_vectors_to_check);
}

struct SelectivityOptionalFilterLocalState : public FunctionLocalState {
	SelectivityOptionalFilterLocalState(ClientContext &context, const Expression &child_filter_expr,
	                                    idx_t n_vectors_to_check, float selectivity_threshold)
	    : stats(n_vectors_to_check, selectivity_threshold), executor(context, child_filter_expr) {
	}

	bool IsActive() const {
		return stats.IsActive();
	}
	void Update(idx_t accepted, idx_t processed) {
		stats.Update(accepted, processed);
	}

	SelectivityOptionalFilterState::SelectivityStats stats;
	ExpressionExecutor executor;
};

static unique_ptr<FunctionLocalState> SelectivityOptionalFilterInitLocalState(ExpressionState &state,
                                                                              const BoundFunctionExpression &expr,
                                                                              FunctionData *bind_data) {
	auto &data = bind_data->Cast<SelectivityOptionalFilterFunctionData>();
	if (!data.child_filter_expr) {
		return nullptr;
	}
	return make_uniq<SelectivityOptionalFilterLocalState>(state.GetContext(), *data.child_filter_expr,
	                                                      data.n_vectors_to_check, data.selectivity_threshold);
}

static idx_t SelectivityOptionalFilterSelect(DataChunk &args, ExpressionState &state,
                                             optional_ptr<const SelectionVector> sel,
                                             optional_ptr<SelectionVector> true_sel,
                                             optional_ptr<SelectionVector> false_sel) {
	auto local_state_ptr = ExecuteFunctionState::GetFunctionState(state);
	if (!local_state_ptr) {
		return SetAllTrueSelection(args.size(), sel, true_sel, false_sel);
	}
	auto &local_state = local_state_ptr->Cast<SelectivityOptionalFilterLocalState>();
	auto count = args.size();
	if (!local_state.IsActive()) {
		local_state.Update(0, 0);
		return SetAllTrueSelection(count, sel, true_sel, false_sel);
	}

	SelectionVector temp_true;
	auto &result_true_sel = GetFilterResultSelection(count, sel, true_sel, temp_true);
	auto approved_count = local_state.executor.SelectExpression(args, result_true_sel);
	approved_count = TranslateSelection(count, sel, result_true_sel, approved_count, true_sel, false_sel);
	local_state.Update(approved_count, count);
	return approved_count;
}

ScalarFunction SelectivityOptionalFilterScalarFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction func(NAME, {input_type}, LogicalType::BOOLEAN, nullptr, TableFilterFunctions::Bind);
	func.GetSignature().GetParameter(0).SetName("col");
	func.SetInitStateCallback(SelectivityOptionalFilterInitLocalState);
	func.SetSelectCallback(SelectivityOptionalFilterSelect);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetFilterPruneCallback(SelectivityOptionalFilterScalarFun::FilterPrune);
	func.SetSerializeCallback(SelectivityOptionalFilterSerialize);
	func.SetDeserializeCallback(SelectivityOptionalFilterDeserialize);
	return func;
}

FilterPropagateResult SelectivityOptionalFilterScalarFun::FilterPrune(const FunctionStatisticsPruneInput &input) {
	if (!input.bind_data) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto &data = input.bind_data->Cast<SelectivityOptionalFilterFunctionData>();
	if (!data.child_filter_expr) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto column_stats = input.ChildStats(0);
	if (!column_stats) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	return ExpressionFilter::CheckExpressionStatistics(*data.child_filter_expr, *column_stats);
}

string SelectivityOptionalFilterScalarFun::ToString(const string &child_filter_string) {
	return FormatOptionalFilterString(child_filter_string);
}

ScalarFunction TableFilterSelectivityOptionalFun::GetFunction() {
	return SelectivityOptionalFilterScalarFun::GetFunction(LogicalType::ANY);
}

} // namespace duckdb
