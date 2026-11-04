#include "duckdb/optimizer/join_order/relation_manager.hpp"

#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/logical_operator_type.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/expression_barrier.hpp"
#include "duckdb/planner/joinside.hpp"
#include "duckdb/planner/operator/list.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

SingleJoinRelation::SingleJoinRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent)
    : op(op), parent(parent) {
}

SingleJoinRelation::SingleJoinRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent, RelationStats stats)
    : op(op), parent(parent), stats(std::move(stats)) {
}

RelationManager::RelationManager(ClientContext &context) : context(context) {
}

static bool OperatorNeedsRelation(LogicalOperatorType op_type);
static bool OperatorIsNonReorderable(const LogicalOperator &op);

static bool CanReuseAlignedStats(const LogicalOperator &op) {
	if (OperatorNeedsRelation(op.type) || OperatorIsNonReorderable(op)) {
		return true;
	}
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT:
		return true;
	default:
		return false;
	}
}

const vector<RelationStats> RelationManager::GetRelationStats() {
	vector<RelationStats> ret;
	for (idx_t i = 0; i < relations.size(); i++) {
		ret.push_back(relations[i]->stats);
	}
	return ret;
}

vector<unique_ptr<SingleJoinRelation>> RelationManager::GetRelations() {
	return std::move(relations);
}

idx_t RelationManager::NumRelations() {
	return relations.size();
}

bool RelationManager::AddRelation(LogicalOperator &op, optional_ptr<LogicalOperator> parent,
                                  const RelationStats &stats) {
	// if parent is null, then this is a root relation
	// if parent is not null, it should have multiple children
	D_ASSERT(!parent || parent->children.size() >= 2);
	auto aligned_stats = AlignStatsWithRelationRoot(op, stats);
	if (!aligned_stats) {
		stats_complete = false;
		return false;
	}
	auto relation = make_uniq<SingleJoinRelation>(op, parent, std::move(*aligned_stats));
	RelationIndex relation_id(relations.size());

	unordered_set<TableIndex> relation_bindings;
	auto table_indexes = op.GetTableIndex();
	auto is_mark = op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	               op.Cast<LogicalComparisonJoin>().join_type == JoinType::MARK;
	if (table_indexes.empty() || is_mark) {
		LogicalJoin::GetTableReferences(op, relation_bindings);
	} else {
		relation_bindings.insert(table_indexes.begin(), table_indexes.end());
	}
	for (auto &column : relation->stats.columns) {
		relation_bindings.insert(column.binding.table_index);
	}
	for (auto &binding : relation_bindings) {
		auto entry = relation_mapping.find(binding);
		if (entry == relation_mapping.end()) {
			relation_mapping.emplace(binding, relation_id);
		} else {
			D_ASSERT(entry->second == relation_id);
		}
	}
	RegisterRelationBindings(op, relation_id, relation->stats);
	operator_relations[op] = relation_id;
	op.estimated_cardinality = relation->stats.cardinality;
	op.has_estimated_cardinality = true;
	relations.push_back(std::move(relation));
	return true;
}

optional<RelationStats> RelationManager::AlignStatsWithRelationRoot(LogicalOperator &op, const RelationStats &stats) {
	auto projected = RelationStatisticsHelper::ProjectOutputStats(stats, op);
	if (projected && (op.children.size() != 1 || CanReuseAlignedStats(op))) {
		return projected;
	}
	if (op.children.size() != 1) {
		return {};
	}
	auto child_stats = AlignStatsWithRelationRoot(*op.children[0], stats);
	if (!child_stats) {
		return {};
	}
	vector<reference<const RelationStats>> children {*child_stats};
	auto derived = RelationStatisticsHelper::ExtractOperatorStats(op, context, children);
	if (derived) {
		return derived;
	}

	return {};
}

bool RelationManager::HasCompleteStats() const {
	return stats_complete;
}

void RelationManager::RegisterRelationBindings(LogicalOperator &op, RelationIndex relation_id,
                                               const RelationStats &stats) {
	auto bindings = op.GetColumnBindings();
	stats.Verify(bindings);
	for (idx_t column_idx = 0; column_idx < bindings.size(); column_idx++) {
		auto normalized = ColumnBinding(TableIndex(relation_id.index), ProjectionIndex(column_idx));
		auto entry = normalized_bindings.find(bindings[column_idx]);
		if (entry == normalized_bindings.end()) {
			normalized_bindings.emplace(bindings[column_idx], normalized);
		} else {
			D_ASSERT(entry->second == normalized);
		}
	}
}

bool RelationManager::TryNormalizeBinding(ColumnBinding binding, ColumnBinding &normalized) const {
	auto entry = normalized_bindings.find(binding);
	if (entry == normalized_bindings.end()) {
		return false;
	}
	normalized = entry->second;
	return true;
}

static bool OperatorNeedsRelation(LogicalOperatorType op_type) {
	switch (op_type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_CHUNK_GET:
	case LogicalOperatorType::LOGICAL_GET:
	case LogicalOperatorType::LOGICAL_UNNEST:
	case LogicalOperatorType::LOGICAL_DELIM_GET:
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
	case LogicalOperatorType::LOGICAL_WINDOW:
	case LogicalOperatorType::LOGICAL_SAMPLE:
		return true;
	default:
		return false;
	}
}

static bool OperatorIsNonReorderable(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_FILTER:
		// Preserve the input rows on which volatile predicates are evaluated.
		for (auto &expression : op.expressions) {
			if (expression->IsVolatile()) {
				return true;
			}
		}
		return false;
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	// DML operators have side effects and must never be reordered away or passed through (a correlated
	// trigger body can place an INSERT/UPDATE/DELETE mid-plan, as a cross-product child for example).
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
		return true;
	default:
		return false;
	}
}

