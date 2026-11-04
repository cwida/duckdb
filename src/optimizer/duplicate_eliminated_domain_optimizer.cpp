#include "duckdb/optimizer/duplicate_eliminated_domain_optimizer.hpp"

#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

DuplicateEliminatedDomainOptimizer::DuplicateEliminatedDomainOptimizer(Optimizer &optimizer_p)
    : optimizer(optimizer_p) {
}

static bool IsWorkReference(const Expression &expression) {
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF:
		return expression.Cast<BoundColumnRefExpression>().Depth() == 0;
	case ExpressionClass::BOUND_CONSTANT:
	case ExpressionClass::BOUND_PARAMETER:
		return true;
	default:
		return false;
	}
}

static optional_idx ReferenceIndex(const Expression &expression, LogicalOperator &input) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF ||
	    expression.Cast<BoundColumnRefExpression>().Depth() != 0) {
		return {};
	}
	return AggregateRewriteHelper::GetDirectReferenceIndex(expression, input);
}

static optional_ptr<LogicalAggregate> FindAggregate(LogicalOperator &op, idx_t key) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		for (auto &expression : op.expressions) {
			if (!IsWorkReference(*expression)) {
				return nullptr;
			}
		}
		auto index = ReferenceIndex(*op.expressions[key], *op.children[0]);
		return index.IsValid() ? FindAggregate(*op.children[0], index.GetIndex()) : nullptr;
	}
	if (op.type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY || key != 0) {
		return nullptr;
	}
	auto &aggregate = op.Cast<LogicalAggregate>();
	if (aggregate.groups.size() != 1 || aggregate.grouping_sets.size() != 1 || aggregate.grouping_sets[0].size() != 1 ||
	    !aggregate.grouping_sets[0].count(ProjectionIndex(0)) || !aggregate.grouping_functions.empty() ||
	    !ReferenceIndex(*aggregate.groups[0], *aggregate.children[0]).IsValid()) {
		return nullptr;
	}
	for (auto &expression : aggregate.expressions) {
		bool supported = true;
		ExpressionIterator::EnumerateChildren(*expression,
		                                      [&](const Expression &child) { supported &= IsWorkReference(child); });
		if (!supported) {
			return nullptr;
		}
	}
	return LogicalSubtreeIsRepeatable(aggregate) ? &aggregate : nullptr;
}

//! An upper bound from the actual closed input, not a cardinality estimate.
static optional_idx FixedRowCount(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		return 1;
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return 0;
	case LogicalOperatorType::LOGICAL_CHUNK_GET: {
		auto &get = op.Cast<LogicalColumnDataGet>();
		return get.collection ? optional_idx(get.collection->Count()) : optional_idx();
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		if (op.children.size() == 1 && op.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN) {
			return op.Cast<LogicalExpressionGet>().expressions.size();
		}
		return {};
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_DISTINCT:
		return FixedRowCount(*op.children[0]);
	default:
		return {};
	}
}

static bool AppendFixedKey(vector<vector<unique_ptr<Expression>>> &rows, const Expression &expression,
                           const LogicalType &type) {
	if ((expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT &&
	     expression.GetExpressionClass() != ExpressionClass::BOUND_PARAMETER) ||
	    !expression.GetReturnType().EqualsIncludingCollation(type)) {
		return false;
	}
	vector<unique_ptr<Expression>> row;
	row.push_back(expression.Copy());
	rows.push_back(std::move(row));
	return true;
}

