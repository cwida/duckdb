#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/optimizer/join_order/join_order_optimizer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/planner.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_expression_get.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/optimizer/join_order/filter_info.hpp"
#include "duckdb/optimizer/join_order/join_predicate.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/relation_manager.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_extractor.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/storage/statistics/node_statistics.hpp"

#include <cmath>
#include <limits>

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

static JoinOrderOperator &AddOperator(vector<unique_ptr<JoinOrderOperator>> &operators, JoinRelationSetManager &sets,
                                      JoinRelationSet &left, JoinRelationSet &right, JoinOrderOperatorType type) {
	auto op = make_uniq<JoinOrderOperator>(operators.size(), type, left, right, sets.Union(left, right),
	                                       vector<JoinCondition>());
	op->left_total_set = left;
	op->right_total_set = right;
	operators.push_back(std::move(op));
	return *operators.back();
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

TEST_CASE("Nested empty SEMI remains exact in logical estimates", "[optimizer][relation_statistics]") {
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

		auto left_stats = CreateStats({left_binding}, {1000}, 1000);
		auto right_stats = CreateStats({right_binding}, {100}, 100);
		auto empty_stats = CreateStats({empty_binding}, {0}, 0);
		vector<unique_ptr<JoinOrderOperator>> operators;
		AddOperator(operators, set_manager, right_set, empty_set, JoinOrderOperatorType::SEMI);
		auto type = outer_join_type == JoinType::SEMI ? JoinOrderOperatorType::SEMI : JoinOrderOperatorType::ANTI;
		AddOperator(operators, set_manager, left_set, inner_set, type);
		estimator.Initialize({left_stats, right_stats, empty_stats}, operators);
		auto inner_rows = estimator.EstimateCardinality(inner_set);
		auto outer_rows = estimator.EstimateCardinality(root_set);
		REQUIRE(outer_rows.GetDouble() == (outer_join_type == JoinType::SEMI ? 0 : 1000));
		REQUIRE(inner_rows.IsZero());
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

	auto nonempty_stats = CreateStats({nonempty_binding}, {100}, 100);
	auto empty_stats = CreateStats({empty_binding}, {0}, 0);
	auto rhs_stats = CreateStats({rhs_binding}, {100}, 100);
	vector<unique_ptr<JoinOrderOperator>> operators;
	AddOperator(operators, set_manager, preserved_set, rhs_set, JoinOrderOperatorType::SEMI);
	estimator.Initialize({nonempty_stats, empty_stats, rhs_stats}, operators);
	REQUIRE(estimator.EstimateCardinality(root_set).IsZero());
	REQUIRE(estimator.EstimateCardinality(preserved_set).IsZero());
}

static unique_ptr<Expression> CreateJoinComparison(ColumnBinding left, ColumnBinding right, ExpressionType type) {
	return BoundComparisonExpression::Create(type, make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, left),
	                                         make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, right));
}

TEST_CASE("Join cardinality divides large products before converting to double",
          "[optimizer][relation_statistics][cardinality_arithmetic]") {
	const idx_t count = 20;
	const idx_t population = idx_t(1) << 60;
	for (idx_t rows : {population, population >> 4}) {
		for (bool equality : {false, true}) {
			CAPTURE(rows, equality);
			JoinRelationSetManager sets;
			JoinPredicateModel model;
			vector<unique_ptr<FilterInfo>> filters;
			vector<RelationStats> stats;
			JoinEqualityClass group;
			group.index = 0;
			auto root = &sets.GetJoinRelation(RelationIndex(0));
			for (idx_t i = 0; i < count; i++) {
				auto binding = ColumnBinding(TableIndex(i), ProjectionIndex(0));
				auto &relation = sets.GetJoinRelation(RelationIndex(i));
				stats.push_back(CreateStats({binding}, {rows}, rows));
				stats.back().columns[0].total_domain = DistinctCount(population, DistinctCountSource::EXACT);
				group.columns.insert(binding);
				group.relations.insert(RelationIndex(i));
				if (i > 0) {
					auto previous = ColumnBinding(TableIndex(i - 1), ProjectionIndex(0));
					auto &left = sets.GetJoinRelation(RelationIndex(i - 1));
					if (equality) {
						auto filter = make_uniq<FilterInfo>(
						    CreateJoinComparison(previous, binding, ExpressionType::COMPARE_EQUAL),
						    sets.Union(left, relation), i - 1);
						filter->SetLeftSet(left);
						filter->SetRightSet(relation);
						filter->left_binding = previous;
						filter->right_binding = binding;
						auto &predicate =
						    model.RegisterPredicate(*filter, JoinPredicateClass::INNER_EQUALITY, previous, binding);
						predicate.SetEqualityClassIndex(optional_idx(0));
						group.edges.emplace_back(predicate, RelationIndex(i - 1), RelationIndex(i), previous, binding);
						filters.push_back(std::move(filter));
					}
					root = &sets.Union(*root, relation);
				}
			}
			if (equality) {
				model.AddEqualityClass(std::move(group));
			}
			CardinalityEstimator estimator(sets, model);

			estimator.Initialize(stats);
			if (equality) {
				// Neither product fits in a double. The quotients are 2^60 and 2^-20.
				auto expected = rows == population ? double(population) : std::ldexp(1.0, -20);
				REQUIRE(estimator.EstimateCardinality(*root).GetDouble() == Approx(expected));
				REQUIRE(estimator.EstimateCardinality(*root).GetIndex() == MaxValue<idx_t>(1, idx_t(expected)));
			} else {
				REQUIRE(std::isinf(estimator.EstimateCardinality(*root).GetDouble()));
				REQUIRE(estimator.EstimateCardinality(*root).GetIndex() == NumericLimits<idx_t>::Maximum());
			}
		}
	}
}