bool ExpressionContainsColumnRef(const Expression &root_expr) {
	bool contains_column_ref = false;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(root_expr,
	                                                              [&](const BoundColumnRefExpression &colref) {
	// Here you have a filter on a single column in a table. Return a binding for the column
	// being filtered on so the filter estimator knows what HLL count to pull
#ifdef DEBUG
		                                                              (void)colref.Depth();
		                                                              D_ASSERT(colref.Depth() == 0);
		                                                              D_ASSERT(colref.Binding().table_index.IsValid());
#endif
		                                                              // map the base table index to the relation index
		                                                              // used by the JoinOrderOptimizer
		                                                              contains_column_ref = true;
	                                                              });
	return contains_column_ref;
}

static void PinFilterAfterLeftJoin(FilterInfo &filter, JoinRelationSet &nullable_set, JoinRelationSet &left_join_set,
                                   JoinRelationSetManager &set_manager) {
	if (JoinRelationSet::Intersects(filter.set.get(), nullable_set)) {
		filter.set = set_manager.Union(filter.set.get(), left_join_set);
		filter.must_remain_filter |= filter.from_logical_filter;
	}
	if (filter.left_set && JoinRelationSet::Intersects(*filter.left_set, nullable_set)) {
		filter.left_set = &set_manager.Union(*filter.left_set, left_join_set);
	}
	if (filter.right_set && JoinRelationSet::Intersects(*filter.right_set, nullable_set)) {
		filter.right_set = &set_manager.Union(*filter.right_set, left_join_set);
	}
}

static bool ContainsRecurringCTERef(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF && op.Cast<LogicalCTERef>().is_recurring) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsRecurringCTERef(*child)) {
			return true;
		}
	}
	return false;
}

static bool JoinIsReorderable(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
		return true;
	}

	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.HasProjectionMap()) {
			return false;
		}
		if ((join.join_type == JoinType::LEFT || join.join_type == JoinType::SEMI ||
		     join.join_type == JoinType::ANTI) &&
		    ContainsRecurringCTERef(*join.children[1])) {
			return false;
		}
		switch (join.join_type) {
		case JoinType::INNER:
		case JoinType::SEMI:
		case JoinType::LEFT:
		case JoinType::ANTI:
			for (auto &cond : join.conditions) {
				if (cond.IsComparison() && ExpressionContainsColumnRef(cond.GetLHS()) &&
				    ExpressionContainsColumnRef(cond.GetRHS())) {
					return true;
				}
			}
			return false;
		default:
			return false;
		}
	}
	return false;
}

static bool IsProjectedRecursiveCTERef(const LogicalOperator &op, const JoinOrderOptimizer &optimizer) {
	optional_ptr<const LogicalOperator> source = op;
	while (source->children.size() == 1) {
		if (OperatorIsNonReorderable(*source) ||
		    (OperatorNeedsRelation(source->type) && source->type != LogicalOperatorType::LOGICAL_PROJECTION)) {
			return false;
		}
		source = source->children[0].get();
	}
	if (source->type != LogicalOperatorType::LOGICAL_CTE_REF) {
		return false;
	}
	auto &cte_ref = source->Cast<LogicalCTERef>();
	return optimizer.recursive_cte_indexes.find(cte_ref.cte_index) != optimizer.recursive_cte_indexes.end();
}

static bool RecursiveCTERefCanReorder(optional_ptr<LogicalOperator> parent) {
	if (!parent || parent->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return false;
	}
	auto &join = parent->Cast<LogicalComparisonJoin>();
	idx_t range_count = 0;
	return join.HasEquality(range_count);
}

static optional<RelationStats> CombineNonReorderableStats(LogicalOperator &op, const vector<RelationStats> &child_stats,
                                                          ClientContext &context) {
	if (child_stats.size() != op.children.size()) {
		return {};
	}
	vector<reference<const RelationStats>> child_references;
	for (idx_t child_idx = 0; child_idx < child_stats.size(); child_idx++) {
		if (!child_stats[child_idx].stats_initialized ||
		    !child_stats[child_idx].MatchesBindings(op.children[child_idx]->GetColumnBindings())) {
			return {};
		}
		child_references.push_back(child_stats[child_idx]);
	}
	auto derived = RelationStatisticsHelper::ExtractOperatorStats(op, context, child_references);
	if (derived) {
		return derived;
	}

	RelationStats result;
	result.cardinality = 0;
	result.stats_initialized = true;
	for (auto &stats : child_stats) {
		result.cardinality = MaxValue(result.cardinality, stats.cardinality);
	}
	if (op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN && child_stats.size() == 2) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		result.cardinality = join.join_type == JoinType::RIGHT || join.join_type == JoinType::OUTER
		                         ? child_stats[1].cardinality
		                         : child_stats[0].cardinality;
	}

	auto bindings = op.GetColumnBindings();
	RelationColumnStatsLookup lookup;
	for (auto &stats : child_stats) {
		lookup.AddRelation(stats);
	}
	for (idx_t column_idx = 0; column_idx < bindings.size(); column_idx++) {
		auto source = lookup.Find(bindings[column_idx]);
		if (!source) {
			return {};
		}
		result.columns.emplace_back(bindings[column_idx], source->total_domain, source->current_domain, source->name);
	}
	result.CapCurrentDomainsToCardinality();
	result.Verify(bindings);
	return result;
}

bool RelationManager::AddRelationWithChildren(JoinOrderOptimizer &optimizer, LogicalOperator &op,
                                              LogicalOperator &input_op, optional_ptr<LogicalOperator> parent,
                                              RelationStats &child_stats) {
	D_ASSERT(!op.children.empty());
	auto child_optimizer = optimizer.CreateChildOptimizer();
	op.children[0] = child_optimizer.Optimize(std::move(op.children[0]), &child_stats);
	vector<reference<const RelationStats>> children_stats {child_stats};
	auto operator_stats = RelationStatisticsHelper::ExtractOperatorStats(op, context, children_stats);
	if (!operator_stats) {
		stats_complete = false;
		return false;
	}
	return AddRelation(input_op, parent, *operator_stats);
}

