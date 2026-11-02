#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"

#include "duckdb/common/types/value_map.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/operator/list.hpp"

#include <math.h>

namespace duckdb {

optional<DistinctCount> RelationStatisticsHelper::SelectTotalDomain(const vector<DistinctCount> &domains) {
	optional<DistinctCount> reliable;
	optional<DistinctCount> min_max;
	optional<DistinctCount> fallback;
	for (auto &domain : domains) {
		switch (domain.source) {
		case DistinctCountSource::HLL:
		case DistinctCountSource::EXACT:
			if (!reliable || domain.distinct_count > reliable->distinct_count) {
				reliable = domain;
			}
			break;
		case DistinctCountSource::MIN_MAX:
			if (!min_max || domain.distinct_count > min_max->distinct_count) {
				min_max = domain;
			}
			break;
		case DistinctCountSource::CARDINALITY:
			if (!fallback || domain.distinct_count < fallback->distinct_count) {
				fallback = domain;
			}
			break;
		}
	}
	if (reliable) {
		return reliable;
	}
	if (min_max) {
		return min_max;
	}
	return fallback;
}

static bool MatchesTargetColumn(const Expression &expression, optional<ColumnBinding> target_binding) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		return !target_binding || expression.Cast<BoundColumnRefExpression>().Binding() == *target_binding;
	}
	return !target_binding && expression.GetExpressionClass() == ExpressionClass::BOUND_REF;
}

optional_idx RelationStatisticsHelper::EstimateDirectFilterDomain(const Expression &expression,
                                                                  optional<ColumnBinding> target_binding,
                                                                  idx_t maximum_domain) {
	if (BoundComparisonExpression::IsComparison(expression)) {
		auto &comparison = expression.Cast<BoundFunctionExpression>();
		if (comparison.GetExpressionType() != ExpressionType::COMPARE_EQUAL) {
			return {};
		}
		auto &left = BoundComparisonExpression::Left(comparison);
		auto &right = BoundComparisonExpression::Right(comparison);
		optional_ptr<const Expression> constant;
		if (MatchesTargetColumn(left, target_binding)) {
			constant = right;
		} else if (MatchesTargetColumn(right, target_binding)) {
			constant = left;
		}
		if (constant && constant->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT &&
		    !constant->Cast<BoundConstantExpression>().GetValue().IsNull()) {
			return optional_idx(MinValue<idx_t>(maximum_domain, 1));
		}
		return {};
	}

	if (expression.GetExpressionType() == ExpressionType::COMPARE_IN &&
	    expression.GetExpressionClass() == ExpressionClass::BOUND_OPERATOR) {
		auto &children = expression.Cast<BoundOperatorExpression>().GetChildren();
		if (children.empty() || !MatchesTargetColumn(*children[0], target_binding)) {
			return {};
		}
		value_set_t constants;
		for (idx_t child_idx = 1; child_idx < children.size(); child_idx++) {
			if (children[child_idx]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				return {};
			}
			auto &value = children[child_idx]->Cast<BoundConstantExpression>().GetValue();
			if (!value.IsNull()) {
				constants.insert(value);
			}
		}
		return optional_idx(MinValue<idx_t>(maximum_domain, constants.size()));
	}

	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONJUNCTION) {
		return {};
	}
	auto &conjunction = expression.Cast<BoundConjunctionExpression>();
	if (expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		optional_idx result;
		for (auto &child : conjunction.GetChildren()) {
			auto child_bound = EstimateDirectFilterDomain(*child, target_binding, maximum_domain);
			if (!child_bound.IsValid()) {
				continue;
			}
			result = result.IsValid() ? MinValue(result.GetIndex(), child_bound.GetIndex()) : child_bound;
		}
		return result;
	}
	if (expression.GetExpressionType() != ExpressionType::CONJUNCTION_OR) {
		return {};
	}
	idx_t result = 0;
	for (auto &child : conjunction.GetChildren()) {
		auto child_bound = EstimateDirectFilterDomain(*child, target_binding, maximum_domain);
		if (!child_bound.IsValid()) {
			return {};
		}
		if (child_bound.GetIndex() >= maximum_domain - result) {
			return optional_idx(maximum_domain);
		}
		result += child_bound.GetIndex();
	}
	return optional_idx(result);
}

