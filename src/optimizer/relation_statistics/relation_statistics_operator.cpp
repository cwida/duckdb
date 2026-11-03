#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/planner/operator/list.hpp"

namespace duckdb {

static optional<RelationStats>
ProjectChildStats(LogicalOperator &op, const vector<reference<const RelationStats>> &children, idx_t cardinality) {
	RelationStats result;
	result.cardinality = cardinality;
	result.stats_initialized = true;
	result.table_name = Identifier(op.GetName());
	RelationColumnStatsLookup child_columns;
	for (auto &child : children) {
		child_columns.AddRelation(child.get());
	}
	for (auto &binding : op.GetColumnBindings()) {
		auto source = child_columns.Find(binding);
		if (!source) {
			return {};
		}
		result.columns.emplace_back(binding, source->total_domain, source->current_domain, source->name);
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(op.GetColumnBindings());
	return result;
}

static idx_t JoinCardinality(LogicalComparisonJoin &join, const RelationStats &left, const RelationStats &right) {
	switch (join.join_type) {
	case JoinType::RIGHT_ANTI:
	case JoinType::RIGHT_SEMI:
		return right.cardinality;
	case JoinType::ANTI:
	case JoinType::SEMI:
	case JoinType::SINGLE:
	case JoinType::MARK:
		return left.cardinality;
	default:
		return MaxValue(left.cardinality, right.cardinality);
	}
}

static optional_ptr<const RelationColumnStats> GetJoinColumn(const Expression &expression, const RelationStats &stats) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		return nullptr;
	}
	return stats.GetColumnStats(expression.Cast<BoundColumnRefExpression>().Binding());
}

SemiAntiJoinCardinalityEstimate RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(LogicalComparisonJoin &join,
                                                                                          const RelationStats &left,
                                                                                          const RelationStats &right) {
	auto preserve_right = join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI;
	auto &preserved = preserve_right ? right : left;
	auto &rhs = preserve_right ? left : right;
	vector<SemiAntiJoinDomain> domains;
	bool has_residual = false;
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		                                  condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
			has_residual = true;
			continue;
		}
		auto lhs_preserved = GetJoinColumn(condition.GetLHS(), preserved);
		auto rhs_column = GetJoinColumn(condition.GetRHS(), rhs);
		if (!lhs_preserved || !rhs_column) {
			lhs_preserved = GetJoinColumn(condition.GetRHS(), preserved);
			rhs_column = GetJoinColumn(condition.GetLHS(), rhs);
		}
		if (!lhs_preserved || !rhs_column) {
			has_residual = true;
			continue;
		}
		bool seen_domain = false;
		for (auto &domain : domains) {
			if (domain.preserved_binding == lhs_preserved->binding && domain.rhs_binding == rhs_column->binding) {
				seen_domain = true;
				break;
			}
		}
		if (seen_domain) {
			continue;
		}
		auto rhs_current_domain = rhs_column->GetSupportedSemiAntiDomainSize();
		if (!rhs_current_domain.IsValid()) {
			has_residual = true;
			continue;
		}
		auto total_domain = SelectTotalDomain({lhs_preserved->total_domain, rhs_column->total_domain});
		if (!total_domain || total_domain->distinct_count == 0) {
			has_residual = true;
			continue;
		}
		domains.push_back(
		    {total_domain->distinct_count, rhs_current_domain.GetIndex(), lhs_preserved->binding, rhs_column->binding});
	}
	return EstimateSemiAntiJoinCardinality(static_cast<double>(preserved.cardinality), rhs.cardinality,
	                                       rhs.row_retention, join.join_type, domains, has_residual);
}

static optional<RelationStats> ExtractGetWithChildStats(LogicalGet &get, ClientContext &context,
                                                        const RelationStats &child_stats) {
	auto result = RelationStatisticsHelper::ExtractGetStats(get, context);
	result.cardinality = child_stats.cardinality;
	RelationColumnStatsLookup child_columns(child_stats);
	for (auto &binding : get.GetColumnBindings()) {
		if (binding.table_index == get.table_index) {
			continue;
		}
		auto child_column = child_columns.Find(binding);
		if (!child_column) {
			return {};
		}
		result.columns.emplace_back(binding, child_column->total_domain, child_column->current_domain,
		                            child_column->name);
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(get.GetColumnBindings());
	return result;
}

static optional<RelationStats> ExtractUnnestStats(LogicalOperator &op, const RelationStats &child_stats) {
	auto &unnest = op.Cast<LogicalUnnest>();
	RelationStats result;
	result.cardinality = child_stats.cardinality;
	result.stats_initialized = true;
	result.table_name = Identifier(op.GetName());
	RelationColumnStatsLookup child_columns(child_stats);
	for (auto &binding : op.GetColumnBindings()) {
		auto child_column = child_columns.Find(binding);
		if (child_column) {
			result.columns.emplace_back(binding, child_column->total_domain, child_column->current_domain,
			                            child_column->name);
		} else if (binding.table_index == unnest.unnest_index) {
			result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
			                            Identifier("unnest"));
		} else {
			return {};
		}
	}
	result.Verify(op.GetColumnBindings());
	return result;
}