bool RelationManager::ExtractJoinRelations(JoinOrderOptimizer &optimizer, LogicalOperator &input_op,
                                           vector<reference<LogicalOperator>> &filter_operators,
                                           optional_ptr<LogicalOperator> parent) {
	optional_ptr<LogicalOperator> op = &input_op;
	// pass through single child operators (but never past a non-reorderable op such as a DML node, which
	// must be preserved as its own relation rather than skipped over and dropped during reconstruction)
	auto filter_chain_start = filter_operators.size();
	while (op->children.size() == 1 && !OperatorNeedsRelation(op->type) && !OperatorIsNonReorderable(*op)) {
		if (op->type == LogicalOperatorType::LOGICAL_FILTER) {
			filter_operators.push_back(*op);
		}
		op = op->children[0].get();
	}
	// stacked filters are re-applied in this order: keep the lower (earlier evaluated) filters first
	std::reverse(filter_operators.begin() + NumericCast<int64_t>(filter_chain_start), filter_operators.end());
	bool non_reorderable_operation = false;
	if (OperatorIsNonReorderable(*op)) {
		// set operation, optimize separately in children
		non_reorderable_operation = true;
	}

	if (op->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		if (JoinIsReorderable(*op)) {
			// extract join conditions from inner join
			filter_operators.push_back(*op);
		} else {
			non_reorderable_operation = true;
		}
	}
	if (non_reorderable_operation) {
		// we encountered a non-reordable operation (setop or non-inner join)
		// we do not reorder non-inner joins yet, however we do want to expand the potential join graph around them
		// non-inner joins are also tricky because we can't freely make conditions through them
		// e.g. suppose we have (left LEFT OUTER JOIN right WHERE right IS NOT NULL), the join can generate
		// new NULL values in the right side, so pushing this condition through the join leads to incorrect results
		// for this reason, we just start a new JoinOptimizer pass in each of the children of the join
		// stats.cardinality will be initiated to highest cardinality of the children.
		vector<RelationStats> children_stats;
		for (auto &child : op->children) {
			auto stats = RelationStats();
			auto child_optimizer = optimizer.CreateChildOptimizer();
			child = child_optimizer.Optimize(std::move(child), &stats);
			children_stats.push_back(stats);
		}

		auto combined_stats = CombineNonReorderableStats(*op, children_stats, context);
		if (!combined_stats) {
			stats_complete = false;
			return false;
		}
		op->SetEstimatedCardinality(combined_stats->cardinality);
		return AddRelation(input_op, parent, *combined_stats);
	}

	switch (op->type) {
	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY: {
		// optimize children
		RelationStats child_stats;
		auto child_optimizer = optimizer.CreateChildOptimizer();
		op->children[0] = child_optimizer.Optimize(std::move(op->children[0]), &child_stats);
		auto &aggr = op->Cast<LogicalAggregate>();
		auto operator_stats = RelationStatisticsHelper::ExtractAggregationStats(aggr, child_stats, context);
		if (!operator_stats) {
			stats_complete = false;
			return false;
		}
		// the extracted cardinality should be set for aggregate
		aggr.SetEstimatedCardinality(operator_stats->cardinality);
		return AddRelation(input_op, parent, *operator_stats);
	}
	case LogicalOperatorType::LOGICAL_WINDOW: {
		// optimize children
		RelationStats child_stats;
		auto child_optimizer = optimizer.CreateChildOptimizer();
		op->children[0] = child_optimizer.Optimize(std::move(op->children[0]), &child_stats);
		auto &window = op->Cast<LogicalWindow>();
		auto operator_stats = RelationStatisticsHelper::ExtractWindowStats(window, child_stats);
		if (!operator_stats) {
			stats_complete = false;
			return false;
		}
		// the extracted cardinality should be set for window
		window.SetEstimatedCardinality(operator_stats->cardinality);
		return AddRelation(input_op, parent, *operator_stats);
	}
	case LogicalOperatorType::LOGICAL_UNNEST: {
		// optimize children of unnest
		RelationStats child_stats;
		return AddRelationWithChildren(optimizer, *op, input_op, parent, child_stats);
	}
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &join = op->Cast<LogicalComparisonJoin>();
		// Adding relations of the left side to the current join order optimizer
		bool can_reorder_left = ExtractJoinRelations(optimizer, *op->children[0], filter_operators, op);
		bool can_reorder_right = true;
		// For semi & anti joins, you only reorder relations in the left side of the join.
		// We do not want to reorder a relation A into the right side because then all column bindings A from A will be
		// lost after the semi or anti join

		// We cannot reorder a relation B out of the right side because any filter/join in the right side
		// between a relation B and another RHS relation will be invalid. The semi join will remove
		// all right column bindings,

		// So we treat the right side of left join as its own relation so no relations
		// are pushed into the right side, or taken out of the right side.
		if (join.join_type == JoinType::SEMI || join.join_type == JoinType::ANTI || join.join_type == JoinType::LEFT) {
			RelationStats child_stats;
			// optimize the child and copy the stats
			auto child_optimizer = optimizer.CreateChildOptimizer();
			op->children[1] = child_optimizer.Optimize(std::move(op->children[1]), &child_stats);
			if (!AddRelation(*op->children[1], op, child_stats)) {
				return false;
			}
			auto right_child_bindings = op->children[1]->GetColumnBindings();
			for (auto &bindings : right_child_bindings) {
				relation_mapping[bindings.table_index] = RelationIndex(relations.size() - 1);
			}
		} else {
			can_reorder_right = ExtractJoinRelations(optimizer, *op->children[1], filter_operators, op);
		}
		return can_reorder_left && can_reorder_right;
	}
	case LogicalOperatorType::LOGICAL_CROSS_PRODUCT: {
		bool can_reorder_left = ExtractJoinRelations(optimizer, *op->children[0], filter_operators, op);
		bool can_reorder_right = ExtractJoinRelations(optimizer, *op->children[1], filter_operators, op);
		return can_reorder_left && can_reorder_right;
	}
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN: {
		auto &dummy_scan = op->Cast<LogicalDummyScan>();
		auto stats = RelationStatisticsHelper::ExtractDummyScanStats(dummy_scan, context);
		return AddRelation(input_op, parent, stats);
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		// base table scan, add to set of relations.
		// create empty stats for dummy scan or logical expression get
		auto &expression_get = op->Cast<LogicalExpressionGet>();
		auto stats = RelationStatisticsHelper::ExtractExpressionGetStats(expression_get, context);
		return AddRelation(input_op, parent, stats);
	}
	case LogicalOperatorType::LOGICAL_CHUNK_GET: {
		auto stats = RelationStatisticsHelper::ExtractColumnDataGetStats(op->Cast<LogicalColumnDataGet>(), context);
		return AddRelation(input_op, parent, stats);
	}
	case LogicalOperatorType::LOGICAL_GET: {
		// TODO: Get stats from a logical GET
		auto &get = op->Cast<LogicalGet>();
		// this is a get that *most likely* has a function (like unnest or json_each).
		// there are new bindings for output of the function, but child bindings also exist, and can
		// be used in joins
		if (!op->children.empty()) {
			RelationStats child_stats;
			return AddRelationWithChildren(optimizer, *op, input_op, parent, child_stats);
		}
		auto stats = RelationStatisticsHelper::ExtractGetStats(get, context);
		// if there is another logical filter that could not be pushed down into the
		// table scan, apply another selectivity.
		get.SetEstimatedCardinality(stats.cardinality);
		return AddRelation(input_op, parent, stats);
	}
	case LogicalOperatorType::LOGICAL_PROJECTION: {
		RelationStats child_stats;
		// optimize the child and copy the stats
		auto child_optimizer = optimizer.CreateChildOptimizer();
		op->children[0] = child_optimizer.Optimize(std::move(op->children[0]), &child_stats);
		auto &proj = op->Cast<LogicalProjection>();
		// Projection can create columns so we need to add them here
		auto proj_stats = RelationStatisticsHelper::ExtractProjectionStats(proj, child_stats);
		if (!proj_stats) {
			stats_complete = false;
			return false;
		}
		proj.SetEstimatedCardinality(proj_stats->cardinality);
		if (!AddRelation(input_op, parent, *proj_stats)) {
			return false;
		}
		// A projection must preserve the recursive reference's join-order restriction.
		return !IsProjectedRecursiveCTERef(proj, optimizer) || RecursiveCTERefCanReorder(parent);
	}
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT: {
		// optimize the child and copy the stats
		auto &empty_result = op->Cast<LogicalEmptyResult>();
		// Projection can create columns so we need to add them here
		auto stats = RelationStatisticsHelper::ExtractEmptyResultStats(empty_result);
		empty_result.SetEstimatedCardinality(stats.cardinality);
		return AddRelation(input_op, parent, stats);
	}
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		RelationStats lhs_stats;
		// optimize the lhs child and copy the stats
		auto lhs_optimizer = optimizer.CreateChildOptimizer();
		op->children[0] = lhs_optimizer.Optimize(std::move(op->children[0]), &lhs_stats);
		// optimize the rhs child
		auto rhs_optimizer = optimizer.CreateChildOptimizer();
		auto table_index = op->Cast<LogicalCTE>().table_index;

		auto child_1_card = lhs_stats.stats_initialized ? lhs_stats.cardinality : 0;
		rhs_optimizer.AddMaterializedCTEStats(table_index, std::move(lhs_stats));
		if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
			rhs_optimizer.recursive_cte_indexes.insert(op->Cast<LogicalCTE>().table_index);
		}
		RelationStats rhs_stats;
		op->children[1] = rhs_optimizer.Optimize(std::move(op->children[1]), &rhs_stats);

		// create the stats for the CTE
		auto child_2_card = rhs_stats.stats_initialized ? rhs_stats.cardinality : 0;
		idx_t cte_cardinality;

		if (op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
			// we cannot really estimate the cardinality of a recursive CTE
			// because we don't know how many times it will be executed
			// we just assume it will be executed 1000 times
			cte_cardinality = child_1_card + child_2_card * 1000;
		} else if (op->type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE) {
			// for a materialized CTE, we just take the cardinality of the right children
			cte_cardinality = child_2_card;
		} else {
			throw InternalException("Unexpected CTE type in join order optimizer");
		}
		op->SetEstimatedCardinality(cte_cardinality);
		auto output_stats = RelationStatisticsHelper::ProjectOutputStats(rhs_stats, input_op);
		if (!output_stats) {
			output_stats = RelationStatisticsHelper::RebindOutputStats(rhs_stats, input_op);
		}
		if (output_stats && op->type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
			output_stats->cardinality = cte_cardinality;
			output_stats->row_retention = 1;
			// Statistics from one recursive term do not describe the complete fixpoint. Keep the output layout, but
			// represent each fixpoint domain using the standard cardinality-based fallback.
			for (auto &column : output_stats->columns) {
				column.total_domain = DistinctCount(cte_cardinality, DistinctCountSource::CARDINALITY);
				column.current_domain = column.total_domain;
				column.semi_anti_join_domain_evidence.Invalidate();
			}
		}
		if (!output_stats || !AddRelation(input_op, parent, *output_stats)) {
			stats_complete = false;
		}

		return false;
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		auto &cte_ref = op->Cast<LogicalCTERef>();
		auto cte_stats = optimizer.GetMaterializedCTEStats(cte_ref.cte_index);
		auto rebound_stats = RelationStatisticsHelper::RebindOutputStats(cte_stats, cte_ref);
		if (!rebound_stats) {
			stats_complete = false;
			return false;
		}
		cte_ref.SetEstimatedCardinality(cte_stats.cardinality);
		if (!AddRelation(input_op, parent, *rebound_stats)) {
			return false;
		}

		auto is_recursive = optimizer.recursive_cte_indexes.find(cte_ref.cte_index);
		if (is_recursive != optimizer.recursive_cte_indexes.end()) {
			// Only let recursive CTE references participate in join reordering when they sit directly
			// under an equality comparison join. This still enables the reusable recursive hash-join
			// case, while avoiding broader reorderings that let residual/range predicates drive the
			// reconstructed join tree into expensive range-join plans.
			return RecursiveCTERefCanReorder(parent);
		}
		return true;
	}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		auto &delim_join = op->Cast<LogicalComparisonJoin>();

		// optimize LHS (duplicate-eliminated) child
		RelationStats lhs_stats;
		auto lhs_optimizer = optimizer.CreateChildOptimizer();
		op->children[0] = lhs_optimizer.Optimize(std::move(op->children[0]), &lhs_stats);

		// create dummy aggregation for the duplicate elimination
		auto dummy_aggr = make_uniq<LogicalAggregate>(TableIndex(DConstants::INVALID_INDEX - 1), TableIndex(),
		                                              vector<unique_ptr<Expression>>());
		dummy_aggr->grouping_sets.emplace_back();
		for (auto &delim_col : delim_join.duplicate_eliminated_columns) {
			dummy_aggr->grouping_sets.back().insert(ProjectionIndex(dummy_aggr->groups.size()));
			dummy_aggr->groups.push_back(delim_col->Copy());
		}
		auto lhs_delim_stats = RelationStatisticsHelper::ExtractAggregationStats(*dummy_aggr, lhs_stats, context);
		if (!lhs_delim_stats) {
			stats_complete = false;
			return false;
		}

		// optimize the other child, which will now have access to the stats
		RelationStats rhs_stats;
		auto rhs_optimizer = optimizer.CreateChildOptimizer();
		rhs_optimizer.AddDelimScanStats(*lhs_delim_stats);
		op->children[1] = rhs_optimizer.Optimize(std::move(op->children[1]), rhs_stats);

		RelationStats dj_stats;
		switch (delim_join.join_type) {
		case JoinType::LEFT:
		case JoinType::INNER:
		case JoinType::OUTER:
		case JoinType::SINGLE:
		case JoinType::MARK:
		case JoinType::SEMI:
		case JoinType::ANTI:
			dj_stats = lhs_stats;
			break;
		case JoinType::RIGHT:
		case JoinType::RIGHT_SEMI:
		case JoinType::RIGHT_ANTI:
			dj_stats = rhs_stats;
			break;
		default:
			throw NotImplementedException("Unsupported join type");
		}

		if (delim_join.join_type == JoinType::SEMI || delim_join.join_type == JoinType::ANTI ||
		    delim_join.join_type == JoinType::RIGHT_SEMI || delim_join.join_type == JoinType::RIGHT_ANTI) {
			auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(delim_join, lhs_stats, rhs_stats);
			auto cardinality = LossyNumericCast<idx_t>(estimate.cardinality);
			RelationStatisticsHelper::ApplyCardinality(dj_stats, cardinality);
		} else {
			for (auto &column : dj_stats.columns) {
				column.semi_anti_join_domain_evidence.Invalidate();
			}
		}

		if (!AddRelation(input_op, parent, dj_stats)) {
			return false;
		}

		return false;
	}
	case LogicalOperatorType::LOGICAL_DELIM_GET: {
		// Used to not be possible to reorder these. We added reordering (without stats) before,
		// but ran into terrible join orders (see internal issue #596), so we removed it again
		// We now have proper statistics for DelimGets, and get an even better query plan for #596
		auto delim_scan_stats = optimizer.GetDelimScanStats();
		auto rebound_stats = RelationStatisticsHelper::RebindOutputStats(delim_scan_stats, *op);
		if (!rebound_stats) {
			stats_complete = false;
			return false;
		}
		op->SetEstimatedCardinality(delim_scan_stats.cardinality);
		return AddRelation(input_op, parent, *rebound_stats);
	}
	default:
		return false;
	}
}

