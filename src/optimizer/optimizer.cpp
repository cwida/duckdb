#include "duckdb/optimizer/optimizer.hpp"

#include "duckdb/common/enums/optimizer_type.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/query_profiler.hpp"
#include "duckdb/optimizer/build_probe_side_optimizer.hpp"
#include "duckdb/optimizer/column_lifetime_analyzer.hpp"
#include "duckdb/optimizer/common_aggregate_optimizer.hpp"
#include "duckdb/optimizer/constant_or_null_simplification.hpp"
#include "duckdb/optimizer/cse_optimizer.hpp"
#include "duckdb/optimizer/cte_inlining.hpp"
#include "duckdb/optimizer/cte_filter_pusher.hpp"
#include "duckdb/optimizer/deliminator.hpp"
#include "duckdb/optimizer/multi_stage_aggregate_rewriter.hpp"
#include "duckdb/optimizer/empty_result_pullup.hpp"
#include "duckdb/optimizer/expression_heuristics.hpp"
#include "duckdb/optimizer/filter_statistics.hpp"
#include "duckdb/optimizer/filter_pullup.hpp"
#include "duckdb/optimizer/filter_pushdown.hpp"
#include "duckdb/optimizer/grouping_sets_optimizer.hpp"
#include "duckdb/optimizer/in_clause_rewriter.hpp"
#include "duckdb/optimizer/join_elimination.hpp"
#include "duckdb/optimizer/join_filter_pushdown_optimizer.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/optimizer/limit_pushdown.hpp"
#include "duckdb/optimizer/regex_range_filter.hpp"
#include "duckdb/optimizer/remove_duplicate_groups.hpp"
#include "duckdb/optimizer/remove_redundant_order_keys.hpp"
#include "duckdb/optimizer/remove_unused_columns.hpp"
#include "duckdb/optimizer/row_group_pruner.hpp"
#include "duckdb/optimizer/rule/distinct_aggregate_optimizer.hpp"
#include "duckdb/optimizer/rule/equal_or_null_simplification.hpp"
#include "duckdb/optimizer/rule/empty_needle_removal.hpp"
#include "duckdb/optimizer/rule/in_clause_simplification.hpp"
#include "duckdb/optimizer/rule/join_dependent_filter.hpp"
#include "duckdb/optimizer/rule/list.hpp"
#include "duckdb/optimizer/sampling_pushdown.hpp"
#include "duckdb/optimizer/scalar_fn_pushdown.hpp"
#include "duckdb/optimizer/statistics_propagator.hpp"
#include "duckdb/optimizer/aggregate_function_rewriter.hpp"
#include "duckdb/optimizer/aggregate_reuse.hpp"
#include "duckdb/optimizer/topn_optimizer.hpp"
#include "duckdb/optimizer/topn_window_elimination.hpp"
#include "duckdb/optimizer/type_pushdown.hpp"
#include "duckdb/optimizer/unnest_rewriter.hpp"
#include "duckdb/optimizer/late_materialization.hpp"
#include "duckdb/optimizer/common_subplan_optimizer.hpp"
#include "duckdb/optimizer/partitioned_execution.hpp"
#include "duckdb/optimizer/window_self_join.hpp"
#include "duckdb/optimizer/row_number_rewriter.hpp"
#include "duckdb/optimizer/optimizer_extension.hpp"
#include "duckdb/optimizer/outer_join_simplification.hpp"
#include "duckdb/optimizer/partial_aggregate_pushdown.hpp"
#include "duckdb/optimizer/projection_pullup.hpp"
#include "duckdb/optimizer/projection_placement.hpp"
#include "duckdb/optimizer/rule/contains_to_in_clause.hpp"
#include "duckdb/optimizer/rule/monotone_preimage.hpp"
#include "duckdb/optimizer/rule/predicate_factoring.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/operator/logical_prepare.hpp"
#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/optimizer/remote_pushdown_optimizer.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

