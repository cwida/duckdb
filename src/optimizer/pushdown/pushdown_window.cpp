#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

using Filter = FilterPushdown::Filter;

namespace {

bool CanPushdownFilter(vector<column_binding_set_t> window_exprs_partition_bindings,
                       const vector<ColumnBinding> &bindings) {
	auto filter_on_all_partitions = true;
	for (auto &partition_binding_set : window_exprs_partition_bindings) {
		auto filter_on_binding_set = true;
		for (auto &binding : bindings) {
			if (partition_binding_set.find(binding) == partition_binding_set.end()) {
				filter_on_binding_set = false;
				break;
			}
		}
		filter_on_all_partitions = filter_on_all_partitions && filter_on_binding_set;
		if (!filter_on_all_partitions) {
			break;
		}
	}
	return filter_on_all_partitions;
}

unique_ptr<Filter> CreateAuxiliaryFilter(const Expression &partition, const Filter &filter) {
	//	Must be comparison
	if (!BoundComparisonExpression::IsComparison(*filter.filter)) {
		return nullptr;
	}

	//	Must compare an expression to a constant
	auto &func = filter.filter->Cast<BoundFunctionExpression>();
	auto &children = func.GetChildren();
	idx_t const_idx = 0;
	for (; const_idx < children.size(); ++const_idx) {
		if (children[const_idx]->IsFoldable()) {
			break;
		}
	}
	if (const_idx >= children.size()) {
		return nullptr;
	}

	//	Every partitioning must have a child equal to the free side of the comparison
	const auto &expr = *children[1 - const_idx];
	const auto &partition_func = partition.Cast<BoundFunctionExpression>();
	if (!expr.Equals(*partition_func.GetChildren()[0])) {
		return nullptr;
	}

	//	It must preserve ordering
	const auto arg_props = partition_func.Function().GetArgProperties(0);
	if (!IsMonotonicIncreasing(arg_props.monotonicity)) {
		return nullptr;
	}

	//	Orient the comparison so the constant is on the rhs
	auto aux_type = filter.filter->GetExpressionType();
	if (!const_idx) {
		aux_type = FlipComparisonExpression(aux_type);
	}

	switch (aux_type) {
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		//	(In)equality is only preserved with strictness
		if (!IsStrict(arg_props.monotonicity)) {
			return nullptr;
		}
		break;
	case ExpressionType::COMPARE_LESSTHAN:
		//	Lack of strictness means we have to use a non-strict comparison
		if (!IsStrict(arg_props.monotonicity)) {
			aux_type = ExpressionType::COMPARE_LESSTHANOREQUALTO;
		}
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		break;
	case ExpressionType::COMPARE_GREATERTHAN:
		//	Lack of strictness means we have to use a non-strict comparison
		if (!IsStrict(arg_props.monotonicity)) {
			aux_type = ExpressionType::COMPARE_GREATERTHANOREQUALTO;
		}
		DUCKDB_EXPLICIT_FALLTHROUGH;
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		break;
	default:
		return nullptr;
	}

	//	Generate an auxiliary filter b 𝜃 c => e(b) 𝜃' e(c)
	auto e_b = partition_func.Copy();
	auto c_b = partition_func.Copy();
	c_b->Cast<BoundFunctionExpression>().GetChildrenMutable()[0] = children[const_idx]->Copy();
	auto aux_comp = BoundComparisonExpression::Create(aux_type, std::move(e_b), std::move(c_b));
	auto aux_filter = make_uniq<Filter>(std::move(aux_comp));
	aux_filter->ExtractBindings();
	aux_filter->ExtractBarrier();

	return aux_filter;
}

} // namespace

unique_ptr<LogicalOperator> FilterPushdown::PushdownWindow(unique_ptr<LogicalOperator> op) {
	D_ASSERT(op->type == LogicalOperatorType::LOGICAL_WINDOW);
	auto &window = op->Cast<LogicalWindow>();
	FilterPushdown pushdown(optimizer, convert_mark_joins, projection_mode);

	// 1. Loop through the expressions, find the window expressions and investigate the partitions
	// if a filter applies to a partition in each window expression then you can push the filter
	// into the children.
	vector<column_binding_set_t> window_exprs_partition_bindings;
	expression_map_t<idx_t> window_partition_funcs;
	for (auto &expr : window.expressions) {
		if (expr->GetExpressionClass() != ExpressionClass::BOUND_WINDOW) {
			continue;
		}
		auto &window_expr = expr->Cast<BoundWindowExpression>();
		auto &partitions = window_expr.Partitions();
		if (partitions.empty()) {
			// If any window expression does not have partitions, we cannot push any filters.
			// all window expressions need to be partitioned by the same column
			// in order to push down the window.
			return FinishPushdown(std::move(op));
		}
		column_binding_set_t partition_bindings;
		// 2. Get the binding information of the partitions of the window expression
		for (auto &partition_expr : partitions) {
			switch (partition_expr->GetExpressionType()) {
			// TODO: Add expressions for function expressions like FLOOR, CEIL etc.
			case ExpressionType::BOUND_COLUMN_REF: {
				auto &partition_col = partition_expr->Cast<BoundColumnRefExpression>();
				partition_bindings.insert(partition_col.Binding());
				break;
			}
			case ExpressionType::BOUND_FUNCTION:
				//	Partition expressions of the form e(b) filtered by b 𝜃 c
				//	can push down a weaker copy of the filter e(b) 𝜃' e(c)
				//	as long as e is order preserving.
				if (partition_expr->IsConsistent()) {
					auto &partition_func = partition_expr->Cast<BoundFunctionExpression>();
					auto &children = partition_func.GetChildren();
					if (children.size() == 1) {
						window_partition_funcs[partition_func]++;
					}
				}
				break;
			default:
				break;
			}
		}
		window_exprs_partition_bindings.push_back(partition_bindings);
	}

	if (window_exprs_partition_bindings.empty()) {
		return FinishPushdown(std::move(op));
	}

	vector<unique_ptr<Filter>> leftover_filters;
	for (idx_t i = 0; i < filters.size(); i++) {
		// If a filter is on a partition in every window expression it can be pushed down.
		vector<ColumnBinding> bindings;
		ExtractFilterBindings(*filters.at(i)->filter, bindings);
		if (CanPushdownFilter(window_exprs_partition_bindings, bindings) && !filters.at(i)->filter->IsVolatile()) {
			pushdown.filters.push_back(std::move(filters.at(i)));
			continue;
		}

		// Now look for filters of the form b 𝜃 c, 𝜃 𝜖 { < <= = >= > IDF INDF}, b 𝜖 P
		// If the partition expression e(b) is order-preserving, we can inject (not push down!)
		// a predicate of the form e(b) 𝜃' e(c)
		for (const auto &p : window_partition_funcs) {
			//	Skip partition functions that are not in all window functions
			if (p.second != window_exprs_partition_bindings.size()) {
				continue;
			}
			auto aux_filter = CreateAuxiliaryFilter(p.first, *filters.at(i));
			if (aux_filter) {
				pushdown.filters.push_back(std::move(aux_filter));
			}
		}
		leftover_filters.push_back(std::move(filters.at(i)));
	}

	op->children[0] = pushdown.Rewrite(std::move(op->children[0]));
	filters = std::move(leftover_filters);
	return PushFinalFilters(std::move(op));
}
} // namespace duckdb
