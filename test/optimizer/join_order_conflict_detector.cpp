#include "catch.hpp"

#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/query_graph.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"

using namespace duckdb;

static unique_ptr<Expression> Column(TableIndex table_index) {
	return make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(table_index, ProjectionIndex(0)));
}

static vector<JoinCondition> Comparison(TableIndex left, TableIndex right, ExpressionType comparison_type) {
	vector<JoinCondition> result;
	result.emplace_back(Column(left), Column(right), comparison_type);
	return result;
}

static vector<JoinCondition> NonRejectingDisjunction(TableIndex shared, TableIndex other) {
	auto equality = BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, Column(shared), Column(other));
	auto is_not_null = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, LogicalType::BOOLEAN);
	is_not_null->GetChildrenMutable().push_back(Column(other));
	auto disjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(equality),
	                                                         std::move(is_not_null));
	vector<JoinCondition> result;
	result.emplace_back(std::move(disjunction));
	return result;
}

static bool ParentMustApplyAsOperator(JoinOrderOperatorType child_type, bool child_on_right) {
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
	auto &r0_r1 = set_manager.Union(r0, r1);
	auto &r1_r2 = set_manager.Union(r1, r2);
	auto &r0_r1_r2 = set_manager.Union(r0_r1, r2);

	vector<unique_ptr<JoinOrderOperator>> operators;
	if (child_on_right) {
		operators.push_back(make_uniq<JoinOrderOperator>(0, child_type, r1, r2, r1_r2, vector<JoinCondition>()));
		operators.push_back(make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::INNER, r0, r1_r2, r0_r1_r2,
		                                                 vector<JoinCondition>()));
		operators.back()->right_operators.push_back(*operators[0]);
	} else {
		operators.push_back(make_uniq<JoinOrderOperator>(0, child_type, r0, r1, r0_r1, vector<JoinCondition>()));
		operators.push_back(make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::INNER, r0_r1, r2, r0_r1_r2,
		                                                 vector<JoinCondition>()));
		operators.back()->left_operators.push_back(*operators[0]);
	}

	const unordered_map<TableIndex, RelationIndex> relation_mapping;
	JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

	REQUIRE(operators[1]->total_set.get().count == operators[1]->syntactic_set.get().count);
	REQUIRE(operators[1]->conflict_rules.empty());
	return JoinOrderConflictDetector::MustApplyAsOperator(*operators[1]);
}

TEST_CASE("CD-C honors conditional SEMI join associativity", "[optimizer][join_order]") {
	for (auto comparison_type : {ExpressionType::COMPARE_EQUAL, ExpressionType::COMPARE_NOT_DISTINCT_FROM}) {
		JoinRelationSetManager set_manager;
		auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
		auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
		auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
		auto &r0_r1 = set_manager.Union(r0, r1);
		auto &r1_r2 = set_manager.Union(r1, r2);

		vector<unique_ptr<JoinOrderOperator>> operators;
		operators.push_back(
		    make_uniq<JoinOrderOperator>(0, JoinOrderOperatorType::SEMI, r0, r1, r0_r1,
		                                 Comparison(TableIndex(0), TableIndex(1), ExpressionType::COMPARE_EQUAL)));
		operators.push_back(make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::SEMI, r0_r1, r2, r1_r2,
		                                                 Comparison(TableIndex(1), TableIndex(2), comparison_type)));
		operators.back()->left_operators.push_back(*operators[0]);

		unordered_map<TableIndex, RelationIndex> relation_mapping;
		relation_mapping[TableIndex(0)] = RelationIndex(0);
		relation_mapping[TableIndex(1)] = RelationIndex(1);
		relation_mapping[TableIndex(2)] = RelationIndex(2);
		JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

		if (comparison_type == ExpressionType::COMPARE_EQUAL) {
			REQUIRE(operators[1]->total_set.get().count == 2);
			REQUIRE(JoinOrderConflictDetector::IsApplicable(*operators[1], r1, r2));
		} else {
			REQUIRE(operators[1]->total_set.get().count == 3);
			REQUIRE_FALSE(JoinOrderConflictDetector::IsApplicable(*operators[1], r1, r2));
		}
	}
}

