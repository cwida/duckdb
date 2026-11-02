#include "duckdb/optimizer/join_order/join_predicate.hpp"

#include "duckdb/common/assert.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"

namespace duckdb {

ExpressionType JoinOrderUtil::GetJoinPredicateComparisonType(const FilterInfo &filter) {
	if (!filter.filter) {
		return ExpressionType::INVALID;
	}
	if (!BoundComparisonExpression::IsComparison(*filter.filter)) {
		return ExpressionType::INVALID;
	}
	return filter.filter->GetExpressionType();
}

JoinPredicateClass JoinOrderUtil::ClassifyJoinPredicate(const FilterInfo &filter) {
	switch (filter.join_type) {
	case JoinType::INNER: {
		const auto comparison_type = GetJoinPredicateComparisonType(filter);
		if (comparison_type == ExpressionType::COMPARE_EQUAL ||
		    comparison_type == ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
			return JoinPredicateClass::INNER_EQUALITY;
		}
		return comparison_type == ExpressionType::INVALID ? JoinPredicateClass::OTHER
		                                                  : JoinPredicateClass::INNER_NON_EQUALITY;
	}
	case JoinType::LEFT:
		return JoinPredicateClass::LEFT_JOIN;
	case JoinType::SEMI:
	case JoinType::ANTI:
		return JoinPredicateClass::SEMI_ANTI_JOIN;
	default:
		return JoinPredicateClass::OTHER;
	}
}

bool JoinOrderUtil::HasAnyValidJoinBinding(const FilterInfo &filter) {
	return filter.left_binding.table_index.IsValid() || filter.right_binding.table_index.IsValid();
}

bool JoinOrderUtil::HasValidJoinEndpoints(const FilterInfo &filter) {
	return filter.left_set && filter.right_set && !filter.left_set->Empty() && !filter.right_set->Empty() &&
	       filter.left_set != filter.right_set;
}

bool JoinOrderUtil::HasDisjointJoinEndpoints(const FilterInfo &filter) {
	if (!HasValidJoinEndpoints(filter)) {
		return false;
	}
	auto &left = *filter.left_set;
	auto &right = *filter.right_set;
	for (idx_t left_idx = 0; left_idx < left.count; left_idx++) {
		for (idx_t right_idx = 0; right_idx < right.count; right_idx++) {
			if (left.relations[left_idx] == right.relations[right_idx]) {
				return false;
			}
		}
	}
	return true;
}

RelationIndex JoinOrderUtil::GetBindingRelation(const ColumnBinding &binding) {
	D_ASSERT(binding.table_index.IsValid());
	return RelationIndex(binding.table_index.index);
}

JoinPredicate::JoinPredicate(idx_t index, FilterInfo &filter, JoinPredicateClass predicate_class,
                             ColumnBinding left_equality_binding, ColumnBinding right_equality_binding)
    : index(index), filter(filter), predicate_class(predicate_class),
      comparison_type(JoinOrderUtil::GetJoinPredicateComparisonType(filter)),
      left_equality_binding(left_equality_binding), right_equality_binding(right_equality_binding) {
}

idx_t JoinPredicate::GetIndex() const {
	return index;
}

FilterInfo &JoinPredicate::GetFilter() const {
	return filter.get();
}

JoinPredicateClass JoinPredicate::GetPredicateClass() const {
	return predicate_class;
}

JoinType JoinPredicate::GetJoinType() const {
	return filter.get().join_type;
}

ExpressionType JoinPredicate::GetComparisonType() const {
	return comparison_type;
}

JoinRelationSet &JoinPredicate::GetSet() const {
	return filter.get().set;
}

JoinRelationSet &JoinPredicate::GetLeftSet() const {
	D_ASSERT(filter.get().left_set);
	return *filter.get().left_set;
}

JoinRelationSet &JoinPredicate::GetRightSet() const {
	D_ASSERT(filter.get().right_set);
	return *filter.get().right_set;
}

optional_ptr<JoinRelationSet> JoinPredicate::GetLeftSetOptional() const {
	return filter.get().left_set;
}

optional_ptr<JoinRelationSet> JoinPredicate::GetRightSetOptional() const {
	return filter.get().right_set;
}

optional_ptr<JoinRelationSet> JoinPredicate::GetSemanticLeftSetOptional() const {
	return filter.get().semantic_left_set;
}

optional_ptr<JoinRelationSet> JoinPredicate::GetSemanticRightSetOptional() const {
	return filter.get().semantic_right_set;
}

const ColumnBinding &JoinPredicate::GetStatsBinding(bool left) const {
	return left ? filter.get().left_binding : filter.get().right_binding;
}

const ColumnBinding &JoinPredicate::GetEqualityBinding(bool left) const {
	return left ? left_equality_binding : right_equality_binding;
}

bool JoinPredicate::HasValidJoinEndpoints() const {
	return JoinOrderUtil::HasValidJoinEndpoints(filter.get());
}

bool JoinPredicate::HasDisjointJoinEndpoints() const {
	return JoinOrderUtil::HasDisjointJoinEndpoints(filter.get());
}

bool JoinPredicate::HasAnyValidStatsBinding() const {
	return JoinOrderUtil::HasAnyValidJoinBinding(filter.get());
}

bool JoinPredicate::HasValidEqualityBindings() const {
	return left_equality_binding.table_index.IsValid() && right_equality_binding.table_index.IsValid();
}

bool JoinPredicate::CanBuildEqualityClosure() const {
	return predicate_class == JoinPredicateClass::INNER_EQUALITY && HasValidEqualityBindings() &&
	       HasValidJoinEndpoints();
}

bool JoinPredicate::CanBuildSelectivityDomain() const {
	if (!HasValidJoinEndpoints() || !HasAnyValidStatsBinding()) {
		return false;
	}
	return predicate_class != JoinPredicateClass::INNER_EQUALITY || !CanBuildEqualityClosure();
}

bool JoinPredicate::IsEquivalencePredicate() const {
	return CanBuildEqualityClosure();
}

void JoinPredicate::SetEqualityClassIndex(optional_idx equality_class_index) {
	this->equality_class_index = equality_class_index;
	filter.get().edge_equivalence_index = equality_class_index;
}

optional_idx JoinPredicate::GetEqualityClassIndex() const {
	return equality_class_index;
}

void JoinPredicate::SetCompositeJoinPair(JoinRelationSet &pair) {
	composite_join_pair = pair;
}

optional_ptr<JoinRelationSet> JoinPredicate::GetCompositeJoinPair() const {
	return composite_join_pair;
}

JoinEqualityPredicateEdge::JoinEqualityPredicateEdge(JoinPredicate &predicate, RelationIndex left_relation,
                                                     RelationIndex right_relation, ColumnBinding left_binding,
                                                     ColumnBinding right_binding)
    : predicate(predicate), left_relation(left_relation), right_relation(right_relation), left_binding(left_binding),
      right_binding(right_binding) {
}

idx_t JoinEqualityGroups::GetOrCreateGroup(const ColumnBinding &binding) {
	auto entry = binding_to_group.emplace(binding, parents.size());
	if (entry.second) {
		parents.push_back(entry.first->second);
	}
	return entry.first->second;
}

idx_t JoinEqualityGroups::FindRoot(idx_t group) {
	while (parents[group] != group) {
		parents[group] = parents[parents[group]];
		group = parents[group];
	}
	return group;
}

void JoinEqualityGroups::AddEquality(const ColumnBinding &left, const ColumnBinding &right) {
	auto left_root = FindRoot(GetOrCreateGroup(left));
	auto right_root = FindRoot(GetOrCreateGroup(right));
	if (left_root != right_root) {
		parents[MaxValue(left_root, right_root)] = MinValue(left_root, right_root);
	}
}

idx_t JoinEqualityGroups::GetGroup(const ColumnBinding &binding) {
	auto entry = binding_to_group.find(binding);
	D_ASSERT(entry != binding_to_group.end());
	return FindRoot(entry->second);
}

void RelationPairEqualitySummary::AddEquality(idx_t equality_class_index, ColumnBinding first_binding,
                                              ColumnBinding second_binding) {
	if (std::find(direct_equality_class_indices.begin(), direct_equality_class_indices.end(), equality_class_index) ==
	    direct_equality_class_indices.end()) {
		direct_equality_class_indices.push_back(equality_class_index);
	}
	first_relation_bindings.insert(first_binding);
	second_relation_bindings.insert(second_binding);
}

bool RelationPairEqualitySummary::HasDirectCompositeEquality() const {
	return direct_equality_class_indices.size() >= 2 && first_relation_bindings.size() >= 2 &&
	       second_relation_bindings.size() >= 2;
}

void JoinPredicateModel::Clear() {
	predicates.clear();
	all_predicates.clear();
	equality_join_predicates.clear();
	selectivity_predicates.clear();
	graph_predicates.clear();
	has_left_join_predicates = false;
	equality_classes.clear();
	equality_pairs.clear();
}

JoinPredicate &JoinPredicateModel::RegisterPredicate(FilterInfo &filter, JoinPredicateClass predicate_class,
                                                     ColumnBinding left_equality_binding,
                                                     ColumnBinding right_equality_binding) {
	auto predicate = make_uniq<JoinPredicate>(predicates.size(), filter, predicate_class, left_equality_binding,
	                                          right_equality_binding);
	auto &result = *predicate;
	predicates.push_back(std::move(predicate));
	all_predicates.push_back(result);
	if (predicate_class == JoinPredicateClass::LEFT_JOIN) {
		has_left_join_predicates = true;
	}
	if (result.HasDisjointJoinEndpoints()) {
		graph_predicates.push_back(result);
	}
	if (result.CanBuildSelectivityDomain()) {
		selectivity_predicates.push_back(result);
	}
	return result;
}

void JoinPredicateModel::AddEqualityClass(JoinEqualityClass equality_class) {
	D_ASSERT(equality_class.index == equality_classes.size());
	for (auto &edge : equality_class.edges) {
		equality_join_predicates.push_back(edge.predicate);
	}
	equality_classes.push_back(std::move(equality_class));
}

void JoinPredicateModel::AddDirectEqualityPairClass(JoinRelationSet &pair, idx_t equality_class_index,
                                                    ColumnBinding first_binding, ColumnBinding second_binding) {
	equality_pairs[pair].AddEquality(equality_class_index, first_binding, second_binding);
}

optional_ptr<const JoinPredicate> JoinPredicateModel::GetPredicateByFilterIndex(idx_t filter_index) const {
	if (filter_index >= all_predicates.size()) {
		return nullptr;
	}
	auto &predicate = all_predicates[filter_index].get();
	D_ASSERT(predicate.GetIndex() == filter_index);
	if (predicate.GetFilter().filter_index != filter_index) {
		return nullptr;
	}
	return predicate;
}

const vector<reference<JoinPredicate>> &JoinPredicateModel::GetPredicates() const {
	return all_predicates;
}

const vector<reference<JoinPredicate>> &JoinPredicateModel::GetEqualityJoinPredicates() const {
	return equality_join_predicates;
}

const vector<reference<JoinPredicate>> &JoinPredicateModel::GetSelectivityPredicates() const {
	return selectivity_predicates;
}

const vector<reference<JoinPredicate>> &JoinPredicateModel::GetGraphPredicates() const {
	return graph_predicates;
}

const vector<JoinEqualityClass> &JoinPredicateModel::GetEqualityClasses() const {
	return equality_classes;
}

bool JoinPredicateModel::HasLeftJoinPredicates() const {
	return has_left_join_predicates;
}

bool JoinPredicateModel::HasDirectCompositeEquality(JoinRelationSet &pair) const {
	auto entry = equality_pairs.find(pair);
	if (entry == equality_pairs.end()) {
		return false;
	}
	return entry->second.HasDirectCompositeEquality();
}

} // namespace duckdb