struct ExpressionBinding {
	bool FoundExpression() const {
		return expression;
	}

	bool FoundColumnRef() const {
		return FoundExpression() && expression->GetExpressionType() == ExpressionType::BOUND_COLUMN_REF;
	}

	optional_ptr<Expression> expression;
	ColumnBinding child_binding;
	bool expression_is_constant = false;
};

static ExpressionBinding GetChildColumnBinding(Expression &expr) {
	ExpressionBinding result;
	if (expr.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		result.expression = expr;
		result.child_binding = expr.Cast<BoundColumnRefExpression>().Binding();
		return result;
	}
	if (expr.IsFoldable()) {
		result.expression = expr;
		result.expression_is_constant = true;
		return result;
	}
	if (expr.IsVolatile()) {
		return result;
	}
	ExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<Expression> &child) {
		if (result.FoundColumnRef()) {
			return;
		}
		auto child_result = GetChildColumnBinding(*child);
		if (child_result.FoundColumnRef()) {
			result = child_result;
		}
	});
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ExtractProjectionStats(LogicalProjection &projection,
                                                                         const RelationStats &child_stats) {
	RelationStats result;
	result.cardinality = child_stats.cardinality;
	result.row_retention = child_stats.row_retention;
	result.table_name = Identifier(projection.GetName());
	result.stats_initialized = true;
	auto bindings = projection.GetColumnBindings();
	D_ASSERT(bindings.size() == projection.expressions.size());
	RelationColumnStatsLookup child_columns(child_stats);
	for (idx_t expression_idx = 0; expression_idx < projection.expressions.size(); expression_idx++) {
		auto &expression = *projection.expressions[expression_idx];
		auto expression_binding = GetChildColumnBinding(expression);
		DistinctCount distinct_count(result.cardinality, DistinctCountSource::CARDINALITY);
		SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence;
		optional_ptr<const RelationColumnStats> child_column;
		if (expression_binding.expression_is_constant) {
			distinct_count = DistinctCount(MinValue<idx_t>(result.cardinality, 1), DistinctCountSource::EXACT);
		} else if (expression_binding.FoundColumnRef()) {
			child_column = child_columns.Find(expression_binding.child_binding);
			if (!child_column) {
				return {};
			}
			distinct_count = child_column->total_domain;
		}
		auto current_domain = distinct_count;
		if (child_column) {
			current_domain = child_column->current_domain;
			if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
				semi_anti_join_domain_evidence = child_column->semi_anti_join_domain_evidence;
			}
		}
		result.columns.emplace_back(bindings[expression_idx], distinct_count, current_domain,
		                            Identifier(expression.GetName()), semi_anti_join_domain_evidence);
	}
	result.Verify(bindings);
	return result;
}

idx_t RelationStatisticsHelper::EstimateDistinctCardinality(const vector<DistinctCount> &distinct_counts,
                                                            idx_t input_cardinality) {
	if (distinct_counts.empty()) {
		return input_cardinality / 2;
	}
	if (distinct_counts.size() == 1) {
		return MinValue(distinct_counts[0].distinct_count, input_cardinality);
	}
	double product = 1;
	for (auto &distinct_count : distinct_counts) {
		product *= static_cast<double>(MaxValue<idx_t>(distinct_count.distinct_count, 1));
	}
	product *= pow(0.95, static_cast<double>(distinct_counts.size() - 1));
	const auto multiplier = 1.0 - exp(-static_cast<double>(input_cardinality) / product);
	const auto estimate = multiplier == 0 ? static_cast<double>(input_cardinality) : product * multiplier;
	auto result = LossyNumericCast<idx_t>(MinValue(estimate, static_cast<double>(input_cardinality)));
	return input_cardinality > 0 ? MaxValue<idx_t>(result, 1) : 0;
}