void RelationManager::GetColumnBindingsFromExpression(const Expression &expression,
                                                      column_binding_set_t &column_bindings) {
	if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		// Here you have a filter on a single column in a table. Return a binding for the column
		// being filtered on so the filter estimator knows what HLL count to pull
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		D_ASSERT(colref.Depth() == 0);
		D_ASSERT(colref.Binding().table_index.IsValid());
		ColumnBinding normalized;
		if (!TryNormalizeBinding(colref.Binding(), normalized)) {
			return;
		}
		column_bindings.insert(normalized);
	}

	// TODO: handle inequality filters with functions.
	ExpressionIterator::EnumerateChildren(
	    expression, [&](const Expression &expr) { GetColumnBindingsFromExpression(expr, column_bindings); });
}

void RelationManager::GetColumnBindingsFromOperator(LogicalOperator &op, column_binding_set_t &column_bindings) {
	for (auto &binding : op.GetColumnBindings()) {
		ColumnBinding normalized;
		if (!TryNormalizeBinding(binding, normalized)) {
			continue;
		}
		column_bindings.insert(normalized);
	}
}

static bool BindingsAreSubset(const column_binding_set_t &bindings, const column_binding_set_t &super) {
	if (bindings.empty()) {
		return false;
	}
	for (auto &binding : bindings) {
		if (super.find(binding) == super.end()) {
			return false;
		}
	}
	return true;
}