TEST_CASE("Logical cardinalities recover across cached overflowing and underflowing subsets",
          "[optimizer][relation_statistics][cardinality_arithmetic]") {
	const idx_t population = idx_t(1) << 60;
	for (bool overflow : {false, true}) {
		CAPTURE(overflow);
		const idx_t count = overflow ? 21 : 22;
		JoinRelationSetManager sets;
		JoinPredicateModel model;
		vector<RelationStats> stats;
		vector<unique_ptr<FilterInfo>> filters;
		for (idx_t i = 0; i < count; i++) {
			const idx_t columns = overflow && i == 20 ? 20 : 2;
			const idx_t rows = overflow || i >= 20 ? population : 1;
			vector<ColumnBinding> bindings;
			vector<idx_t> domains;
			for (idx_t column = 0; column < columns; column++) {
				bindings.emplace_back(TableIndex(i), ProjectionIndex(column));
				domains.push_back(rows);
			}
			stats.push_back(CreateStats(bindings, domains, rows));
			for (auto &column : stats.back().columns) {
				column.total_domain = DistinctCount(population, DistinctCountSource::EXACT);
			}
		}
		for (idx_t i = 0; i < (overflow ? 20 : 19); i++) {
			auto right_index = overflow ? 20 : i + 1;
			auto a = ColumnBinding(TableIndex(i), ProjectionIndex(1));
			auto b = ColumnBinding(TableIndex(right_index), ProjectionIndex(overflow ? i : 0));
			auto &left = sets.GetJoinRelation(RelationIndex(i));
			auto &right = sets.GetJoinRelation(RelationIndex(right_index));
			auto filter = make_uniq<FilterInfo>(CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL),
			                                    sets.Union(left, right), i);
			filter->SetLeftSet(left);
			filter->SetRightSet(right);
			filter->left_binding = a;
			filter->right_binding = b;
			auto &predicate = model.RegisterPredicate(*filter, JoinPredicateClass::INNER_EQUALITY, a, b);
			predicate.SetEqualityClassIndex(optional_idx(i));
			JoinEqualityClass group;
			group.index = i;
			group.columns = {a, b};
			group.relations = {RelationIndex(i), RelationIndex(right_index)};
			group.edges.emplace_back(predicate, RelationIndex(i), RelationIndex(right_index), a, b);
			model.AddEqualityClass(std::move(group));
			filters.push_back(std::move(filter));
		}
		CardinalityEstimator estimator(sets, model);

		estimator.Initialize(stats);
		reference<JoinRelationSet> prefix = sets.GetJoinRelation(RelationIndex(0));
		CardinalityValue rows(static_cast<double>(stats[0].cardinality));
		for (idx_t i = 1; i < count; i++) {
			auto &right = sets.GetJoinRelation(RelationIndex(i));
			prefix = sets.Union(prefix, right);
			rows = estimator.EstimateCardinality(prefix);
			if (i == 19) {
				// The stored candidate is 2^1200 or 2^-1140; both must survive conversion boundaries.
				REQUIRE_FALSE(rows.IsZero());
				if (overflow) {
					REQUIRE(std::isinf(rows.GetDouble()));
				} else {
					REQUIRE(rows.GetDouble() == 0);
				}
			}
		}
		auto expected = overflow ? double(population) : std::ldexp(1.0, -1020);
		REQUIRE(rows.GetDouble() == expected);
	}
}

TEST_CASE("Tiny positive cardinalities remain distinct from zero",
          "[optimizer][relation_statistics][cardinality_arithmetic]") {
	CardinalityValue factor(std::ldexp(1.0, -600));
	auto tiny = factor * factor;
	REQUIRE(tiny.GetDouble() == 0);
	REQUIRE_FALSE(tiny.IsZero());
	REQUIRE(tiny.GetIndex() == 1);
	REQUIRE(CardinalityValue(0).GetIndex() == 0);
	REQUIRE(CardinalityValue(0.5).GetIndex() == 1);
	REQUIRE(CardinalityValue(2.5).GetIndex() == 2);
	REQUIRE(tiny > CardinalityValue(0));
	REQUIRE(tiny < factor);
	for (auto type : {JoinType::SEMI, JoinType::ANTI}) {
		auto positive = RelationStatisticsHelper::EstimateSemiAntiJoinFallbackCardinality(CardinalityValue(102),
		                                                                                  tiny.IsZero() ? 0 : 1, type);
		REQUIRE(positive.GetDouble() == Approx(20.4));
		auto empty = RelationStatisticsHelper::EstimateSemiAntiJoinFallbackCardinality(CardinalityValue(102), 0, type);
		REQUIRE(empty.GetDouble() == (type == JoinType::SEMI ? 0 : 102));
	}
}

TEST_CASE("SEMI and ANTI estimator arithmetic preserves fractional inputs and outputs",
          "[optimizer][relation_statistics][cardinality_arithmetic]") {
	for (bool fractional_input : {false, true}) {
		for (auto join_type : {JoinType::SEMI, JoinType::ANTI}) {
			for (idx_t rhs_rows : {idx_t(0), idx_t(1)}) {
				CAPTURE(fractional_input, join_type, rhs_rows);
				JoinRelationSetManager sets;
				auto &left = sets.GetJoinRelation(RelationIndex(0));
				auto &second = sets.GetJoinRelation(RelationIndex(1));
				auto &rhs = sets.GetJoinRelation(RelationIndex(2));
				auto &pair = sets.Union(left, second);
				auto &preserved = fractional_input ? pair : left;
				auto &root = sets.Union(preserved, rhs);
				auto a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
				auto b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
				auto c = ColumnBinding(TableIndex(2), ProjectionIndex(0));
				FilterInfo equality(CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL), pair, 0);
				equality.SetLeftSet(left);
				equality.SetRightSet(second);
				equality.left_binding = a;
				equality.right_binding = b;
				FilterInfo semi(CreateJoinComparison(a, c, ExpressionType::COMPARE_EQUAL), root, 1, join_type);
				semi.SetLeftSet(preserved);
				semi.SetRightSet(rhs);
				semi.semantic_left_set = preserved;
				semi.semantic_right_set = rhs;
				JoinPredicateModel model;
				if (fractional_input) {
					auto &predicate = model.RegisterPredicate(equality, JoinPredicateClass::INNER_EQUALITY, a, b);
					predicate.SetEqualityClassIndex(optional_idx(0));
					JoinEqualityClass group;
					group.index = 0;
					group.columns = {a, b};
					group.relations = {RelationIndex(0), RelationIndex(1)};
					group.edges.emplace_back(predicate, RelationIndex(0), RelationIndex(1), a, b);
					model.AddEqualityClass(std::move(group));
				}
				model.RegisterPredicate(semi, JoinPredicateClass::SEMI_ANTI_JOIN, a, c);
				auto left_stats =
				    CreateStats({a}, {fractional_input ? idx_t(2) : idx_t(102)}, fractional_input ? 2 : 102);
				auto second_stats = CreateStats({b}, {1}, 1);
				if (fractional_input) {
					left_stats.columns[0].total_domain = DistinctCount(4, DistinctCountSource::EXACT);
					second_stats.columns[0].total_domain = DistinctCount(4, DistinctCountSource::EXACT);
				}
				auto rhs_stats = CreateStats({c}, {rhs_rows}, rhs_rows);
				CardinalityEstimator estimator(sets, model);

				vector<unique_ptr<JoinOrderOperator>> operators;
				auto type = join_type == JoinType::SEMI ? JoinOrderOperatorType::SEMI : JoinOrderOperatorType::ANTI;
				AddOperator(operators, sets, preserved, rhs, type);
				estimator.Initialize({left_stats, second_stats, rhs_stats}, operators);
				double input = fractional_input ? 0.5 : 102;
				auto expected = rhs_rows ? (fractional_input ? 0.5 : 20.4) : (join_type == JoinType::ANTI ? input : 0);
				auto estimate = [&]() {
					return estimator.EstimateCardinality(root);
				};
				REQUIRE(estimate().GetDouble() == Approx(expected));
				REQUIRE(estimator.EstimateCardinality(preserved).GetDouble() == Approx(input));
				estimator.EstimateCardinality(root);
				REQUIRE(estimate().GetDouble() == Approx(expected));
			}
		}
	}
}

