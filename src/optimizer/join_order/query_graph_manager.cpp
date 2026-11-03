#include "duckdb/optimizer/join_order/query_graph_manager.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/logical_operator.hpp"
#include "duckdb/planner/operator/list.hpp"

namespace duckdb {

GenerateJoinRelation::GenerateJoinRelation(optional_ptr<JoinRelationSet> set, unique_ptr<LogicalOperator> op_p,
                                           idx_t cardinality_p)
    : set(set), op(std::move(op_p)), cardinality(cardinality_p) {
}

QueryGraphManager::QueryGraphManager(ClientContext &context) : context(context), relation_manager(context) {
}

void QueryGraphManager::BuildPredicateModel() {
	predicate_model.Clear();

	JoinEqualityGroups equality_groups;

	// First union all normalized equality predicates. Do not assign edge ids in this pass,
	// because a later predicate can merge two previously separate components.
	for (auto &filter : filters_and_bindings) {
		filter->edge_equivalence_index = optional_idx();
		auto predicate_class = JoinOrderUtil::ClassifyJoinPredicate(*filter);
		ColumnBinding left_equality_binding;
		ColumnBinding right_equality_binding;
		if (BoundComparisonExpression::IsComparison(*filter->filter)) {
			auto &comparison = filter->filter->Cast<BoundFunctionExpression>();
			GetEquivalenceBinding(BoundComparisonExpression::Left(comparison), left_equality_binding);
			GetEquivalenceBinding(BoundComparisonExpression::Right(comparison), right_equality_binding);
		}
		auto &predicate =
		    predicate_model.RegisterPredicate(*filter, predicate_class, left_equality_binding, right_equality_binding);
		if (!predicate.CanBuildEqualityClosure()) {
			continue;
		}

		equality_groups.AddEquality(predicate.GetEqualityBinding(true), predicate.GetEqualityBinding(false));
	}

	// Then assign stable final ids to every equality edge using the final roots.
	unordered_map<idx_t, idx_t> root_to_equivalence_id;
	vector<vector<reference<JoinPredicate>>> equality_class_predicates;
	for (auto predicate_ref : predicate_model.GetPredicates()) {
		auto &predicate = predicate_ref.get();
		if (!predicate.CanBuildEqualityClosure()) {
			continue;
		}
		auto root = equality_groups.GetGroup(predicate.GetEqualityBinding(true));
		auto entry = root_to_equivalence_id.find(root);
		if (entry == root_to_equivalence_id.end()) {
			entry = root_to_equivalence_id.insert(make_pair(root, root_to_equivalence_id.size())).first;
			equality_class_predicates.emplace_back();
		}
		predicate.SetEqualityClassIndex(optional_idx(entry->second));
		equality_class_predicates[entry->second].push_back(predicate);
	}

	for (idx_t equality_class_index = 0; equality_class_index < equality_class_predicates.size();
	     equality_class_index++) {
		JoinEqualityClass equality_class;
		equality_class.index = equality_class_index;
		for (auto predicate_ref : equality_class_predicates[equality_class_index]) {
			auto &predicate = predicate_ref.get();
			auto &left_binding = predicate.GetEqualityBinding(true);
			auto &right_binding = predicate.GetEqualityBinding(false);
			auto left_relation = JoinOrderUtil::GetBindingRelation(left_binding);
			auto right_relation = JoinOrderUtil::GetBindingRelation(right_binding);
			equality_class.columns.insert(left_binding);
			equality_class.columns.insert(right_binding);
			equality_class.relations.insert(left_relation);
			equality_class.relations.insert(right_relation);
			equality_class.edges.emplace_back(predicate, left_relation, right_relation, left_binding, right_binding);
		}
		predicate_model.AddEqualityClass(std::move(equality_class));
	}

	for (auto &equality_class : predicate_model.GetEqualityClasses()) {
		for (auto &edge : equality_class.edges) {
			auto &left = set_manager.GetJoinRelation(edge.left_relation);
			auto &right = set_manager.GetJoinRelation(edge.right_relation);
			auto &pair = set_manager.Union(left, right);
			auto first_binding = edge.left_binding;
			auto second_binding = edge.right_binding;
			if (pair.relations[0] != edge.left_relation) {
				D_ASSERT(pair.relations[0] == edge.right_relation);
				first_binding = edge.right_binding;
				second_binding = edge.left_binding;
			}
			predicate_model.AddDirectEqualityPairClass(pair, equality_class.index, first_binding, second_binding);
		}
	}
	BuildLeftEqualityPairs();
}

void QueryGraphManager::BuildLeftEqualityPairs() {
	for (auto &op : join_operators) {
		if (op->type != JoinOrderOperatorType::LEFT) {
			continue;
		}
		JoinEqualityGroups groups;
		auto add_equality = [&](ExpressionType type, const Expression &left, const Expression &right) {
			if (type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
				return;
			}
			ColumnBinding left_binding;
			ColumnBinding right_binding;
			GetEquivalenceBinding(left, left_binding);
			GetEquivalenceBinding(right, right_binding);
			if (left_binding.table_index.IsValid() && right_binding.table_index.IsValid()) {
				groups.AddEquality(left_binding, right_binding);
			}
		};
		vector<reference<const Expression>> residuals;
		for (auto &condition : op->conditions) {
			if (condition.IsComparison()) {
				add_equality(condition.GetComparisonType(), condition.GetLHS(), condition.GetRHS());
			} else {
				residuals.push_back(condition.GetJoinExpression());
			}
		}
		// Same-side ON equalities can connect keys even though they are not costing predicates.
		while (!residuals.empty()) {
			auto &expression = residuals.back().get();
			residuals.pop_back();
			if (expression.GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
				for (auto &child : expression.Cast<BoundConjunctionExpression>().GetChildren()) {
					residuals.push_back(*child);
				}
			} else if (BoundComparisonExpression::IsComparison(expression)) {
				auto &comparison = expression.Cast<BoundFunctionExpression>();
				add_equality(expression.GetExpressionType(), BoundComparisonExpression::Left(comparison),
				             BoundComparisonExpression::Right(comparison));
			}
		}
		reference_map_t<JoinRelationSet, RelationPairEqualitySummary> summaries;
		vector<pair<reference<JoinPredicate>, reference<JoinRelationSet>>> pairs;
		for (auto predicate_index : op->costing_predicate_indices) {
			auto &predicate = predicate_model.GetPredicates()[predicate_index].get();
			auto type = predicate.GetComparisonType();
			if (!predicate.CanBuildSelectivityDomain() || !predicate.HasValidEqualityBindings() ||
			    (type != ExpressionType::COMPARE_EQUAL && type != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
				continue;
			}
			auto first_binding = predicate.GetEqualityBinding(true);
			auto second_binding = predicate.GetEqualityBinding(false);
			auto first_relation = JoinOrderUtil::GetBindingRelation(first_binding);
			auto second_relation = JoinOrderUtil::GetBindingRelation(second_binding);
			if (first_relation == second_relation) {
				continue;
			}
			auto &pair = set_manager.Union(set_manager.GetJoinRelation(first_relation),
			                               set_manager.GetJoinRelation(second_relation));
			if (pair.relations[0] != first_relation) {
				std::swap(first_binding, second_binding);
			}
			summaries[pair].AddEquality(groups.GetGroup(first_binding), first_binding, second_binding);
			pairs.emplace_back(predicate, pair);
		}
		for (auto &entry : pairs) {
			if (summaries.at(entry.second).HasDirectCompositeEquality()) {
				entry.first.get().SetCompositeJoinPair(entry.second);
			}
		}
	}
}

void QueryGraphManager::BuildInnerCompanionSets() {
	inner_companion_roots.resize(relation_manager.NumRelations());
	for (idx_t relation_idx = 0; relation_idx < inner_companion_roots.size(); relation_idx++) {
		inner_companion_roots[relation_idx] = relation_idx;
	}
	auto find_root = [&](idx_t relation_idx) {
		while (inner_companion_roots[relation_idx] != relation_idx) {
			inner_companion_roots[relation_idx] = inner_companion_roots[inner_companion_roots[relation_idx]];
			relation_idx = inner_companion_roots[relation_idx];
		}
		return relation_idx;
	};
	auto merge_set = [&](const JoinRelationSet &set) {
		if (set.count < 2) {
			return;
		}
		auto first_root = find_root(set.relations[0].index);
		for (idx_t set_idx = 1; set_idx < set.count; set_idx++) {
			auto next_root = find_root(set.relations[set_idx].index);
			if (first_root != next_root) {
				inner_companion_roots[MaxValue(first_root, next_root)] = MinValue(first_root, next_root);
				first_root = MinValue(first_root, next_root);
			}
		}
	};
	for (auto &op : join_operators) {
		if (op->type == JoinOrderOperatorType::INNER) {
			merge_set(op->syntactic_set);
		}
	}
	for (auto &filter : filters_and_bindings) {
		if (!filter->source_operator_index.IsValid() && filter->join_type == JoinType::INNER) {
			merge_set(filter->set);
		}
	}
	for (idx_t relation_idx = 0; relation_idx < inner_companion_roots.size(); relation_idx++) {
		inner_companion_roots[relation_idx] = find_root(relation_idx);
	}
}

bool QueryGraphManager::Build(JoinOrderOptimizer &optimizer, LogicalOperator &op) {
	// have the relation manager extract the join relations and create a reference list of all the
	// filter operators.
	auto can_reorder = relation_manager.ExtractJoinRelations(optimizer, op, filter_operators);
	auto num_relations = relation_manager.NumRelations();
	if (num_relations <= 1 || !can_reorder) {
		// nothing to optimize/reorder
		return false;
	}
	// extract the edges of the hypergraph, creating a list of filters and their associated bindings.
	auto extraction = relation_manager.ExtractEdges(op, filter_operators, set_manager);
	filters_and_bindings = std::move(extraction.filters);
	join_operators = std::move(extraction.join_operators);
	JoinOrderConflictDetector::Build(join_operators, set_manager, relation_manager.relation_mapping);
	for (auto &join_operator : join_operators) {
		for (auto predicate_index : join_operator->costing_predicate_indices) {
			filters_and_bindings[predicate_index]->semantic_left_set =
			    optional_ptr<JoinRelationSet>(join_operator->left_total_set.get());
			filters_and_bindings[predicate_index]->semantic_right_set =
			    optional_ptr<JoinRelationSet>(join_operator->right_total_set.get());
		}
		if (!JoinOrderConflictDetector::MustApplyAsOperator(*join_operator)) {
			continue;
		}
		for (auto predicate_index : join_operator->costing_predicate_indices) {
			auto inserted = operator_costing_predicates.insert(predicate_index);
			if (!inserted.second) {
				throw InternalException("Join-order predicate %llu belongs to multiple operator occurrences",
				                        predicate_index);
			}
		}
	}
	// Populate left/right endpoints and stats bindings before building the predicate model.
	BindFilterEndpoints();
	// Build the predicate model after BindFilterEndpoints so that left_binding/right_binding are populated.
	BuildPredicateModel();
	BuildInnerCompanionSets();
	// Create query_graph hyper edges from the normalized predicate model.
	CreateHyperGraphEdges();
	return true;
}

const JoinPredicateModel &QueryGraphManager::GetPredicateModel() const {
	return predicate_model;
}

void QueryGraphManager::GetColumnBinding(const Expression &root_expr, ColumnBinding &binding) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(
	    root_expr, [&](const BoundColumnRefExpression &colref) {
		    D_ASSERT(colref.Depth() == 0);
		    D_ASSERT(colref.Binding().table_index.IsValid());
		    // Map the logical output binding to the relation-local statistics column.
		    auto normalized = relation_manager.TryNormalizeBinding(colref.Binding(), binding);
		    D_ASSERT(normalized);
		    if (!normalized) {
			    return;
		    }
	    });
}

void QueryGraphManager::GetEquivalenceBinding(const Expression &expression, ColumnBinding &binding) {
	switch (expression.GetExpressionClass()) {
	case ExpressionClass::BOUND_COLUMN_REF: {
		auto &colref = expression.Cast<BoundColumnRefExpression>();
		D_ASSERT(colref.Depth() == 0);
		if (!colref.Binding().table_index.IsValid()) {
			return;
		}
		auto normalized = relation_manager.TryNormalizeBinding(colref.Binding(), binding);
		D_ASSERT(normalized);
		if (!normalized) {
			return;
		}
		return;
	}
	case ExpressionClass::BOUND_FUNCTION: {
		if (!BoundCastExpression::IsCast(expression)) {
			return;
		}
		auto &cast = expression.Cast<BoundFunctionExpression>();
		if (BoundCastExpression::IsTryCast(cast) ||
		    !BoundCastExpression::CastIsInvertible(BoundCastExpression::SourceType(cast), cast.GetReturnType())) {
			return;
		}
		GetEquivalenceBinding(BoundCastExpression::Child(cast), binding);
		return;
	}
	default:
		return;
	}
}

static unique_ptr<LogicalOperator> PushFilter(unique_ptr<LogicalOperator> node, unique_ptr<Expression> expr) {
	// push an expression into a filter
	// first check if we have any filter to push it into
	if (node->type != LogicalOperatorType::LOGICAL_FILTER) {
		// we don't, we need to create one
		auto filter = make_uniq<LogicalFilter>();
		filter->children.push_back(std::move(node));
		node = std::move(filter);
	}
	// push the filter into the LogicalFilter
	D_ASSERT(node->type == LogicalOperatorType::LOGICAL_FILTER);
	auto &filter = node->Cast<LogicalFilter>();
	filter.expressions.push_back(std::move(expr));
	return node;
}

static bool RelationSetsEqual(const JoinRelationSet &left, const JoinRelationSet &right) {
	return JoinRelationSet::IsSubset(left, right) && JoinRelationSet::IsSubset(right, left);
}

void QueryGraphManager::BindFilterEndpoints() {
	for (auto &filter_info : filters_and_bindings) {
		if (filter_info->must_remain_filter) {
			D_ASSERT(filter_info->from_logical_filter);
			D_ASSERT(!filter_info->left_set);
			D_ASSERT(!filter_info->right_set);
			continue;
		}
		auto &filter = filter_info->filter;
		// now check if it can be used as a join predicate
		if (BoundComparisonExpression::IsComparison(*filter)) {
			auto &comparison = filter->Cast<BoundFunctionExpression>();
			auto &left = BoundComparisonExpression::Left(comparison);
			auto &right = BoundComparisonExpression::Right(comparison);
			// extract the bindings that are required for the left and right side of the comparison
			unordered_set<RelationIndex> left_bindings, right_bindings;
			relation_manager.ExtractBindings(left, left_bindings);
			relation_manager.ExtractBindings(right, right_bindings);
			GetColumnBinding(left, filter_info->left_binding);
			GetColumnBinding(right, filter_info->right_binding);
			if (!left_bindings.empty() && !right_bindings.empty()) {
				// both the left and the right side have bindings
				// first create the relation sets, if they do not exist
				if (!filter_info->left_set) {
					filter_info->left_set = &set_manager.GetJoinRelation(left_bindings);
				}
				if (!filter_info->right_set) {
					filter_info->right_set = &set_manager.GetJoinRelation(right_bindings);
				}
				D_ASSERT(filter_info->left_set && filter_info->right_set);
				auto &condition_set = set_manager.Union(*filter_info->left_set, *filter_info->right_set);
				if (!RelationSetsEqual(filter_info->set.get(), condition_set)) {
					filter_info->SetLeftSet(nullptr);
					filter_info->SetRightSet(nullptr);
				}
			}
		} else if (filter->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
			auto &conjunction = filter->Cast<BoundConjunctionExpression>();
			if (conjunction.GetExpressionType() == ExpressionType::CONJUNCTION_OR) {
				continue;
			}
			if (filter_info->join_type == JoinType::INNER || filter_info->join_type == JoinType::INVALID) {
				continue;
			}
			unordered_set<RelationIndex> left_bindings, right_bindings;
			D_ASSERT(filter_info->left_set);
			D_ASSERT(filter_info->right_set);
			D_ASSERT(filter_info->join_type == JoinType::SEMI || filter_info->join_type == JoinType::ANTI);
			for (auto &child_comp : conjunction.GetChildren()) {
				if (!BoundComparisonExpression::IsComparison(*child_comp)) {
					continue;
				}
				auto &comparison = child_comp->Cast<BoundFunctionExpression>();
				auto &left = BoundComparisonExpression::Left(comparison);
				auto &right = BoundComparisonExpression::Right(comparison);
				// extract the bindings that are required for the left and right side of the comparison
				relation_manager.ExtractBindings(left, left_bindings);
				relation_manager.ExtractBindings(right, right_bindings);
				if (!filter_info->left_binding.table_index.IsValid() &&
				    !filter_info->left_binding.column_index.IsValid()) {
					GetColumnBinding(left, filter_info->left_binding);
				}
				if (!filter_info->right_binding.table_index.IsValid() &&
				    !filter_info->right_binding.column_index.IsValid()) {
					GetColumnBinding(right, filter_info->right_binding);
				}
			}
			if (!left_bindings.empty() && !right_bindings.empty()) {
				D_ASSERT(filter_info->left_set && filter_info->right_set);
			}
		}
	}
}

void QueryGraphManager::CreateHyperGraphEdges() {
	graph_component_roots.resize(relation_manager.NumRelations());
	for (idx_t relation_idx = 0; relation_idx < graph_component_roots.size(); relation_idx++) {
		graph_component_roots[relation_idx] = relation_idx;
	}

	vector<optional_ptr<JoinPredicate>> predicates_by_index(filters_and_bindings.size(), nullptr);
	for (auto predicate_ref : predicate_model.GetPredicates()) {
		auto &predicate = predicate_ref.get();
		D_ASSERT(predicate.GetIndex() < predicates_by_index.size());
		predicates_by_index[predicate.GetIndex()] = predicate;
	}

	// Predicates originating in LogicalFilters remain ordinary query-graph edges. Predicates carried by binary
	// operators are emitted below using the operator's CD-C eligibility sets.
	for (auto predicate_ref : predicate_model.GetGraphPredicates()) {
		auto &predicate = predicate_ref.get();
		D_ASSERT(predicate.GetLeftSetOptional() && predicate.GetRightSetOptional());
		auto operator_index = filters_and_bindings[predicate.GetIndex()]->source_operator_index;
		if (operator_index.IsValid()) {
			D_ASSERT(operator_index.GetIndex() < join_operators.size());
			if (JoinOrderConflictDetector::MustApplyAsOperator(*join_operators[operator_index.GetIndex()])) {
				continue;
			}
		}
		query_graph.CreateEdge(predicate.GetLeftSet(), predicate.GetRightSet(), predicate);
		query_graph.CreateEdge(predicate.GetRightSet(), predicate.GetLeftSet(), predicate);
		ConnectGraphComponents(predicate.GetLeftSet(), predicate.GetRightSet());
	}

	for (auto &operator_ptr : join_operators) {
		auto &op = *operator_ptr;
		if (op.type == JoinOrderOperatorType::CROSS_PRODUCT) {
			continue;
		}
		if (!JoinOrderConflictDetector::MustApplyAsOperator(op)) {
			D_ASSERT(op.type == JoinOrderOperatorType::INNER);
			// Filter pushdown can place an INNER predicate on an original operator boundary that does not match the
			// predicate's executable expression endpoints. If CD-C found no constraint, preserve DuckDB's existing
			// independently movable predicate edge. It was emitted above in the existing filter order; the descriptor
			// remains available for ancestor conflict detection.
			continue;
		}

		bool created_edge = false;
		for (auto predicate_index : op.costing_predicate_indices) {
			if (filters_and_bindings[predicate_index]->from_residual_predicate) {
				continue;
			}
			auto predicate = predicates_by_index[predicate_index];
			if (!predicate || !predicate->GetLeftSetOptional() || !predicate->GetRightSetOptional()) {
				continue;
			}
			query_graph.CreateEdge(op.left_total_set, op.right_total_set, *predicate, op);
			query_graph.CreateEdge(op.right_total_set, op.left_total_set, *predicate, op);
			ConnectGraphComponents(op.left_total_set, op.right_total_set);
			created_edge = true;
		}
		if (!created_edge) {
			query_graph.CreateEdge(op.left_total_set, op.right_total_set, nullptr, op);
			query_graph.CreateEdge(op.right_total_set, op.left_total_set, nullptr, op);
			ConnectGraphComponents(op.left_total_set, op.right_total_set);
		}
	}
}

static unique_ptr<LogicalOperator> ExtractJoinRelation(unique_ptr<SingleJoinRelation> &rel) {
	auto &children = rel->parent->children;
	for (idx_t i = 0; i < children.size(); i++) {
		if (children[i].get() == &rel->op) {
			// found it! take ownership o/**/f it from the parent
			auto result = std::move(children[i]);
			children.erase_at(i);
			return result;
		}
	}
	throw InternalException("Could not find relation in parent node (?)");
}

void QueryGraphManager::ClearExtractedExpressions() {
	for (auto &operator_ref : filter_operators) {
		auto &op = operator_ref.get();
		if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
		    op.type == LogicalOperatorType::LOGICAL_ASOF_JOIN) {
			op.Cast<LogicalComparisonJoin>().conditions.clear();
			continue;
		}
		vector<unique_ptr<Expression>> remaining_expressions;
		for (auto &expression : op.expressions) {
			unordered_set<RelationIndex> bindings;
			if (!relation_manager.ExtractFilterBindings(*expression, bindings)) {
				remaining_expressions.push_back(std::move(expression));
			}
		}
		op.expressions = std::move(remaining_expressions);
	}
}

