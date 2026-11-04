#include "duckdb/optimizer/duplicate_eliminated_domain_optimizer.hpp"

#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/parser/expression_map.hpp"

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

//! Unlike demand extraction, this must not look through an operator that can remove keys.
static bool CopyExactFixedKeys(LogicalOperator &op, idx_t key, const LogicalType &type,
                               vector<vector<unique_ptr<Expression>>> &rows) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &expression = *op.expressions[key];
		auto index = ReferenceIndex(expression, *op.children[0]);
		if (index.IsValid()) {
			return CopyExactFixedKeys(*op.children[0], index.GetIndex(), type, rows);
		}
		return op.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN &&
		       AppendFixedKey(rows, expression, type);
	}
	case LogicalOperatorType::LOGICAL_DISTINCT: {
		auto &distinct = op.Cast<LogicalDistinct>();
		bool preserves_key = distinct.distinct_type == DistinctType::DISTINCT;
		for (auto &target : distinct.distinct_targets) {
			preserves_key |= ReferenceIndex(*target, *op.children[0]) == optional_idx(key);
		}
		return preserves_key && CopyExactFixedKeys(*op.children[0], key, type, rows);
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = op.Cast<LogicalAggregate>();
		if (aggregate.grouping_sets.size() != 1 || aggregate.grouping_sets[0].size() != aggregate.groups.size() ||
		    key >= aggregate.groups.size() || !aggregate.grouping_functions.empty()) {
			return false;
		}
		auto index = ReferenceIndex(*aggregate.groups[key], *op.children[0]);
		return index.IsValid() && CopyExactFixedKeys(*op.children[0], index.GetIndex(), type, rows);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		auto &join = op.Cast<LogicalJoin>();
		if (join.join_type != JoinType::LEFT) {
			return false;
		}
		BoundColumnRefExpression reference(type, op.GetColumnBindings()[key]);
		auto index = ReferenceIndex(reference, *op.children[0]);
		return index.IsValid() && CopyExactFixedKeys(*op.children[0], index.GetIndex(), type, rows);
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return CopyFixedKeys(op, key, type, rows);
	default:
		return false;
	}
}

static optional_ptr<LogicalComparisonJoin> FindRestriction(LogicalOperator &input, const Expression &key,
                                                           vector<reference<LogicalProjection>> &projections) {
	if (input.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto index = ReferenceIndex(key, input);
		if (!index.IsValid()) {
			return nullptr;
		}
		projections.push_back(input.Cast<LogicalProjection>());
		return FindRestriction(*input.children[0], *input.expressions[index.GetIndex()], projections);
	}
	if (input.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return nullptr;
	}
	auto &join = input.Cast<LogicalComparisonJoin>();
	if (join.join_type != JoinType::SEMI || join.conditions.size() != 1 || !join.expressions.empty() ||
	    !join.duplicate_eliminated_columns.empty() || join.filter_pushdown || !join.conditions[0].IsComparison()) {
		return nullptr;
	}
	auto &condition = join.conditions[0];
	if ((condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
	     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) ||
	    !key.Equals(condition.GetLHS()) || !ReferenceIndex(key, *join.children[0]).IsValid() ||
	    !ReferenceIndex(condition.GetRHS(), *join.children[1]).IsValid()) {
		return nullptr;
	}
	return join;
}

static bool ReferencesRestrictionKey(const Expression &expression, LogicalOperator &input,
                                     LogicalComparisonJoin &restriction) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
		bool depends_on_key = false;
		ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) {
			depends_on_key |= ReferencesRestrictionKey(child, input, restriction);
		});
		return depends_on_key;
	}
	if (input.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto index = ReferenceIndex(expression, input);
		return index.IsValid() &&
		       ReferencesRestrictionKey(*input.expressions[index.GetIndex()], *input.children[0], restriction);
	}
	return &input == &restriction && expression.Equals(restriction.conditions[0].GetLHS());
}

static bool HasKeyStatisticsCallback(LogicalAggregate &aggregate, LogicalComparisonJoin &restriction) {
	for (auto &expression : aggregate.expressions) {
		auto &bound = expression->Cast<BoundAggregateExpression>();
		if (!bound.Function().HasStatisticsCallback()) {
			continue;
		}
		for (auto &child : bound.GetChildren()) {
			if (ReferencesRestrictionKey(*child, *aggregate.children[0], restriction)) {
				// For example, implicit bitstring bounds can change even for surviving groups.
				return true;
			}
		}
	}
	return false;
}

static bool HasSharedSource(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF || op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasSharedSource(*child)) {
			return true;
		}
	}
	return false;
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

