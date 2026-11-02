#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/filter_info.hpp"
#include "duckdb/optimizer/join_order/join_predicate.hpp"
#include "duckdb/optimizer/join_order/relation_manager.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_distinct.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

using namespace duckdb;

static RelationStats CreateStats(const vector<ColumnBinding> &bindings, const vector<idx_t> &distinct_counts,
                                 idx_t cardinality) {
	REQUIRE(bindings.size() == distinct_counts.size());
	RelationStats result;
	result.cardinality = cardinality;
	result.stats_initialized = true;
	for (idx_t column_idx = 0; column_idx < bindings.size(); column_idx++) {
		auto domain = DistinctCount(distinct_counts[column_idx], DistinctCountSource::EXACT);
		result.columns.emplace_back(bindings[column_idx], domain, domain,
		                            Identifier("column_" + to_string(column_idx)));
	}
	result.Verify(bindings);
	return result;
}

static unique_ptr<LogicalOperator> CreateProjectedDummy(TableIndex input_index, TableIndex projection_index) {
	auto dummy = make_uniq<LogicalDummyScan>(input_index);
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(input_index, ProjectionIndex(0))));
	auto projection = make_uniq<LogicalProjection>(projection_index, std::move(expressions));
	projection->children.push_back(std::move(dummy));
	return std::move(projection);
}

class UnsupportedOutputOperator : public LogicalOperator {
public:
	explicit UnsupportedOutputOperator(TableIndex output_index)
	    : LogicalOperator(LogicalOperatorType::LOGICAL_PIVOT), output_index(output_index) {
	}

public:
	vector<ColumnBinding> GetColumnBindings() override {
		return {ColumnBinding(output_index, ProjectionIndex(0))};
	}

protected:
	void ResolveTypes() override {
		types = {LogicalType::INTEGER};
	}

private:
	TableIndex output_index;
};

TEST_CASE("Composite SEMI domains retain their correlation constraints", "[optimizer][relation_statistics]") {
	auto left_binding = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto second_left_binding = ColumnBinding(TableIndex(10), ProjectionIndex(1));
	auto right_binding = ColumnBinding(TableIndex(20), ProjectionIndex(0));
	auto second_right_binding = ColumnBinding(TableIndex(20), ProjectionIndex(1));
	vector<SemiAntiJoinDomain> composite_domains {{1000, 100, left_binding, right_binding},
	                                              {1000, 100, second_left_binding, second_right_binding}};
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(1000, 1000, 0.5, JoinType::SEMI,
	                                                                  composite_domains, false)
	            .cardinality == 100);
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(1000, 1000, 0.5, JoinType::SEMI,
	                                                                  composite_domains, true)
	            .cardinality == 200);

	vector<SemiAntiJoinDomain> shared_endpoint_domains {{1000, 100, left_binding, right_binding},
	                                                    {1000, 100, left_binding, second_right_binding}};
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(1000, 1000, 0.5, JoinType::SEMI,
	                                                                  shared_endpoint_domains, false)
	            .cardinality == 10);
	vector<SemiAntiJoinDomain> full_coverage {{1000, 1000, left_binding, right_binding}};
	REQUIRE(
	    RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(1000, 1000, 1, JoinType::ANTI, full_coverage, false)
	        .cardinality == 1);
}