unique_ptr<LogicalOperator> QueryGraphManager::Reconstruct(unique_ptr<LogicalOperator> plan,
                                                           const vector<RelationStats> &relation_stats) {
	// now we have to rewrite the plan
	bool root_is_join = plan->children.size() > 1;
	ClearExtractedExpressions();

	unordered_set<RelationIndex> bindings;
	for (idx_t i = 0; i < relation_manager.NumRelations(); i++) {
		bindings.emplace(i);
	}
	auto &total_relation = set_manager.GetJoinRelation(bindings);

	// first we will extract all relations from the main plan
	vector<unique_ptr<LogicalOperator>> extracted_relations;
	extracted_relations.reserve(relation_manager.NumRelations());
	for (auto &relation : relation_manager.GetRelations()) {
		extracted_relations.push_back(ExtractJoinRelation(relation));
	}

	// now we generate the actual joins
	auto join_tree = GenerateJoins(extracted_relations, total_relation, relation_stats);
	for (auto &join_operator : join_operators) {
		if (JoinOrderConflictDetector::MustApplyAsOperator(*join_operator) &&
		    !reconstructed_operators.count(join_operator->index)) {
			throw InternalException("Join-order optimizer did not reconstruct operator occurrence %llu",
			                        join_operator->index);
		}
	}

	// perform the final pushdown of remaining filters
	for (auto &filter : filters_and_bindings) {
		if (operator_costing_predicates.count(filter->filter_index)) {
			if (filter->filter) {
				throw InternalException("Cost predicate %llu escaped operator reconstruction", filter->filter_index);
			}
			continue;
		}
		// check if the filter has already been extracted
		if (filter->filter) {
			// if not we need to push it
			join_tree.op = PushFilter(std::move(join_tree.op), std::move(filter->filter));
		}
	}
	join_tree.op->SetEstimatedCardinality(join_tree.cardinality);

	// find the first join in the relation to know where to place this node
	if (root_is_join) {
		// first node is the join, return it immediately
		return std::move(join_tree.op);
	}
	D_ASSERT(plan->children.size() == 1);
	// have to move up through the relations
	auto op = plan.get();
	auto parent = plan.get();
	while (op->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT &&
	       op->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
	       op->type != LogicalOperatorType::LOGICAL_ASOF_JOIN) {
		D_ASSERT(op->children.size() == 1);
		parent = op;
		op = op->children[0].get();
	}
	// have to replace at this node
	parent->children[0] = std::move(join_tree.op);
	return plan;
}