static bool IsReplacementProfitable(Optimizer &optimizer, LogicalAggregate &aggregate,
                                    LogicalComparisonJoin &restriction, LogicalDistinct &domain,
                                    const vector<reference<LogicalProjection>> &projections) {
	auto &condition = restriction.conditions[0];
	auto &fixed = domain.children[0]->Cast<LogicalExpressionGet>();
	RelationStatsExtractor extractor(optimizer.context);
	auto input = extractor.Extract(*restriction.children[0]);
	auto old_keys = extractor.Extract(*restriction.children[1]);
	auto old_input = extractor.Extract(*aggregate.children[0]);
	auto old_groups = extractor.Extract(aggregate);
	auto new_keys = extractor.Extract(domain);
	if (!input || !old_keys || !old_input || !old_groups || !new_keys) {
		return false;
	}
	LogicalComparisonJoin replacement(JoinType::SEMI);
	replacement.conditions.emplace_back(condition.GetLHS().Copy(), domain.distinct_targets[0]->Copy(),
	                                    condition.GetComparisonType());
	optional<RelationStats> new_input = *input;
	RelationStatisticsHelper::ApplySemiAntiJoinStats(replacement, *input, *new_keys, *new_input);
	for (auto projection = projections.rbegin(); projection != projections.rend(); ++projection) {
		new_input = RelationStatisticsHelper::ExtractProjectionStats(projection->get(), *new_input);
		if (!new_input) {
			return false;
		}
	}
	auto column = new_input->GetColumnStats(aggregate.groups[0]->Cast<BoundColumnRefExpression>().Binding());
	if (!column) {
		return false;
	}
	auto new_groups =
	    RelationStatisticsHelper::EstimateDistinctCardinality({column->current_domain}, new_input->cardinality);
	auto aggregate_work = static_cast<double>(aggregate.expressions.size()) + 1;
	// The fact scan/probe is unchanged. Ignore other removed producer work: its build rows alone are a saving.
	auto original = SumWork({aggregate_work * static_cast<double>(old_input->cardinality),
	                         (aggregate_work + 1) * static_cast<double>(old_groups->cardinality),
	                         static_cast<double>(old_keys->cardinality), static_cast<double>(old_input->cardinality)});
	auto alternative = SumWork(
	    {aggregate_work * static_cast<double>(new_input->cardinality),
	     (aggregate_work + 1) * static_cast<double>(new_groups), 2 * static_cast<double>(fixed.expressions.size()),
	     2 * static_cast<double>(new_keys->cardinality), static_cast<double>(new_input->cardinality), 3});
	auto tolerance = 32 * std::numeric_limits<double>::epsilon() * MaxValue(original, alternative);
	return std::isfinite(original) && std::isfinite(alternative) && original - alternative > tolerance;
}

static bool TryReplaceRestriction(Optimizer &optimizer, LogicalAggregate &aggregate,
                                  unique_ptr<LogicalDistinct> &domain) {
	vector<reference<LogicalProjection>> projections;
	auto restriction = FindRestriction(*aggregate.children[0], *aggregate.groups[0], projections);
	if (!restriction || HasKeyStatisticsCallback(aggregate, *restriction) ||
	    HasSharedSource(*restriction->children[1])) {
		return false;
	}
	auto &condition = restriction->conditions[0];
	auto key = ReferenceIndex(condition.GetRHS(), *restriction->children[1]).GetIndex();
	vector<vector<unique_ptr<Expression>>> original_rows;
	if (!CopyExactFixedKeys(*restriction->children[1], key, condition.GetRHS().GetReturnType(), original_rows)) {
		return false;
	}
	expression_set_t original_keys;
	for (auto &row : original_rows) {
		original_keys.insert(*row[0]);
	}
	auto &fixed = domain->children[0]->Cast<LogicalExpressionGet>();
	for (auto &row : fixed.expressions) {
		if (!original_keys.count(*row[0])) {
			return false;
		}
	}
	if (!IsReplacementProfitable(optimizer, aggregate, *restriction, *domain, projections)) {
		return false;
	}
	condition.RightReference() = domain->distinct_targets[0]->Copy();
	restriction->children[1] = std::move(domain);
	return true;
}

static bool TryRestrict(Optimizer &optimizer, LogicalComparisonJoin &join, idx_t aggregate_side,
                        bool allow_replacement) {
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
	if (allow_replacement && TryReplaceRestriction(optimizer, *aggregate, domain)) {
		return true;
	}
	// Replacing an existing restriction keeps its filter opportunity. A new one may duplicate the consumer's work.
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(*aggregate)) {
		return false;
	}
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
	auto previous_preserve_statistics = preserve_input_statistics;
	// CTE statistics can reach consumers outside the current ancestor path.
	preserve_input_statistics |= op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE;
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		for (auto &expression : op.expressions) {
			preserve_input_statistics |=
			    expression->Cast<BoundAggregateExpression>().Function().HasStatisticsCallback();
		}
	} else if (op.type == LogicalOperatorType::LOGICAL_WINDOW) {
		for (auto &expression : op.expressions) {
			auto &window = expression->Cast<BoundWindowExpression>();
			preserve_input_statistics |=
			    window.AggregateFunction() && window.AggregateFunction()->HasStatisticsCallback();
		}
	}
	// Visit only original children; generated domain operators are never reconsidered.
	VisitOperatorChildren(op);
	preserve_input_statistics = previous_preserve_statistics;
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
	// Changing an existing restriction can change statistics observed by an ancestor aggregate.
	auto allow_replacement = !preserve_input_statistics;
	if (join.join_type == JoinType::INNER && TryRestrict(optimizer, join, 0, allow_replacement)) {
		return;
	}
	if (join.join_type == JoinType::INNER || join.join_type == JoinType::LEFT || join.join_type == JoinType::SINGLE) {
		TryRestrict(optimizer, join, 1, allow_replacement);
	}
}

} // namespace duckdb