TEST_CASE("Nested empty SEMI remains exact in the enumeration cache", "[optimizer][relation_statistics]") {
	for (auto outer_join_type : {JoinType::SEMI, JoinType::ANTI}) {
		JoinRelationSetManager set_manager;
		auto &left_set = set_manager.GetJoinRelation(RelationIndex(0));
		auto &right_set = set_manager.GetJoinRelation(RelationIndex(1));
		auto &empty_set = set_manager.GetJoinRelation(RelationIndex(2));
		auto &inner_set = set_manager.Union(right_set, empty_set);
		auto &root_set = set_manager.Union(left_set, inner_set);
		auto left_binding = ColumnBinding(TableIndex(0), ProjectionIndex(0));
		auto right_binding = ColumnBinding(TableIndex(1), ProjectionIndex(0));
		auto empty_binding = ColumnBinding(TableIndex(2), ProjectionIndex(0));

		auto inner_comparison = BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_EQUAL, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, right_binding),
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, empty_binding));
		FilterInfo inner_filter(std::move(inner_comparison), inner_set, 0, JoinType::SEMI);
		inner_filter.SetLeftSet(right_set);
		inner_filter.SetRightSet(empty_set);
		inner_filter.left_binding = right_binding;
		inner_filter.right_binding = empty_binding;
		inner_filter.source_operator_index = optional_idx(0);
		inner_filter.semantic_left_set = right_set;
		inner_filter.semantic_right_set = empty_set;

		auto outer_comparison = BoundComparisonExpression::Create(
		    ExpressionType::COMPARE_EQUAL, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, left_binding),
		    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, right_binding));
		FilterInfo outer_filter(std::move(outer_comparison), root_set, 1, outer_join_type);
		outer_filter.SetLeftSet(left_set);
		outer_filter.SetRightSet(inner_set);
		outer_filter.left_binding = left_binding;
		outer_filter.right_binding = right_binding;
		outer_filter.source_operator_index = optional_idx(1);
		outer_filter.semantic_left_set = left_set;
		outer_filter.semantic_right_set = inner_set;

		JoinPredicateModel predicate_model;
		predicate_model.RegisterPredicate(inner_filter, JoinPredicateClass::SEMI_ANTI_JOIN, right_binding,
		                                  empty_binding);
		predicate_model.RegisterPredicate(outer_filter, JoinPredicateClass::SEMI_ANTI_JOIN, left_binding,
		                                  right_binding);
		CardinalityEstimator estimator(set_manager, predicate_model);
		estimator.InitEquivalentRelations();
		auto left_stats = CreateStats({left_binding}, {1000}, 1000);
		auto right_stats = CreateStats({right_binding}, {100}, 100);
		auto empty_stats = CreateStats({empty_binding}, {0}, 0);
		estimator.InitCardinalityEstimatorProps(left_set, left_stats);
		estimator.InitCardinalityEstimatorProps(right_set, right_stats);
		estimator.InitCardinalityEstimatorProps(empty_set, empty_stats);

		REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root_set) ==
		        (outer_join_type == JoinType::SEMI ? 0 : 1000));
		REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(inner_set) == 0);
	}
}

TEST_CASE("Empty multi-relation preserved SEMI remains exact", "[optimizer][relation_statistics]") {
	JoinRelationSetManager set_manager;
	auto &nonempty_set = set_manager.GetJoinRelation(RelationIndex(0));
	auto &empty_set = set_manager.GetJoinRelation(RelationIndex(1));
	auto &rhs_set = set_manager.GetJoinRelation(RelationIndex(2));
	auto &preserved_set = set_manager.Union(nonempty_set, empty_set);
	auto &root_set = set_manager.Union(preserved_set, rhs_set);
	auto nonempty_binding = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto empty_binding = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto rhs_binding = ColumnBinding(TableIndex(2), ProjectionIndex(0));

	auto comparison = BoundComparisonExpression::Create(
	    ExpressionType::COMPARE_EQUAL, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, nonempty_binding),
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, rhs_binding));
	FilterInfo filter(std::move(comparison), root_set, 0, JoinType::SEMI);
	filter.SetLeftSet(preserved_set);
	filter.SetRightSet(rhs_set);
	filter.left_binding = nonempty_binding;
	filter.right_binding = rhs_binding;
	filter.source_operator_index = optional_idx(0);
	filter.semantic_left_set = preserved_set;
	filter.semantic_right_set = rhs_set;

	JoinPredicateModel predicate_model;
	predicate_model.RegisterPredicate(filter, JoinPredicateClass::SEMI_ANTI_JOIN, nonempty_binding, rhs_binding);
	CardinalityEstimator estimator(set_manager, predicate_model);
	estimator.InitEquivalentRelations();
	auto nonempty_stats = CreateStats({nonempty_binding}, {100}, 100);
	auto empty_stats = CreateStats({empty_binding}, {0}, 0);
	auto rhs_stats = CreateStats({rhs_binding}, {100}, 100);
	estimator.InitCardinalityEstimatorProps(nonempty_set, nonempty_stats);
	estimator.InitCardinalityEstimatorProps(empty_set, empty_stats);
	estimator.InitCardinalityEstimatorProps(rhs_set, rhs_stats);

	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root_set) == 0);
	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(preserved_set) == 100);
}