static JoinCondition MaybeInvertConditions(unique_ptr<Expression> condition, bool invert) {
	auto &comparison = condition->Cast<BoundFunctionExpression>();
	auto &left_ref = BoundComparisonExpression::LeftMutable(comparison);
	auto &right_ref = BoundComparisonExpression::RightMutable(comparison);
	unique_ptr<Expression> left, right;
	if (!invert) {
		left = std::move(left_ref);
		right = std::move(right_ref);
	} else {
		left = std::move(right_ref);
		right = std::move(left_ref);
	}
	auto comp_type = condition->GetExpressionType();
	if (invert) {
		// reverse comparison expression if we reverse the order of the children
		comp_type = FlipComparisonExpression(comp_type);
	}
	return JoinCondition(std::move(left), std::move(right), comp_type);
}

static bool ShouldInvertJoinCondition(JoinRelationSetManager &set_manager, GenerateJoinRelation &left,
                                      GenerateJoinRelation &right, FilterInfo &filter, bool fallback_invert) {
	if (!filter.left_binding.table_index.IsValid()) {
		return fallback_invert;
	}
	auto &condition_left_set = set_manager.GetJoinRelation(RelationIndex(filter.left_binding.table_index.index));
	if (JoinRelationSet::IsSubset(*right.set, condition_left_set)) {
		return true;
	}
	if (JoinRelationSet::IsSubset(*left.set, condition_left_set)) {
		return false;
	}
	return fallback_invert;
}