Optimizer::Optimizer(Binder &binder, ClientContext &context) : context(context), binder(binder), rewriter(context) {
	rewriter.rules.push_back(make_uniq<ConstantOrderNormalizationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ConstantFoldingRule>(rewriter));
	rewriter.rules.push_back(make_uniq<StructExtractStructPackFoldingRule>(rewriter));
	rewriter.rules.push_back(make_uniq<DistributivityRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ArithmeticSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<LeastGreatestSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<CaseSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ConjunctionSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<DatePartSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<DateTruncSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ComparisonSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<RowComparisonSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<InClauseSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<InEnumSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EnumCompareSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EqualOrNullSimplification>(rewriter));
	rewriter.rules.push_back(make_uniq<MoveConstantsRule>(rewriter));
	rewriter.rules.push_back(make_uniq<MoveUnaryMinusRule>(rewriter));
	rewriter.rules.push_back(make_uniq<MonotonePreimageRule>(rewriter));
	rewriter.rules.push_back(make_uniq<LikeOptimizationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<StringPrefixRule>(rewriter));
	rewriter.rules.push_back(make_uniq<InstrPrefixRule>(rewriter));
	rewriter.rules.push_back(make_uniq<OrderedAggregateOptimizer>(rewriter));
	rewriter.rules.push_back(make_uniq<DistinctAggregateOptimizer>(rewriter));
	rewriter.rules.push_back(make_uniq<DistinctWindowedOptimizer>(rewriter));
	rewriter.rules.push_back(make_uniq<RegexOptimizationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<RegexpReplaceExtractRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EmptyNeedleRemovalRule>(rewriter));
	rewriter.rules.push_back(make_uniq<NoopReplaceRemovalRule>(rewriter));
	rewriter.rules.push_back(make_uniq<EnumComparisonRule>(rewriter));
	rewriter.rules.push_back(make_uniq<JoinDependentFilterRule>(rewriter));
	rewriter.rules.push_back(make_uniq<TimeStampComparison>(rewriter));
	rewriter.rules.push_back(make_uniq<PredicateFactoringRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ListComprehensionRewriteRule>(rewriter));
	rewriter.rules.push_back(make_uniq<ContainsToInClauseRule>(rewriter));
	rewriter.rules.push_back(make_uniq<NotComparisonSimplificationRule>(rewriter));
	rewriter.rules.push_back(make_uniq<NotConjunctionSimplificationRule>(rewriter));

#ifdef DEBUG
	for (auto &rule : rewriter.rules) {
		// root not defined in rule
		D_ASSERT(rule->root);
	}
#endif
}

ClientContext &Optimizer::GetContext() {
	return context;
}

bool Optimizer::OptimizerDisabled(OptimizerType type) {
	return OptimizerDisabled(context, type);
}

bool Optimizer::OptimizerDisabled(ClientContext &context_p, OptimizerType type) {
	if (!Settings::Get<EnableOptimizerSetting>(context_p)) {
		// all optimizes are disabled
		return true;
	}
	auto &config = DBConfig::GetConfig(context_p);
	return config.IsOptimizerDisabled(type);
}

void Optimizer::RunOptimizer(OptimizerType type, const std::function<void()> &callback) {
	context.InterruptCheck();

	if (OptimizerDisabled(type)) {
		// optimizer is marked as disabled: skip
		return;
	}
	auto &profiler = QueryProfiler::Get(context);
	{
		auto optimizer_timer = profiler.StartTimerInternal("optimizer." + StringUtil::Lower(EnumUtil::ToString(type)));
		callback();
	}
	if (plan) {
		Verify(*plan);
	}
}

void Optimizer::Verify(LogicalOperator &op) {
	LogicalPlanVerifier::Verify(context, op);
}

// RemoveUnusedColumns renumbers bindings, so RemapProjectionMap can match a stale binding to a
// different column rather than dropping it
static void ClearProjectionMaps(LogicalOperator &op) {
	for (idx_t child_index = 0; child_index < op.children.size(); child_index++) {
		if (auto projection_map = LogicalOperatorVisitor::GetProjectionMap(op, child_index)) {
			projection_map->clear();
		}
		ClearProjectionMaps(*op.children[child_index]);
	}
}