TEST_CASE("Large LEFT and composite joins retain their cardinality bounds",
          "[optimizer][relation_statistics][cardinality_arithmetic]") {
	const idx_t count = 20;
	const idx_t population = idx_t(1) << 60;
	for (auto configuration : {make_pair(JoinType::LEFT, population / 2), make_pair(JoinType::LEFT, population * 2),
	                           make_pair(JoinType::INNER, population / 2)}) {
		auto join_type = configuration.first;
		auto final_domain = configuration.second;
		CAPTURE(join_type, final_domain);
		JoinRelationSetManager sets;
		JoinPredicateModel model;
		vector<unique_ptr<FilterInfo>> filters;
		vector<RelationStats> stats;
		JoinEqualityClass chain;
		chain.index = 0;
		auto root = &sets.GetJoinRelation(RelationIndex(0));
		for (idx_t i = 0; i < count; i++) {
			auto binding = ColumnBinding(TableIndex(i), ProjectionIndex(0));
			auto first_key = ColumnBinding(TableIndex(i), ProjectionIndex(1));
			auto second_key = ColumnBinding(TableIndex(i), ProjectionIndex(2));
			stats.push_back(CreateStats(
			    {binding, first_key, second_key},
			    {population, MinValue(population, final_domain), MinValue(population, final_domain)}, population));
			stats.back().columns[0].total_domain = DistinctCount(population * 4, DistinctCountSource::EXACT);
			stats.back().columns[1].total_domain = DistinctCount(final_domain, DistinctCountSource::EXACT);
			stats.back().columns[2].total_domain = DistinctCount(final_domain, DistinctCountSource::EXACT);
			if (i == count - 1) {
				break;
			}
			chain.columns.insert(binding);
			chain.relations.insert(RelationIndex(i));
			if (i == 0) {
				continue;
			}
			auto previous = ColumnBinding(TableIndex(i - 1), ProjectionIndex(0));
			auto &left = sets.GetJoinRelation(RelationIndex(i - 1));
			auto &right = sets.GetJoinRelation(RelationIndex(i));
			auto filter = make_uniq<FilterInfo>(CreateJoinComparison(previous, binding, ExpressionType::COMPARE_EQUAL),
			                                    sets.Union(left, right), filters.size());
			filter->SetLeftSet(left);
			filter->SetRightSet(right);
			filter->left_binding = previous;
			filter->right_binding = binding;
			auto &predicate = model.RegisterPredicate(*filter, JoinPredicateClass::INNER_EQUALITY, previous, binding);
			predicate.SetEqualityClassIndex(optional_idx(0));
			chain.edges.emplace_back(predicate, RelationIndex(i - 1), RelationIndex(i), previous, binding);
			filters.push_back(std::move(filter));
			root = &sets.Union(*root, right);
		}
		model.AddEqualityClass(std::move(chain));
		auto &last_left = sets.GetJoinRelation(RelationIndex(count - 2));
		auto &right = sets.GetJoinRelation(RelationIndex(count - 1));
		auto &pair = sets.Union(last_left, right);
		auto &left = last_left;
		for (idx_t key = 1; key <= (join_type == JoinType::LEFT ? 1 : 2); key++) {
			auto a = ColumnBinding(TableIndex(count - 2), ProjectionIndex(key));
			auto b = ColumnBinding(TableIndex(count - 1), ProjectionIndex(key));
			auto filter = make_uniq<FilterInfo>(CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL),
			                                    sets.Union(left, right), filters.size(), join_type);
			filter->SetLeftSet(left);
			filter->SetRightSet(right);
			filter->left_binding = a;
			filter->right_binding = b;
			filter->source_operator_index = optional_idx(0);
			auto &predicate = model.RegisterPredicate(
			    *filter,
			    join_type == JoinType::LEFT ? JoinPredicateClass::LEFT_JOIN : JoinPredicateClass::INNER_EQUALITY, a, b);
			if (join_type == JoinType::INNER) {
				predicate.SetEqualityClassIndex(optional_idx(key));
				JoinEqualityClass group;
				group.index = key;
				group.columns = {a, b};
				group.relations = {RelationIndex(count - 2), RelationIndex(count - 1)};
				group.edges.emplace_back(predicate, RelationIndex(count - 2), RelationIndex(count - 1), a, b);
				model.AddEqualityClass(std::move(group));
				model.AddDirectEqualityPairClass(pair, key, a, b);
			}
			filters.push_back(std::move(filter));
		}
		auto &preserved_set = *root;
		root = &sets.Union(*root, right);
		CardinalityEstimator estimator(sets, model);

		vector<unique_ptr<JoinOrderOperator>> operators;
		auto type = join_type == JoinType::LEFT ? JoinOrderOperatorType::LEFT : JoinOrderOperatorType::INNER;
		AddOperator(operators, sets, preserved_set, right, type);
		estimator.Initialize(stats, operators);
		// The chain has 2^24 rows despite its overflowing numerator.
		// LEFT doubles/preserves this input; the composite cap preserves it.
		auto preserved_cardinality = population >> (2 * (count - 2));
		auto expected = preserved_cardinality * (join_type == JoinType::LEFT && final_domain < population ? 2 : 1);
		auto rows = estimator.EstimateCardinality(*root);
		REQUIRE(rows.GetDouble() == Approx(double(expected)));
		REQUIRE(LossyNumericCast<idx_t>(rows.GetDouble()) == expected);
	}
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

	auto first_stats = CreateStats({a}, {1000}, 1000);
	auto second_stats = CreateStats({b}, {1}, 1);
	auto third_stats = CreateStats({c}, {100}, 100);
	auto fourth_stats = CreateStats({d}, {100}, 100);
	vector<unique_ptr<JoinOrderOperator>> operators;
	AddOperator(operators, set_manager, preserved, third, JoinOrderOperatorType::SEMI);
	AddOperator(operators, set_manager, inner, fourth, JoinOrderOperatorType::SEMI);
	estimator.Initialize({first_stats, second_stats, third_stats, fourth_stats}, operators);
	auto estimate = [&]() {
		REQUIRE(estimator.EstimateCardinality(preserved).GetDouble() == 200);
		REQUIRE(estimator.EstimateCardinality(inner).GetDouble() == 40);
		return estimator.EstimateCardinality(root);
	};
	REQUIRE(estimate().GetDouble() == 8);
	estimator.EstimateCardinality(root);
	estimator.EstimateCardinality(inner);
	REQUIRE(estimate().GetDouble() == 8);
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

		auto left_stats = CreateStats({a}, {1000}, 1000);
		auto right_stats = CreateStats({b}, {100}, 100);
		vector<unique_ptr<JoinOrderOperator>> operators;
		auto type = join_type == JoinType::SEMI ? JoinOrderOperatorType::SEMI : JoinOrderOperatorType::ANTI;
		AddOperator(operators, set_manager, left, right, type);
		estimator.Initialize({left_stats, right_stats}, operators);
		auto estimate = [&]() {
			return estimator.EstimateCardinality(root);
		};
		REQUIRE(estimate().GetDouble() == 200);
		estimator.EstimateCardinality(root);
		REQUIRE(estimate().GetDouble() == 200);
	}
}