static JoinType GetJoinType(JoinOrderOperatorType type) {
	switch (type) {
	case JoinOrderOperatorType::INNER:
		return JoinType::INNER;
	case JoinOrderOperatorType::LEFT:
		return JoinType::LEFT;
	case JoinOrderOperatorType::SEMI:
		return JoinType::SEMI;
	case JoinOrderOperatorType::ANTI:
		return JoinType::ANTI;
	default:
		throw InternalException("Operator occurrence type %d has no comparison join type", static_cast<int>(type));
	}
}

static bool ContainsRelation(const JoinRelationSet &set, const ColumnBinding &binding) {
	if (!binding.table_index.IsValid()) {
		return false;
	}
	for (idx_t set_idx = 0; set_idx < set.count; set_idx++) {
		if (set.relations[set_idx].index == binding.table_index.index) {
			return true;
		}
	}
	return false;
}

static optional_ptr<const RelationColumnStats> GetSelectedJoinColumn(const vector<RelationStats> &relation_stats,
                                                                     const ColumnBinding &binding) {
	if (!binding.table_index.IsValid() || !binding.column_index.IsValid() ||
	    binding.table_index.index >= relation_stats.size()) {
		return nullptr;
	}
	auto &relation = relation_stats[binding.table_index.index];
	auto column_index = binding.column_index.GetIndex();
	if (column_index >= relation.columns.size()) {
		return nullptr;
	}
	return relation.columns[column_index];
}