optional_ptr<JoinRelationSet> RelationManager::GetJoinRelations(column_binding_set_t &column_bindings,
                                                                JoinRelationSetManager &set_manager) {
	optional_ptr<JoinRelationSet> ret = set_manager.GetEmptyJoinRelationSet();
	for (auto &binding : column_bindings) {
		// binding.table_index stores a RelationIndex wrapped in a TableIndex
		optional_ptr<JoinRelationSet> binding_set =
		    set_manager.GetJoinRelation(RelationIndex(binding.table_index.index));
		ret = set_manager.Union(*ret, *binding_set);
	}
	return *ret;
}

bool RelationManager::ExtractBindings(const Expression &expression, unordered_set<RelationIndex> &bindings) {
	if (expression.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		D_ASSERT(colref.Depth() == 0);
		D_ASSERT(colref.Binding().table_index.IsValid());
		// map the base table index to the relation index used by the JoinOrderOptimizer
		if (expression.GetAlias() == "SUBQUERY" &&
		    relation_mapping.find(colref.Binding().table_index) == relation_mapping.end()) {
			// most likely a BoundSubqueryExpression that was created from an uncorrelated subquery
			// Here we return true and don't fill the bindings, the expression can be reordered.
			// A filter will be created using this expression, and pushed back on top of the parent
			// operator during plan reconstruction
			return true;
		}
		if (relation_mapping.find(colref.Binding().table_index) != relation_mapping.end()) {
			bindings.insert(relation_mapping[colref.Binding().table_index]);
		}
	}
	if (expression.GetExpressionType() == ExpressionType::BOUND_REF) {
		// bound expression, clear bindings as we can't use this
		bindings.clear();
		return false;
	}
	D_ASSERT(expression.GetExpressionType() != ExpressionType::SUBQUERY);
	bool can_reorder = true;
	ExpressionIterator::EnumerateChildren(expression, [&](const Expression &expr) {
		if (!ExtractBindings(expr, bindings)) {
			can_reorder = false;
			return;
		}
	});
	return can_reorder;
}

