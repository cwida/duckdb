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
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/parser/expression_map.hpp"

#include <cmath>
#include <limits>

namespace duckdb {

DuplicateEliminatedDomainOptimizer::DuplicateEliminatedDomainOptimizer(Optimizer &optimizer_p)
    : optimizer(optimizer_p) {
}

static bool HasImplicitBitstringAggregate(const LogicalOperator &op) {
	bool found = false;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expression) {
		ExpressionIterator::VisitExpression<BoundAggregateExpression>(**expression, [&](const auto &aggregate) {
			found |= aggregate.Function().GetName() == "bitstring_agg" && aggregate.Function().HasStatisticsCallback();
		});
		ExpressionIterator::VisitExpression<BoundWindowExpression>(**expression, [&](const auto &window) {
			auto &function = window.AggregateFunction();
			found |= function && function->GetName() == "bitstring_agg" && function->HasStatisticsCallback();
		});
	});
	if (found) {
		return true;
	}
	for (auto &child : op.children) {
		if (HasImplicitBitstringAggregate(*child)) {
			return true;
		}
	}
	return false;
}

bool DuplicateEliminatedDomainOptimizer::CanOptimize(const LogicalOperator &op) {
	// Implicit bitstring bounds depend on statistics, including those propagated through CTE references.
	return !HasImplicitBitstringAggregate(op);
}

void DuplicateEliminatedDomainOptimizer::Optimize(unique_ptr<LogicalOperator> &op) {
	if (CanOptimize(*op)) {
		VisitOperator(op);
		op->ResolveOperatorTypes();
	}
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

static optional_idx ReferenceIndex(const Expression &expression, LogicalOperator &input, const LogicalType &type) {
	if (!expression.GetReturnType().EqualsIncludingCollation(type)) {
		return {};
	}
	return ReferenceIndex(expression, input);
}

static bool HasSimpleGrouping(const LogicalAggregate &aggregate) {
	return !aggregate.groups.empty() && aggregate.grouping_functions.empty() &&
	       (aggregate.grouping_sets.empty() ||
	        (aggregate.grouping_sets.size() == 1 && aggregate.grouping_sets[0].size() == aggregate.groups.size()));
}

//! One full grouping set preserves the directly referenced key and its comparison semantics.
static optional_idx GroupKeyIndex(LogicalAggregate &aggregate, idx_t key) {
	if (!HasSimpleGrouping(aggregate) || key >= aggregate.groups.size()) {
		return {};
	}
	return ReferenceIndex(*aggregate.groups[key], *aggregate.children[0]);
}

static optional_idx GroupKeyIndex(LogicalAggregate &aggregate, idx_t key, const LogicalType &type) {
	auto index = GroupKeyIndex(aggregate, key);
	if (!index.IsValid() || !aggregate.groups[key]->GetReturnType().EqualsIncludingCollation(type)) {
		return {};
	}
	return index;
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
	if (aggregate.preserve_input_order || aggregate.groups.size() != 1 || !HasSimpleGrouping(aggregate) ||
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
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto &aggregate = op.Cast<LogicalAggregate>();
		// Empty grouping sets can produce a row from empty input.
		if (aggregate.grouping_sets.size() != 1 || aggregate.grouping_sets[0].empty()) {
			return {};
		}
		return FixedRowCount(*op.children[0]);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
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

//! Copy keys already represented by expressions, leaving columnar inputs on the runtime path.
//! The caller's payload and its evaluation remain in place.
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
	case LogicalOperatorType::LOGICAL_FILTER: {
		auto binding = op.GetColumnBindings()[key];
		BoundColumnRefExpression reference(type, binding);
		auto index = ReferenceIndex(reference, *op.children[0]);
		return index.IsValid() && CopyFixedKeys(*op.children[0], index.GetIndex(), type, rows);
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto index = GroupKeyIndex(op.Cast<LogicalAggregate>(), key, type);
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
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return true;
	default:
		return false;
	}
}

//! The old restriction must be proven to contain every demanded key.
static bool CopyExactFixedKeys(LogicalOperator &op, idx_t key, const LogicalType &type,
                               vector<vector<unique_ptr<Expression>>> &rows) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		auto &expression = *op.expressions[key];
		auto index = ReferenceIndex(expression, *op.children[0], type);
		if (index.IsValid()) {
			return CopyExactFixedKeys(*op.children[0], index.GetIndex(), type, rows);
		}
		return op.children[0]->type == LogicalOperatorType::LOGICAL_DUMMY_SCAN &&
		       AppendFixedKey(rows, expression, type);
	}
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		auto index = GroupKeyIndex(op.Cast<LogicalAggregate>(), key, type);
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
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return CopyFixedKeys(op, key, type, rows);
	default:
		return false;
	}
}

enum class RestrictionAction { KEEP, REPLACE, REMOVE };

static unique_ptr<LogicalAggregate> CreateKeyDomain(Optimizer &optimizer, unique_ptr<LogicalOperator> input,
                                                    unique_ptr<Expression> key) {
	auto domain = make_uniq<LogicalAggregate>(optimizer.binder.GenerateTableIndex(),
	                                          optimizer.binder.GenerateTableIndex(), vector<unique_ptr<Expression>>());
	domain->groups.push_back(std::move(key));
	domain->grouping_sets.push_back({ProjectionIndex(0)});
	domain->children.push_back(std::move(input));
	return domain;
}

static unique_ptr<Expression> DomainKey(const LogicalAggregate &domain) {
	return make_uniq<BoundColumnRefExpression>(domain.groups[0]->GetReturnType(),
	                                           ColumnBinding(domain.group_index, ProjectionIndex(0)));
}

//! A matched input restriction and the projections between it and its aggregate.
//! Borrows the plan for one decision. Apply may destroy the matched join.
class DomainRestriction {
public:
	DomainRestriction(LogicalAggregate &aggregate_p, ExpressionType consumer_comparison_p)
	    : aggregate(aggregate_p), consumer_comparison(consumer_comparison_p) {
		join = Find(*aggregate.children[0], *aggregate.groups[0]);
	}

public:
	optional_ptr<LogicalComparisonJoin> GetJoin() const {
		return join;
	}