static optional<SemiAntiJoinCardinalityEstimate>
EstimateSelectedSemiAntiJoin(const JoinOrderOperator &join_operator, const GenerateJoinRelation &left,
                             const GenerateJoinRelation &right, const JoinPredicateModel &predicate_model,
                             const vector<RelationStats> &relation_stats) {
	if (join_operator.type != JoinOrderOperatorType::SEMI && join_operator.type != JoinOrderOperatorType::ANTI) {
		return {};
	}
	if (!RelationSetsEqual(*left.set, join_operator.left_total_set) ||
	    !RelationSetsEqual(*right.set, join_operator.right_total_set)) {
		return {};
	}
	auto join_type = GetJoinType(join_operator.type);
	if (right.set->count != 1) {
		return {};
	}

	vector<SemiAntiJoinDomain> domains;
	for (auto predicate_index : join_operator.costing_predicate_indices) {
		auto predicate = predicate_model.GetPredicateByFilterIndex(predicate_index);
		if (!predicate || predicate->GetJoinType() != join_type || !predicate->HasValidEqualityBindings() ||
		    (predicate->GetComparisonType() != ExpressionType::COMPARE_EQUAL &&
		     predicate->GetComparisonType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM)) {
			return {};
		}
		auto semantic_left = predicate->GetSemanticLeftSetOptional();
		auto semantic_right = predicate->GetSemanticRightSetOptional();
		if (!semantic_left || !semantic_right || !RelationSetsEqual(*semantic_left, join_operator.left_total_set) ||
		    !RelationSetsEqual(*semantic_right, join_operator.right_total_set)) {
			return {};
		}

		auto preserved_binding = predicate->GetEqualityBinding(true);
		auto rhs_binding = predicate->GetEqualityBinding(false);
		auto lhs_is_rhs = ContainsRelation(*right.set, preserved_binding);
		auto rhs_is_rhs = ContainsRelation(*right.set, rhs_binding);
		if (lhs_is_rhs == rhs_is_rhs) {
			return {};
		}
		if (lhs_is_rhs) {
			std::swap(preserved_binding, rhs_binding);
		}
		if (!ContainsRelation(*left.set, preserved_binding)) {
			return {};
		}
		auto preserved_column = GetSelectedJoinColumn(relation_stats, preserved_binding);
		auto rhs_column = GetSelectedJoinColumn(relation_stats, rhs_binding);
		if (!preserved_column || !rhs_column) {
			return {};
		}
		auto rhs_current_domain = rhs_column->GetSupportedSemiAntiDomainSize();
		auto total_domain =
		    RelationStatisticsHelper::SelectTotalDomain({preserved_column->total_domain, rhs_column->total_domain});
		if (!rhs_current_domain.IsValid() || !total_domain || total_domain->distinct_count == 0) {
			return {};
		}
		bool seen_domain = false;
		for (auto &domain : domains) {
			if (domain.preserved_binding == preserved_binding && domain.rhs_binding == rhs_binding) {
				seen_domain = true;
				break;
			}
		}
		if (!seen_domain) {
			domains.push_back(
			    {total_domain->distinct_count, rhs_current_domain.GetIndex(), preserved_binding, rhs_binding});
		}
	}
	if (domains.empty()) {
		return {};
	}

	auto rhs_relation_index = right.set->relations[0].index;
	if (rhs_relation_index >= relation_stats.size()) {
		return {};
	}
	return RelationStatisticsHelper::EstimateSemiAntiJoinCardinality(
	    static_cast<double>(left.cardinality), right.cardinality, relation_stats[rhs_relation_index].row_retention,
	    join_type, domains, false);
}

static void OrientInnerConditions(RelationManager &relation_manager, JoinRelationSetManager &set_manager,
                                  vector<JoinCondition> &conditions, const JoinRelationSet &left,
                                  const JoinRelationSet &right) {
	for (auto &condition : conditions) {
		if (!condition.IsComparison()) {
			continue;
		}
		unordered_set<RelationIndex> left_bindings;
		unordered_set<RelationIndex> right_bindings;
		relation_manager.ExtractBindings(condition.GetLHS(), left_bindings);
		relation_manager.ExtractBindings(condition.GetRHS(), right_bindings);
		if (left_bindings.empty() || right_bindings.empty()) {
			continue;
		}
		auto &condition_left = set_manager.GetJoinRelation(left_bindings);
		auto &condition_right = set_manager.GetJoinRelation(right_bindings);
		if (JoinRelationSet::IsSubset(right, condition_left) && JoinRelationSet::IsSubset(left, condition_right)) {
			condition.Swap();
		}
	}
}