TEST_CASE("CD-C expands TES for a conflicting non-inner descendant", "[optimizer][join_order]") {
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
	auto &r0_r1 = set_manager.Union(r0, r1);
	auto &r1_r2 = set_manager.Union(r1, r2);

	vector<unique_ptr<JoinOrderOperator>> operators;
	operators.push_back(
	    make_uniq<JoinOrderOperator>(0, JoinOrderOperatorType::LEFT, r0, r1, r0_r1, vector<JoinCondition>()));
	operators.push_back(
	    make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::ANTI, r0_r1, r2, r1_r2, vector<JoinCondition>()));
	operators.back()->left_operators.push_back(*operators[0]);

	const unordered_map<TableIndex, RelationIndex> relation_mapping;
	JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

	REQUIRE(operators[1]->total_set.get().count == 3);
	REQUIRE(operators[1]->conflict_rules.empty());
	REQUIRE_FALSE(JoinOrderConflictDetector::IsApplicable(*operators[1], r1, r2));
}

TEST_CASE("CD-C preserves operator ownership after simplifying absorbed conflicts", "[optimizer][join_order]") {
	REQUIRE(ParentMustApplyAsOperator(JoinOrderOperatorType::LEFT, false));
	REQUIRE(ParentMustApplyAsOperator(JoinOrderOperatorType::LEFT, true));
	REQUIRE_FALSE(ParentMustApplyAsOperator(JoinOrderOperatorType::INNER, false));
}

TEST_CASE("CD-C splits filtering join predicates only when each satisfies the conflicts", "[optimizer][join_order]") {
	const auto child_type = GENERATE(JoinOrderOperatorType::SEMI, JoinOrderOperatorType::ANTI);
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
	auto &r3 = set_manager.GetJoinRelation(RelationIndex(3));
	auto &r0_r1 = set_manager.Union(r0, r1);
	auto &r2_r3 = set_manager.Union(r2, r3);
	auto &r0_r1_r2 = set_manager.Union(r0_r1, r2);
	unordered_map<TableIndex, RelationIndex> relation_mapping;
	for (idx_t relation_idx = 0; relation_idx < 6; relation_idx++) {
		relation_mapping[TableIndex(relation_idx)] = RelationIndex(relation_idx);
	}
	auto first_lhs = Column(TableIndex(0));
	auto second_rhs = Column(TableIndex(2));
	bool residual = false;
	bool left_region = false;
	bool must_apply = true;
	SECTION("Each predicate contains the required visible input") {
		must_apply = false;
	}
	SECTION("One predicate lacks the requirement") {
		second_rhs = Column(TableIndex(0));
	}
	SECTION("A binding is unmapped") {
		relation_mapping.erase(TableIndex(0));
	}
	SECTION("A binding is correlated") {
		first_lhs->Cast<BoundColumnRefExpression>().DepthMutable() = 1;
	}
	SECTION("A binding is already physical") {
		first_lhs = make_uniq<BoundReferenceExpression>(LogicalType::INTEGER, 0);
	}
	SECTION("A subquery has not been flattened") {
		first_lhs = make_uniq<BoundSubqueryExpression>(LogicalType::INTEGER);
	}
	SECTION("A condition is residual") {
		residual = true;
	}
	SECTION("A sibling introduces NULLs") {
		left_region = true;
	}
	vector<JoinCondition> conditions;
	conditions.emplace_back(std::move(first_lhs), Column(TableIndex(2)), ExpressionType::COMPARE_EQUAL);
	if (residual) {
		conditions.emplace_back(BoundComparisonExpression::Create(ExpressionType::COMPARE_EQUAL, Column(TableIndex(1)),
		                                                          std::move(second_rhs)));
	} else {
		conditions.emplace_back(Column(TableIndex(1)), std::move(second_rhs), ExpressionType::COMPARE_EQUAL);
	}
	vector<unique_ptr<JoinOrderOperator>> operators;
	operators.push_back(make_uniq<JoinOrderOperator>(
	    0, child_type, r2, r3, r2_r3, Comparison(TableIndex(2), TableIndex(3), ExpressionType::COMPARE_EQUAL)));
	operators.push_back(
	    make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::INNER, r0_r1, r2_r3, r0_r1_r2, std::move(conditions)));
	operators[1]->right_operators.push_back(*operators[0]);
	if (left_region) {
		auto &r4 = set_manager.GetJoinRelation(RelationIndex(4));
		auto &r5 = set_manager.GetJoinRelation(RelationIndex(5));
		operators.push_back(
		    make_uniq<JoinOrderOperator>(2, JoinOrderOperatorType::LEFT, r4, r5, set_manager.Union(r4, r5),
		                                 Comparison(TableIndex(4), TableIndex(5), ExpressionType::COMPARE_EQUAL)));
	}
	JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);
	REQUIRE(JoinOrderConflictDetector::MustApplyAsOperator(*operators[0]));
	REQUIRE(JoinOrderConflictDetector::MustApplyAsOperator(*operators[1]) == must_apply);
}

