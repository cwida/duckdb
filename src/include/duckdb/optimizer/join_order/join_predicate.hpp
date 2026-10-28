//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_predicate.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/join_order/filter_info.hpp"
#include "duckdb/planner/column_binding_map.hpp"

namespace duckdb {

enum class JoinPredicateClass { INNER_EQUALITY, INNER_NON_EQUALITY, LEFT_JOIN, SEMI_ANTI_JOIN, OTHER };

struct JoinOrderUtil {
	//! Return the comparison type when the predicate expression itself is a comparison.
	static ExpressionType GetJoinPredicateComparisonType(const FilterInfo &filter);
	//! Classify the predicate for equivalence closure, denominator assembly and cost-model use.
	static JoinPredicateClass ClassifyJoinPredicate(const FilterInfo &filter);
	static bool HasAnyValidJoinBinding(const FilterInfo &filter);
	static bool HasValidJoinEndpoints(const FilterInfo &filter);
	static bool HasDisjointJoinEndpoints(const FilterInfo &filter);
	static RelationIndex GetBindingRelation(const ColumnBinding &binding);
};

class JoinPredicate {
public:
	JoinPredicate(idx_t index, FilterInfo &filter, JoinPredicateClass predicate_class,
	              ColumnBinding left_equality_binding, ColumnBinding right_equality_binding);

public:
	idx_t GetIndex() const;
	FilterInfo &GetFilter() const;
	JoinPredicateClass GetPredicateClass() const;
	JoinType GetJoinType() const;
	ExpressionType GetComparisonType() const;
	JoinRelationSet &GetSet() const;
	JoinRelationSet &GetLeftSet() const;
	JoinRelationSet &GetRightSet() const;
	optional_ptr<JoinRelationSet> GetLeftSetOptional() const;
	optional_ptr<JoinRelationSet> GetRightSetOptional() const;
	optional_ptr<JoinRelationSet> GetSemanticLeftSetOptional() const;
	optional_ptr<JoinRelationSet> GetSemanticRightSetOptional() const;
	const ColumnBinding &GetStatsBinding(bool left) const;
	const ColumnBinding &GetEqualityBinding(bool left) const;
	bool HasValidJoinEndpoints() const;
	bool HasDisjointJoinEndpoints() const;
	bool HasAnyValidStatsBinding() const;
	bool HasValidEqualityBindings() const;
	bool CanBuildEqualityClosure() const;
	bool CanBuildSelectivityDomain() const;
	bool IsEquivalencePredicate() const;
	void SetEqualityClassIndex(optional_idx equality_class_index);
	optional_idx GetEqualityClassIndex() const;
	void SetCompositeJoinPair(JoinRelationSet &pair);
	optional_ptr<JoinRelationSet> GetCompositeJoinPair() const;

private:
	idx_t index;
	reference<FilterInfo> filter;
	JoinPredicateClass predicate_class;
	ExpressionType comparison_type;
	ColumnBinding left_equality_binding;
	ColumnBinding right_equality_binding;
	optional_idx equality_class_index;
	optional_ptr<JoinRelationSet> composite_join_pair;
};

struct JoinEqualityPredicateEdge {
public:
	JoinEqualityPredicateEdge(JoinPredicate &predicate, RelationIndex left_relation, RelationIndex right_relation,
	                          ColumnBinding left_binding, ColumnBinding right_binding);

public:
	reference<JoinPredicate> predicate;
	RelationIndex left_relation;
	RelationIndex right_relation;
	ColumnBinding left_binding;
	ColumnBinding right_binding;
};

struct JoinEqualityClass {
	idx_t index = DConstants::INVALID_INDEX;
	column_binding_set_t columns;
	unordered_set<RelationIndex> relations;
	vector<JoinEqualityPredicateEdge> edges;
};

class JoinEqualityGroups {
public:
	void AddEquality(const ColumnBinding &left, const ColumnBinding &right);
	idx_t GetGroup(const ColumnBinding &binding);

private:
	idx_t GetOrCreateGroup(const ColumnBinding &binding);
	idx_t FindRoot(idx_t group);

private:
	column_binding_map_t<idx_t> binding_to_group;
	vector<idx_t> parents;
};

struct RelationPairEqualitySummary {
	void AddEquality(idx_t equality_class_index, ColumnBinding first_binding, ColumnBinding second_binding);
	bool HasDirectCompositeEquality() const;

	vector<idx_t> direct_equality_class_indices;
	column_binding_set_t first_relation_bindings;
	column_binding_set_t second_relation_bindings;
};

class JoinPredicateModel {
public:
	void Clear();
	JoinPredicate &RegisterPredicate(FilterInfo &filter, JoinPredicateClass predicate_class,
	                                 ColumnBinding left_equality_binding, ColumnBinding right_equality_binding);
	void AddEqualityClass(JoinEqualityClass equality_class);
	void AddDirectEqualityPairClass(JoinRelationSet &pair, idx_t equality_class_index, ColumnBinding first_binding,
	                                ColumnBinding second_binding);

	optional_ptr<const JoinPredicate> GetPredicateByFilterIndex(idx_t filter_index) const;
	const vector<reference<JoinPredicate>> &GetPredicates() const;
	const vector<reference<JoinPredicate>> &GetEqualityJoinPredicates() const;
	const vector<reference<JoinPredicate>> &GetSelectivityPredicates() const;
	const vector<reference<JoinPredicate>> &GetGraphPredicates() const;
	const vector<JoinEqualityClass> &GetEqualityClasses() const;
	bool HasLeftJoinPredicates() const;

	bool HasDirectCompositeEquality(JoinRelationSet &pair) const;

private:
	vector<unique_ptr<JoinPredicate>> predicates;
	vector<reference<JoinPredicate>> all_predicates;
	vector<reference<JoinPredicate>> equality_join_predicates;
	vector<reference<JoinPredicate>> selectivity_predicates;
	vector<reference<JoinPredicate>> graph_predicates;
	bool has_left_join_predicates = false;
	vector<JoinEqualityClass> equality_classes;
	reference_map_t<JoinRelationSet, RelationPairEqualitySummary> equality_pairs;
};

} // namespace duckdb