static optional<RelationStats> ExtractComparisonJoinStats(LogicalComparisonJoin &join,
                                                          const vector<reference<const RelationStats>> &child_stats) {
	if (child_stats.size() != 2) {
		return {};
	}
	if (join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI ||
	    join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI) {
		auto preserve_right = join.join_type == JoinType::RIGHT_SEMI || join.join_type == JoinType::RIGHT_ANTI;
		auto &preserved = child_stats[preserve_right ? 1 : 0].get();
		auto result = ProjectChildStats(join, child_stats, preserved.cardinality);
		if (!result) {
			return {};
		}
		result->row_retention = preserved.row_retention;
		auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(join, child_stats[0], child_stats[1]);
		auto cardinality = LossyNumericCast<idx_t>(estimate.cardinality);
		RelationStatisticsHelper::ApplyCardinality(*result, cardinality);
		result->Verify(join.GetColumnBindings());
		return result;
	}
	auto cardinality = JoinCardinality(join, child_stats[0], child_stats[1]);
	auto result = ProjectChildStats(join, child_stats, cardinality);
	if (result || join.join_type != JoinType::MARK) {
		return result;
	}

	RelationStats mark_result;
	mark_result.cardinality = cardinality;
	mark_result.stats_initialized = true;
	mark_result.table_name = Identifier(join.GetName());
	RelationColumnStatsLookup left_columns(child_stats[0].get());
	for (auto &binding : join.GetColumnBindings()) {
		auto column = left_columns.Find(binding);
		if (column) {
			mark_result.columns.emplace_back(binding, column->total_domain, column->current_domain, column->name);
		} else if (binding.table_index == join.mark_index) {
			mark_result.columns.emplace_back(
			    binding, DistinctCount(MinValue<idx_t>(cardinality, 3), DistinctCountSource::CARDINALITY),
			    Identifier("mark"));
		} else {
			return {};
		}
	}
	mark_result.Verify(join.GetColumnBindings());
	return mark_result;
}

static optional<RelationStats> ExtractCrossProductStats(LogicalOperator &op,
                                                        const vector<reference<const RelationStats>> &child_stats) {
	if (child_stats.size() != 2) {
		return {};
	}
	idx_t cardinality;
	if (!TryMultiplyOperator::Operation(child_stats[0].get().cardinality, child_stats[1].get().cardinality,
	                                    cardinality)) {
		cardinality = NumericLimits<idx_t>::Maximum();
	}
	return ProjectChildStats(op, child_stats, cardinality);
}

static idx_t AddDistinctCounts(idx_t left, idx_t right) {
	idx_t result;
	if (!TryAddOperator::Operation(left, right, result)) {
		return NumericLimits<idx_t>::Maximum();
	}
	return result;
}