static bool ContainsDML(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_INSERT:
	case LogicalOperatorType::LOGICAL_UPDATE:
	case LogicalOperatorType::LOGICAL_DELETE:
	case LogicalOperatorType::LOGICAL_MERGE_INTO:
		return true;
	default:
		break;
	}
	for (auto &child : op.children) {
		if (ContainsDML(*child)) {
			return true;
		}
	}
	return false;
}

static bool ContainsDataSource(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_GET) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsDataSource(*child)) {
			return true;
		}
	}
	return false;
}

// Returns true if the plan contains a DML statement inside a CTE body. A top-level
// DML statement is not flagged. COPY TO is side-effecting, but does not invalidate
// table statistics and is deliberately excluded here.
static bool CTEContainsDML(const LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_MATERIALIZED_CTE ||
	    op.type == LogicalOperatorType::LOGICAL_RECURSIVE_CTE) {
		if (!op.children.empty() && ContainsDML(*op.children[0])) {
			return true;
		}
	}
	for (auto &child : op.children) {
		if (CTEContainsDML(*child)) {
			return true;
		}
	}
	return false;
}

void Optimizer::OptimizeStatement(unique_ptr<SQLStatement> &statement) {
	if (!Settings::Get<EnableOptimizerSetting>(context)) {
		return;
	}
	if (DatabaseManager::Get(context).GetRemoteCatalogCount() > 0) {
		// if we have any remote catalogs attached then pushdown into remote
		RunOptimizer(OptimizerType::REMOTE_PUSHDOWN, [&]() {
			RemotePushdownOptimizer optimizer(binder);
			optimizer.Rewrite(statement);
		});
	}
}