DistinctCount RelationStatisticsHelper::EstimateCurrentDomain(const DistinctCount &current_domain,
                                                              idx_t input_cardinality, idx_t output_cardinality) {
	auto count = MinValue(current_domain.distinct_count, input_cardinality);
	if (count == 0 || output_cardinality == 0 || input_cardinality == 0) {
		return DistinctCount(0, DistinctCountSource::CARDINALITY);
	}
	if (output_cardinality >= input_cardinality) {
		return DistinctCount(count, current_domain.source);
	}

	const auto row_survival = static_cast<double>(output_cardinality) / static_cast<double>(input_cardinality);
	const auto multiplicity = static_cast<double>(input_cardinality) / static_cast<double>(count);
	const auto retained_fraction = -expm1(multiplicity * log1p(-row_survival));
	const auto estimate = round(static_cast<double>(count) * retained_fraction);
	auto result = LossyNumericCast<idx_t>(MinValue(estimate, static_cast<double>(output_cardinality)));
	result = MinValue(result, count);
	return DistinctCount(MaxValue<idx_t>(result, 1), DistinctCountSource::CARDINALITY);
}

void RelationStatisticsHelper::ApplyCardinality(RelationStats &stats, idx_t output_cardinality) {
	const auto input_cardinality = stats.cardinality;
	for (auto &column : stats.columns) {
		column.current_domain = EstimateCurrentDomain(column.current_domain, input_cardinality, output_cardinality);
		column.current_domain.distinct_count =
		    MinValue(column.current_domain.distinct_count, column.total_domain.distinct_count);
	}
	if (input_cardinality > 0 && output_cardinality < input_cardinality) {
		stats.row_retention *= double(output_cardinality) / double(input_cardinality);
	}
	stats.cardinality = output_cardinality;
}

static double RoundSemiAntiCardinality(double preserved_cardinality, double selectivity) {
	if (preserved_cardinality <= 0) {
		return 0;
	}
	auto estimate = round(preserved_cardinality * MaxValue(0.0, MinValue(selectivity, 1.0)));
	if (estimate <= 0) {
		estimate = 1;
	}
	return MinValue(estimate, preserved_cardinality);
}

SemiAntiJoinCardinalityEstimate RelationStatisticsHelper::EstimateSemiAntiJoinFallback(double preserved_cardinality,
                                                                                       idx_t rhs_cardinality,
                                                                                       JoinType join_type) {
	auto is_anti = join_type == JoinType::ANTI || join_type == JoinType::RIGHT_ANTI;
	if (rhs_cardinality == 0) {
		return {is_anti ? MaxValue(0.0, preserved_cardinality) : 0, false};
	}
	// Keep fractional estimates while composing costs. Rounding a small preserved input
	// can otherwise make equivalent selectivities depend on where the SEMI/ANTI is placed.
	auto estimate = MinValue(preserved_cardinality, MaxValue(1.0, preserved_cardinality * DEFAULT_SELECTIVITY));
	return {MaxValue(0.0, estimate), false};
}

static constexpr double MAX_BASE_CARDINALITY_TO_DOMAIN_RATIO = 8;