GenerateJoinRelation QueryGraphManager::GenerateJoins(vector<unique_ptr<LogicalOperator>> &extracted_relations,
                                                      JoinRelationSet &set,
                                                      const vector<RelationStats> &relation_stats) {
	optional_ptr<JoinRelationSet> left_node;
	optional_ptr<JoinRelationSet> right_node;
	optional_ptr<JoinRelationSet> result_relation;
	unique_ptr<LogicalOperator> result_operator;

	auto dp_entry = plans->find(set);
	if (dp_entry == plans->end()) {
		throw InternalException("Join Order Optimizer Error: No full plan was created");
	}
	auto &node = dp_entry->second;
	auto result_cardinality = node->cardinality;
	if (!dp_entry->second->is_leaf) {
		// generate the left and right children
		auto left = GenerateJoins(extracted_relations, node->left_set, relation_stats);
		auto right = GenerateJoins(extracted_relations, node->right_set, relation_stats);
		auto descriptor = node->join_operator;
		if (descriptor) {
			D_ASSERT(descriptor->type == JoinOrderOperatorType::CROSS_PRODUCT ||
			         JoinOrderConflictDetector::MustApplyAsOperator(*descriptor));
			bool direct;
			bool inverted;
			if (descriptor->type == JoinOrderOperatorType::CROSS_PRODUCT) {
				direct = JoinRelationSet::Intersects(*left.set, descriptor->left_relations) &&
				         JoinRelationSet::Intersects(*right.set, descriptor->right_relations);
				inverted = JoinRelationSet::Intersects(*left.set, descriptor->right_relations) &&
				           JoinRelationSet::Intersects(*right.set, descriptor->left_relations);
			} else {
				direct = JoinRelationSet::IsSubset(*left.set, descriptor->left_total_set) &&
				         JoinRelationSet::IsSubset(*right.set, descriptor->right_total_set);
				inverted = JoinOrderConflictDetector::IsCommutative(descriptor->type) &&
				           JoinRelationSet::IsSubset(*left.set, descriptor->right_total_set) &&
				           JoinRelationSet::IsSubset(*right.set, descriptor->left_total_set);
			}
			if (!direct && !inverted) {
				throw InternalException("Could not orient operator occurrence %llu in reconstructed join tree",
				                        descriptor->index);
			}
			if (descriptor->type != JoinOrderOperatorType::CROSS_PRODUCT &&
			    !reconstructed_operators.insert(descriptor->index).second) {
				throw InternalException("Operator occurrence %llu was reconstructed more than once", descriptor->index);
			}
			auto semi_anti_estimate =
			    EstimateSelectedSemiAntiJoin(*descriptor, left, right, predicate_model, relation_stats);
			if (semi_anti_estimate && semi_anti_estimate->used_domain_evidence) {
				result_cardinality = LossyNumericCast<idx_t>(semi_anti_estimate->cardinality);
			}

			for (auto predicate_index : descriptor->costing_predicate_indices) {
				auto &filter = filters_and_bindings.at(predicate_index);
				if (!filter->filter) {
					throw InternalException("Cost predicate %llu was consumed before operator occurrence %llu",
					                        predicate_index, descriptor->index);
				}
				filter->filter.reset();
			}
			if (descriptor->type == JoinOrderOperatorType::CROSS_PRODUCT) {
				result_operator = LogicalCrossProduct::Create(std::move(left.op), std::move(right.op));
			} else {
				auto join = make_uniq<LogicalComparisonJoin>(GetJoinType(descriptor->type));
				join->children.push_back(std::move(left.op));
				join->children.push_back(std::move(right.op));
				join->conditions = std::move(descriptor->conditions);
				if (descriptor->type == JoinOrderOperatorType::INNER) {
					OrientInnerConditions(relation_manager, set_manager, join->conditions, *left.set, *right.set);
				}
				if (join->conditions.empty()) {
					throw InternalException("Operator occurrence %llu has no conditions", descriptor->index);
				}
				result_operator = std::move(join);
			}
		} else if (node->predicates.empty()) {
			// no filters, create a cross product
			result_operator = LogicalCrossProduct::Create(std::move(left.op), std::move(right.op));
		} else {
			// we have filters, create a join node
			auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
			// Here we optimize build side probe side. Our build side is the right side
			// So the right plans should have lower cardinalities.
			join->children.push_back(std::move(left.op));
			join->children.push_back(std::move(right.op));

			// set the join conditions from the join node
			for (auto predicate_ref : node->predicates) {
				auto f = &predicate_ref.get().GetFilter();
				// extract the filter from the operator it originally belonged to
				auto &filter_and_binding = filters_and_bindings.at(f->filter_index);
				D_ASSERT(filter_and_binding->filter);
				// now create the actual join condition
				D_ASSERT((JoinRelationSet::IsSubset(*left.set, *f->left_set) &&
				          JoinRelationSet::IsSubset(*right.set, *f->right_set)) ||
				         (JoinRelationSet::IsSubset(*left.set, *f->right_set) &&
				          JoinRelationSet::IsSubset(*right.set, *f->left_set)));

				bool invert_children = !JoinRelationSet::IsSubset(*left.set, *f->left_set);

				// If the left and right set are inverted for LEFT/SEMI/ANTI joins then swap them back
				// and set invert = false. This is to preserve left/rightedness of relations
				if (invert_children && (f->join_type == JoinType::LEFT || f->join_type == JoinType::SEMI ||
				                        f->join_type == JoinType::ANTI)) {
					std::swap(join->children[0], join->children[1]);
					std::swap(left, right);
					invert_children = false;
				}
				auto condition = std::move(filter_and_binding->filter);
				if (BoundComparisonExpression::IsComparison(*condition)) {
					auto invert = ShouldInvertJoinCondition(set_manager, left, right, *f, invert_children);
					auto cond = MaybeInvertConditions(std::move(condition), invert);
					join->conditions.push_back(std::move(cond));
				} else if (condition->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION) {
					auto &conjunction = condition->Cast<BoundConjunctionExpression>();
					for (auto &child : conjunction.GetChildrenMutable()) {
						D_ASSERT(BoundComparisonExpression::IsComparison(*child));
						auto cond = MaybeInvertConditions(std::move(child), invert_children);
						join->conditions.push_back(std::move(cond));
					}
				}
			}
			D_ASSERT(!join->conditions.empty());
			result_operator = std::move(join);
		}
		left_node = left.set;
		right_node = right.set;
		result_relation = &set_manager.Union(*left.set, *right.set);
	} else {
		// base node, get the entry from the list of extracted relations
		D_ASSERT(node->set.count == 1);
		D_ASSERT(extracted_relations[node->set.relations[0].index]);
		result_relation = &node->set;
		result_operator = std::move(extracted_relations[result_relation->relations[0].index]);
	}
	// TODO: this is where estimated properties start coming into play.
	//  when creating the result operator, we should ask the cost model and cardinality estimator what
	//  the cost and cardinality are
	result_operator->estimated_cardinality = result_cardinality;
	result_operator->has_estimated_cardinality = true;

	// Collect unused residual predicates that belong to this join. Semantic non-inner joins own their complete
	// condition directly, so only ordinary (notably INNER) residual predicates reach this path. Predicates that
	// belong to a must-apply operator occurrence are consumed by that occurrence during reconstruction, instead
	// of being added to unused_residual_predicates.
	vector<unique_ptr<Expression>> unused_residual_predicates;
	for (auto &filter_info : filters_and_bindings) {
		if (filter_info->from_residual_predicate && filters_and_bindings[filter_info->filter_index]->filter &&
		    !operator_costing_predicates.count(filter_info->filter_index) && filter_info->set.get().count > 0 &&
		    JoinRelationSet::IsSubset(*result_relation, filter_info->set)) {
			unused_residual_predicates.push_back(std::move(filters_and_bindings[filter_info->filter_index]->filter));
		}
	}
	if (!unused_residual_predicates.empty()) {
		unique_ptr<Expression> combined = std::move(unused_residual_predicates[0]);
		for (idx_t i = 1; i < unused_residual_predicates.size(); i++) {
			combined = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(combined),
			                                                 std::move(unused_residual_predicates[i]));
		}
		if (result_operator->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN &&
		    result_operator->Cast<LogicalComparisonJoin>().join_type == JoinType::INNER) {
			result_operator->Cast<LogicalComparisonJoin>().conditions.emplace_back(std::move(combined));
		} else {
			result_operator = PushFilter(std::move(result_operator), std::move(combined));
		}
	}

	// check if we should do a pushdown on this node
	// basically, any remaining filter that is a subset of the current relation will no longer be used in joins
	// hence we should push it here
	for (auto &filter_info : filters_and_bindings) {
		// check if the filter has already been extracted
		auto &info = *filter_info;
		if (operator_costing_predicates.count(info.filter_index)) {
			continue;
		}
		if (filters_and_bindings[info.filter_index]->filter) {
			if (info.from_residual_predicate) {
				continue;
			}
			if (info.must_remain_at_root) {
				// the filter carries a barrier - it is only placed once the full join tree is built
				continue;
			}
			// now check if the filter is a subset of the current relation
			// note that infos with an empty relation set are a special case and we do not push them down
			if (info.join_type == JoinType::LEFT) {
				// any left join is most definitely a filter that joins two relations,
				// so do not push the filter preemptively here
				continue;
			}
			if (info.set.get().count > 0 && JoinRelationSet::IsSubset(*result_relation, info.set)) {
				auto &filter_and_binding = filters_and_bindings[info.filter_index];
				auto filter = std::move(filter_and_binding->filter);
				// if it is, we can push the filter
				// we can push it either into a join or as a filter
				// check if we are in a join or in a base table
				if (!left_node || !info.left_set) {
					// base table or non-comparison expression, push it as a filter
					result_operator = PushFilter(std::move(result_operator), std::move(filter));
					continue;
				}
				// the node below us is a join or cross product and the expression is a comparison
				// check if the nodes can be split up into left/right
				bool found_subset = false;
				bool invert = false;
				if (JoinRelationSet::IsSubset(*left_node, *info.left_set) &&
				    JoinRelationSet::IsSubset(*right_node, *info.right_set)) {
					found_subset = true;
				} else if (JoinRelationSet::IsSubset(*right_node, *info.left_set) &&
				           JoinRelationSet::IsSubset(*left_node, *info.right_set)) {
					invert = true;
					found_subset = true;
				}
				if (!found_subset) {
					// could not be split up into left/right
					result_operator = PushFilter(std::move(result_operator), std::move(filter));
					continue;
				}
				// Ordinary predicates have INNER semantics. They can become join conditions only when the current
				// operator is an INNER join (or a cross product that can be converted into one).
				auto join_node = result_operator.get();
				if (join_node->type == LogicalOperatorType::LOGICAL_FILTER) {
					join_node = join_node->children[0].get();
				}
				if (join_node->type != LogicalOperatorType::LOGICAL_CROSS_PRODUCT &&
				    (join_node->type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN ||
				     join_node->Cast<LogicalComparisonJoin>().join_type != JoinType::INNER)) {
					result_operator = PushFilter(std::move(result_operator), std::move(filter));
					continue;
				}
				// create the join condition
				D_ASSERT(BoundComparisonExpression::IsComparison(*filter));
				auto &comparison = filter->Cast<BoundFunctionExpression>();
				auto &left_ref = BoundComparisonExpression::LeftMutable(comparison);
				auto &right_ref = BoundComparisonExpression::RightMutable(comparison);
				// we need to figure out which side is which by looking at the relations available to us
				unique_ptr<Expression> left, right;
				if (!invert) {
					left = std::move(left_ref);
					right = std::move(right_ref);
				} else {
					left = std::move(right_ref);
					right = std::move(left_ref);
				}
				auto comp_type = comparison.GetExpressionType();
				if (invert) {
					// reverse comparison expression if we reverse the order of the children
					comp_type = FlipComparisonExpression(comp_type);
				}
				JoinCondition cond(std::move(left), std::move(right), comp_type);
				// now find the join to push it into
				if (join_node->type == LogicalOperatorType::LOGICAL_CROSS_PRODUCT) {
					// turn into comparison join
					auto comp_join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
					comp_join->children.push_back(std::move(join_node->children[0]));
					comp_join->children.push_back(std::move(join_node->children[1]));
					comp_join->conditions.push_back(std::move(cond));
					if (join_node == result_operator.get()) {
						result_operator = std::move(comp_join);
					} else {
						D_ASSERT(result_operator->type == LogicalOperatorType::LOGICAL_FILTER);
						result_operator->children[0] = std::move(comp_join);
					}
				} else {
					D_ASSERT(join_node->type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN);
					auto &comp_join = join_node->Cast<LogicalComparisonJoin>();
					comp_join.conditions.push_back(std::move(cond));
				}
			}
		}
	}
	result_operator->SetEstimatedCardinality(result_cardinality);
	auto result = GenerateJoinRelation(result_relation, std::move(result_operator), result_cardinality);
	return result;
}