//! Only keys are copied. The caller's payload and its evaluation remain in place.
static bool CopyFixedKeys(LogicalOperator &op, idx_t key, const LogicalType &type,
                          vector<vector<unique_ptr<Expression>>> &rows) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &expression = *op.expressions[key];
		auto index = ReferenceIndex(expression, *op.children[0]);
		if (index.IsValid()) {
			return CopyFixedKeys(*op.children[0], index.GetIndex(), type, rows);
		}
		auto count = FixedRowCount(*op.children[0]);
		return count.IsValid() && (count.GetIndex() == 0 || AppendFixedKey(rows, expression, type));
	}
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto binding = op.GetColumnBindings()[key];
		BoundColumnRefExpression reference(type, binding);
		auto index = ReferenceIndex(reference, *op.children[0]);
		return index.IsValid() && CopyFixedKeys(*op.children[0], index.GetIndex(), type, rows);
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		if (!FixedRowCount(op).IsValid()) {
			return false;
		}
		for (auto &row : op.Cast<LogicalExpressionGet>().expressions) {
			if (!AppendFixedKey(rows, *row[key], type)) {
				return false;
			}
		}
		return true;
	}
	case LogicalOperatorType::LOGICAL_CHUNK_GET: {
		auto &get = op.Cast<LogicalColumnDataGet>();
		if (!get.collection) {
			return false;
		}
		ColumnDataScanState scan_state;
		DataChunk chunk;
		get.collection->InitializeScan(scan_state, {get.GetColumnIds()[key]});
		get.collection->InitializeScanChunk(scan_state, chunk);
		while (get.collection->Scan(scan_state, chunk)) {
			for (idx_t row = 0; row < chunk.size(); row++) {
				BoundConstantExpression value(chunk.GetValue(0, row));
				if (!AppendFixedKey(rows, value, type)) {
					return false;
				}
			}
		}
		return true;
	}
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return true;
	default:
		return false;
	}
}

static double SumWork(std::initializer_list<double> terms) {
	double sum = 0;
	double correction = 0;
	for (auto term : terms) {
		if (!std::isfinite(term) || term < 0) {
			return std::numeric_limits<double>::quiet_NaN();
		}
		auto adjusted = term - correction;
		auto next = sum + adjusted;
		correction = (next - sum) - adjusted;
		sum = next;
	}
	return sum;
}

static bool HasRuntimeFilterTargets(LogicalAggregate &aggregate) {
	JoinFilterPushdownColumn column;
	if (!JoinFilterPushdownUtil::PushdownJoinFilterExpression(*aggregate.groups[0], column)) {
		return false;
	}
	vector<PushdownFilterTarget> targets;
	JoinFilterPushdownOptimizer::GetPushdownFilterTargets(*aggregate.children[0], {std::move(column)}, targets,
	                                                      EqualityPropagation::ENABLED);
	return !targets.empty();
}

static bool IsProfitable(ClientContext &context, LogicalAggregate &aggregate, LogicalComparisonJoin &restriction,
                         LogicalOperator &domain) {
	RelationStatsExtractor extractor(context);
	auto input = extractor.Extract(*aggregate.children[0]);
	auto groups = extractor.Extract(aggregate);
	auto keys = extractor.Extract(domain);
	if (!input || !groups || !keys) {
		return false;
	}
	auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(restriction, *input, *keys);
	auto restricted = *input;
	RelationStatisticsHelper::ApplySemiAntiJoinStats(restriction, *input, *keys, restricted);
	auto column = restricted.GetColumnStats(aggregate.groups[0]->Cast<BoundColumnRefExpression>().Binding());
	if (!column) {
		return false;
	}
	// The inserted SEMI is the new aggregate input; old input-shape refinements no longer apply.
	auto group_count =
	    RelationStatisticsHelper::EstimateDistinctCardinality({column->current_domain}, restricted.cardinality);
	auto input_rows = static_cast<double>(input->cardinality);
	auto restricted_rows = estimate.cardinality;
	auto original_groups = static_cast<double>(groups->cardinality);
	auto restricted_groups = static_cast<double>(group_count);
	auto fixed_key_rows = static_cast<double>(domain.children[0]->Cast<LogicalExpressionGet>().expressions.size());
	auto distinct_key_rows = static_cast<double>(keys->cardinality);
	auto aggregate_work_per_row = static_cast<double>(aggregate.expressions.size()) + 1;
	// Cancel unchanged input/caller work and the consumer's identical output.
	auto canonical =
	    SumWork({aggregate_work_per_row * input_rows, aggregate_work_per_row * original_groups, original_groups});
	auto candidate = SumWork({aggregate_work_per_row * restricted_rows, aggregate_work_per_row * restricted_groups,
	                          restricted_groups, fixed_key_rows, fixed_key_rows, distinct_key_rows, input_rows,
	                          distinct_key_rows, restricted_rows, 3});
	auto tolerance = 32 * std::numeric_limits<double>::epsilon() * MaxValue(canonical, candidate);
	return std::isfinite(canonical) && std::isfinite(candidate) && canonical - candidate > tolerance;
}