TEST_CASE("Completed operators use their projected logical input scopes", "[optimizer][relation_statistics]") {
	for (auto type : {JoinOrderOperatorType::LEFT, JoinOrderOperatorType::SEMI, JoinOrderOperatorType::ANTI}) {
		for (idx_t extra_rows : {idx_t(0), idx_t(10)}) {
			JoinRelationSetManager sets;
			auto &a = sets.GetJoinRelation(RelationIndex(0));
			auto &b = sets.GetJoinRelation(RelationIndex(1));
			auto &c = sets.GetJoinRelation(RelationIndex(2));
			auto &ab = sets.Union(a, b);
			auto &ac = sets.Union(a, c);
			auto &root = sets.Union(ab, c);
			JoinPredicateModel model;
			vector<unique_ptr<JoinOrderOperator>> operators;
			AddOperator(operators, sets, a, c, JoinOrderOperatorType::LEFT);
			auto &outer = AddOperator(operators, sets, ac, b, type);
			outer.total_set = ab;
			outer.left_total_set = a;
			vector<RelationStats> stats;
			for (idx_t i = 0; i < 3; i++) {
				auto rows = i == 0 ? 100 : i == 1 ? 20 : extra_rows;
				stats.push_back(CreateStats({ColumnBinding(TableIndex(i), ProjectionIndex(0))}, {rows}, rows));
			}
			for (bool root_first : {false, true}) {
				CardinalityEstimator estimator(sets, model);
				estimator.Initialize(stats, operators);
				auto expected_ab = type == JoinOrderOperatorType::LEFT ? 2000.0 : 20.0;
				auto expected_root = expected_ab * MaxValue(idx_t(1), extra_rows);
				if (root_first) {
					REQUIRE(estimator.EstimateCardinality(root).GetDouble() == expected_root);
				}
				REQUIRE(estimator.EstimateCardinality(ab).GetDouble() == expected_ab);
				REQUIRE(estimator.EstimateCardinality(root).GetDouble() == expected_root);
			}
		}
	}
}

TEST_CASE("SEMI boundaries use the same estimate before or after an INNER candidate",
          "[optimizer][relation_statistics]") {
	JoinRelationSetManager sets;
	auto &a = sets.GetJoinRelation(RelationIndex(0));
	auto &b = sets.GetJoinRelation(RelationIndex(1));
	auto &c = sets.GetJoinRelation(RelationIndex(2));
	auto &ab = sets.Union(a, b);
	auto &ac = sets.Union(a, c);
	auto &root = sets.Union(ab, c);
	auto x = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto y = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto z = ColumnBinding(TableIndex(2), ProjectionIndex(0));
	FilterInfo filter(CreateJoinComparison(x, z, ExpressionType::COMPARE_EQUAL), ac, 0);
	filter.SetLeftSet(a);
	filter.SetRightSet(c);
	filter.left_binding = x;
	filter.right_binding = z;
	JoinPredicateModel model;
	auto &predicate = model.RegisterPredicate(filter, JoinPredicateClass::INNER_EQUALITY, x, z);
	predicate.SetEqualityClassIndex(optional_idx(0));
	JoinEqualityClass group;
	group.index = 0;
	group.columns = {x, z};
	group.relations = {RelationIndex(0), RelationIndex(2)};
	group.edges.emplace_back(predicate, RelationIndex(0), RelationIndex(2), x, z);
	model.AddEqualityClass(std::move(group));
	auto left_stats = CreateStats({x}, {1}, 1);
	left_stats.columns[0].total_domain = DistinctCount(1250, DistinctCountSource::EXACT);
	vector<RelationStats> stats {left_stats, CreateStats({y}, {10}, 10), CreateStats({z}, {1250}, 10000)};
	vector<unique_ptr<JoinOrderOperator>> operators;
	AddOperator(operators, sets, a, b, JoinOrderOperatorType::SEMI);
	for (bool root_first : {false, true}) {
		CardinalityEstimator estimator(sets, model);
		estimator.Initialize(stats, operators);
		if (root_first) {
			REQUIRE(estimator.EstimateCardinality(root).GetDouble() == 8);
		}
		REQUIRE(estimator.EstimateCardinality(ab).GetDouble() == 1);
		REQUIRE(estimator.EstimateCardinality(ac).GetDouble() == 8);
		// Applying the SEMI fallback to AC would incorrectly lower this to 1.6.
		auto estimate = estimator.EstimateCardinality(root);
		REQUIRE_FALSE(estimate < CardinalityValue(8));
		REQUIRE_FALSE(estimate > CardinalityValue(8));
	}
}

TEST_CASE("LEFT ON residuals precede preservation and post-filters follow it", "[optimizer][relation_statistics]") {
	for (idx_t right_rows : {idx_t(1), idx_t(10000)}) {
		for (bool post_filter : {false, true}) {
			JoinRelationSetManager sets;
			auto &left = sets.GetJoinRelation(RelationIndex(0));
			auto &right = sets.GetJoinRelation(RelationIndex(1));
			auto &root = sets.Union(left, right);
			auto a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
			auto b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
			FilterInfo equality(CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL), root, 0, JoinType::LEFT);
			equality.SetLeftSet(left);
			equality.SetRightSet(right);
			equality.left_binding = a;
			equality.right_binding = b;
			equality.source_operator_index = optional_idx(0);
			FilterInfo residual(
			    make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR,
			                                          CreateJoinComparison(a, b, ExpressionType::COMPARE_EQUAL),
			                                          CreateJoinComparison(a, b, ExpressionType::COMPARE_GREATERTHAN)),
			    root, 1);
			residual.must_remain_filter = true;
			JoinPredicateModel model;
			model.RegisterPredicate(equality, JoinPredicateClass::LEFT_JOIN, a, b);
			if (post_filter) {
				model.RegisterPredicate(residual, JoinPredicateClass::OTHER, a, b);
			}
			vector<unique_ptr<JoinOrderOperator>> operators;
			auto &op = AddOperator(operators, sets, left, right, JoinOrderOperatorType::LEFT);
			op.conditions.emplace_back(residual.filter->Copy());
			op.residual_predicates.push_back(op.conditions.back().GetJoinExpression());
			CardinalityEstimator estimator(sets, model);
			estimator.Initialize(
			    {CreateStats({a}, {100}, 100), CreateStats({b}, {MinValue(idx_t(100), right_rows)}, right_rows)},
			    operators);
			auto expected = right_rows == 1 ? 100.0 : 2000.0;
			if (post_filter) {
				expected *= 0.2;
			}
			REQUIRE(estimator.EstimateCardinality(root).GetDouble() == Approx(expected));
		}
	}
}