TEST_CASE("CD-C tests conditional associativity against the shared input", "[optimizer][join_order]") {
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
	auto &r0_r1 = set_manager.Union(r0, r1);
	auto &r1_r2 = set_manager.Union(r1, r2);

	vector<unique_ptr<JoinOrderOperator>> operators;
	operators.push_back(make_uniq<JoinOrderOperator>(0, JoinOrderOperatorType::SEMI, r1, r2, r1_r2,
	                                                 NonRejectingDisjunction(TableIndex(1), TableIndex(2))));
	operators.push_back(
	    make_uniq<JoinOrderOperator>(1, JoinOrderOperatorType::SEMI, r0, r1_r2, r0_r1,
	                                 Comparison(TableIndex(0), TableIndex(1), ExpressionType::COMPARE_EQUAL)));
	operators.back()->right_operators.push_back(*operators[0]);

	unordered_map<TableIndex, RelationIndex> relation_mapping;
	relation_mapping[TableIndex(0)] = RelationIndex(0);
	relation_mapping[TableIndex(1)] = RelationIndex(1);
	relation_mapping[TableIndex(2)] = RelationIndex(2);
	JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

	// p12 OR (r2 IS NOT NULL) can be true when the shared r1 input is NULL. Nulling both r1 and r2 would
	// incorrectly classify it as null rejecting and permit associativity.
	REQUIRE(operators[1]->total_set.get().count == 3);
	REQUIRE(operators[1]->conflict_rules.empty());
	REQUIRE_FALSE(JoinOrderConflictDetector::IsApplicable(*operators[1], r0, r1));
}

TEST_CASE("CD-C implements the supported associativity matrix", "[optimizer][join_order]") {
	const vector<JoinOrderOperatorType> types {JoinOrderOperatorType::INNER, JoinOrderOperatorType::LEFT,
	                                           JoinOrderOperatorType::SEMI, JoinOrderOperatorType::ANTI};
	for (auto child_type : types) {
		for (auto parent_type : types) {
			JoinRelationSetManager set_manager;
			auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
			auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
			auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
			auto &r0_r1 = set_manager.Union(r0, r1);
			auto &r1_r2 = set_manager.Union(r1, r2);

			vector<unique_ptr<JoinOrderOperator>> operators;
			operators.push_back(make_uniq<JoinOrderOperator>(0, child_type, r0, r1, r0_r1, vector<JoinCondition>()));
			vector<JoinCondition> parent_conditions;
			if (parent_type == JoinOrderOperatorType::SEMI) {
				parent_conditions = Comparison(TableIndex(1), TableIndex(2), ExpressionType::COMPARE_EQUAL);
			}
			operators.push_back(
			    make_uniq<JoinOrderOperator>(1, parent_type, r0_r1, r2, r1_r2, std::move(parent_conditions)));
			operators.back()->left_operators.push_back(*operators[0]);

			unordered_map<TableIndex, RelationIndex> relation_mapping;
			relation_mapping[TableIndex(0)] = RelationIndex(0);
			relation_mapping[TableIndex(1)] = RelationIndex(1);
			relation_mapping[TableIndex(2)] = RelationIndex(2);
			JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

			auto associative =
			    child_type == JoinOrderOperatorType::INNER ||
			    (child_type == JoinOrderOperatorType::SEMI && parent_type == JoinOrderOperatorType::SEMI);
			INFO("child type " << static_cast<int>(child_type) << ", parent type " << static_cast<int>(parent_type));
			REQUIRE(operators[1]->total_set.get().count == (associative ? 2 : 3));
		}
	}
}