static bool TryRestrict(Optimizer &optimizer, LogicalComparisonJoin &join, idx_t aggregate_side) {
	auto &condition = join.conditions[0];
	auto &aggregate_key = aggregate_side == 0 ? condition.GetLHS() : condition.GetRHS();
	auto &demand_key = aggregate_side == 0 ? condition.GetRHS() : condition.GetLHS();
	auto aggregate_index = ReferenceIndex(aggregate_key, *join.children[aggregate_side]);
	auto demand_index = ReferenceIndex(demand_key, *join.children[1 - aggregate_side]);
	if (!aggregate_index.IsValid() || !demand_index.IsValid() ||
	    !aggregate_key.GetReturnType().EqualsIncludingCollation(demand_key.GetReturnType())) {
		return false;
	}
	auto aggregate = FindAggregate(*join.children[aggregate_side], aggregate_index.GetIndex());
	if (!aggregate) {
		return false;
	}
	// Existing consumer runtime filters can already save this work; their combined cost is outside this model.
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(*aggregate)) {
		return false;
	}
	vector<vector<unique_ptr<Expression>>> rows;
	if (!CopyFixedKeys(*join.children[1 - aggregate_side], demand_index.GetIndex(), demand_key.GetReturnType(), rows)) {
		return false;
	}
	auto key_index = optimizer.binder.GenerateTableIndex();
	auto fixed =
	    make_uniq<LogicalExpressionGet>(key_index, vector<LogicalType> {demand_key.GetReturnType()}, std::move(rows));
	fixed->children.push_back(make_uniq<LogicalDummyScan>(optimizer.binder.GenerateTableIndex()));
	auto domain = make_uniq<LogicalDistinct>(DistinctType::DISTINCT);
	domain->distinct_targets.push_back(
	    make_uniq<BoundColumnRefExpression>(demand_key.GetReturnType(), ColumnBinding(key_index, ProjectionIndex(0))));
	domain->children.push_back(std::move(fixed));
	auto restriction = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	restriction->conditions.emplace_back(aggregate->groups[0]->Copy(), domain->distinct_targets[0]->Copy(),
	                                     condition.GetComparisonType());
	if (!IsProfitable(optimizer.context, *aggregate, *restriction, *domain)) {
		return false;
	}
	// All analysis borrows have expired. Preserve every original binding and payload column.
	restriction->children.push_back(std::move(aggregate->children[0]));
	restriction->children.push_back(std::move(domain));
	aggregate->children[0] = std::move(restriction);
	return true;
}

void DuplicateEliminatedDomainOptimizer::VisitOperator(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return;
	}
	// Visit only original children; generated domain operators are never reconsidered.
	VisitOperatorChildren(op);
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	if (join.conditions.size() != 1 || !join.expressions.empty() || !join.duplicate_eliminated_columns.empty()) {
		return;
	}
	auto &condition = join.conditions[0];
	if (!condition.IsComparison() || (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
	                                  condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
		return;
	}
	if (join.join_type == JoinType::INNER && TryRestrict(optimizer, join, 0)) {
		return;
	}
	if (join.join_type == JoinType::INNER || join.join_type == JoinType::LEFT || join.join_type == JoinType::SINGLE) {
		TryRestrict(optimizer, join, 1);
	}
}

} // namespace duckdb