TEST_CASE("Owned INNER residuals wait for their complete operator", "[optimizer][relation_statistics]") {
	for (bool has_bindings : {false, true}) {
		JoinRelationSetManager sets;
		auto &a = sets.GetJoinRelation(RelationIndex(0));
		auto &b = sets.GetJoinRelation(RelationIndex(1));
		auto &d = sets.GetJoinRelation(RelationIndex(2));
		auto &ab = sets.Union(a, b);
		auto &ad = sets.Union(a, d);
		auto &root = sets.Union(ab, d);
		auto x = ColumnBinding(TableIndex(0), ProjectionIndex(0));
		auto y = ColumnBinding(TableIndex(1), ProjectionIndex(0));
		auto z = ColumnBinding(TableIndex(2), ProjectionIndex(0));
		unique_ptr<Expression> expression;
		if (has_bindings) {
			expression = make_uniq<BoundConjunctionExpression>(
			    ExpressionType::CONJUNCTION_OR, CreateJoinComparison(x, z, ExpressionType::COMPARE_EQUAL),
			    CreateJoinComparison(x, z, ExpressionType::COMPARE_GREATERTHAN));
		} else {
			expression = make_uniq<BoundParameterExpression>(Identifier("1"));
		}
		auto &referenced = has_bindings ? ad : sets.GetJoinRelation(unordered_set<RelationIndex>());
		FilterInfo residual(std::move(expression), referenced, 0);
		residual.source_operator_index = optional_idx(1);
		residual.from_residual_predicate = true;
		JoinPredicateModel model;
		model.RegisterPredicate(residual, JoinPredicateClass::OTHER, ColumnBinding(), ColumnBinding());
		vector<unique_ptr<JoinOrderOperator>> operators;
		AddOperator(operators, sets, a, b, JoinOrderOperatorType::LEFT);
		auto &op = AddOperator(operators, sets, ab, d, JoinOrderOperatorType::INNER);
		op.must_apply_as_operator = true;
		CardinalityEstimator estimator(sets, model);
		estimator.Initialize({CreateStats({x}, {100}, 100), CreateStats({y}, {1}, 1), CreateStats({z}, {10}, 10)},
		                     operators);
		REQUIRE(estimator.EstimateCardinality(ad).GetDouble() == 1000);
		REQUIRE(estimator.EstimateCardinality(ab).GetDouble() == 100);
		REQUIRE(estimator.EstimateCardinality(root).GetDouble() == 200);
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
	for (auto join_type : {JoinType::SEMI, JoinType::ANTI, JoinType::RIGHT_SEMI, JoinType::RIGHT_ANTI}) {
		auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 1, join_type);
		REQUIRE(estimate.cardinality == Approx(20.4));
		REQUIRE_FALSE(estimate.used_domain_evidence);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(0, 1, join_type).cardinality == 0);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(0.5, 1, join_type).cardinality == 0.5);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(-1, 1, join_type).cardinality == 0);
		REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(-1, 0, join_type).cardinality == 0);
	}
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::SEMI).cardinality == 0);
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::ANTI).cardinality == 102);
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::RIGHT_SEMI).cardinality == 0);
	REQUIRE(RelationStatisticsHelper::EstimateSemiAntiJoinFallback(102, 0, JoinType::RIGHT_ANTI).cardinality == 102);
}

TEST_CASE("SEMI and ANTI fallback is independent of positive RHS cardinality", "[optimizer][relation_statistics]") {
	for (auto type : {JoinOrderOperatorType::SEMI, JoinOrderOperatorType::ANTI}) {
		for (idx_t left_rows : {idx_t(100003), idx_t(11997210)}) {
			double expected = 0;
			for (idx_t right_rows : {idx_t(1), idx_t(3), idx_t(17), idx_t(1000003), idx_t(59986052)}) {
				JoinRelationSetManager sets;
				auto &a = sets.GetJoinRelation(RelationIndex(0));
				auto &b = sets.GetJoinRelation(RelationIndex(1));
				auto &left = sets.Union(a, b);
				auto &right = sets.GetJoinRelation(RelationIndex(2));
				auto &root = sets.Union(left, right);
				auto x = ColumnBinding(TableIndex(0), ProjectionIndex(0));
				auto y = ColumnBinding(TableIndex(1), ProjectionIndex(0));
				auto z = ColumnBinding(TableIndex(2), ProjectionIndex(0));
				FilterInfo filter(CreateJoinComparison(x, y, ExpressionType::COMPARE_EQUAL), left, 0);
				filter.SetLeftSet(a);
				filter.SetRightSet(b);
				filter.left_binding = x;
				filter.right_binding = y;
				JoinPredicateModel model;
				auto &predicate = model.RegisterPredicate(filter, JoinPredicateClass::INNER_EQUALITY, x, y);
				predicate.SetEqualityClassIndex(optional_idx(0));
				JoinEqualityClass group;
				group.index = 0;
				group.columns = {x, y};
				group.relations = {RelationIndex(0), RelationIndex(1)};
				group.edges.emplace_back(predicate, RelationIndex(0), RelationIndex(1), x, y);
				model.AddEqualityClass(std::move(group));
				vector<unique_ptr<JoinOrderOperator>> operators;
				AddOperator(operators, sets, left, right, type);
				CardinalityEstimator estimator(sets, model);
				estimator.Initialize(
				    {CreateStats({x}, {7}, left_rows), CreateStats({y}, {7}, 31), CreateStats({z}, {1}, right_rows)},
				    operators);
				auto rows = estimator.EstimateCardinality(root).GetDouble();
				if (right_rows == 1) {
					expected = rows;
				}
				// Even a one-ULP difference can bypass the enumerator's cost tie-breaker.
				REQUIRE(rows == expected);
			}
		}
	}
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
				estimator.Initialize({left_stats, right_stats});
				auto expected = filter_both && source != DistinctCountSource::CARDINALITY ? 2000 / bound : 200;
				REQUIRE(estimator.EstimateCardinality(joined).GetIndex() == expected);
			}
		}
	}
}

TEST_CASE("Large cardinality estimates saturate and preserve integer bounds", "[optimizer][relation_statistics]") {
	auto maximum = NumericLimits<idx_t>::Maximum();
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(0) == 0);
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(-1) == 0);
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(std::numeric_limits<double>::quiet_NaN()) == 0);
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(std::numeric_limits<double>::infinity()) == maximum);
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(static_cast<double>(maximum)) == maximum);
	REQUIRE(RelationStatisticsHelper::CardinalityFromDouble(20.4) == 20);

	vector<DistinctCount> domains {{maximum, DistinctCountSource::EXACT}, {maximum, DistinctCountSource::EXACT}};
	REQUIRE(RelationStatisticsHelper::EstimateDistinctCardinality(domains, maximum) == maximum);
	REQUIRE(RelationStatisticsHelper::EstimateDistinctCardinality(domains, maximum - 1) == maximum - 1);
	REQUIRE(RelationStatisticsHelper::EstimateCurrentDomain(domains[0], maximum, maximum - 1).distinct_count ==
	        maximum - 1);
}