static unique_ptr<Expression> CreateJoinComparison(ColumnBinding left, ColumnBinding right, ExpressionType type) {
	return BoundComparisonExpression::Create(type, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, left),
	                                         make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, right));
}

TEST_CASE("Deferred OR selectivity is applied once through nested SEMI joins", "[optimizer][relation_statistics]") {
	JoinRelationSetManager set_manager;
	auto &first = set_manager.GetJoinRelation(RelationIndex(0));
	auto &second = set_manager.GetJoinRelation(RelationIndex(1));
	auto &third = set_manager.GetJoinRelation(RelationIndex(2));
	auto &fourth = set_manager.GetJoinRelation(RelationIndex(3));
	auto &preserved = set_manager.Union(first, second);
	auto &inner = set_manager.Union(preserved, third);
	auto &root = set_manager.Union(inner, fourth);
	auto a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto c = ColumnBinding(TableIndex(2), ProjectionIndex(0));
	auto d = ColumnBinding(TableIndex(3), ProjectionIndex(0));
	FilterInfo or_filter(make_uniq<BoundConjunctionExpression>(
	                         ExpressionType::CONJUNCTION_OR, CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL),
	                         CreateJoinComparison(a, b, ExpressionType::COMPARE_GREATERTHAN)),
	                     preserved, 0);
	FilterInfo inner_filter(CreateJoinComparison(a, c, ExpressionType::COMPARE_EQUAL), inner, 1, JoinType::SEMI);
	inner_filter.SetLeftSet(preserved);
	inner_filter.SetRightSet(third);
	inner_filter.semantic_left_set = preserved;
	inner_filter.semantic_right_set = third;
	FilterInfo outer_filter(CreateJoinComparison(a, d, ExpressionType::COMPARE_EQUAL), root, 2, JoinType::SEMI);
	outer_filter.SetLeftSet(inner);
	outer_filter.SetRightSet(fourth);
	outer_filter.semantic_left_set = inner;
	outer_filter.semantic_right_set = fourth;
	JoinPredicateModel model;
	model.RegisterPredicate(or_filter, JoinPredicateClass::OTHER, a, b);
	model.RegisterPredicate(inner_filter, JoinPredicateClass::SEMI_ANTI_JOIN, a, c);
	model.RegisterPredicate(outer_filter, JoinPredicateClass::SEMI_ANTI_JOIN, a, d);
	CardinalityEstimator estimator(set_manager, model);
	estimator.InitEquivalentRelations();
	auto first_stats = CreateStats({a}, {1000}, 1000);
	auto second_stats = CreateStats({b}, {1}, 1);
	auto third_stats = CreateStats({c}, {100}, 100);
	auto fourth_stats = CreateStats({d}, {100}, 100);
	estimator.InitCardinalityEstimatorProps(first, first_stats);
	estimator.InitCardinalityEstimatorProps(second, second_stats);
	estimator.InitCardinalityEstimatorProps(third, third_stats);
	estimator.InitCardinalityEstimatorProps(fourth, fourth_stats);
	// Query both cached and uncached subsets in different orders.
	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root) == 8);
	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(preserved) == 200);
	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(inner) == 40);
	REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root) == 8);
}

TEST_CASE("SEMI and ANTI residual OR conditions use one fallback", "[optimizer][relation_statistics]") {
	for (auto join_type : {JoinType::SEMI, JoinType::ANTI}) {
		JoinRelationSetManager set_manager;
		auto &left = set_manager.GetJoinRelation(RelationIndex(0));
		auto &right = set_manager.GetJoinRelation(RelationIndex(1));
		auto &root = set_manager.Union(left, right);
		auto a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
		auto b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
		FilterInfo filter(make_uniq<BoundConjunctionExpression>(
		                      ExpressionType::CONJUNCTION_OR, CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL),
		                      CreateJoinComparison(a, b, ExpressionType::COMPARE_GREATERTHAN)),
		                  root, 0, join_type);
		filter.SetLeftSet(left);
		filter.SetRightSet(right);
		filter.semantic_left_set = left;
		filter.semantic_right_set = right;
		JoinPredicateModel model;
		model.RegisterPredicate(filter, JoinPredicateClass::SEMI_ANTI_JOIN, a, b);
		CardinalityEstimator estimator(set_manager, model);
		estimator.InitEquivalentRelations();
		auto left_stats = CreateStats({a}, {1000}, 1000);
		auto right_stats = CreateStats({b}, {100}, 100);
		estimator.InitCardinalityEstimatorProps(left, left_stats);
		estimator.InitCardinalityEstimatorProps(right, right_stats);
		REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root) == 200);
		REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(root) == 200);
	}
}