void Optimizer::RunBuiltInOptimizers() {
	switch (plan->type) {
	case LogicalOperatorType::LOGICAL_TRANSACTION:
	case LogicalOperatorType::LOGICAL_PRAGMA:
	case LogicalOperatorType::LOGICAL_SET:
	case LogicalOperatorType::LOGICAL_ATTACH:
	case LogicalOperatorType::LOGICAL_UPDATE_EXTENSIONS:
	case LogicalOperatorType::LOGICAL_CREATE_SECRET:
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR:
		// skip optimizing simple & often-occurring plans unaffected by rewrites
		if (plan->children.empty()) {
			return;
		}
		break;
	default:
		break;
	}
	// determine which secure views may expose the statistics of their contents - this runs before any rewrites so
	// that the filters the optimizer pushes into a view are not mistaken for the view restricting its own rows
	LogicalSecureView::AnalyzeStatistics(*plan);

	// first we perform expression rewrites using the ExpressionRewriter
	// this does not change the logical plan structure, but only simplifies the expression trees
	RunOptimizer(OptimizerType::EXPRESSION_REWRITER, [&]() {
		rewriter.VisitOperator(*plan);
		ConstantOrNullSimplification constant_or_null_simplification(context);
		plan = constant_or_null_simplification.Optimize(std::move(plan));
	});

	// try to inline CTEs instead of materialization
	RunOptimizer(OptimizerType::CTE_INLINING, [&]() {
		CTEInlining cte_inlining(*this);
		plan = cte_inlining.Optimize(std::move(plan));
	});

	// Rewrites AVG(x) -> SUM(x)/COUNT(x) and SUM(x+C) -> SUM(x) + C*COUNT(x)
	RunOptimizer(OptimizerType::AGGREGATE_FUNCTION_REWRITER, [&]() {
		AggregateFunctionRewriter aggregate_function_rewriter(*this);
		aggregate_function_rewriter.Optimize(plan);
	});

	// perform filter pullup
	RunOptimizer(OptimizerType::FILTER_PULLUP, [&]() {
		FilterPullup filter_pullup;
		plan = filter_pullup.Rewrite(std::move(plan));
	});

	// perform filter pushdown
	RunOptimizer(OptimizerType::FILTER_PUSHDOWN, [&]() {
		FilterPushdown filter_pushdown(*this);
		unordered_set<TableIndex> top_bindings;
		filter_pushdown.CheckMarkToSemi(*plan, top_bindings);
		plan = filter_pushdown.Rewrite(std::move(plan));
	});

	if (!CTEContainsDML(*plan)) {
		FilterStatisticsOptimizer filter_statistics(*this);
		filter_statistics.Optimize(plan);
	}

	// derive and push filters into materialized CTEs
	RunOptimizer(OptimizerType::CTE_FILTER_PUSHER, [&]() {
		CTEFilterPusher cte_filter_pusher(*this);
		plan = cte_filter_pusher.Optimize(std::move(plan));
	});
	CTEFilterPusher::ClearDependencies(*plan);

	RunOptimizer(OptimizerType::REGEX_RANGE, [&]() {
		RegexRangeFilter regex_opt;
		plan = regex_opt.Rewrite(std::move(plan));
	});

	RunOptimizer(OptimizerType::IN_CLAUSE, [&]() {
		InClauseRewriter ic_rewriter(context, *this);
		plan = ic_rewriter.Rewrite(std::move(plan));
	});

	// removes any redundant DelimGets/DelimJoins
	RunOptimizer(OptimizerType::DELIMINATOR, [&]() {
		Deliminator deliminator(context);
		plan = deliminator.Optimize(std::move(plan));
	});

	// rewrite aggregates over multiple grouping sets (ROLLUP/CUBE/GROUPING SETS) into explicit aggregate plans
	RunOptimizer(OptimizerType::GROUPING_SETS, [&]() {
		GroupingSetsOptimizer grouping_sets_optimizer(*this);
		grouping_sets_optimizer.VisitOperator(plan);
	});

	// Optional aggregate recipes own their optimizer strategy; DISTINCT remains controlled by its dedicated setting.
	MultiStageAggregateRewriter aggregate_rewriter(*this, AggregateRewritePolicy::UNCONDITIONAL,
	                                               !OptimizerDisabled(OptimizerType::DISTINCT_AGGREGATE_REWRITE));
	aggregate_rewriter.VisitOperator(plan);

	// try to inline CTEs instead of materialization
	RunOptimizer(OptimizerType::CTE_INLINING, [&]() {
		CTEInlining cte_inlining(*this);
		plan = cte_inlining.Optimize(std::move(plan));
	});

	// Pulls up empty results
	RunOptimizer(OptimizerType::EMPTY_RESULT_PULLUP, [&]() {
		EmptyResultPullup empty_result_pullup;
		plan = empty_result_pullup.Optimize(std::move(plan));
	});

	// Replaces some window computations with self-joins
	RunOptimizer(OptimizerType::WINDOW_SELF_JOIN, [&]() {
		WindowSelfJoinOptimizer window_self_join_optimizer(*this);
		plan = window_self_join_optimizer.Optimize(std::move(plan));
	});

	// Pull up projection from joins
	RunOptimizer(OptimizerType::PROJECTION_PULLUP, [&]() {
		ProjectionPullup projection_pullup(*this, plan);
		projection_pullup.Optimize(plan);
	});

	// Simplifies FULL OUTER -> LEFT/RIGHT OUTER -> INNER if NULLs are filtered anyway
	RunOptimizer(OptimizerType::OUTER_JOIN_SIMPLIFICATION, [&]() {
		OuterJoinSimplification outer_join_simplification;
		outer_join_simplification.VisitOperator(*plan);
	});

	// then we perform the join ordering optimization
	// this also rewrites cross products + filters into joins and performs filter pushdowns
	RunOptimizer(OptimizerType::JOIN_ORDER, [&]() {
		JoinOrderOptimizer optimizer(context);
		plan = optimizer.Optimize(std::move(plan));
	});

	// Reuse exact aggregate payloads exposed by filtering SEMI joins.
	RunOptimizer(OptimizerType::AGGREGATE_REUSE, [&]() {
		plan->ResolveOperatorTypes();
		AggregateReuseOptimizer aggregate_reuse(*this);
		aggregate_reuse.CollectCTEs(*plan);
		aggregate_reuse.VisitOperator(plan);
	});

	// Pre-aggregate SUM/COUNT below joins when GROUP BY is on dimension columns
	// and the fact side dominates cardinality — reduces probe-side work.
	RunOptimizer(OptimizerType::PARTIAL_AGGREGATE_PUSHDOWN, [&]() {
		PartialAggregatePushdown partial_aggregate_pushdown(*this);
		partial_aggregate_pushdown.VisitOperator(plan);
	});

	RunOptimizer(OptimizerType::JOIN_ELIMINATION, [&]() {
		JoinElimination join_elimination;
		plan = join_elimination.Optimize(std::move(plan));
	});

	RunOptimizer(OptimizerType::REDUNDANT_ORDER_KEYS, [&]() {
		RemoveRedundantOrderKeys remove_redundant_order_keys;
		remove_redundant_order_keys.Optimize(*plan);
	});

	// rewrites UNNESTs in DelimJoins by moving them to the projection
	RunOptimizer(OptimizerType::UNNEST_REWRITER, [&]() {
		UnnestRewriter unnest_rewriter(context);
		plan = unnest_rewriter.Optimize(std::move(plan));
	});

	// removes unused columns
	RunOptimizer(OptimizerType::UNUSED_COLUMNS, [&]() {
		RemoveUnusedColumns unused(*this);
		unused.VisitOperator(plan);
	});

	// Remove duplicate groups from aggregates
	RunOptimizer(OptimizerType::DUPLICATE_GROUPS, [&]() {
		RemoveDuplicateGroups remove(*this);
		remove.VisitOperator(*plan);
	});

	// then we extract common subexpressions inside the different operators
	RunOptimizer(OptimizerType::COMMON_SUBEXPRESSIONS, [&]() {
		CommonSubExpressionOptimizer cse_optimizer(binder);
		cse_optimizer.VisitOperator(*plan);
	});

	// creates projection maps so unused columns are projected out early
	RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(*this, *plan, true);
		column_lifetime.VisitOperator(*plan);
	});

	// Once we know the column lifetime, we have more information regarding
	// what relations should be the build side/probe side.
	RunOptimizer(OptimizerType::BUILD_SIDE_PROBE_SIDE, [&]() {
		BuildProbeSideOptimizer build_probe_side_optimizer(context, *plan);
		build_probe_side_optimizer.VisitOperator(*plan);
	});

	// convert common subplans into materialized CTEs
	// Skip when the plan contains a DML CTE: table statistics are stale at plan
	// time and could cause incorrect deduplication of scans across a DML boundary.
	if (!CTEContainsDML(*plan)) {
		RunOptimizer(OptimizerType::COMMON_SUBPLAN, [&]() {
			CommonSubplanOptimizer common_subplan_optimizer(*this);
			plan = common_subplan_optimizer.Optimize(std::move(plan));
		});
	}

	// pushes LIMIT below PROJECTION
	RunOptimizer(OptimizerType::LIMIT_PUSHDOWN, [&]() {
		LimitPushdown limit_pushdown;
		plan = limit_pushdown.Optimize(std::move(plan));
	});

	RunOptimizer(OptimizerType::ROW_GROUP_PRUNER, [&]() {
		RowGroupPruner row_group_pruner(context);
		plan = row_group_pruner.Optimize(std::move(plan));
	});

	// perform sampling pushdown
	RunOptimizer(OptimizerType::SAMPLING_PUSHDOWN, [&]() {
		SamplingPushdown sampling_pushdown(context);
		plan = sampling_pushdown.Optimize(std::move(plan));
	});

	// transform ORDER BY + LIMIT to TopN
	RunOptimizer(OptimizerType::TOP_N, [&]() {
		TopN topn(context);
		plan = topn.Optimize(std::move(plan));
	});

	// try to use late materialization
	RunOptimizer(OptimizerType::LATE_MATERIALIZATION, [&]() {
		LateMaterialization late_materialization(*this);
		plan = late_materialization.Optimize(std::move(plan));
	});

	// perform statistics propagation
	// DML CTEs can invalidate the table statistics captured during planning.
	column_binding_map_t<unique_ptr<BaseStatistics>> statistics_map;
	bool propagated_statistics = false;
	bool removed_expressions = false;
	if (!CTEContainsDML(*plan)) {
		RunOptimizer(OptimizerType::STATISTICS_PROPAGATION, [&]() {
			StatisticsPropagator propagator(*this, plan);
			propagator.PropagateStatistics(plan);
			statistics_map = propagator.GetStatisticsMap();
			propagated_statistics = true;
			removed_expressions = propagator.HasRemovedExpressions();
		});
	}
	if (propagated_statistics) {
		MultiStageAggregateRewriter costed_rewriter(*this, AggregateRewritePolicy::COST_BASED, false, statistics_map);
		costed_rewriter.VisitOperator(plan);
		if (costed_rewriter.WasChanged()) {
			StatisticsPropagator propagator(*this, plan);
			propagator.PropagateStatistics(plan);
			statistics_map = propagator.GetStatisticsMap();
			removed_expressions |= propagator.HasRemovedExpressions();
		}
		RunOptimizer(OptimizerType::PROJECTION_PLACEMENT, [&]() {
			ProjectionPlacementOptimizer projection_placement(*this, statistics_map);
			projection_placement.Optimize(plan);
		});
	}

	// rewrite row_number window function + filter on row_number to aggregate
	RunOptimizer(OptimizerType::TOP_N_WINDOW_ELIMINATION, [&]() {
		TopNWindowElimination topn_window_elimination(context, *this, &statistics_map);
		plan = topn_window_elimination.Optimize(std::move(plan));
	});

	// remove duplicate aggregates
	RunOptimizer(OptimizerType::COMMON_AGGREGATE, [&]() {
		CommonAggregateOptimizer common_aggregate;
		common_aggregate.VisitOperator(*plan);
	});

	// statistics propagation removes filters, join conditions and aggregate children that it proves redundant.
	// this can leave columns in a scan that nothing references any more - prune them again
	if (removed_expressions) {
		RunOptimizer(OptimizerType::UNUSED_COLUMNS, [&]() {
			ClearProjectionMaps(*plan);
			RemoveUnusedColumns unused(*this);
			unused.VisitOperator(plan);
		});
	}

	// creates projection maps so unused columns are projected out early
	RunOptimizer(OptimizerType::COLUMN_LIFETIME, [&]() {
		ColumnLifetimeAnalyzer column_lifetime(*this, *plan, true);
		column_lifetime.VisitOperator(*plan);
	});

	// apply simple expression heuristics to get an initial reordering
	RunOptimizer(OptimizerType::REORDER_FILTER, [&]() {
		ExpressionHeuristics expression_heuristics(*this);
		plan = expression_heuristics.Rewrite(std::move(plan));
	});

	// split pipelines into partitions and union them back together
	RunOptimizer(OptimizerType::PARTITIONED_EXECUTION, [&]() {
		PartitionedExecution partitioned_execution(*this, plan);
		partitioned_execution.Optimize(plan);
	});

	// perform join filter pushdown after the dust has settled
	RunOptimizer(OptimizerType::JOIN_FILTER_PUSHDOWN, [&]() {
		JoinFilterPushdownOptimizer join_filter_pushdown(*this);
		join_filter_pushdown.VisitOperator(*plan);
	});

	// Rewrite ROW_NUMBER() OVER() window functions to use the row_number virtual column
	RunOptimizer(OptimizerType::ROW_NUMBER_REWRITER, [&]() {
		RowNumberRewriter window_rewriter;
		plan = window_rewriter.Optimize(std::move(plan));
	});

	// Push down type casts in SELECT e.g. SELECT num::UHUGEINT to file readers.
	RunOptimizer(OptimizerType::TYPE_PUSHDOWN, [&] {
		TypePushdown type_pushdown(context);
		plan = type_pushdown.Optimize(std::move(plan));
	});

	// Pushdown scalar functions in SELECT like SELECT strlen(str) to readers
	RunOptimizer(OptimizerType::SCALAR_FN_PUSHDOWN, [&] {
		ScalarFnPushdown fn_pushdown(context);
		plan = fn_pushdown.Optimize(std::move(plan));
	});
}