struct CardinalityProviderData : public TableFunctionData {
	NodeStatistics statistics {0, 0};
	shared_ptr<idx_t> calls = make_shared_ptr<idx_t>(0);
	shared_ptr<idx_t> column_calls = make_shared_ptr<idx_t>(0);
};

static unique_ptr<LogicalGet> CreateCardinalityProvider(optional_idx estimate, optional_idx maximum) {
	auto data = make_uniq<CardinalityProviderData>();
	data->statistics.has_estimated_cardinality = estimate.IsValid();
	data->statistics.has_max_cardinality = maximum.IsValid();
	if (estimate.IsValid()) {
		data->statistics.estimated_cardinality = estimate.GetIndex();
	}
	if (maximum.IsValid()) {
		data->statistics.max_cardinality = maximum.GetIndex();
	}
	TableFunction function("cardinality_provider", {}, nullptr);
	function.cardinality = [](ClientContext &, auto bind_data) -> unique_ptr<NodeStatistics> {
		auto &data = bind_data->template Cast<CardinalityProviderData>();
		(*data.calls)++;
		return make_uniq<NodeStatistics>(data.statistics);
	};
	auto get = make_uniq<LogicalGet>(TableIndex(10), BoundTableFunction(function), std::move(data),
	                                 vector<LogicalType> {LogicalType::INTEGER}, vector<Identifier> {"i"});
	get->AddColumnId(0);
	return get;
}

TEST_CASE("Scan filters acquire hidden input columns once and apply conjunction rows once",
          "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto scan = CreateCardinalityProvider(100, 1000);
	scan->returned_types.push_back(LogicalType::INTEGER);
	scan->names.emplace_back("hidden");
	scan->AddColumnId(1);
	scan->projection_ids = {ProjectionIndex(0)};
	auto &data = scan->bind_data->Cast<CardinalityProviderData>();
	scan->function.statistics = [](ClientContext &, auto bind_data, column_t) -> unique_ptr<BaseStatistics> {
		auto &data = bind_data->template Cast<CardinalityProviderData>();
		(*data.column_calls)++;
		auto result = BaseStatistics::CreateUnknown(LogicalType::INTEGER);
		result.SetDistinctCount(10);
		return result.ToUnique();
	};
	scan->table_filters.PushFilter(
	    ProjectionIndex(1), ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, Value::INTEGER(1)));
	scan->table_filters.PushFilter(ProjectionIndex(0), ExpressionFilter::CreateComparisonFilter(
	                                                       ExpressionType::COMPARE_GREATERTHAN, Value::INTEGER(1)));
	RelationStatsExtractor extractor(*connection.context);
	auto result = extractor.Extract(*scan);
	REQUIRE(result);
	REQUIRE(result->columns.size() == 1);
	REQUIRE(result->cardinality == 10);
	REQUIRE(result->row_retention == Approx(0.1));
	REQUIRE(result->columns[0].current_domain.distinct_count ==
	        RelationStatisticsHelper::EstimateCurrentDomain(DistinctCount(10, DistinctCountSource::HLL), 100, 10)
	            .distinct_count);
	REQUIRE(extractor.Extract(*scan));
	REQUIRE(*data.column_calls == 2);
	REQUIRE(*data.calls == 1);
}

static unique_ptr<LogicalOperator> PlanStatisticsQuery(Connection &connection, const string &query) {
	Parser parser(*connection.context);
	parser.ParseQuery(query);
	Planner planner(*connection.context);
	planner.CreatePlan(std::move(parser.statements[0]));
	planner.plan->ResolveOperatorTypes();
	return std::move(planner.plan);
}

static void SetStatisticsTestCardinalities(LogicalOperator &op) {
	op.SetEstimatedCardinality(0);
	for (auto &child : op.children) {
		SetStatisticsTestCardinalities(*child);
	}
}

static void CreateStatisticsJoinTables(Connection &connection) {
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE TABLE stats_a AS SELECT i::INTEGER x, (i % 10)::INTEGER k FROM range(1000) t(i)"));
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE TABLE stats_b AS SELECT i::INTEGER y, (i % 10)::INTEGER k FROM range(1000) t(i)"));
	REQUIRE_NO_FAIL(
	    connection.Query("CREATE TABLE stats_c AS SELECT i::INTEGER z, (i % 10)::INTEGER k FROM range(100) t(i)"));
	REQUIRE_NO_FAIL(connection.Query("SET disabled_optimizers='join_order'"));
}

TEST_CASE("UNION grouping estimates ignore cached arm cardinalities", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	auto input = make_uniq<LogicalSetOperation>(TableIndex(30), 1, make_uniq<LogicalDummyScan>(TableIndex(10)),
	                                            make_uniq<LogicalDummyScan>(TableIndex(20)),
	                                            LogicalOperatorType::LOGICAL_UNION, true);
	LogicalAggregate aggregate(TableIndex(40), TableIndex(50), {});
	aggregate.groups.push_back(
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(TableIndex(30), ProjectionIndex(0))));
	aggregate.grouping_sets.push_back({ProjectionIndex(0)});
	aggregate.children.push_back(std::move(input));

	auto bindings = aggregate.GetColumnBindings();
	for (auto cached_cardinality : {idx_t(0), idx_t(1000)}) {
		for (auto &arm : aggregate.children[0]->children) {
			arm->SetEstimatedCardinality(cached_cardinality);
		}
		RelationStatsExtractor extractor(*connection.context);
		auto stats = extractor.Extract(aggregate);
		REQUIRE(stats);
		REQUIRE(stats->cardinality == 2);
		REQUIRE(stats->MatchesBindings(bindings));
		REQUIRE(extractor.ExtractedOperatorCount() == 4);
		for (auto &arm : aggregate.children[0]->children) {
			REQUIRE(arm->has_estimated_cardinality);
			REQUIRE(arm->estimated_cardinality == cached_cardinality);
		}
		REQUIRE_FALSE(aggregate.has_estimated_cardinality);
		REQUIRE_FALSE(aggregate.children[0]->has_estimated_cardinality);
	}
}