TEST_CASE("Projection and window preserve the statistics source", "[optimizer][relation_statistics]") {
	auto input = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto child_stats = CreateStats({input}, {10}, 100);
	child_stats.row_retention = 0.04;
	child_stats.columns[0].total_domain = DistinctCount(100, DistinctCountSource::EXACT);
	child_stats.columns[0].semi_anti_join_domain_evidence.TightenFilterDomainBound(10);
	auto projection = CreateProjectedDummy(TableIndex(10), TableIndex(20));
	auto projected =
	    RelationStatisticsHelper::ExtractProjectionStats(projection->Cast<LogicalProjection>(), child_stats);
	REQUIRE(projected);
	REQUIRE(projected->row_retention == child_stats.row_retention);
	REQUIRE(projected->columns[0].GetSupportedSemiAntiDomainSize().GetIndex() == 10);
	LogicalWindow window(TableIndex(30));
	window.children.push_back(std::move(projection));
	auto windowed = RelationStatisticsHelper::ExtractWindowStats(window, *projected);
	REQUIRE(windowed);
	REQUIRE(windowed->row_retention == child_stats.row_retention);
	REQUIRE(windowed->columns[0].GetSupportedSemiAntiDomainSize().GetIndex() == 10);
}

TEST_CASE("Grouping sets discard inherited SEMI domain evidence", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto input = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto child_stats = CreateStats({input}, {1}, 1);
	child_stats.columns[0].semi_anti_join_domain_evidence.MarkUnique();
	child_stats.columns[0].semi_anti_join_domain_evidence.TightenFilterDomainBound(1);
	LogicalAggregate aggregate(TableIndex(20), TableIndex(30), vector<unique_ptr<Expression>>());
	aggregate.groups.push_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, input));
	aggregate.grouping_sets.emplace_back();
	aggregate.grouping_sets[0].insert(ProjectionIndex(0));
	auto ordinary = RelationStatisticsHelper::ExtractAggregationStats(aggregate, child_stats, *connection.context);
	REQUIRE(ordinary);
	REQUIRE(ordinary->columns[0].GetSupportedSemiAntiDomainSize().IsValid());
	aggregate.grouping_sets.emplace_back();
	auto rollup = RelationStatisticsHelper::ExtractAggregationStats(aggregate, child_stats, *connection.context);
	REQUIRE(rollup);
	REQUIRE_FALSE(rollup->columns[0].GetSupportedSemiAntiDomainSize().IsValid());
	aggregate.grouping_sets[1].insert(ProjectionIndex(0));
	auto duplicate_groups =
	    RelationStatisticsHelper::ExtractAggregationStats(aggregate, child_stats, *connection.context);
	REQUIRE(duplicate_groups);
	REQUIRE_FALSE(duplicate_groups->columns[0].GetSupportedSemiAntiDomainSize().IsValid());
}

TEST_CASE("DISTINCT establishes a new statistics source on supported and fallback targets",
          "[optimizer][relation_statistics]") {
	auto input = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto child_stats = CreateStats({input}, {10}, 100);
	child_stats.row_retention = 0.04;
	LogicalDistinct distinct(DistinctType::DISTINCT);
	distinct.children.push_back(make_uniq<LogicalDummyScan>(TableIndex(10)));
	auto ordinary = RelationStatisticsHelper::ExtractDistinctStats(distinct, child_stats);
	REQUIRE(ordinary);
	REQUIRE(ordinary->row_retention == 1);
	REQUIRE(ordinary->cardinality == 10);
	auto target = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NULL, LogicalType::BOOLEAN);
	target->GetChildrenMutable().push_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, input));
	distinct.distinct_type = DistinctType::DISTINCT_ON;
	distinct.distinct_targets.push_back(std::move(target));
	auto fallback = RelationStatisticsHelper::ExtractDistinctStats(distinct, child_stats);
	REQUIRE(fallback);
	REQUIRE(fallback->row_retention == 1);
	REQUIRE(fallback->cardinality == 100);
}