const QueryGraphEdges &QueryGraphManager::GetQueryGraphEdges() const {
	return query_graph;
}

void QueryGraphManager::CreateQueryGraphCrossProduct(JoinRelationSet &left, JoinRelationSet &right) {
	query_graph.CreateEdge(left, right, nullptr, nullptr, true);
	query_graph.CreateEdge(right, left, nullptr, nullptr, true);
}

idx_t QueryGraphManager::FindGraphComponent(RelationIndex relation) {
	auto component = relation.index;
	D_ASSERT(component < graph_component_roots.size());
	while (graph_component_roots[component] != component) {
		graph_component_roots[component] = graph_component_roots[graph_component_roots[component]];
		component = graph_component_roots[component];
	}
	return component;
}

idx_t QueryGraphManager::GetGraphComponent(RelationIndex relation) const {
	auto component = relation.index;
	D_ASSERT(component < graph_component_roots.size());
	while (graph_component_roots[component] != component) {
		component = graph_component_roots[component];
	}
	return component;
}

void QueryGraphManager::ConnectGraphComponents(const JoinRelationSet &left, const JoinRelationSet &right) {
	D_ASSERT(!left.Empty() && !right.Empty());
	auto root = FindGraphComponent(left.relations[0]);
	auto connect = [&](const JoinRelationSet &relations) {
		for (idx_t relation_idx = 0; relation_idx < relations.count; relation_idx++) {
			auto next_root = FindGraphComponent(relations.relations[relation_idx]);
			if (root == next_root) {
				continue;
			}
			graph_component_roots[MaxValue(root, next_root)] = MinValue(root, next_root);
			root = MinValue(root, next_root);
		}
	};
	connect(left);
	connect(right);
}