unique_ptr<LogicalOperator> Optimizer::LowerMandatoryAggregateRewrites(unique_ptr<LogicalOperator> plan_p) {
	Verify(*plan_p);
	GroupingSetsOptimizer grouping_sets_optimizer(*this, true);
	grouping_sets_optimizer.VisitOperator(plan_p);
	MultiStageAggregateRewriter aggregate_rewriter(*this, AggregateRewritePolicy::MANDATORY);
	aggregate_rewriter.VisitOperator(plan_p);
	Verify(*plan_p);
	return plan_p;
}

unique_ptr<LogicalOperator> Optimizer::Optimize(unique_ptr<LogicalOperator> plan_p) {
	plan_p = LowerMandatoryAggregateRewrites(std::move(plan_p));
	if (!Settings::Get<EnableOptimizerSetting>(context)) {
		CTEFilterPusher::ClearDependencies(*plan_p);
		return plan_p;
	}
	Verify(*plan_p);

	this->plan = std::move(plan_p);
	if (plan->type == LogicalOperatorType::LOGICAL_PREPARE) {
		auto &prepared = plan->Cast<LogicalPrepare>().prepared;
		// Optimizers can embed the current database state in the executable plan.
		if (!prepared->properties.read_databases.empty() && ContainsDataSource(*plan)) {
			prepared->properties.always_require_rebind = true;
		}
	}

	for (auto &pre_optimizer_extension : OptimizerExtension::Iterate(context)) {
		RunOptimizer(OptimizerType::EXTENSION, [&]() {
			OptimizerExtensionInput input {GetContext(), *this, pre_optimizer_extension.optimizer_info.get()};
			if (pre_optimizer_extension.pre_optimize_function) {
				if (!OptimizerExtension::PreservesCTEFilterDependencies(context,
				                                                        pre_optimizer_extension.pre_optimize_function)) {
					CTEFilterPusher::ClearDependencies(*plan);
				}
				pre_optimizer_extension.pre_optimize_function(input, plan);
			}
		});
	}

	RunBuiltInOptimizers();

	for (auto &optimizer_extension : OptimizerExtension::Iterate(context)) {
		RunOptimizer(OptimizerType::EXTENSION, [&]() {
			OptimizerExtensionInput input {GetContext(), *this, optimizer_extension.optimizer_info.get()};
			if (optimizer_extension.optimize_function) {
				optimizer_extension.optimize_function(input, plan);
			}
		});
	}

	Planner::VerifyPlan(context, plan);

	return std::move(plan);
}

unique_ptr<Expression> Optimizer::BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(c1));
	return BindScalarFunction(name, std::move(children));
}

unique_ptr<Expression> Optimizer::BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1,
                                                     unique_ptr<Expression> c2) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(c1));
	children.push_back(std::move(c2));
	return BindScalarFunction(name, std::move(children));
}

unique_ptr<Expression> Optimizer::BindScalarFunction(const Identifier &name, unique_ptr<Expression> c1,
                                                     unique_ptr<Expression> c2, unique_ptr<Expression> c3) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(c1));
	children.push_back(std::move(c2));
	children.push_back(std::move(c3));
	return BindScalarFunction(name, std::move(children));
}

unique_ptr<Expression> Optimizer::BindScalarFunction(const Identifier &name, vector<unique_ptr<Expression>> children) {
	FunctionBinder binder(context);
	ErrorData error;
	auto expr = binder.BindScalarFunction(Identifier::DefaultSchema(), name, std::move(children), error);
	if (error.HasError()) {
		throw InternalException("Optimizer exception - failed to bind function %s: %s", name, error.Message());
	}
	return expr;
}

} // namespace duckdb