SemiAntiJoinCardinalityEstimate RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(
    double preserved_cardinality, idx_t rhs_cardinality, double rhs_row_retention, JoinType join_type,
    const vector<SemiAntiJoinDomain> &domains, bool has_residual) {
	auto fallback = EstimateSemiAntiJoinFallback(preserved_cardinality, rhs_cardinality, join_type);
	if (preserved_cardinality <= 0 || rhs_cardinality == 0 || has_residual || domains.empty()) {
		return fallback;
	}

	double match_fraction = 1;
	double strongest_fraction = 1;
	idx_t strongest_total_domain = 0;
	idx_t maximum_total_domain = 0;
	column_binding_set_t preserved_bindings;
	column_binding_set_t rhs_bindings;
	for (auto &domain : domains) {
		if (domain.total_domain == 0) {
			return fallback;
		}
		double fraction = MinValue(1.0, double(domain.rhs_current_domain) / double(domain.total_domain));
		match_fraction *= fraction;
		if (fraction < strongest_fraction) {
			strongest_fraction = fraction;
			strongest_total_domain = domain.total_domain;
		}
		maximum_total_domain = MaxValue(maximum_total_domain, domain.total_domain);
		preserved_bindings.insert(domain.preserved_binding);
		rhs_bindings.insert(domain.rhs_binding);
	}
	if (domains.size() >= 2 && preserved_bindings.size() >= 2 && rhs_bindings.size() >= 2 && maximum_total_domain > 0) {
		auto retention = MaxValue(0.0, MinValue(rhs_row_retention, 1.0));
		auto rhs_base_cardinality = retention > 0 ? double(rhs_cardinality) / retention : double(rhs_cardinality);
		if (rhs_base_cardinality <= double(maximum_total_domain) * MAX_BASE_CARDINALITY_TO_DOMAIN_RATIO &&
		    (strongest_total_domain == 0 || double(strongest_total_domain) <= rhs_base_cardinality)) {
			match_fraction = MaxValue(match_fraction, MinValue(strongest_fraction, retention));
		}
	}
	match_fraction = MaxValue(0.0, MinValue(match_fraction, 1.0));
	auto is_anti = join_type == JoinType::ANTI || join_type == JoinType::RIGHT_ANTI;
	auto selectivity = is_anti ? 1 - match_fraction : match_fraction;
	return {RoundSemiAntiCardinality(preserved_cardinality, selectivity), true};
}

static optional_idx EstimateUnionGroups(LogicalOperator &input, vector<idx_t> groups, idx_t maximum,
                                        ClientContext &context) {
	RelationStatsExtractor extractor(context);
	vector<pair<reference<LogicalOperator>, vector<idx_t>>> pending;
	pending.emplace_back(input, std::move(groups));
	idx_t result = 0;
	bool found_union = false;
	while (!pending.empty()) {
		auto entry = std::move(pending.back());
		pending.pop_back();
		while (entry.first.get().type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = entry.first.get();
			auto bindings = projection.children[0]->GetColumnBindings();
			vector<idx_t> mapped;
			for (auto index : entry.second) {
				auto &expr = *projection.expressions[index];
				if (expr.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
					break;
				}
				auto binding = expr.Cast<BoundColumnRefExpression>().Binding();
				auto match = std::find(bindings.begin(), bindings.end(), binding);
				if (match == bindings.end()) {
					break;
				}
				mapped.push_back(idx_t(match - bindings.begin()));
			}
			if (mapped.size() != entry.second.size()) {
				break;
			}
			entry.first = *projection.children[0];
			entry.second = std::move(mapped);
		}
		auto &op = entry.first.get();
		if (op.type == LogicalOperatorType::LOGICAL_UNION) {
			found_union = true;
			for (auto &child : op.children) {
				pending.emplace_back(*child, entry.second);
			}
			continue;
		}
		if (!found_union) {
			return {};
		}
		auto stats = extractor.Extract(op);
		if (!stats) {
			return {};
		}
		auto cardinality = op.has_estimated_cardinality ? op.estimated_cardinality : stats->cardinality;
		auto estimate = MinValue<idx_t>(cardinality, 1);
		for (auto index : entry.second) {
			auto domain = MaxValue<idx_t>(stats->columns[index].current_domain.distinct_count, 1);
			if (estimate > cardinality / domain) {
				estimate = cardinality;
				break;
			}
			estimate *= domain;
		}
		result += MinValue(estimate, maximum - result);
	}
	return optional_idx(result);
}