bool RelationManager::ExtractFilterBindings(const Expression &expression, unordered_set<RelationIndex> &bindings) {
	const auto can_reorder = ExtractBindings(expression, bindings);
	if (!bindings.empty()) {
		return true;
	}
	// without bindings the filter can only move if it references no columns at all
	return can_reorder && !ExpressionContainsColumnRef(expression);
}

JoinOrderExtraction RelationManager::ExtractEdges(LogicalOperator &op,
                                                  vector<reference<LogicalOperator>> &filter_operators,
                                                  JoinRelationSetManager &set_manager) {
	// now that we know we are going to perform join ordering we actually extract the filters, eliminating duplicate
	// filters in the process
	JoinOrderExtraction result;
	auto &filters_and_bindings = result.filters;
	auto &join_operators = result.join_operators;
	reference_map_t<LogicalOperator, vector<idx_t>> operator_predicates;
	reference_map_t<LogicalOperator, vector<JoinCondition>> operator_conditions;
	expression_set_t filter_set;
	auto normalize_conditions = [&](LogicalComparisonJoin &join, const column_binding_set_t &semantic_left_bindings,
	                                const column_binding_set_t &semantic_right_bindings) {
		vector<JoinCondition> result;
		for (auto &condition : join.conditions) {
			if (!condition.IsComparison()) {
				result.push_back(condition.Copy());
				continue;
			}
			column_binding_set_t left_bindings;
			column_binding_set_t right_bindings;
			GetColumnBindingsFromExpression(condition.GetLHS(), left_bindings);
			GetColumnBindingsFromExpression(condition.GetRHS(), right_bindings);
			auto lhs_is_left = BindingsAreSubset(left_bindings, semantic_left_bindings);
			auto lhs_is_right = BindingsAreSubset(left_bindings, semantic_right_bindings);
			auto rhs_is_left = BindingsAreSubset(right_bindings, semantic_left_bindings);
			auto rhs_is_right = BindingsAreSubset(right_bindings, semantic_right_bindings);
			if ((!lhs_is_left || !rhs_is_right) && (!lhs_is_right || !rhs_is_left)) {
				result.emplace_back(JoinCondition::CreateExpression(condition.Copy()));
				continue;
			}
			auto copy = condition.Copy();
			if (lhs_is_right) {
				copy.Swap();
			}
			result.push_back(std::move(copy));
		}
		return result;
	};
	auto get_semantic_sets = [&](const vector<JoinCondition> &conditions,
	                             const column_binding_set_t &semantic_left_bindings,
	                             const column_binding_set_t &semantic_right_bindings) {
		column_binding_set_t left_bindings;
		column_binding_set_t right_bindings;
		for (auto &condition : conditions) {
			column_binding_set_t condition_bindings;
			if (condition.IsComparison()) {
				GetColumnBindingsFromExpression(condition.GetLHS(), condition_bindings);
				GetColumnBindingsFromExpression(condition.GetRHS(), condition_bindings);
			} else {
				GetColumnBindingsFromExpression(condition.GetJoinExpression(), condition_bindings);
			}
			for (auto &binding : condition_bindings) {
				if (semantic_right_bindings.count(binding)) {
					right_bindings.insert(binding);
				} else if (semantic_left_bindings.count(binding)) {
					left_bindings.insert(binding);
				} else {
					throw InternalException("Non-inner join condition references a binding outside both inputs");
				}
			}
		}
		return make_pair(GetJoinRelations(left_bindings, set_manager), GetJoinRelations(right_bindings, set_manager));
	};
	for (auto &filter_op : filter_operators) {
		auto &f_op = filter_op.get();
		if (f_op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
		    f_op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			auto &join = f_op.Cast<LogicalComparisonJoin>();
			D_ASSERT(join.expressions.empty());
			switch (join.join_type) {
			case JoinType::SEMI:
			case JoinType::ANTI: {
				column_binding_set_t semantic_left_bindings;
				column_binding_set_t semantic_right_bindings;
				GetColumnBindingsFromOperator(*join.children[0], semantic_left_bindings);
				GetColumnBindingsFromOperator(*join.children[1], semantic_right_bindings);
				auto conditions = normalize_conditions(join, semantic_left_bindings, semantic_right_bindings);
				auto semantic_sets = get_semantic_sets(conditions, semantic_left_bindings, semantic_right_bindings);
				auto left_set = semantic_sets.first;
				auto right_set = semantic_sets.second;
				D_ASSERT(left_set && left_set->count > 0);
				D_ASSERT(right_set && right_set->count == 1);
				auto &full_set = set_manager.Union(*left_set, *right_set);
				// Keep each condition as separate costing evidence, while every predicate retains the complete semantic
				// left/right sets so the operator cannot be applied before all required inputs are available.
				for (auto &cond : conditions) {
					unique_ptr<Expression> expression;
					bool is_residual = false;
					if (cond.IsComparison()) {
						expression = BoundComparisonExpression::Create(cond.GetComparisonType(), cond.GetLHS().Copy(),
						                                               cond.GetRHS().Copy());
					} else {
						expression = cond.GetJoinExpression().Copy();
						is_residual = true;
					}
					auto filter_index = filters_and_bindings.size();
					auto filter_info =
					    make_uniq<FilterInfo>(std::move(expression), full_set, filter_index, join.join_type);
					filter_info->SetLeftSet(left_set);
					filter_info->SetRightSet(right_set);
					filter_info->from_residual_predicate = is_residual;
					filters_and_bindings.push_back(std::move(filter_info));
					operator_predicates[f_op].push_back(filter_index);
				}
				D_ASSERT(!operator_predicates[f_op].empty());
				operator_conditions[f_op] = std::move(conditions);
				break;
			}
			case JoinType::LEFT: {
				auto filter_count_before_left_join = filters_and_bindings.size();
				// For LEFT joins, create one FilterInfo per comparison condition.
				// Each uses the full left/right relation sets from the comparison conditions
				// to preserve join-ordering constraints, but individual column bindings so that
				// the cardinality estimator gets an accurate distinct count for each condition.
				// With all per-condition distinct counts available, GetDenominator can multiply
				// them together for a correct multi-condition LEFT join cardinality estimate.

				// Predicate expression sides are not reliable here: SQL can write a LEFT join condition as
				// rhs_col = lhs_col. Normalize comparison sides so left_set is the preserved side and right_set
				// is the nullable RHS side, without expanding left_set to every relation in the actual left child.
				column_binding_set_t semantic_left_bindings;
				column_binding_set_t semantic_right_bindings;
				GetColumnBindingsFromOperator(*join.children[0], semantic_left_bindings);
				GetColumnBindingsFromOperator(*join.children[1], semantic_right_bindings);
				auto conditions = normalize_conditions(join, semantic_left_bindings, semantic_right_bindings);
				auto semantic_sets = get_semantic_sets(conditions, semantic_left_bindings, semantic_right_bindings);
				auto full_left_set = semantic_sets.first;
				auto full_right_set = semantic_sets.second;
				D_ASSERT(full_left_set && full_left_set->count > 0);
				D_ASSERT(full_right_set && full_right_set->count == 1);
				auto &full_set = set_manager.Union(*full_left_set, *full_right_set);

				for (auto &cond : conditions) {
					if (!cond.IsComparison()) {
						continue;
					}
					auto comparison = BoundComparisonExpression::Create(cond.GetComparisonType(), cond.GetLHS().Copy(),
					                                                    cond.GetRHS().Copy());

					auto filter_index = filters_and_bindings.size();
					auto filter_info =
					    make_uniq<FilterInfo>(std::move(comparison), full_set, filter_index, join.join_type);
					filter_info->SetLeftSet(full_left_set);
					filter_info->SetRightSet(full_right_set);
					filters_and_bindings.push_back(std::move(filter_info));
					operator_predicates[f_op].push_back(filter_index);
				}

				// Filters above a LEFT join that reference nullable-side bindings must be evaluated
				// after the LEFT join has introduced NULL-extended rows. Pin them to the full LEFT
				// join output, otherwise reconstruction can push null-aware predicates like
				// "lhs IS DISTINCT FROM rhs" below the LEFT join and turn them into inner filters.
				for (idx_t filter_idx = 0; filter_idx < filter_count_before_left_join; filter_idx++) {
					PinFilterAfterLeftJoin(*filters_and_bindings[filter_idx], *full_right_set, full_set, set_manager);
				}
				D_ASSERT(!operator_predicates[f_op].empty());
				operator_conditions[f_op] = std::move(conditions);
				break;
			}
			default: {
				for (auto &cond : join.conditions) {
					operator_conditions[f_op].push_back(cond.Copy());
				}
				// can extract every inner join condition individually.
				for (auto &cond : join.conditions) {
					unique_ptr<Expression> expr;
					bool is_residual = false;

					if (cond.IsComparison()) {
						auto comp_type = cond.GetComparisonType();
						expr = BoundComparisonExpression::Create(comp_type, cond.GetLHS().Copy(), cond.GetRHS().Copy());
					} else {
						expr = cond.GetJoinExpression().Copy();
						is_residual = true;
					}

					if (filter_set.find(*expr) == filter_set.end()) {
						filter_set.insert(*expr);
						unordered_set<RelationIndex> bindings;
						ExtractBindings(*expr, bindings);
						auto &set = set_manager.GetJoinRelation(bindings);
						auto filter_info =
						    make_uniq<FilterInfo>(std::move(expr), set, filters_and_bindings.size(), join.join_type);
						filter_info->from_residual_predicate = is_residual;
						filters_and_bindings.push_back(std::move(filter_info));
						operator_predicates[f_op].push_back(filters_and_bindings.size() - 1);
					}
				}
				break;
			}
			}

		} else {
			// handle filters from logical filters
			for (auto &expression : f_op.expressions) {
				unordered_set<RelationIndex> bindings;
				if (!ExtractFilterBindings(*expression, bindings)) {
					// the filter is on a column that is not in our relational map. (example: limit_rownum)
					// in this case we do not create a FilterInfo for it. (duckdb-internal/#1493)
					continue;
				}
				if (filter_set.find(*expression) == filter_set.end()) {
					filter_set.insert(*expression);
					auto &set = set_manager.GetJoinRelation(bindings);
					auto filter_info = make_uniq<FilterInfo>(expression->Copy(), set, filters_and_bindings.size());
					filter_info->from_logical_filter = true;
					if (ExpressionBarrier::Contains(*expression)) {
						// the barrier must not end up below one of the joins we are reordering
						filter_info->must_remain_filter = true;
						filter_info->must_remain_at_root = true;
					}
					filters_and_bindings.push_back(std::move(filter_info));
				}
			}
		}
	}

	struct ExtractedOperatorSubtree {
		reference<JoinRelationSet> relations;
		vector<reference<JoinOrderOperator>> operators;
	};
	std::function<ExtractedOperatorSubtree(LogicalOperator &)> extract_operator_tree;
	extract_operator_tree = [&](LogicalOperator &current) -> ExtractedOperatorSubtree {
		auto relation_entry = operator_relations.find(current);
		if (relation_entry != operator_relations.end()) {
			return {set_manager.GetJoinRelation(relation_entry->second), {}};
		}
		if (current.children.size() == 1) {
			return extract_operator_tree(*current.children[0]);
		}
		if (current.children.size() != 2 || !JoinIsReorderable(current)) {
			throw InternalException("Could not map operator %s into the extracted join-order tree",
			                        EnumUtil::ToChars(current.type));
		}

		auto left = extract_operator_tree(*current.children[0]);
		auto right = extract_operator_tree(*current.children[1]);
		JoinOrderOperatorType operator_type;
		if (current.type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
			operator_type = JoinOrderOperatorType::CROSS_PRODUCT;
		} else {
			auto &join = current.Cast<LogicalComparisonJoin>();
			switch (join.join_type) {
			case JoinType::INNER:
				operator_type = JoinOrderOperatorType::INNER;
				break;
			case JoinType::LEFT:
				operator_type = JoinOrderOperatorType::LEFT;
				break;
			case JoinType::SEMI:
				operator_type = JoinOrderOperatorType::SEMI;
				break;
			case JoinType::ANTI:
				operator_type = JoinOrderOperatorType::ANTI;
				break;
			default:
				throw InternalException("Unexpected reorderable join type %s", EnumUtil::ToChars(join.join_type));
			}
		}

		auto predicate_entry = operator_predicates.find(current);
		auto conditions_entry = operator_conditions.find(current);
		vector<JoinCondition> conditions;
		if (conditions_entry != operator_conditions.end()) {
			conditions = std::move(conditions_entry->second);
		}
		unordered_set<RelationIndex> syntactic_relations;
		for (auto &condition : conditions) {
			if (condition.IsComparison()) {
				ExtractBindings(condition.GetLHS(), syntactic_relations);
				ExtractBindings(condition.GetRHS(), syntactic_relations);
			} else {
				ExtractBindings(condition.GetJoinExpression(), syntactic_relations);
			}
		}
		auto &syntactic_set = set_manager.GetJoinRelation(syntactic_relations);
		vector<idx_t> predicate_indices;
		if (predicate_entry != operator_predicates.end()) {
			predicate_indices = predicate_entry->second;
		}
		auto descriptor = make_uniq<JoinOrderOperator>(join_operators.size(), operator_type, left.relations,
		                                               right.relations, syntactic_set, std::move(conditions));
		descriptor->left_operators = left.operators;
		descriptor->right_operators = right.operators;
		descriptor->costing_predicate_indices = std::move(predicate_indices);
		for (auto predicate_index : descriptor->costing_predicate_indices) {
			filters_and_bindings[predicate_index]->source_operator_index = optional_idx(descriptor->index);
		}
		auto &current_operator = *descriptor;
		join_operators.push_back(std::move(descriptor));

		auto subtree_operators = std::move(left.operators);
		subtree_operators.insert(subtree_operators.end(), right.operators.begin(), right.operators.end());
		subtree_operators.push_back(current_operator);
		return {set_manager.Union(left.relations, right.relations), std::move(subtree_operators)};
	};
	extract_operator_tree(op);
	return result;
}

// LCOV_EXCL_START

void RelationManager::PrintRelationStats() {
#ifdef DEBUG
	string to_print;
	for (idx_t i = 0; i < relations.size(); i++) {
		auto &relation = relations.at(i);
		auto &stats = relation->stats;
		for (auto &column : stats.columns) {
			to_print = column.name + " has estimated current/total domains " +
			           to_string(column.current_domain.distinct_count) + "/" +
			           to_string(column.total_domain.distinct_count);
			Printer::Print(to_print);
		}
		to_print = stats.table_name + " has estimated cardinality " + to_string(stats.cardinality);
		to_print += " and relation id " + to_string(i) + "\n";
		Printer::Print(to_print);
	}
#endif
}

// LCOV_EXCL_STOP

} // namespace duckdb