TEST_CASE("UNION grouping refinement shares the CTE extraction snapshot", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	vector<vector<unique_ptr<Expression>>> rows;
	for (idx_t row = 0; row < 3; row++) {
		vector<unique_ptr<Expression>> values;
		values.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(1)));
		rows.push_back(std::move(values));
	}
	auto input =
	    make_uniq<LogicalExpressionGet>(TableIndex(10), vector<LogicalType> {LogicalType::INTEGER}, std::move(rows));
	vector<unique_ptr<Expression>> expressions;
	expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(10)));
	expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(20)));
	auto definition = make_uniq<LogicalProjection>(TableIndex(20), std::move(expressions));
	definition->children.push_back(std::move(input));

	auto cte_index = TableIndex(30);
	auto first = make_uniq<LogicalCTERef>(TableIndex(40), cte_index,
	                                      vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                                      vector<Identifier> {"a", "b"});
	auto second = make_uniq<LogicalCTERef>(TableIndex(50), cte_index,
	                                       vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                                       vector<Identifier> {"c", "d"});
	auto union_op = make_uniq<LogicalSetOperation>(TableIndex(60), 2, std::move(first), std::move(second),
	                                               LogicalOperatorType::LOGICAL_UNION, true);
	LogicalAggregate aggregate(TableIndex(70), TableIndex(80), {});
	for (idx_t column = 0; column < 2; column++) {
		aggregate.groups.push_back(make_uniq<BoundColumnRefExpression>(
		    LogicalType::INTEGER, ColumnBinding(TableIndex(60), ProjectionIndex(column))));
	}
	aggregate.grouping_sets.push_back({ProjectionIndex(0), ProjectionIndex(1)});
	aggregate.children.push_back(std::move(union_op));

	idx_t cte_lookups = 0;
	RelationStatsExtractor extractor(*connection.context, [&](TableIndex index) -> optional_ptr<LogicalOperator> {
		REQUIRE(index == cte_index);
		cte_lookups++;
		return *definition;
	});
	auto stats = extractor.Extract(aggregate);
	REQUIRE(stats);
	// Each arm has three rows but only one pair of keys, rather than the UNION's four domain combinations.
	REQUIRE(stats->cardinality == 2);
	REQUIRE(stats->MatchesBindings(aggregate.GetColumnBindings()));
	REQUIRE(cte_lookups == 2);
	REQUIRE(extractor.ExtractedOperatorCount() == 6);
	REQUIRE(extractor.Extract(aggregate) == stats);
	REQUIRE(cte_lookups == 2);
	REQUIRE(extractor.ExtractedOperatorCount() == 6);
}

TEST_CASE("Standalone join statistics share estimation without mutating the plan", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	CreateStatisticsJoinTables(connection);
	connection.BeginTransaction();
	for (auto query : {"SELECT a.x,b.y FROM stats_a a JOIN stats_b b USING(k)",
	                   "SELECT a.x,b.y FROM stats_a a JOIN stats_b b ON a.k<b.k",
	                   "SELECT a.x,b.y FROM stats_a a JOIN stats_b b ON a.k<=b.k",
	                   "SELECT a.x,b.y FROM stats_a a,stats_b b WHERE a.k=b.k",
	                   "SELECT a.x,b.y,c.z FROM stats_a a JOIN stats_b b USING(k) JOIN stats_c c USING(k)",
	                   "SELECT a.x,b.y,c.z FROM stats_a a,stats_b b,stats_c c WHERE a.k=b.k AND b.k=c.k AND a.k=c.k",
	                   "SELECT a.x,b.y FROM (SELECT * FROM stats_a WHERE x<100) a JOIN stats_b b USING(k)",
	                   "SELECT x,y,count(*) FROM (SELECT a.x,b.y FROM stats_a a JOIN stats_b b USING(k) UNION ALL "
	                   "SELECT a.x,b.y FROM stats_a a JOIN stats_b b USING(k)) u GROUP BY x,y"}) {
		CAPTURE(query);
		auto plan = PlanStatisticsQuery(connection, query);
		auto reference_plan = PlanStatisticsQuery(connection, query);
		RelationStats reference_stats;
		JoinOrderOptimizer reference_optimizer(*connection.context);
		reference_plan = reference_optimizer.Optimize(std::move(reference_plan), reference_stats);
		REQUIRE(reference_stats.stats_initialized);

		SetStatisticsTestCardinalities(*plan);
		auto bindings = plan->GetColumnBindings();
		MemoryStream before;
		BinarySerializer::Serialize(*plan, before);
		RelationStatsExtractor extractor(*connection.context);
		auto stats = extractor.Extract(*plan);
		REQUIRE(stats);
		REQUIRE(stats->cardinality == reference_stats.cardinality);
		REQUIRE(stats->cardinality > 10000);
		REQUIRE(stats->MatchesBindings(bindings));
		MemoryStream after;
		BinarySerializer::Serialize(*plan, after);
		REQUIRE(before.GetPosition() == after.GetPosition());
		REQUIRE(memcmp(before.GetData(), after.GetData(), before.GetPosition()) == 0);
	}
	connection.Rollback();
}

TEST_CASE("Standalone join estimates preserve positive fractional cardinalities", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	connection.BeginTransaction();
	for (auto query : {"SELECT * FROM range(1) a(i) JOIN range(1) b(i) ON a.i<b.i",
	                   "SELECT * FROM (SELECT * FROM range(1000) LIMIT 1) a "
	                   "JOIN (SELECT * FROM range(1000) LIMIT 1) b USING(range)"}) {
		CAPTURE(query);
		auto plan = PlanStatisticsQuery(connection, query);
		auto reference_plan = PlanStatisticsQuery(connection, query);
		RelationStats reference_stats;
		JoinOrderOptimizer optimizer(*connection.context);
		reference_plan = optimizer.Optimize(std::move(reference_plan), reference_stats);
		REQUIRE(reference_stats.stats_initialized);
		REQUIRE(reference_stats.cardinality == 1);
		RelationStatsExtractor extractor(*connection.context);
		auto stats = extractor.Extract(*plan);
		REQUIRE(stats);
		REQUIRE(stats->cardinality == reference_stats.cardinality);
	}
	connection.Rollback();
}

TEST_CASE("Standalone join estimates preserve empty inputs", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	CreateStatisticsJoinTables(connection);
	connection.BeginTransaction();
	vector<pair<string, idx_t>> queries {
	    {"SELECT a.x,b.y FROM (SELECT * FROM stats_a LIMIT 0) a CROSS JOIN stats_b b", 0},
	    {"SELECT a.x,b.y FROM stats_a a CROSS JOIN (SELECT * FROM stats_b LIMIT 0) b", 0},
	    {"SELECT a.x,b.y FROM (SELECT * FROM stats_a LIMIT 0) a JOIN stats_b b USING(k)", 0},
	    {"SELECT b.y FROM (SELECT * FROM stats_a LIMIT 0) a CROSS JOIN stats_b b GROUP BY b.y", 0},
	    {"SELECT a.x FROM stats_a a SEMI JOIN "
	     "(SELECT b.k FROM (SELECT * FROM stats_c LIMIT 0) c CROSS JOIN stats_b b GROUP BY b.k) q USING(k)",
	     0},
	    {"SELECT a.x FROM stats_a a ANTI JOIN "
	     "(SELECT b.k FROM (SELECT * FROM stats_c LIMIT 0) c CROSS JOIN stats_b b GROUP BY b.k) q USING(k)",
	     1000}};
	for (bool empty_first : {false, true}) {
		string query = "SELECT 1 FROM ";
		const string empty = "(SELECT * FROM range(1) LIMIT 0) e";
		if (empty_first) {
			query += empty + " CROSS JOIN ";
		}
		for (idx_t i = 0; i < 18; i++) {
			if (i > 0) {
				query += " CROSS JOIN ";
			}
			query += "range(1000000000000000000) r" + to_string(i);
		}
		if (!empty_first) {
			query += " CROSS JOIN " + empty;
		}
		queries.emplace_back(std::move(query), 0);
	}
	for (auto &query : queries) {
		CAPTURE(query.first);
		auto plan = PlanStatisticsQuery(connection, query.first);
		SetStatisticsTestCardinalities(*plan);
		MemoryStream before;
		BinarySerializer::Serialize(*plan, before);
		RelationStatsExtractor extractor(*connection.context);
		auto stats = extractor.Extract(*plan);
		REQUIRE(stats);
		REQUIRE(stats->cardinality == query.second);
		MemoryStream after;
		BinarySerializer::Serialize(*plan, after);
		REQUIRE(before.GetPosition() == after.GetPosition());
		REQUIRE(memcmp(before.GetData(), after.GetData(), before.GetPosition()) == 0);
	}
	connection.Rollback();
}