static optional<RelationStats> ExtractSetOperationStats(LogicalSetOperation &setop,
                                                        const vector<reference<const RelationStats>> &child_stats) {
	if (child_stats.empty()) {
		return {};
	}

	RelationStats result;
	result.stats_initialized = true;
	result.table_name = Identifier(setop.GetName());
	switch (setop.type) {
	case LogicalOperatorType::LOGICAL_UNION:
		result.cardinality = 0;
		for (auto &child : child_stats) {
			result.cardinality = AddDistinctCounts(result.cardinality, child.get().cardinality);
		}
		break;
	case LogicalOperatorType::LOGICAL_INTERSECT:
		result.cardinality = child_stats[0].get().cardinality;
		for (idx_t child_idx = 1; child_idx < child_stats.size(); child_idx++) {
			result.cardinality = MinValue(result.cardinality, child_stats[child_idx].get().cardinality);
		}
		break;
	case LogicalOperatorType::LOGICAL_EXCEPT:
		result.cardinality = child_stats[0].get().cardinality;
		break;
	default:
		return {};
	}

	auto bindings = setop.GetColumnBindings();
	for (idx_t column_idx = 0; column_idx < bindings.size(); column_idx++) {
		if (column_idx >= child_stats[0].get().columns.size()) {
			return {};
		}
		auto &first_column = child_stats[0].get().columns[column_idx];
		auto total_domain = first_column.total_domain;
		auto current_domain = first_column.current_domain;
		for (idx_t child_idx = 1; child_idx < child_stats.size(); child_idx++) {
			if (column_idx >= child_stats[child_idx].get().columns.size()) {
				return {};
			}
			auto &child_column = child_stats[child_idx].get().columns[column_idx];
			switch (setop.type) {
			case LogicalOperatorType::LOGICAL_UNION:
				total_domain.distinct_count =
				    AddDistinctCounts(total_domain.distinct_count, child_column.total_domain.distinct_count);
				current_domain.distinct_count =
				    AddDistinctCounts(current_domain.distinct_count, child_column.current_domain.distinct_count);
				total_domain.source = DistinctCountSource::CARDINALITY;
				current_domain.source = DistinctCountSource::CARDINALITY;
				break;
			case LogicalOperatorType::LOGICAL_INTERSECT:
				total_domain.distinct_count =
				    MinValue(total_domain.distinct_count, child_column.total_domain.distinct_count);
				current_domain.distinct_count =
				    MinValue(current_domain.distinct_count, child_column.current_domain.distinct_count);
				total_domain.source = DistinctCountSource::CARDINALITY;
				current_domain.source = DistinctCountSource::CARDINALITY;
				break;
			case LogicalOperatorType::LOGICAL_EXCEPT:
				break;
			default:
				return {};
			}
		}
		total_domain.distinct_count = MaxValue(total_domain.distinct_count, current_domain.distinct_count);
		result.columns.emplace_back(bindings[column_idx], total_domain, current_domain, first_column.name);
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(bindings);
	return result;
}

RelationStats RelationStatisticsHelper::ExtractExplainStats(LogicalOperator &op) {
	RelationStats result;
	result.cardinality = 3;
	result.stats_initialized = true;
	result.table_name = Identifier(op.GetName());
	for (auto &binding : op.GetColumnBindings()) {
		result.columns.emplace_back(binding, DistinctCount(result.cardinality, DistinctCountSource::CARDINALITY),
		                            Identifier("explain"));
	}
	result.Verify(op.GetColumnBindings());
	return result;
}

optional<RelationStats>
RelationStatisticsHelper::ExtractOperatorStats(LogicalOperator &op, ClientContext &context,
                                               const vector<reference<const RelationStats>> &child_stats) {
	if (child_stats.size() != op.children.size()) {
		return {};
	}
	for (idx_t child_idx = 0; child_idx < child_stats.size(); child_idx++) {
		auto &stats = child_stats[child_idx].get();
		if (!stats.stats_initialized || !stats.MatchesBindings(op.children[child_idx]->GetColumnBindings())) {
			return {};
		}
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_GET:
		if (child_stats.empty()) {
			return ExtractGetStats(op.Cast<LogicalGet>(), context);
		}
		return child_stats.size() == 1 ? ExtractGetWithChildStats(op.Cast<LogicalGet>(), context, child_stats[0].get())
		                               : optional<RelationStats>();
	case LogicalOperatorType::LOGICAL_DELIM_GET:
		return ExtractDelimGetStats(op.Cast<LogicalDelimGet>(), context);
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		return ExtractDummyScanStats(op.Cast<LogicalDummyScan>(), context);
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		return ExtractExpressionGetStats(op.Cast<LogicalExpressionGet>(), context);
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
		return ExtractColumnDataGetStats(op.Cast<LogicalColumnDataGet>(), context);
	case LogicalOperatorType::LOGICAL_PROJECTION:
		return ExtractProjectionStats(op.Cast<LogicalProjection>(), child_stats[0].get());
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		return ExtractAggregationStats(op.Cast<LogicalAggregate>(), child_stats[0].get(), context);
	case LogicalOperatorType::LOGICAL_WINDOW:
		return ExtractWindowStats(op.Cast<LogicalWindow>(), child_stats[0].get());
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return ExtractDistinctStats(op.Cast<LogicalDistinct>(), child_stats[0].get());
	case LogicalOperatorType::LOGICAL_FILTER: {
		if (child_stats.size() != 1) {
			return {};
		}
		return ExtractFilterStats(op.Cast<LogicalFilter>(), child_stats[0].get());
	}
	case LogicalOperatorType::LOGICAL_UNNEST:
		return ExtractUnnestStats(op, child_stats[0].get());
	case LogicalOperatorType::LOGICAL_LIMIT: {
		if (child_stats.size() != 1) {
			return {};
		}
		return ExtractLimitStats(op.Cast<LogicalLimit>(), child_stats[0].get());
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
		return ExtractComparisonJoinStats(op.Cast<LogicalComparisonJoin>(), child_stats);
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT:
		return ExtractCrossProductStats(op, child_stats);
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_EXCEPT:
		return ExtractSetOperationStats(op.Cast<LogicalSetOperation>(), child_stats);
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return ExtractEmptyResultStats(op.Cast<LogicalEmptyResult>());
	case LogicalOperatorType::LOGICAL_EXPLAIN:
		return ExtractExplainStats(op);
	default:
		return {};
	}
}

} // namespace duckdb