optional<RelationStats> RelationStatisticsHelper::ExtractAggregationStats(LogicalAggregate &aggregate,
                                                                          const RelationStats &child_stats,
                                                                          ClientContext &context) {
	RelationColumnStatsLookup child_columns(child_stats);
	vector<DistinctCount> cardinality_counts;
	for (auto &grouping_set : aggregate.grouping_sets) {
		vector<DistinctCount> set_counts;
		for (auto group_idx : grouping_set) {
			auto &group = aggregate.GetGroupExpression(group_idx);
			if (group.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				continue;
			}
			auto column = child_columns.Find(group.Cast<BoundColumnRefExpression>().Binding());
			if (!column) {
				return {};
			}
			auto count = column->current_domain;
			count.distinct_count = MaxValue<idx_t>(count.distinct_count, 1);
			set_counts.push_back(count);
		}
		if (set_counts.size() > cardinality_counts.size()) {
			cardinality_counts = std::move(set_counts);
		}
	}

	RelationStats result;
	result.cardinality =
	    aggregate.groups.empty() ? 1 : EstimateDistinctCardinality(cardinality_counts, child_stats.cardinality);
	result.table_name = Identifier(aggregate.GetName());
	result.stats_initialized = true;
	auto bindings = aggregate.GetColumnBindings();
	auto preserves_group_evidence =
	    aggregate.grouping_sets.size() == 1 && aggregate.grouping_sets[0].size() == aggregate.groups.size();
	if (preserves_group_evidence && !aggregate.groups.empty() && aggregate.children.size() == 1 &&
	    cardinality_counts.size() == aggregate.groups.size()) {
		vector<idx_t> groups;
		for (auto &group : aggregate.groups) {
			groups.push_back(child_stats.FindColumn(group->Cast<BoundColumnRefExpression>().Binding()).GetIndex());
		}
		auto union_groups =
		    EstimateUnionGroups(*aggregate.children[0], std::move(groups), child_stats.cardinality, context);
		if (union_groups.IsValid()) {
			// Per-arm domain products avoid inventing cross-arm combinations of grouping keys.
			result.cardinality = MinValue(result.cardinality, union_groups.GetIndex());
		}
	}
	for (idx_t group_idx = 0; group_idx < aggregate.groups.size(); group_idx++) {
		auto &group = *aggregate.groups[group_idx];
		DistinctCount distinct_count(result.cardinality, DistinctCountSource::CARDINALITY);
		SemiAntiJoinDomainEvidence semi_anti_join_domain_evidence;
		if (group.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			auto child_column = child_columns.Find(group.Cast<BoundColumnRefExpression>().Binding());
			if (!child_column) {
				return {};
			}
			distinct_count = child_column->total_domain;
			if (preserves_group_evidence) {
				semi_anti_join_domain_evidence = child_column->semi_anti_join_domain_evidence;
			}
		}
		auto current_domain = distinct_count;
		if (group.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
			current_domain =
			    child_stats.GetColumnStats(group.Cast<BoundColumnRefExpression>().Binding())->current_domain;
		}
		if (aggregate.groups.size() == 1 && aggregate.grouping_sets.size() == 1 &&
		    aggregate.grouping_sets[0].size() == 1 && aggregate.grouping_sets[0].count(ProjectionIndex(0)) > 0) {
			semi_anti_join_domain_evidence.MarkUnique();
		}
		result.columns.emplace_back(bindings[result.columns.size()], distinct_count, current_domain,
		                            Identifier(group.GetName()), semi_anti_join_domain_evidence);
	}
	for (auto &expression : aggregate.expressions) {
		result.columns.emplace_back(bindings[result.columns.size()],
		                            DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
		                            Identifier(expression->GetName()));
	}
	for (idx_t grouping_idx = 0; grouping_idx < aggregate.grouping_functions.size(); grouping_idx++) {
		auto grouping_count = MinValue<idx_t>(result.cardinality, MaxValue<idx_t>(aggregate.grouping_sets.size(), 1));
		result.columns.emplace_back(bindings[result.columns.size()],
		                            DistinctCount(grouping_count, DistinctCountSource::CARDINALITY),
		                            Identifier("grouping"));
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(bindings);
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ExtractWindowStats(LogicalWindow &window,
                                                                     const RelationStats &child_stats) {
	RelationStats result;
	result.cardinality = child_stats.cardinality;
	result.row_retention = child_stats.row_retention;
	result.table_name = Identifier(window.GetName());
	result.stats_initialized = true;
	RelationColumnStatsLookup child_columns(child_stats);
	for (auto &binding : window.GetColumnBindings()) {
		auto child_column = child_columns.Find(binding);
		if (child_column) {
			result.columns.emplace_back(binding, child_column->total_domain, child_column->current_domain,
			                            child_column->name, child_column->semi_anti_join_domain_evidence);
		} else if (binding.table_index == window.window_index) {
			result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
			                            Identifier("window"));
		} else {
			return {};
		}
	}
	result.Verify(window.GetColumnBindings());
	return result;
}

static optional<DistinctCount> GetDistinctTargetCount(Expression &target, const RelationStats &child_stats,
                                                      const RelationColumnStatsLookup &child_columns) {
	switch (target.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto column = child_columns.Find(target.Cast<BoundColumnRefExpression>().Binding());
		return column ? optional<DistinctCount>(column->current_domain) : optional<DistinctCount>();
	}
	case ExpressionClass::BOUND_REF: {
		auto index = target.Cast<BoundReferenceExpression>().Index();
		return index < child_stats.columns.size() ? optional<DistinctCount>(child_stats.columns[index].current_domain)
		                                          : optional<DistinctCount>();
	}
	case ExpressionClass::BOUND_CONSTANT:
		return DistinctCount(MinValue<idx_t>(child_stats.cardinality, 1), DistinctCountSource::EXACT);
	default:
		return {};
	}
}