TEST_CASE("Relation statistics extraction is cached by operator", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto plan = CreateProjectedDummy(TableIndex(10), TableIndex(20));
	RelationStatsExtractor extractor(*connection.context);

	auto first = extractor.Extract(*plan);
	REQUIRE(first);
	REQUIRE(first->MatchesBindings(plan->GetColumnBindings()));
	REQUIRE(extractor.ExtractedOperatorCount() == 2);
	auto second = extractor.Extract(*plan);
	REQUIRE(second == first);
	REQUIRE(extractor.ExtractedOperatorCount() == 2);
}

TEST_CASE("Delim SEMI and ANTI joins use domain coverage", "[optimizer][relation_statistics]") {
	auto left_binding = ColumnBinding(TableIndex(10), ProjectionIndex(0));
	auto right_binding = ColumnBinding(TableIndex(20), ProjectionIndex(0));
	auto left_stats = CreateStats({left_binding}, {1000}, 1000);
	auto right_stats = CreateStats({right_binding}, {100}, 100);
	right_stats.columns[0].total_domain = DistinctCount(1000, DistinctCountSource::EXACT);
	right_stats.row_retention = 0.1;
	right_stats.columns[0].semi_anti_join_domain_evidence.TightenFilterDomainBound(100);
	LogicalComparisonJoin delim_join(JoinType::SEMI, LogicalOperatorType::LOGICAL_DELIM_JOIN);
	delim_join.conditions.emplace_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, left_binding),
	                                   make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, right_binding),
	                                   ExpressionType::COMPARE_EQUAL);
	REQUIRE(
	    RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(delim_join, left_stats, right_stats).cardinality ==
	    100);
	delim_join.join_type = JoinType::ANTI;
	REQUIRE(
	    RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(delim_join, left_stats, right_stats).cardinality ==
	    900);
}

TEST_CASE("Operator statistics reject incomplete child layouts", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto projection = CreateProjectedDummy(TableIndex(10), TableIndex(20));
	auto incomplete_stats = CreateStats({ColumnBinding(TableIndex(11), ProjectionIndex(0))}, {1}, 1);
	vector<reference<const RelationStats>> child_stats {incomplete_stats};
	REQUIRE_FALSE(RelationStatisticsHelper::ExtractOperatorStats(*projection, *connection.context, child_stats));
}

TEST_CASE("Unsupported output operators do not receive guessed statistics", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto child = make_uniq<LogicalDummyScan>(TableIndex(10));
	auto child_stats = CreateStats(child->GetColumnBindings(), {1}, 1);
	UnsupportedOutputOperator unsupported(TableIndex(20));
	unsupported.children.push_back(std::move(child));

	RelationStatsExtractor extractor(*connection.context);
	REQUIRE_FALSE(extractor.Extract(unsupported));

	RelationManager relation_manager(*connection.context);
	REQUIRE_FALSE(relation_manager.AddRelation(unsupported, nullptr, child_stats));
	REQUIRE_FALSE(relation_manager.HasCompleteStats());

	auto same_binding_child = make_uniq<LogicalDummyScan>(TableIndex(30));
	auto same_binding_stats = CreateStats(same_binding_child->GetColumnBindings(), {1}, 1);
	UnsupportedOutputOperator same_binding_unsupported(TableIndex(30));
	same_binding_unsupported.children.push_back(std::move(same_binding_child));
	RelationManager same_binding_manager(*connection.context);
	REQUIRE_FALSE(same_binding_manager.AddRelation(same_binding_unsupported, nullptr, same_binding_stats));
	REQUIRE_FALSE(same_binding_manager.HasCompleteStats());
}