TEST_CASE("CD-C implements the supported right associative-commutative matrix", "[optimizer][join_order]") {
	const vector<JoinOrderOperatorType> types {JoinOrderOperatorType::INNER, JoinOrderOperatorType::LEFT,
	                                           JoinOrderOperatorType::SEMI, JoinOrderOperatorType::ANTI};
	for (auto parent_type : types) {
		for (auto child_type : types) {
			JoinRelationSetManager set_manager;
			auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
			auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
			auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
			auto &r0_r2 = set_manager.Union(r0, r2);
			auto &r1_r2 = set_manager.Union(r1, r2);

			vector<unique_ptr<JoinOrderOperator>> operators;
			vector<JoinCondition> child_conditions;
			if (child_type == JoinOrderOperatorType::SEMI) {
				child_conditions = Comparison(TableIndex(1), TableIndex(2), ExpressionType::COMPARE_EQUAL);
			}
			operators.push_back(
			    make_uniq<JoinOrderOperator>(0, child_type, r1, r2, r1_r2, std::move(child_conditions)));
			operators.push_back(
			    make_uniq<JoinOrderOperator>(1, parent_type, r0, r1_r2, r0_r2, vector<JoinCondition>()));
			operators.back()->right_operators.push_back(*operators[0]);

			unordered_map<TableIndex, RelationIndex> relation_mapping;
			relation_mapping[TableIndex(0)] = RelationIndex(0);
			relation_mapping[TableIndex(1)] = RelationIndex(1);
			relation_mapping[TableIndex(2)] = RelationIndex(2);
			JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

			auto is_right_associative_commutative =
			    parent_type == JoinOrderOperatorType::INNER && child_type == JoinOrderOperatorType::INNER;
			INFO("parent type " << static_cast<int>(parent_type) << ", child type " << static_cast<int>(child_type));
			REQUIRE(operators[1]->total_set.get().count == (is_right_associative_commutative ? 2 : 3));
		}
	}
}

TEST_CASE("CD-C preserves explicit cross-product sides", "[optimizer][join_order]") {
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	auto &r2 = set_manager.GetJoinRelation(RelationIndex(2));
	auto &r0_r1 = set_manager.Union(r0, r1);
	auto &empty = set_manager.GetJoinRelation(unordered_set<RelationIndex>());

	vector<unique_ptr<JoinOrderOperator>> operators;
	operators.push_back(make_uniq<JoinOrderOperator>(0, JoinOrderOperatorType::CROSS_PRODUCT, r0_r1, r2, empty,
	                                                 vector<JoinCondition>()));
	const unordered_map<TableIndex, RelationIndex> relation_mapping;
	JoinOrderConflictDetector::Build(operators, set_manager, relation_mapping);

	REQUIRE_FALSE(JoinOrderConflictDetector::MustApplyAsOperator(*operators[0]));
	REQUIRE(JoinOrderConflictDetector::IsApplicable(*operators[0], r0, r2));
	REQUIRE(JoinOrderConflictDetector::IsApplicable(*operators[0], r2, r1));
	REQUIRE_FALSE(JoinOrderConflictDetector::IsApplicable(*operators[0], r0, r1));
}

TEST_CASE("Query graph distinguishes generated cross products", "[optimizer][join_order]") {
	JoinRelationSetManager set_manager;
	auto &r0 = set_manager.GetJoinRelation(RelationIndex(0));
	auto &r1 = set_manager.GetJoinRelation(RelationIndex(1));
	QueryGraphEdges graph;
	graph.CreateEdge(r0, r1, nullptr);
	graph.CreateEdge(r0, r1, nullptr, nullptr, true);

	auto connections = graph.GetConnections(r0, r1);
	REQUIRE(connections.size() == 2);
	REQUIRE(connections[0].get().generated_cross_product != connections[1].get().generated_cross_product);
}