static optional<ColumnBinding> GetDistinctTargetBinding(Expression &target, const RelationStats &child_stats) {
	switch (target.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return target.Cast<BoundColumnRefExpression>().Binding();
	case ExpressionClass::BOUND_REF: {
		auto index = target.Cast<BoundReferenceExpression>().Index();
		return index < child_stats.columns.size() ? optional<ColumnBinding>(child_stats.columns[index].binding)
		                                          : optional<ColumnBinding>();
	}
	default:
		return {};
	}
}

optional<RelationStats> RelationStatisticsHelper::ExtractDistinctStats(LogicalDistinct &distinct,
                                                                       const RelationStats &child_stats) {
	auto result = ProjectOutputStats(child_stats, distinct);
	if (!result) {
		return {};
	}
	result->row_retention = 1;
	vector<DistinctCount> distinct_counts;
	if (distinct.distinct_targets.empty()) {
		for (auto &column : child_stats.columns) {
			distinct_counts.push_back(column.current_domain);
		}
	} else {
		RelationColumnStatsLookup child_columns(child_stats);
		for (auto &target : distinct.distinct_targets) {
			auto count = GetDistinctTargetCount(*target, child_stats, child_columns);
			if (!count) {
				return result;
			}
			distinct_counts.push_back(*count);
		}
	}
	result->cardinality = EstimateDistinctCardinality(distinct_counts, child_stats.cardinality);
	result->CapCurrentDomainsToCardinality();
	if (result->columns.size() == 1 && distinct.distinct_targets.empty()) {
		result->columns[0].semi_anti_join_domain_evidence.MarkUnique();
	} else if (distinct.distinct_targets.size() == 1) {
		auto target_binding = GetDistinctTargetBinding(*distinct.distinct_targets[0], child_stats);
		if (target_binding) {
			for (auto &column : result->columns) {
				if (column.binding == *target_binding) {
					column.semi_anti_join_domain_evidence.MarkUnique();
					break;
				}
			}
		}
	}
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ExtractFilterStats(LogicalFilter &filter,
                                                                     const RelationStats &child_stats) {
	auto result = ProjectOutputStats(child_stats, filter);
	if (!result) {
		return {};
	}
	auto cardinality = child_stats.cardinality;
	if (cardinality > 0) {
		cardinality = MaxValue<idx_t>(LossyNumericCast<idx_t>(double(cardinality) * DEFAULT_SELECTIVITY), 1);
	}
	ApplyCardinality(*result, cardinality);
	for (auto &column : result->columns) {
		for (auto &expression : filter.expressions) {
			auto direct_bound =
			    EstimateDirectFilterDomain(*expression, column.binding, column.total_domain.distinct_count);
			if (!direct_bound.IsValid()) {
				continue;
			}
			column.current_domain =
			    DistinctCount(MinValue(column.current_domain.distinct_count, direct_bound.GetIndex()),
			                  DistinctCountSource::CARDINALITY);
			column.semi_anti_join_domain_evidence.TightenFilterDomainBound(direct_bound.GetIndex());
		}
	}
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ExtractLimitStats(LogicalLimit &limit,
                                                                    const RelationStats &child_stats) {
	auto result = ProjectOutputStats(child_stats, limit);
	if (!result) {
		return {};
	}
	auto available_cardinality = child_stats.cardinality;
	if (limit.offset_val.Type() == LimitNodeType::CONSTANT_VALUE) {
		auto offset = limit.offset_val.GetConstantValue();
		available_cardinality = offset >= available_cardinality ? 0 : available_cardinality - offset;
	}
	auto cardinality = available_cardinality;
	switch (limit.limit_val.Type()) {
	case LimitNodeType::CONSTANT_VALUE:
		cardinality = MinValue(cardinality, limit.limit_val.GetConstantValue());
		break;
	case LimitNodeType::CONSTANT_PERCENTAGE: {
		auto percentage = limit.limit_val.GetConstantPercentage();
		auto percentage_cardinality = child_stats.cardinality;
		if (percentage < 100) {
			percentage_cardinality = LossyNumericCast<idx_t>(double(child_stats.cardinality) * percentage / 100.0);
		}
		cardinality = MinValue(cardinality, percentage_cardinality);
		break;
	}
	default:
		break;
	}
	ApplyCardinality(*result, cardinality);
	return result;
}

RelationStats RelationStatisticsHelper::ExtractEmptyResultStats(LogicalEmptyResult &empty) {
	RelationStats result;
	result.cardinality = 0;
	result.table_name = Identifier(empty.GetName());
	result.stats_initialized = true;
	for (auto &binding : empty.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(0, DistinctCountSource::CARDINALITY),
		                            Identifier("empty_result_column"));
	}
	result.Verify(empty.GetColumnBindings());
	return result;
}

optional<RelationStats> RelationStatisticsHelper::ProjectOutputStats(const RelationStats &stats, LogicalOperator &op) {
	if (!stats.stats_initialized) {
		return {};
	}
	RelationStats result;
	result.cardinality = stats.cardinality;
	result.row_retention = stats.row_retention;
	result.stats_initialized = true;
	result.table_name = stats.table_name;
	RelationColumnStatsLookup lookup(stats);
	auto bindings = op.GetColumnBindings();
	for (auto &binding : bindings) {
		auto column = lookup.Find(binding);
		if (!column) {
			return {};
		}
		result.columns.push_back(*column);
	}
	result.Verify(bindings);
	return result;
}

optional<RelationStats> RelationStatisticsHelper::RebindOutputStats(const RelationStats &stats, LogicalOperator &op) {
	auto bindings = op.GetColumnBindings();
	if (!stats.stats_initialized || bindings.size() != stats.columns.size()) {
		return {};
	}
	auto result = stats;
	for (idx_t column_idx = 0; column_idx < bindings.size(); column_idx++) {
		result.columns[column_idx].binding = bindings[column_idx];
	}
	result.Verify(bindings);
	return result;
}

} // namespace duckdb