TEST_CASE("Relation statistics extraction rejects recurring and cyclic CTEs", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto cte_index = TableIndex(30);
	LogicalCTERef recurring(TableIndex(40), cte_index, {LogicalType::INTEGER}, {Identifier("i")}, true);
	RelationStatsExtractor recurring_extractor(*connection.context,
	                                           [&](TableIndex) -> optional_ptr<LogicalOperator> { return recurring; });
	REQUIRE_FALSE(recurring_extractor.Extract(recurring));

	LogicalCTERef cyclic(TableIndex(50), cte_index, {LogicalType::INTEGER}, {Identifier("i")});
	RelationStatsExtractor cyclic_extractor(*connection.context,
	                                        [&](TableIndex) -> optional_ptr<LogicalOperator> { return cyclic; });
	REQUIRE_FALSE(cyclic_extractor.Extract(cyclic));
	REQUIRE(cyclic_extractor.ExtractedOperatorCount() == 1);
}

TEST_CASE("SEMI fallback retains fractional cardinalities for enumeration", "[optimizer][relation_statistics]") {
	for (auto join_type : {JoinType::SEMI, JoinType::ANTI}) {
		auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 1, join_type);
		REQUIRE(estimate.cardinality == Approx(20.4));
		REQUIRE_FALSE(estimate.used_domain_evidence);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(0, 1, join_type).cardinality == 0);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(0.5, 1, join_type).cardinality == 0.5);
	}
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::SEMI).cardinality == 0);
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::ANTI).cardinality == 102);
}

TEST_CASE("Shared key filters restrict domains only with supported input statistics",
          "[optimizer][relation_statistics]") {
	for (auto source : {DistinctCountSource::MIN_MAX, DistinctCountSource::HLL, DistinctCountSource::EXACT,
	                    DistinctCountSource::CARDINALITY}) {
		for (bool filter_both : {false, true}) {
			for (idx_t bound : {idx_t(1), idx_t(2)}) {
				JoinRelationSetManager sets;
				auto &left = sets.GetJoinRelation(RelationIndex(0));
				auto &right = sets.GetJoinRelation(RelationIndex(1));
				auto &joined = sets.Union(left, right);
				auto a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
				auto b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
				FilterInfo filter(CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL), joined, 0);
				filter.SetLeftSet(left);
				filter.SetRightSet(right);
				filter.left_binding = a;
				filter.right_binding = b;
				JoinPredicateModel model;
				auto &predicate = model.RegisterPredicate(filter, JoinPredicateClass::INNER_EQUALITY, a, b);
				predicate.SetEqualityClassIndex(optional_idx(0));
				JoinEqualityClass equality;
				equality.index = 0;
				equality.columns = {a, b};
				equality.relations = {RelationIndex(0), RelationIndex(1)};
				equality.edges.emplace_back(predicate, RelationIndex(0), RelationIndex(1), a, b);
				model.AddEqualityClass(std::move(equality));
				CardinalityEstimator estimator(sets, model);
				estimator.InitEquivalentRelations();
				auto left_stats = CreateStats({a}, {1}, 20);
				auto right_stats = CreateStats({b}, {bound}, 100);
				left_stats.columns[0].total_domain = DistinctCount(10, DistinctCountSource::MIN_MAX);
				right_stats.columns[0].total_domain = DistinctCount(10, source);
				left_stats.columns[0].semi_anti_join_domain_evidence.TightenFilterDomainBound(1);
				if (filter_both) {
					right_stats.columns[0].semi_anti_join_domain_evidence.TightenFilterDomainBound(bound);
				} else {
					// A modeled current domain or uniqueness is not an explicit filter restriction.
					right_stats.columns[0].semi_anti_join_domain_evidence.MarkUnique();
				}
				estimator.InitCardinalityEstimatorProps(left, left_stats);
				estimator.InitCardinalityEstimatorProps(right, right_stats);
				estimator.RestrictFilterDomains({left_stats, right_stats});
				auto expected = filter_both && source != DistinctCountSource::CARDINALITY ? 2000 / bound : 200;
				REQUIRE(estimator.EstimateCardinalityWithSet<idx_t>(joined) == expected);
			}
		}
	}
}
