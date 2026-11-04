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
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator_repeatability.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_column_data_get.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
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

void DuplicateEliminatedDomainOptimizer::Optimize(unique_ptr<LogicalOperator> &op) {
	// Implicit bitstring bounds depend on statistics, including those propagated through CTE references.
	if (!HasImplicitBitstringAggregate(*op)) {
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

//! The old restriction must be proven to contain every demanded key.
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

static bool IsCheaper(double candidate, double original) {
	auto tolerance = 32 * std::numeric_limits<double>::epsilon() * MaxValue(original, candidate);
	return std::isfinite(original) && std::isfinite(candidate) && original - candidate > tolerance;
}

static double EstimateAggregateWork(const LogicalAggregate &aggregate, double input_rows, double group_count) {
	auto aggregate_work_per_row = static_cast<double>(aggregate.expressions.size()) + 1;
	return aggregate_work_per_row * input_rows + (aggregate_work_per_row + 1) * group_count;
}

static double EstimateSemiJoinWork(double input_rows, double output_rows, double key_rows) {
	return key_rows + input_rows + output_rows;
}

static double EstimateFixedDomainWork(double key_rows, double distinct_key_rows) {
	return 2 * key_rows + distinct_key_rows + 3;
}

static double EstimateAggregateWork(const LogicalAggregate &aggregate, const RelationStats &input,
                                    ColumnBinding group_binding) {
	auto column = input.GetColumnStats(group_binding);
	if (!column) {
		return std::numeric_limits<double>::quiet_NaN();
	}
	auto group_count =
	    RelationStatisticsHelper::EstimateDistinctCardinality({column->current_domain}, input.cardinality);
	return EstimateAggregateWork(aggregate, static_cast<double>(input.cardinality), static_cast<double>(group_count));
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

static bool IsProfitable(RelationStatsExtractor &extractor, LogicalAggregate &aggregate,
                         LogicalComparisonJoin &restriction, LogicalOperator &domain, double key_input_rows,
                         double source_read_write_work) {
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
	auto distinct_key_rows = static_cast<double>(keys->cardinality);
	// Cancel unchanged input/caller work and the consumer's identical output.
	auto canonical = EstimateAggregateWork(aggregate, input_rows, original_groups);
	auto candidate = EstimateAggregateWork(aggregate, restricted_rows, restricted_groups) +
	                 EstimateSemiJoinWork(input_rows, restricted_rows, distinct_key_rows) +
	                 EstimateFixedDomainWork(key_input_rows, distinct_key_rows) + source_read_write_work;
	return IsCheaper(candidate, canonical);
}

enum class RestrictionAction { KEEP, REPLACE, REMOVE };

static bool CanExpandRestriction(const vector<reference<LogicalProjection>> &projections) {
	for (auto &projection : projections) {
		for (auto &expression : projection.get().expressions) {
			if (!IsWorkReference(*expression)) {
				return false;
			}
		}
	}
	return true;
}

static RestrictionAction ChooseRestrictionAction(RelationStatsExtractor &extractor, LogicalAggregate &aggregate,
                                                 LogicalComparisonJoin &restriction, LogicalDistinct &domain,
                                                 const vector<reference<LogicalProjection>> &projections,
                                                 bool filter_null_keys, double key_input_rows,
                                                 double source_read_write_work) {
	auto &condition = restriction.conditions[0];
	auto input = extractor.Extract(*restriction.children[0]);
	auto old_keys = extractor.Extract(*restriction.children[1]);
	auto old_input = extractor.Extract(*aggregate.children[0]);
	auto old_groups = extractor.Extract(aggregate);
	auto new_keys = extractor.Extract(domain);
	if (!input || !old_keys || !old_input || !old_groups || !new_keys) {
		return RestrictionAction::KEEP;
	}
	LogicalComparisonJoin replacement(JoinType::SEMI);
	replacement.conditions.emplace_back(condition.GetLHS().Copy(), domain.distinct_targets[0]->Copy(),
	                                    condition.GetComparisonType());
	auto restricted = *input;
	RelationStatisticsHelper::ApplySemiAntiJoinStats(replacement, *input, *new_keys, restricted);
	// The projections preserve row counts and directly alias this grouping key.
	auto group_binding = condition.GetLHS().Cast<BoundColumnRefExpression>().Binding();
	auto input_rows = static_cast<double>(input->cardinality);
	auto old_input_rows = static_cast<double>(old_input->cardinality);
	auto distinct_key_rows = static_cast<double>(new_keys->cardinality);
	// Ignore other removed producer work: its build rows alone are a saving.
	auto best_work = EstimateAggregateWork(aggregate, old_input_rows, static_cast<double>(old_groups->cardinality)) +
	                 EstimateSemiJoinWork(input_rows, old_input_rows, static_cast<double>(old_keys->cardinality));
	auto action = RestrictionAction::KEEP;
	auto consider = [&](double work, RestrictionAction alternative) {
		if (IsCheaper(work, best_work)) {
			best_work = work;
			action = alternative;
		}
	};
	consider(EstimateAggregateWork(aggregate, restricted, group_binding) +
	             EstimateSemiJoinWork(input_rows, static_cast<double>(restricted.cardinality), distinct_key_rows) +
	             EstimateFixedDomainWork(key_input_rows, distinct_key_rows) + source_read_write_work,
	         RestrictionAction::REPLACE);
	if (CanExpandRestriction(projections)) {
		// Charge the NULL filter without taking credit for rows it removes.
		consider(EstimateAggregateWork(aggregate, *input, group_binding) + (filter_null_keys ? input_rows : 0),
		         RestrictionAction::REMOVE);
	}
	return action;
}

static void ApplyRestrictionAction(LogicalAggregate &aggregate, LogicalComparisonJoin &restriction,
                                   const vector<reference<LogicalProjection>> &projections,
                                   unique_ptr<LogicalDistinct> &domain, RestrictionAction action,
                                   bool filter_null_keys) {
	auto &condition = restriction.conditions[0];
	if (action == RestrictionAction::REPLACE) {
		condition.RightReference() = domain->distinct_targets[0]->Copy();
		restriction.children[1] = std::move(domain);
	} else {
		D_ASSERT(action == RestrictionAction::REMOVE);
		auto &slot = projections.empty() ? aggregate.children[0] : projections.back().get().children[0];
		D_ASSERT(slot.get() == &restriction);
		auto input = std::move(restriction.children[0]);
		if (filter_null_keys) {
			auto predicate =
			    make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
			predicate->GetChildrenMutable().push_back(condition.GetLHS().Copy());
			auto filter = make_uniq<LogicalFilter>(std::move(predicate));
			filter->children.push_back(std::move(input));
			input = std::move(filter);
		}
		slot = LogicalJoin::PromoteJoinSide(std::move(input), std::move(restriction.left_projection_map));
	}
}

static bool TryOptimizeRestriction(Optimizer &optimizer, LogicalAggregate &aggregate,
                                   unique_ptr<LogicalDistinct> &domain, ExpressionType consumer_comparison) {
	vector<reference<LogicalProjection>> projections;
	auto restriction = FindRestriction(*aggregate.children[0], *aggregate.groups[0], projections);
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
	auto filter_null_keys = condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
	                        consumer_comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	RelationStatsExtractor extractor(optimizer.context);
	auto action = ChooseRestrictionAction(extractor, aggregate, *restriction, *domain, projections, filter_null_keys,
	                                      static_cast<double>(fixed.expressions.size()), 0);
	if (action == RestrictionAction::KEEP) {
		return false;
	}
	ApplyRestrictionAction(aggregate, *restriction, projections, domain, action, filter_null_keys);
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
	if (op.type == LogicalOperatorType::LOGICAL_DISTINCT) {
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

static bool CanChangeRuntimeKeyReaders(LogicalOperator &op, const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                       unordered_map<TableIndex, bool> &checked_owners) {
	if (ClassifyLogicalOperatorRepeatability(op) != LogicalOperatorRepeatability::REPEATABLE) {
		return false;
	}
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &cte = op.Cast<LogicalCTERef>();
		auto owner = FindVisibleOwner(cte.cte_index, cte_scopes);
		if (!owner || cte.is_recurring || cte.correlated_columns || owner->filter_dependency) {
			return false;
		}
		auto entry = checked_owners.emplace(cte.cte_index, false);
		if (!entry.second) {
			return entry.first->second;
		}
		// Reader changes can expose or prevent producer filters and alter volatile payload evaluation.
		auto repeatable = CanChangeRuntimeKeyReaders(*owner->children[0], cte_scopes, checked_owners);
		checked_owners[cte.cte_index] = repeatable;
		return repeatable;
	}
	for (auto &child : op.children) {
		if (!CanChangeRuntimeKeyReaders(*child, cte_scopes, checked_owners)) {
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
	if (shared) {
		auto owner = FindVisibleOwner(shared->cte_index, cte_scopes);
		// An unfiltered old reader already prevents scalar CTE pushdown; do not invalidate a dependency proof.
		if (!owner || owner->filter_dependency) {
			return false;
		}
		source.input = *owner->children[0];
		source.owner = owner;
	} else {
		unordered_map<TableIndex, bool> checked_owners;
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
			if (!ReferenceIndex(expression, *current->children[0]).IsValid()) {
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
		case LogicalOperatorType::LOGICAL_DISTINCT: {
			auto &distinct = current->Cast<LogicalDistinct>();
			bool preserves_key = distinct.distinct_type == DistinctType::DISTINCT;
			for (auto &target : distinct.distinct_targets) {
				preserves_key |=
				    ReferenceIndex(*target, *distinct.children[0]) == ReferenceIndex(reference, *distinct.children[0]);
			}
			if (!preserves_key) {
				return;
			}
			break;
		}
		case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
			auto &aggregate = current->Cast<LogicalAggregate>();
			if (aggregate.grouping_sets.size() != 1 || aggregate.grouping_sets[0].size() != aggregate.groups.size() ||
			    index.GetIndex() >= aggregate.groups.size() || !aggregate.grouping_functions.empty()) {
				return;
			}
			auto &expression = *aggregate.groups[index.GetIndex()];
			if (!ReferenceIndex(expression, *aggregate.children[0]).IsValid()) {
				return;
			}
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

static RestrictionAction TryOptimizeRuntimeRestriction(RelationStatsExtractor &extractor, LogicalAggregate &aggregate,
                                                       LogicalOperator &demand, idx_t demand_index,
                                                       unique_ptr<LogicalDistinct> &domain,
                                                       ExpressionType consumer_comparison,
                                                       const vector<reference<LogicalMaterializedCTE>> &cte_scopes,
                                                       double source_rows, double source_read_write_work) {
	vector<reference<LogicalProjection>> projections;
	auto restriction = FindRestriction(*aggregate.children[0], *aggregate.groups[0], projections);
	if (!restriction || !CanExpandRestriction(projections)) {
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
	auto filter_null_keys = condition.GetComparisonType() == ExpressionType::COMPARE_EQUAL &&
	                        consumer_comparison == ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	auto action = ChooseRestrictionAction(extractor, aggregate, *restriction, *domain, projections, filter_null_keys,
	                                      source_rows, source_read_write_work);
	if (action != RestrictionAction::KEEP) {
		ApplyRestrictionAction(aggregate, *restriction, projections, domain, action, filter_null_keys);
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
	vector<unique_ptr<Expression>> key_expression;
	key_expression.push_back(
	    make_uniq<BoundColumnRefExpression>(source_types[source.key], key_reference->GetColumnBindings()[source.key]));
	auto projection = make_uniq<LogicalProjection>(optimizer.binder.GenerateTableIndex(), std::move(key_expression));
	projection->children.push_back(std::move(key_reference));
	auto domain = make_uniq<LogicalDistinct>(DistinctType::DISTINCT);
	domain->distinct_targets.push_back(
	    make_uniq<BoundColumnRefExpression>(source_types[source.key], projection->GetColumnBindings()[0]));
	domain->children.push_back(std::move(projection));
	auto restriction = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	restriction->conditions.emplace_back(aggregate.groups[0]->Copy(), domain->distinct_targets[0]->Copy(),
	                                     join.conditions[0].GetComparisonType());
	auto source_rows = static_cast<double>(source_stats->cardinality);
	// CTE readers scan the full row. A new owner also writes it and replaces the consumer with another reader.
	auto source_read_write_work = (source.owner ? 1 : 3) * source_rows * static_cast<double>(source_types.size());
	auto action = TryOptimizeRuntimeRestriction(extractor, aggregate, demand, demand_index, domain,
	                                            join.conditions[0].GetComparisonType(), cte_scopes, source_rows,
	                                            source_read_write_work);
	if (action != RestrictionAction::KEEP) {
		if (action == RestrictionAction::REPLACE && !source.owner) {
			replacements.Merge(MaterializeRuntimeDomain(optimizer, op, 1 - aggregate_side, domain_reference));
		}
		return true;
	}
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(aggregate)) {
		return false;
	}
	if (!IsProfitable(extractor, aggregate, *restriction, *domain, source_rows, source_read_write_work)) {
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
	auto domain = make_uniq<LogicalDistinct>(DistinctType::DISTINCT);
	domain->distinct_targets.push_back(
	    make_uniq<BoundColumnRefExpression>(demand_key.GetReturnType(), ColumnBinding(key_index, ProjectionIndex(0))));
	domain->children.push_back(std::move(fixed));
	if (TryOptimizeRestriction(optimizer, *aggregate, domain, condition.GetComparisonType())) {
		return true;
	}
	// Replacing an existing restriction keeps its filter opportunity. A new one may duplicate the consumer's work.
	if (join.join_type != JoinType::SINGLE && HasRuntimeFilterTargets(*aggregate)) {
		return false;
	}
	auto restriction = make_uniq<LogicalComparisonJoin>(JoinType::SEMI);
	restriction->conditions.emplace_back(aggregate->groups[0]->Copy(), domain->distinct_targets[0]->Copy(),
	                                     condition.GetComparisonType());
	RelationStatsExtractor extractor(optimizer.context);
	auto key_input_rows = static_cast<double>(domain->children[0]->Cast<LogicalExpressionGet>().expressions.size());
	if (!IsProfitable(extractor, *aggregate, *restriction, *domain, key_input_rows, 0)) {
		return false;
	}
	// All analysis borrows have expired. Preserve every original binding and payload column.
	restriction->children.push_back(std::move(aggregate->children[0]));
	restriction->children.push_back(std::move(domain));
	aggregate->children[0] = std::move(restriction);
	return true;
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
		ColumnBindingRewrite::RewriteChild(op, child_index,
		                                   [&](unique_ptr<LogicalOperator> &child, BindingReplacementGraph &bindings) {
			                                   VisitOperator(child);
			                                   bindings = std::move(replacements);
			                                   child_replacements.Merge(bindings);
		                                   });
		if (publish_owner) {
			cte_scopes.pop_back();
		}
	}
	replacements = std::move(child_replacements);
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
	replacements = replacements.ScopedToBoundary(op->GetColumnBindings());
}

} // namespace duckdb