	bool HasSimpleProjections() const;
	bool CanRemove() const;
	bool FiltersNullKeys() const {
		return join->conditions[0].GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
		       consumer_comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	}
	void Apply(unique_ptr<LogicalAggregate> &domain, RestrictionAction action);

private:
	optional_ptr<LogicalComparisonJoin> Find(LogicalOperator &input, const Expression &key);

private:
	LogicalAggregate &aggregate;
	ExpressionType consumer_comparison;
	optional_ptr<LogicalComparisonJoin> join;
	vector<reference<LogicalProjection>> projections;
};

optional_ptr<LogicalComparisonJoin> DomainRestriction::Find(LogicalOperator &input, const Expression &key) {
	if (input.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto index = ReferenceIndex(key, input);
		if (!index.IsValid()) {
			return nullptr;
		}
		projections.push_back(input.Cast<LogicalProjection>());
		return Find(*input.children[0], *input.expressions[index.GetIndex()]);
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

bool DomainRestriction::HasSimpleProjections() const {
	for (auto &projection : projections) {
		for (auto &expression : projection.get().expressions) {
			if (!IsWorkReference(*expression)) {
				return false;
			}
		}
	}
	return true;
}

bool DomainRestriction::CanRemove() const {
	if (!HasSimpleProjections()) {
		return false;
	}
	// Removing the restriction exposes other groups to these aggregates.
	for (auto &expression : aggregate.expressions) {
		if (expression->CanThrow()) {
			return false;
		}
	}
	return true;
}

void DomainRestriction::Apply(unique_ptr<LogicalAggregate> &domain, RestrictionAction action) {
	auto &restriction = *join;
	auto &condition = restriction.conditions[0];
	if (action == RestrictionAction::REPLACE) {
		condition.RightReference() = DomainKey(*domain);
		restriction.children[1] = std::move(domain);
	} else {
		D_ASSERT(action == RestrictionAction::REMOVE);
		auto &slot = projections.empty() ? aggregate.children[0] : projections.back().get().children[0];
		D_ASSERT(slot.get() == &restriction);
		auto input = std::move(restriction.children[0]);
		if (FiltersNullKeys()) {
			auto predicate =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			predicate->GetChildrenMutable().push_back(condition.GetLHS().Copy());
			auto filter = make_uniq<LogicalFilter>(std::move(predicate));
			filter->children.push_back(std::move(input));
			input = std::move(filter);
		}
		slot = LogicalJoin::PromoteJoinSide(std::move(input), std::move(restriction.left_projection_map));
		join = nullptr;
	}
}

//! Compare alternatives for one aggregate using the same statistics source and domain-production costs.
//! Statistics are extracted for each decision, never retained across a plan rewrite.
class DomainCostModel {
public:
	DomainCostModel(RelationStatsExtractor &extractor_p, LogicalAggregate &aggregate_p, double key_input_rows_p,
	                double source_read_write_work_p)
	    : extractor(extractor_p), aggregate(aggregate_p), key_input_rows(key_input_rows_p),
	      source_read_write_work(source_read_write_work_p) {
	}

public:
	bool IsProfitable(LogicalComparisonJoin &restriction, LogicalOperator &domain);
	RestrictionAction ChooseRestrictionAction(const DomainRestriction &existing, LogicalAggregate &domain);

public:
	static bool IsCheaper(double candidate, double original);
	static double EstimateAggregateWork(const LogicalAggregate &aggregate, double input_rows, double group_count);
	static double EstimateHashBuildWork(double key_rows);
	static double EstimateSemiJoinWork(double input_rows, double output_rows, double key_rows);

private:
	double EstimateAggregateWork(double input_rows, double group_count) const;
	double EstimateAggregateWork(const RelationStats &input, ColumnBinding group_binding) const;
	static double EstimateFixedDomainWork(double key_rows, double distinct_key_rows);

private:
	RelationStatsExtractor &extractor;
	LogicalAggregate &aggregate;
	double key_input_rows;
	double source_read_write_work;
};

bool DomainCostModel::IsCheaper(double candidate, double original) {
	auto tolerance = 32 * std::numeric_limits<double>::epsilon() * MaxValue(original, candidate);
	return std::isfinite(original) && std::isfinite(candidate) && original - candidate > tolerance;
}

double DomainCostModel::EstimateAggregateWork(double input_rows, double group_count) const {
	return EstimateAggregateWork(aggregate, input_rows, group_count);
}

double DomainCostModel::EstimateAggregateWork(const LogicalAggregate &aggregate, double input_rows,
                                              double group_count) {
	auto aggregate_work_per_row = static_cast<double>(aggregate.expressions.size()) + 1;
	return aggregate_work_per_row * input_rows + (aggregate_work_per_row + 1) * group_count;
}

double DomainCostModel::EstimateHashBuildWork(double key_rows) {
	// Materialize build rows, then insert them into the hash table.
	return 2 * key_rows;
}

double DomainCostModel::EstimateSemiJoinWork(double input_rows, double output_rows, double key_rows) {
	return EstimateHashBuildWork(key_rows) + input_rows + output_rows;
}

double DomainCostModel::EstimateFixedDomainWork(double key_rows, double distinct_key_rows) {
	return 2 * key_rows + distinct_key_rows + 3;
}

double DomainCostModel::EstimateAggregateWork(const RelationStats &input, ColumnBinding group_binding) const {
	auto column = input.GetColumnStats(group_binding);
	if (!column) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	auto group_count =
	    RelationStatisticsHelper::EstimateDistinctCardinality({column->current_domain}, input.cardinality);
	return EstimateAggregateWork(static_cast<double>(input.cardinality), static_cast<double>(group_count));
}

static RelationStats EstimateRestrictionStats(LogicalComparisonJoin &restriction, const RelationStats &input,
                                              const RelationStats &keys) {
	auto restricted = input;
	auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(restriction, input, keys);
	// The fallback is not evidence of savings, including reduced grouping domains.
	if (estimate.used_domain_evidence) {
		RelationStatisticsHelper::ApplySemiAntiJoinStats(restriction, input, keys, restricted);
	}
	return restricted;
}

bool DomainCostModel::IsProfitable(LogicalComparisonJoin &restriction, LogicalOperator &domain) {
	auto input = extractor.Extract(*aggregate.children[0]);
	auto groups = extractor.Extract(aggregate);
	auto keys = extractor.Extract(domain);
	if (!input || !groups || !keys) {
		return false;
	}
	auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(restriction, *input, *keys);
	if (!estimate.used_domain_evidence) {
		return false;
	}
	auto restricted = EstimateRestrictionStats(restriction, *input, *keys);
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
	auto distinct_key_rows = static_cast<double>(keys->cardinality);
	// Cancel unchanged input/caller work and the consumer's identical output.
	auto canonical = EstimateAggregateWork(input_rows, original_groups);
	auto candidate = EstimateAggregateWork(restricted_rows, restricted_groups) +
	                 EstimateSemiJoinWork(input_rows, restricted_rows, distinct_key_rows) +
	                 EstimateFixedDomainWork(key_input_rows, distinct_key_rows) + source_read_write_work;
	return IsCheaper(candidate, canonical);
}

RestrictionAction DomainCostModel::ChooseRestrictionAction(const DomainRestriction &existing,
                                                           LogicalAggregate &domain) {
	auto &restriction = *existing.GetJoin();
	auto &condition = restriction.conditions[0];
	auto input = extractor.Extract(*restriction.children[0]);
	auto old_keys = extractor.Extract(*restriction.children[1]);
	auto new_keys = extractor.Extract(domain);
	if (!input || !old_keys || !new_keys) {
		return RestrictionAction::KEEP;
	}
	LogicalComparisonJoin replacement(JoinType::SEMI);
	replacement.conditions.emplace_back(condition.GetLHS().Copy(), DomainKey(domain), condition.GetComparisonType());
	auto restricted = EstimateRestrictionStats(replacement, *input, *new_keys);
	auto old_input = EstimateRestrictionStats(restriction, *input, *old_keys);
	// The projections preserve row counts and directly alias this grouping key.
	auto group_binding = condition.GetLHS().Cast<BoundColumnRefExpression>().Binding();
	auto input_rows = static_cast<double>(input->cardinality);
	auto old_input_rows = static_cast<double>(old_input.cardinality);
	auto distinct_key_rows = static_cast<double>(new_keys->cardinality);
	// Ignore other removed producer work: its build rows alone are a saving.
	auto best_work = EstimateAggregateWork(old_input, group_binding) +
	                 EstimateSemiJoinWork(input_rows, old_input_rows, static_cast<double>(old_keys->cardinality));
	auto action = RestrictionAction::KEEP;
	auto consider = [&](double work, RestrictionAction alternative) {
		if (IsCheaper(work, best_work)) {
			best_work = work;
			action = alternative;
		}
	};
	consider(EstimateAggregateWork(restricted, group_binding) +
	             EstimateSemiJoinWork(input_rows, static_cast<double>(restricted.cardinality), distinct_key_rows) +
	             EstimateFixedDomainWork(key_input_rows, distinct_key_rows) + source_read_write_work,
	         RestrictionAction::REPLACE);
	if (existing.CanRemove()) {
		// Charge the NULL filter without taking credit for rows it removes.
		consider(EstimateAggregateWork(*input, group_binding) + (existing.FiltersNullKeys() ? input_rows : 0),
		         RestrictionAction::REMOVE);
	}
	return action;
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

static bool TryOptimizeRestriction(Optimizer &optimizer, LogicalAggregate &aggregate,
                                   unique_ptr<LogicalAggregate> &domain, ExpressionType consumer_comparison) {
	DomainRestriction existing(aggregate, consumer_comparison);
	auto restriction = existing.GetJoin();
	if (!restriction || HasSharedSource(*restriction->children[1])) {
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
	RelationStatsExtractor extractor(optimizer.context);
	DomainCostModel costs(extractor, aggregate, static_cast<double>(fixed.expressions.size()), 0);
	auto action = costs.ChooseRestrictionAction(existing, *domain);
	if (action == RestrictionAction::KEEP) {
		return false;
	}
	existing.Apply(domain, action);
	return true;
}

static optional_ptr<LogicalCTERef> FindSharedKey(LogicalOperator &op, idx_t &key) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto index = ReferenceIndex(*op.expressions[key], *op.children[0]);
		if (!index.IsValid()) {
			return nullptr;
		}
		key = index.GetIndex();
		return FindSharedKey(*op.children[0], key);
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		auto index = GroupKeyIndex(aggregate, key);
		if (!index.IsValid()) {
			return nullptr;
		}
		key = index.GetIndex();
		return FindSharedKey(*op.children[0], key);
	}
	if (op.type != LogicalOperatorType::LOGICAL_CTE_REF) {
		return nullptr;
	}
	auto &reference = op.Cast<LogicalCTERef>();
	return reference.is_recurring || reference.correlated_columns ? nullptr : &reference;
}

struct RuntimeDomainSource {
	optional_ptr<LogicalOperator> input;
	optional_ptr<const LogicalMaterializedCTE> owner;
	idx_t key = 0;
};

static optional_ptr<const LogicalMaterializedCTE>
FindVisibleOwner(TableIndex index, const vector<reference<LogicalMaterializedCTE>> &cte_scopes) {
	for (auto &owner : cte_scopes) {
		if (owner.get().table_index == index) {
			return owner.get();
		}
	}
	return nullptr;
}

static bool ReferencesVisibleOwners(LogicalOperator &op, const vector<reference<LogicalMaterializedCTE>> &cte_scopes) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte = op.Cast<LogicalCTERef>();
		return !cte.is_recurring && !cte.correlated_columns && FindVisibleOwner(cte.cte_index, cte_scopes);
	}
	for (auto &child : op.children) {
		if (!ReferencesVisibleOwners(*child, cte_scopes)) {
			return false;
		}
	}
	return true;
}

static bool RuntimeKeysAreRepeatable(LogicalOperator &op, const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                     unordered_map<TableIndex, bool> &checked_owners) {
	if (ClassifyLogicalOperatorRepeatability(op) != LogicalOperatorRepeatability::REPEATABLE) {
		return false;
	}
	// A SINGLE join can throw when additional rows are evaluated.
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	    op.Cast<LogicalComparisonJoin>().join_type == JoinType::SINGLE) {
		return false;
	}
	// Reader changes and materialization can expose rows that later filters or limits would skip.
	bool can_throw = false;
	LogicalOperatorVisitor::EnumerateExpressions(
	    op, [&](unique_ptr<Expression> *expression) { can_throw |= (*expression)->CanThrow(); });
	// Pushed-down scan predicates are not enumerated by the logical operator visitor.
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		for (auto &entry : op.Cast<LogicalGet>().table_filters) {
			can_throw |=
			    ExpressionFilter::GetExpressionFilter(entry.Filter(), "CanChangeRuntimeKeyReaders").expr->CanThrow();
		}
	}
	if (can_throw) {
		return false;
	}
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
		if (!RuntimeKeysAreRepeatable(*op.children[0], cte_scopes, checked_owners)) {
			return false;
		}
		auto child_scopes = cte_scopes;
		child_scopes.emplace_back(op.Cast<LogicalMaterializedCTE>());
		return RuntimeKeysAreRepeatable(*op.children[1], child_scopes, checked_owners);
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte = op.Cast<LogicalCTERef>();
		auto owner = FindVisibleOwner(cte.cte_index, cte_scopes);
		if (!owner || cte.is_recurring || cte.correlated_columns) {
			return false;
		}
		auto entry = checked_owners.emplace(cte.cte_index, false);
		if (!entry.second) {
			return entry.first->second;
		}
		// Reader changes can expose or prevent producer filters and alter volatile payload evaluation.
		auto repeatable = RuntimeKeysAreRepeatable(*owner->children[0], cte_scopes, checked_owners);
		checked_owners[cte.cte_index] = repeatable;
		return repeatable;
	}
	for (auto &child : op.children) {
		if (!RuntimeKeysAreRepeatable(*child, cte_scopes, checked_owners)) {
			return false;
		}
	}
	return true;
}

static bool CanChangeRuntimeKeyReaders(LogicalOperator &op, const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                       unordered_map<TableIndex, bool> &checked_owners) {
	if (!RuntimeKeysAreRepeatable(op, cte_scopes, checked_owners)) {
		return false;
	}
	for (auto &entry : checked_owners) {
		auto owner = FindVisibleOwner(entry.first, cte_scopes);
		if (!owner || owner->filter_dependency) {
			return false;
		}
	}
	return true;
}

static bool TryGetRuntimeDomainSource(LogicalOperator &demand, idx_t demand_index,
                                      const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                      RuntimeDomainSource &source) {
	source.key = demand_index;
	auto shared = FindSharedKey(demand, source.key);
	unordered_map<TableIndex, bool> checked_owners;
	if (shared) {
		auto owner = FindVisibleOwner(shared->cte_index, cte_scopes);
		// Statistics can make the original reader filterable after this pass.
		if (!owner || !CanChangeRuntimeKeyReaders(*shared, cte_scopes, checked_owners)) {
			return false;
		}
		source.input = *owner->children[0];
		source.owner = owner;
	} else {
		if (!CanChangeRuntimeKeyReaders(demand, cte_scopes, checked_owners)) {
			return false;
		}
		source.input = demand;
		source.key = demand_index;
	}
	return true;
}

static void TraceSharedKeys(LogicalOperator &op, idx_t key, const LogicalType &type,
                            const vector<reference<LogicalMaterializedCTE>> &cte_scopes, bool allow_filters,
                            column_binding_set_t &keys) {
	optional_ptr<LogicalOperator> current = op;
	auto binding = op.GetColumnBindings()[key];
	while (true) {
		BoundColumnRefExpression reference(type, binding);
		auto index = ReferenceIndex(reference, *current);
		if (!index.IsValid()) {
			return;
		}
		switch (current->type) {
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &expression = *current->expressions[index.GetIndex()];
			if (!ReferenceIndex(expression, *current->children[0], type).IsValid()) {
				return;
			}
			binding = expression.Cast<BoundColumnRefExpression>().Binding();
			break;
		}
		case LogicalOperatorType::LOGICAL_FILTER:
			if (!allow_filters) {
				return;
			}
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggregate = current->Cast<LogicalAggregate>();
			if (!GroupKeyIndex(aggregate, index.GetIndex(), type).IsValid()) {
				return;
			}
			auto &expression = *aggregate.groups[index.GetIndex()];
			binding = expression.Cast<BoundColumnRefExpression>().Binding();
			break;
		}
		case LogicalOperatorType::LOGICAL_CTE_REF: {
			auto &cte = current->Cast<LogicalCTERef>();
			auto owner = FindVisibleOwner(cte.cte_index, cte_scopes);
			if (!owner || cte.is_recurring || cte.correlated_columns ||
			    !keys.emplace(cte.cte_index, ProjectionIndex(index.GetIndex())).second) {
				return;
			}
			current = *owner->children[0];
			binding = current->GetColumnBindings()[index.GetIndex()];
			continue;
		}
		default:
			return;
		}
		current = *current->children[0];
	}
}

static RestrictionAction TryOptimizeRuntimeRestriction(DomainCostModel &costs, LogicalAggregate &aggregate,
                                                       LogicalOperator &demand, idx_t demand_index,
                                                       unique_ptr<LogicalAggregate> &domain,
                                                       ExpressionType consumer_comparison,
                                                       const vector<reference<LogicalMaterializedCTE>> &cte_scopes) {
	DomainRestriction existing(aggregate, consumer_comparison);
	auto restriction = existing.GetJoin();
	if (!restriction || !existing.HasSimpleProjections()) {
		return RestrictionAction::KEEP;
	}
	unordered_map<TableIndex, bool> checked_owners;
	if (!CanChangeRuntimeKeyReaders(*restriction->children[1], cte_scopes, checked_owners)) {
		return RestrictionAction::KEEP;
	}
	auto &condition = restriction->conditions[0];
	auto key = ReferenceIndex(condition.GetRHS(), *restriction->children[1]).GetIndex();
	column_binding_set_t demanded_keys, original_keys;
	TraceSharedKeys(demand, demand_index, condition.GetRHS().GetReturnType(), cte_scopes, true, demanded_keys);
	TraceSharedKeys(*restriction->children[1], key, condition.GetRHS().GetReturnType(), cte_scopes, false,
	                original_keys);
	bool contains_demand = false;
	for (auto &binding : original_keys) {
		contains_demand |= demanded_keys.count(binding) != 0;
	}
	if (!contains_demand) {
		return RestrictionAction::KEEP;
	}
	auto action = costs.ChooseRestrictionAction(existing, *domain);
	if (action != RestrictionAction::KEEP) {
		existing.Apply(domain, action);
	}
	return action;
}

static BindingReplacementGraph MaterializeRuntimeDomain(Optimizer &optimizer, unique_ptr<LogicalOperator> &op,
                                                        idx_t demand_side, const LogicalCTERef &domain) {
	BindingReplacementGraph replacements;
	unique_ptr<LogicalOperator> definition;
	ColumnBindingRewrite::RewriteChild(
	    op, demand_side, [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &bindings) {
		    auto reference = make_uniq<LogicalCTERef>(optimizer.binder.GenerateTableIndex(), domain.cte_index,
		                                              domain.chunk_types, domain.bound_columns);
		    auto old_bindings = child->GetColumnBindings();
		    auto new_bindings = reference->GetColumnBindings();
		    for (idx_t index = 0; index < old_bindings.size(); index++) {
			    if (!bindings.Find(old_bindings[index])) {
				    bindings.Add(old_bindings[index], new_bindings[index]);
			    }
		    }
		    replacements.Merge(bindings);
		    definition = std::move(child);
		    child = std::move(reference);
	    });
	op =
	    make_uniq<LogicalMaterializedCTE>("domain_source", domain.cte_index, domain.chunk_types.size(),
	                                      std::move(definition), std::move(op), CTEMaterialize::CTE_MATERIALIZE_ALWAYS);
	return replacements;
}

bool DuplicateEliminatedDomainOptimizer::TryOptimizeRuntimeDomain(unique_ptr<LogicalOperator> &op,
                                                                  LogicalAggregate &aggregate, idx_t aggregate_side,
                                                                  idx_t demand_index) {
	auto &join = op->Cast<LogicalComparisonJoin>();
	auto &demand = *join.children[1 - aggregate_side];
	if (!ReferencesVisibleOwners(demand, cte_scopes) || !ReferencesVisibleOwners(aggregate, cte_scopes)) {
		return false;
	}
	RuntimeDomainSource source;
	if (!TryGetRuntimeDomainSource(demand, demand_index, cte_scopes, source)) {
		return false;
	}
	auto owner_index = source.owner ? source.owner->table_index : optimizer.binder.GenerateTableIndex();
	RelationStatsExtractor extractor(optimizer.context, [&](TableIndex index) -> optional_ptr<LogicalOperator> {
		if (index == owner_index) {
			return source.input;
		}
		auto owner = FindVisibleOwner(index, cte_scopes);
		return owner ? owner->children[0].get() : nullptr;
	});
	auto source_stats = extractor.Extract(*source.input);
	if (!source_stats) {
		return false;
	}
	source.input->ResolveOperatorTypes();
	auto &source_types = source.input->types;
	auto key_reference =
	    make_uniq<LogicalCTERef>(optimizer.binder.GenerateTableIndex(), owner_index, source_types,
	                             AggregateRewriteHelper::GenerateColumnNames("domain", source_types.size()));
	auto &domain_reference = *key_reference;
	auto key_expression =
	    make_uniq<BoundColumnRefExpression>(source_types[source.key], key_reference->GetColumnBindings()[source.key]);
	auto domain = CreateKeyDomain(optimizer, std::move(key_reference), std::move(key_expression));
	auto restriction = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	restriction->conditions.emplace_back(aggregate.groups[0]->Copy(), DomainKey(*domain),
	                                     join.conditions[0].GetComparisonType());
	auto source_rows = static_cast<double>(source_stats->cardinality);
	// CTE readers scan the full row. A new owner also writes it and replaces the consumer with another reader.
	auto source_read_write_work = (source.owner ? 1 : 3) * source_rows * static_cast<double>(source_types.size());
	DomainCostModel costs(extractor, aggregate, source_rows, source_read_write_work);
	auto action = TryOptimizeRuntimeRestriction(costs, aggregate, demand, demand_index, domain,
	                                            join.conditions[0].GetComparisonType(), cte_scopes);
	if (action != RestrictionAction::KEEP) {
		if (action == RestrictionAction::REPLACE && !source.owner) {
			replacements.Merge(MaterializeRuntimeDomain(optimizer, op, 1 - aggregate_side, domain_reference));
		}
		return true;
	}
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(aggregate)) {
		return false;
	}
	if (!costs.IsProfitable(*restriction, *domain)) {
		return false;
	}
	restriction->children.push_back(std::move(aggregate.children[0]));
	restriction->children.push_back(std::move(domain));
	aggregate.children[0] = std::move(restriction);
	if (!source.owner) {
		replacements.Merge(MaterializeRuntimeDomain(optimizer, op, 1 - aggregate_side, domain_reference));
	}
	return true;
}

bool DuplicateEliminatedDomainOptimizer::TryOptimizeDomain(unique_ptr<LogicalOperator> &op, idx_t aggregate_side) {
	auto &join = op->Cast<LogicalComparisonJoin>();
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
		return TryOptimizeRuntimeDomain(op, *aggregate, aggregate_side, demand_index.GetIndex());
	}
	auto key_index = optimizer.binder.GenerateTableIndex();
	auto fixed =
	    make_uniq<LogicalExpressionGet>(key_index, vector<LogicalType> {demand_key.GetReturnType()}, std::move(rows));
	fixed->children.push_back(make_uniq<LogicalDummyScan>(optimizer.binder.GenerateTableIndex()));
	auto key_expression =
	    make_uniq<BoundColumnRefExpression>(demand_key.GetReturnType(), ColumnBinding(key_index, ProjectionIndex(0)));
	auto domain = CreateKeyDomain(optimizer, std::move(fixed), std::move(key_expression));
	if (TryOptimizeRestriction(optimizer, *aggregate, domain, condition.GetComparisonType())) {
		return true;
	}
	// Replacing an existing restriction keeps its filter opportunity. A new one may duplicate the consumer's work.
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(*aggregate)) {
		return false;
	}
	auto restriction = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	restriction->conditions.emplace_back(aggregate->groups[0]->Copy(), DomainKey(*domain),
	                                     condition.GetComparisonType());
	RelationStatsExtractor extractor(optimizer.context);
	auto key_input_rows = static_cast<double>(domain->children[0]->Cast<LogicalExpressionGet>().expressions.size());
	DomainCostModel costs(extractor, *aggregate, key_input_rows, 0);
	if (!costs.IsProfitable(*restriction, *domain)) {
		return false;
	}
	// All analysis borrows have expired. Preserve every original binding and payload column.
	restriction->children.push_back(std::move(aggregate->children[0]));
	restriction->children.push_back(std::move(domain));
	aggregate->children[0] = std::move(restriction);
	return true;
}

static bool CanExpandDomainWork(LogicalOperator &op) {
	// Statement wrappers do not evaluate expressions on the query's rows.
	if (op.type == LogicalOperatorType::LOGICAL_EXPLAIN || op.type == LogicalOperatorType::LOGICAL_PREPARE ||
	    op.type == LogicalOperatorType::LOGICAL_EXECUTE) {
		return true;
	}
	if (ClassifyLogicalOperatorRepeatability(op) != LogicalOperatorRepeatability::REPEATABLE) {
		return false;
	}
	// Extra keys can trigger scalar-subquery cardinality errors without evaluating a throwing expression.
	if ((op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN || op.type == LogicalOperatorType::LOGICAL_ANY_JOIN) &&
	    op.Cast<LogicalJoin>().join_type == JoinType::SINGLE) {
		return false;
	}
	bool safe = true;
	LogicalOperatorVisitor::EnumerateExpressions(op, [&](const unique_ptr<Expression> *expression) {
		safe &=
		    !(**expression).CanThrow() && !(**expression).IsVolatile() && !ExpressionBarrier::Contains(**expression);
		ExpressionIterator::VisitExpression<BoundFunctionExpression>(
		    **expression, [&](const auto &function) { safe &= !function.Function().RequiresOrderedExecution(); });
	});
	return safe;
}

static bool MapDomainKeys(const vector<unique_ptr<Expression>> &expressions, const vector<ColumnBinding> &outputs,
                          vector<column_binding_set_t> &key_aliases) {
	for (auto &aliases : key_aliases) {
		column_binding_set_t mapped;
		for (idx_t index = 0; index < expressions.size(); index++) {
			auto &expression = *expressions[index];
			if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    expression.Cast<BoundColumnRefExpression>().Depth() == 0 &&
			    aliases.count(expression.Cast<BoundColumnRefExpression>().Binding())) {
				mapped.insert(outputs[index]);
			}
		}
		if (mapped.empty()) {
			return false;
		}
		aliases = std::move(mapped);
	}
	return true;
}

//! Once this consumer rejects all extra keys, expressions above it do not gain new input rows.
static bool DomainCoversConsumer(LogicalComparisonJoin &restriction, LogicalComparisonJoin &consumer, idx_t child_index,
                                 const vector<ColumnBinding> &keys,
                                 const vector<reference<LogicalMaterializedCTE>> &cte_scopes) {
	auto &domain = *restriction.children[1];
	auto &demand = *consumer.children[1 - child_index];
	unordered_set<TableIndex> covering_owners;
	vector<idx_t> demand_positions;
	for (idx_t key_index = 0; key_index < keys.size(); key_index++) {
		optional_ptr<const Expression> demand_key;
		for (auto &condition : consumer.conditions) {
			if (!condition.IsComparison() ||
			    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
			     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
				continue;
			}
			auto &expression = child_index == 0 ? condition.GetLHS() : condition.GetRHS();
			if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
			    expression.Cast<BoundColumnRefExpression>().Binding() == keys[key_index]) {
				demand_key = child_index == 0 ? condition.GetRHS() : condition.GetLHS();
				break;
			}
		}
		if (!demand_key) {
			return false;
		}
		auto demand_position = ReferenceIndex(*demand_key, demand);
		auto &domain_key = restriction.conditions[key_index].GetRHS();
		if (!demand_position.IsValid() ||
		    !domain_key.GetReturnType().EqualsIncludingCollation(demand_key->GetReturnType())) {
			return false;
		}
		demand_positions.push_back(demand_position.GetIndex());
		auto domain_position = ReferenceIndex(domain_key, domain).GetIndex();
		column_binding_set_t domain_sources, demand_sources;
		TraceSharedKeys(domain, domain_position, domain_key.GetReturnType(), cte_scopes, false, domain_sources);
		TraceSharedKeys(demand, demand_position.GetIndex(), domain_key.GetReturnType(), cte_scopes, true,
		                demand_sources);
		unordered_set<TableIndex> matched_owners;
		for (auto &binding : domain_sources) {
			if (demand_sources.count(binding) && (key_index == 0 || covering_owners.count(binding.table_index))) {
				matched_owners.insert(binding.table_index);
			}
		}
		covering_owners = std::move(matched_owners);
	}
	if (!covering_owners.empty()) {
		return true;
	}
	// The existing closed-input proof also covers inlined single-key callers.
	if (keys.size() != 1) {
		return false;
	}
	auto &key = restriction.conditions[0].GetRHS();
	vector<vector<unique_ptr<Expression>>> domain_rows, demand_rows;
	if (!CopyExactFixedKeys(domain, ReferenceIndex(key, domain).GetIndex(), key.GetReturnType(), domain_rows) ||
	    !CopyFixedKeys(demand, demand_positions[0], key.GetReturnType(), demand_rows)) {
		return false;
	}
	expression_set_t domain_keys;
	for (auto &row : domain_rows) {
		domain_keys.insert(*row[0]);
	}
	for (auto &row : demand_rows) {
		if (!domain_keys.count(*row[0])) {
			return false;
		}
	}
	return true;
}

//! Follow only key-preserving scalar/grouped work, up to the consumer that selects the demanded keys.
static bool CollectDeferredWork(const vector<pair<reference<LogicalOperator>, idx_t>> &parents,
                                LogicalComparisonJoin &restriction, vector<column_binding_set_t> key_aliases,
                                const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                vector<pair<reference<LogicalOperator>, idx_t>> &work_path) {
	bool grouped = false;
	for (auto entry = parents.rbegin(); entry != parents.rend(); ++entry) {
		auto &parent = entry->first.get();
		if (!CanExpandDomainWork(parent)) {
			return false;
		}
		switch (parent.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			if (!MapDomainKeys(parent.expressions, parent.GetColumnBindings(), key_aliases)) {
				return false;
			}
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			for (auto &aliases : key_aliases) {
				column_binding_set_t visible;
				for (auto binding : parent.GetColumnBindings()) {
					if (aliases.count(binding)) {
						visible.insert(binding);
					}
				}
				if (visible.empty()) {
					return false;
				}
				aliases = std::move(visible);
			}
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggregate = parent.Cast<LogicalAggregate>();
			if (grouped || !HasSimpleGrouping(aggregate)) {
				return false;
			}
			for (auto &group : aggregate.groups) {
				if (!ReferenceIndex(*group, *aggregate.children[0]).IsValid()) {
					return false;
				}
			}
			if (!MapDomainKeys(aggregate.groups, aggregate.GetColumnBindings(), key_aliases)) {
				return false;
			}
			grouped = true;
			break;
		}
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			auto &consumer = parent.Cast<LogicalComparisonJoin>();
			auto child_index = entry->second;
			auto evidence =
			    child_index == 1 && (consumer.join_type == JoinType::SEMI || consumer.join_type == JoinType::ANTI ||
			                         consumer.join_type == JoinType::MARK);
			auto grouped_consumer = grouped && (consumer.join_type == JoinType::INNER ||
			                                    (child_index == 1 && consumer.join_type == JoinType::LEFT));
			if ((!evidence && !grouped_consumer) || !consumer.expressions.empty()) {
				return false;
			}
			vector<ColumnBinding> consumer_keys;
			for (auto &aliases : key_aliases) {
				bool matched = false;
				for (auto &condition : consumer.conditions) {
					if (!condition.IsComparison() ||
					    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
					     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
						continue;
					}
					auto &expression = child_index == 0 ? condition.GetLHS() : condition.GetRHS();
					if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF &&
					    aliases.count(expression.Cast<BoundColumnRefExpression>().Binding())) {
						consumer_keys.push_back(expression.Cast<BoundColumnRefExpression>().Binding());
						matched = true;
						break;
					}
				}
				if (!matched) {
					return false;
				}
			}
			work_path.emplace_back(parent, child_index);
			// A keyed joining ancestor need not be the final demand consumer. Check later evaluation too.
			for (auto ancestor = entry + 1; ancestor != parents.rend(); ++ancestor) {
				if (!CanExpandDomainWork(ancestor->first.get()) ||
				    (ancestor->first.get().type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE &&
				     ancestor->second == 0)) {
					return DomainCoversConsumer(restriction, consumer, child_index, consumer_keys, cte_scopes);
				}
			}
			return true;
		}
		default:
			return false;
		}
		work_path.emplace_back(parent, entry->second);
	}
	return false;
}

static double ScalarWorkPerRow(LogicalOperator &op) {
	double work = 0;
	for (auto &expression : op.expressions) {
		work += IsWorkReference(*expression) ? 0 : 1;
	}
	return work;
}

static optional<double> EstimateDeferredWork(const vector<pair<reference<LogicalOperator>, idx_t>> &work_path,
                                             RelationStats input, RelationStatsExtractor &extractor) {
	double work = 0;
	for (auto &entry : work_path) {
		auto &op = entry.first.get();
		auto input_rows = static_cast<double>(input.cardinality);
		optional<RelationStats> output;
		switch (op.type) {
		case LogicalOperatorType::LOGICAL_PROJECTION:
			output = RelationStatisticsHelper::ExtractProjectionStats(op.Cast<LogicalProjection>(), input);
			work += ScalarWorkPerRow(op) * input_rows;
			break;
		case LogicalOperatorType::LOGICAL_FILTER:
			output = RelationStatisticsHelper::ExtractFilterStats(op.Cast<LogicalFilter>(), input);
			work += static_cast<double>(op.expressions.size()) * input_rows;
			break;
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggregate = op.Cast<LogicalAggregate>();
			// This path ends at the SEMI, so no input-shape refinement can look through it into a UNION.
			output = RelationStatisticsHelper::ExtractAggregationStats(aggregate, input, extractor);
			if (output) {
				work += DomainCostModel::EstimateAggregateWork(aggregate, input_rows,
				                                               static_cast<double>(output->cardinality));
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
			auto &join = op.Cast<LogicalComparisonJoin>();
			auto other_input = extractor.Extract(*join.children[1 - entry.second]);
			if (!other_input) {
				return {};
			}
			auto other_rows = static_cast<double>(other_input->cardinality);
			auto build_this_input = DomainCostModel::EstimateHashBuildWork(input_rows) + other_rows;
			auto build_other_input = DomainCostModel::EstimateHashBuildWork(other_rows) + input_rows;
			// MARK keeps its RHS as the build side; other supported consumers may swap sides.
			if (join.join_type == JoinType::MARK) {
				return work + (entry.second == 1 ? build_this_input : build_other_input);
			}
			return work + MinValue(build_this_input, build_other_input);
		}
		default:
			return {};
		}
		if (!output) {
			return {};
		}
		input = std::move(*output);
	}
	return {};
}

//! Known production work for a key subtree. Unsupported producers receive no speculative savings.
static optional<double> EstimateKeyProductionWork(LogicalOperator &op, RelationStatsExtractor &extractor) {
	auto stats = extractor.Extract(op);
	if (!stats) {
		return {};
	}
	auto rows = static_cast<double>(stats->cardinality);
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_CTE_REF:
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
		return rows * static_cast<double>(op.GetColumnBindings().size());
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return rows;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_FILTER:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		break;
	default:
		return {};
	}
	auto input = extractor.Extract(*op.children[0]);
	auto input_work = EstimateKeyProductionWork(*op.children[0], extractor);
	if (!input || !input_work) {
		return {};
	}
	auto input_rows = static_cast<double>(input->cardinality);
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return *input_work + DomainCostModel::EstimateAggregateWork(op.Cast<LogicalAggregate>(), input_rows, rows);
	}
	auto work_per_row = op.type == LogicalOperatorType::LOGICAL_FILTER ? static_cast<double>(op.expressions.size())
	                                                                   : ScalarWorkPerRow(op);
	return *input_work + work_per_row * input_rows;
}

static void CountKeyReaders(const LogicalOperator &op, unordered_map<TableIndex, idx_t> &readers) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		readers[op.Cast<LogicalCTERef>().cte_index]++;
	}
	for (auto &child : op.children) {
		CountKeyReaders(*child, readers);
	}
}

static double EstimateRemovedKeyWork(LogicalOperator &domain, RelationStatsExtractor &extractor,
                                     const vector<reference<LogicalMaterializedCTE>> &cte_scopes) {
	auto domain_work = EstimateKeyProductionWork(domain, extractor);
	double work = domain_work ? *domain_work : 0;
	unordered_map<TableIndex, idx_t> removed_readers;
	CountKeyReaders(domain, removed_readers);
	// Definitions only refer to enclosing owners. Account for a producer once, when its last reader disappears.
	for (auto entry = cte_scopes.rbegin(); entry != cte_scopes.rend(); ++entry) {
		auto &owner = entry->get();
		auto removed = removed_readers.find(owner.table_index);
		if (removed == removed_readers.end()) {
			continue;
		}
		unordered_map<TableIndex, idx_t> current_readers;
		CountKeyReaders(*owner.children[1], current_readers);
		if (removed->second != current_readers[owner.table_index]) {
			continue;
		}
		auto definition = extractor.Extract(*owner.children[0]);
		auto definition_work = EstimateKeyProductionWork(*owner.children[0], extractor);
		if (definition && definition_work) {
			work += *definition_work + static_cast<double>(definition->cardinality) *
			                               static_cast<double>(owner.children[0]->GetColumnBindings().size());
		}
		CountKeyReaders(*owner.children[0], removed_readers);
	}
	return work;
}

void DuplicateEliminatedDomainOptimizer::ResolveDeferredDomain(unique_ptr<LogicalOperator> &op) {
	auto &restriction = op->Cast<LogicalComparisonJoin>();
	if (!restriction.optional_delim_domain) {
		return;
	}
	restriction.optional_delim_domain = false;
	if (restriction.join_type != JoinType::SEMI || restriction.conditions.empty() || !restriction.expressions.empty() ||
	    !restriction.duplicate_eliminated_columns.empty() || restriction.filter_pushdown) {
		return;
	}
	vector<column_binding_set_t> key_aliases;
	for (auto &condition : restriction.conditions) {
		if (!condition.IsComparison() ||
		    (condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		     condition.GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) ||
		    !ReferenceIndex(condition.GetLHS(), *restriction.children[0]).IsValid() ||
		    !ReferenceIndex(condition.GetRHS(), *restriction.children[1]).IsValid()) {
			return;
		}
		key_aliases.push_back({condition.GetLHS().Cast<BoundColumnRefExpression>().Binding()});
	}
	vector<pair<reference<LogicalOperator>, idx_t>> work_path;
	unordered_map<TableIndex, bool> checked_owners;
	if (!CollectDeferredWork(parent_path, restriction, std::move(key_aliases), cte_scopes, work_path) ||
	    !RuntimeKeysAreRepeatable(*restriction.children[1], cte_scopes, checked_owners)) {
		return;
	}
	// Removing a reader invalidates MatchesConsumers; the CTE filter pusher owns and rechecks that dependency.
	RelationStatsExtractor extractor(optimizer.context, [&](TableIndex index) -> optional_ptr<LogicalOperator> {
		auto owner = FindVisibleOwner(index, cte_scopes);
		return owner ? owner->children[0].get() : nullptr;
	});
	auto input = extractor.Extract(*restriction.children[0]);
	auto domain = extractor.Extract(*restriction.children[1]);
	if (!input || !domain) {
		return;
	}
	auto restricted = EstimateRestrictionStats(restriction, *input, *domain);
	auto original_work = EstimateDeferredWork(work_path, restricted, extractor);
	auto unrestricted_work = EstimateDeferredWork(work_path, *input, extractor);
	if (!original_work || !unrestricted_work) {
		return;
	}
	auto restriction_work = DomainCostModel::EstimateSemiJoinWork(static_cast<double>(input->cardinality),
	                                                              static_cast<double>(restricted.cardinality),
	                                                              static_cast<double>(domain->cardinality)) +
	                        EstimateRemovedKeyWork(*restriction.children[1], extractor, cte_scopes);
	if (DomainCostModel::IsCheaper(*unrestricted_work, *original_work + restriction_work)) {
		op = LogicalJoin::PromoteJoinSide(std::move(restriction.children[0]),
		                                  std::move(restriction.left_projection_map));
	}
}

void DuplicateEliminatedDomainOptimizer::VisitOperator(unique_ptr<LogicalOperator> &op) {
	replacements = BindingReplacementGraph();
	if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		return;
	}
	BindingReplacementGraph child_replacements;
	// Visit only original children; generated domain operators are never reconsidered.
	for (idx_t child_index = 0; child_index < op->children.size(); child_index++) {
		auto publish_owner = op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE && child_index == 1;
		if (publish_owner) {
			cte_scopes.push_back(op->Cast<LogicalMaterializedCTE>());
		}
		parent_path.emplace_back(*op, child_index);
		ColumnBindingRewrite::RewriteChild(op, child_index,
		                                   [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &bindings) {
			                                   VisitOperator(child);
			                                   bindings = std::move(replacements);
			                                   child_replacements.Merge(bindings);
		                                   });
		parent_path.pop_back();
		if (publish_owner) {
			cte_scopes.pop_back();
		}
	}
	replacements = std::move(child_replacements);
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		ResolveDeferredDomain(op);
	}
	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op->Cast<LogicalComparisonJoin>();
		if (join.conditions.size() == 1 && join.expressions.empty() && join.duplicate_eliminated_columns.empty()) {
			auto &condition = join.conditions[0];
			if (condition.IsComparison() &&
			    (condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL ||
			     condition.GetComparisonType() == ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
				auto join_type = join.join_type;
				auto rewritten = join_type == JoinType::INNER && TryOptimizeDomain(op, 0);
				if (!rewritten &&
				    (join_type == JoinType::INNER || join_type == JoinType::LEFT || join_type == JoinType::SINGLE)) {
					TryOptimizeDomain(op, 1);
				}
			}
		}
	}
	if (!replacements.Empty()) {
		replacements = replacements.ScopedToBoundary(op->GetColumnBindings());
	}
}

} // namespace duckdb