TEST_CASE("Standalone join preparation declines unavailable predicate bindings", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	for (auto depth : {idx_t(0), idx_t(1)}) {
		LogicalComparisonJoin join(JoinType::INNER);
		join.children.push_back(make_uniq<LogicalDummyScan>(TableIndex(10)));
		join.children.push_back(make_uniq<LogicalDummyScan>(TableIndex(20)));
		auto left_binding = ColumnBinding(TableIndex(depth == 0 ? 30 : 10), ProjectionIndex(0));
		join.conditions.emplace_back(make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, left_binding, depth),
		                             make_uniq<BoundColumnRefExpression>(
		                                 LogicalType::INTEGER, ColumnBinding(TableIndex(20), ProjectionIndex(0))),
		                             ExpressionType::COMPARE_EQUAL);
		RelationStatsExtractor extractor(*connection.context);
		auto stats = extractor.Extract(join);
		REQUIRE(stats);
		REQUIRE(stats->cardinality == 1);
		REQUIRE(stats->MatchesBindings(join.GetColumnBindings()));
	}
}

TEST_CASE("Repeated relation outputs share a normalized binding", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	LogicalComparisonJoin join(JoinType::SINGLE);
	join.children.push_back(make_uniq<LogicalDummyScan>(TableIndex(10)));
	join.children.push_back(make_uniq<LogicalDummyScan>(TableIndex(20)));
	join.left_projection_map = {ProjectionIndex(0), ProjectionIndex(0)};
	join.ResolveOperatorTypes();
	auto bindings = join.GetColumnBindings();
	REQUIRE(bindings.size() == 3);
	REQUIRE(bindings[0] == bindings[1]);

	RelationStatsExtractor extractor(*connection.context);
	auto stats = extractor.Extract(join);
	REQUIRE(stats);
	REQUIRE(stats->MatchesBindings(bindings));
	RelationManager normal(*connection.context);
	REQUIRE(normal.AddRelation(join, nullptr, *stats));
	RelationManager standalone(*connection.context);
	vector<reference<LogicalOperator>> filters;
	REQUIRE(standalone.ExtractJoinRelationsForEstimation(extractor, join, filters));
	for (auto &manager : {reference<RelationManager>(normal), reference<RelationManager>(standalone)}) {
		ColumnBinding normalized;
		REQUIRE(manager.get().TryNormalizeBinding(bindings[0], normalized));
		REQUIRE(normalized == ColumnBinding(TableIndex(0), ProjectionIndex(0)));
		REQUIRE(manager.get().TryNormalizeBinding(bindings[2], normalized));
		REQUIRE(normalized == ColumnBinding(TableIndex(0), ProjectionIndex(2)));
		REQUIRE(manager.get().GetRelationStats()[0].MatchesBindings(bindings));
	}
}

TEST_CASE("Standalone joins inside CTE definitions retain the caller snapshot", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	CreateStatisticsJoinTables(connection);
	connection.BeginTransaction();
	auto definition = PlanStatisticsQuery(connection, "SELECT a.x,b.y FROM stats_a a JOIN stats_b b USING(k)");
	SetStatisticsTestCardinalities(*definition);
	auto cte_index = TableIndex(10000);
	auto first = make_uniq<LogicalCTERef>(TableIndex(10001), cte_index,
	                                      vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                                      vector<Identifier> {"x", "y"});
	auto second = make_uniq<LogicalCTERef>(TableIndex(10002), cte_index,
	                                       vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                                       vector<Identifier> {"x", "y"});
	LogicalSetOperation union_op(TableIndex(10003), 2, std::move(first), std::move(second),
	                             LogicalOperatorType::LOGICAL_UNION, true);
	idx_t lookups = 0;
	RelationStatsExtractor extractor(*connection.context, [&](TableIndex index) -> optional_ptr<LogicalOperator> {
		REQUIRE(index == cte_index);
		lookups++;
		return *definition;
	});
	MemoryStream before;
	BinarySerializer::Serialize(*definition, before);
	auto stats = extractor.Extract(union_op);
	REQUIRE(stats);
	REQUIRE(stats->cardinality > 100000);
	auto definition_stats = extractor.Extract(*definition);
	REQUIRE(definition_stats);
	REQUIRE(stats->cardinality == 2 * definition_stats->cardinality);
	REQUIRE(lookups == 2);
	MemoryStream after;
	BinarySerializer::Serialize(*definition, after);
	REQUIRE(before.GetPosition() == after.GetPosition());
	REQUIRE(memcmp(before.GetData(), after.GetData(), before.GetPosition()) == 0);
	connection.Rollback();
}

TEST_CASE("Standalone joins retain projected and repeated output bindings", "[optimizer][relation_statistics]") {
	DuckDB db;
	Connection connection(db);
	CreateStatisticsJoinTables(connection);
	connection.BeginTransaction();
	auto plan = PlanStatisticsQuery(connection, "SELECT * FROM stats_a a JOIN stats_b b ON a.k=b.k");
	auto &join = plan->children[0]->Cast<LogicalComparisonJoin>();
	RelationStatsExtractor original(*connection.context);
	auto full = original.Extract(join);
	REQUIRE(full);
	join.left_projection_map = {ProjectionIndex(0)};
	join.right_projection_map = {ProjectionIndex(0), ProjectionIndex(0)};
	join.ResolveOperatorTypes();
	SetStatisticsTestCardinalities(join);
	MemoryStream before;
	BinarySerializer::Serialize(join, before);
	RelationStatsExtractor projected(*connection.context);
	auto stats = projected.Extract(join);
	REQUIRE(stats);
	REQUIRE(stats->cardinality == full->cardinality);
	REQUIRE(stats->MatchesBindings(join.GetColumnBindings()));
	REQUIRE(stats->columns.size() == 3);
	REQUIRE(stats->columns[1].binding == stats->columns[2].binding);
	for (auto &column : stats->columns) {
		auto source = full->GetColumnStats(column.binding);
		REQUIRE(source);
		REQUIRE(column.current_domain.distinct_count == source->current_domain.distinct_count);
	}
	MemoryStream after;
	BinarySerializer::Serialize(join, after);
	REQUIRE(before.GetPosition() == after.GetPosition());
	REQUIRE(memcmp(before.GetData(), after.GetData(), before.GetPosition()) == 0);
	connection.Rollback();
}