bool QueryGraphManager::ActivateRequiredCrossProducts() {
	if (required_cross_products_activated || Settings::Get<DebugForceNoCrossProductSetting>(context)) {
		return false;
	}
	required_cross_products_activated = true;

	bool added_edge = false;
	// Hypergraph connectivity does not guarantee that an operator's input sets can be assembled.
	// On a stall, admit the original cross products and let the eligibility checks enforce their legality.
	for (auto &operator_ptr : join_operators) {
		auto &op = *operator_ptr;
		if (op.type != JoinOrderOperatorType::CROSS_PRODUCT) {
			continue;
		}
		D_ASSERT(!op.left_relations.get().Empty() && !op.right_relations.get().Empty());
		for (idx_t left_idx = 0; left_idx < op.left_relations.get().count; left_idx++) {
			auto &left = set_manager.GetJoinRelation(op.left_relations.get().relations[left_idx]);
			for (idx_t right_idx = 0; right_idx < op.right_relations.get().count; right_idx++) {
				auto &right = set_manager.GetJoinRelation(op.right_relations.get().relations[right_idx]);
				query_graph.CreateEdge(left, right, nullptr, op);
				query_graph.CreateEdge(right, left, nullptr, op);
			}
		}
		ConnectGraphComponents(op.left_relations, op.right_relations);
		added_edge = true;
	}
	return added_edge;
}

bool QueryGraphManager::RequiresCrossProduct() const {
	D_ASSERT(!graph_component_roots.empty());
	auto root = GetGraphComponent(RelationIndex(0));
	for (idx_t relation_idx = 1; relation_idx < graph_component_roots.size(); relation_idx++) {
		if (GetGraphComponent(RelationIndex(relation_idx)) != root) {
			return true;
		}
	}
	return false;
}

bool QueryGraphManager::IsConnectionApplicable(const NeighborInfo &connection, const JoinRelationSet &left,
                                               const JoinRelationSet &right) const {
	if ((connection.generated_cross_product ||
	     (connection.join_operator && connection.join_operator->type == JoinOrderOperatorType::CROSS_PRODUCT)) &&
	    Settings::Get<DebugForceNoCrossProductSetting>(context)) {
		return false;
	}
	if (!connection.join_operator) {
		return true;
	}
	return JoinOrderConflictDetector::IsApplicable(*connection.join_operator, left, right);
}

bool QueryGraphManager::IsJoinOrderCandidate(optional_ptr<JoinOrderOperator> selected_operator,
                                             bool generated_cross_product, JoinRelationSet &left,
                                             JoinRelationSet &right) {
	if (generated_cross_product && !CanCreateCrossProduct(left, right)) {
		return false;
	}
	auto &combined = set_manager.Union(left, right);
	if (selected_operator && (!JoinOrderConflictDetector::IsApplicable(*selected_operator, left, right) ||
	                          JoinOrderConflictDetector::IsCompletedBy(*selected_operator, left) ||
	                          JoinOrderConflictDetector::IsCompletedBy(*selected_operator, right) ||
	                          !JoinOrderConflictDetector::IsCompletedBy(*selected_operator, combined))) {
		return false;
	}
	for (auto &operator_ptr : join_operators) {
		auto &op = *operator_ptr;
		if (JoinOrderConflictDetector::IsCompletedBy(op, left) || JoinOrderConflictDetector::IsCompletedBy(op, right) ||
		    !JoinOrderConflictDetector::IsCompletedBy(op, combined)) {
			continue;
		}
		if (op.type == JoinOrderOperatorType::CROSS_PRODUCT) {
			if (!JoinOrderConflictDetector::IsApplicable(op, left, right)) {
				return false;
			}
		} else if (JoinOrderConflictDetector::MustApplyAsOperator(op) && selected_operator != &op) {
			return false;
		}
	}
	return true;
}

bool QueryGraphManager::CanCreateCrossProduct(const JoinRelationSet &left, const JoinRelationSet &right) const {
	if (Settings::Get<DebugForceNoCrossProductSetting>(context) || left.Empty() || right.Empty() ||
	    inner_companion_roots.empty()) {
		return false;
	}
	auto root = inner_companion_roots[left.relations[0].index];
	for (idx_t left_idx = 0; left_idx < left.count; left_idx++) {
		if (inner_companion_roots[left.relations[left_idx].index] != root) {
			return false;
		}
	}
	for (idx_t right_idx = 0; right_idx < right.count; right_idx++) {
		if (inner_companion_roots[right.relations[right_idx].index] != root) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb
