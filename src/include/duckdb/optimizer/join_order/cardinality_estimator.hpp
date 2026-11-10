//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/cardinality_estimator.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/reference_map.hpp"
#include "duckdb/optimizer/join_order/cardinality_value.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

namespace duckdb {

class FilterInfo;
class JoinPredicateModel;
struct CardinalityEstimatorState;
struct CompositeJoinPairStats;
struct SelectivityState;
struct FilterInfoWithTotalDomains;
struct SelectivitySubgraph;

class CardinalityEstimator {
public:
	CardinalityEstimator(JoinRelationSetManager &set_manager, const JoinPredicateModel &predicate_model);
	~CardinalityEstimator();

public:
	//! Initialize immutable logical statistics before estimating any relation set.
	void Initialize(const vector<RelationStats> &relations,
	                const vector<unique_ptr<JoinOrderOperator>> &operators = {});
	//! All equivalent plans in a memo group share this estimate.
	CardinalityValue EstimateCardinality(JoinRelationSet &set);

private:
	void InitEquivalentRelations();
	void UpdateTotalDomains(RelationIndex relation, const RelationStats &stats);
	void RestrictFilterDomains(const vector<RelationStats> &relation_stats);
	bool IsFilterApplicable(const FilterInfo &filter, JoinRelationSet &set) const;
	CardinalityValue GetNumerator(JoinRelationSet &set);
	CardinalityValue GetInnerSelectivity(JoinRelationSet &set);
	CardinalityValue GetJoinSelectivity(JoinRelationSet &set, JoinRelationSet &left, JoinRelationSet &right);
	CardinalityValue GetSelectivity(JoinRelationSet &set);
	void ProcessSelectivityEdge(FilterInfoWithTotalDomains &edge, JoinRelationSet &requested_set,
	                            SelectivityState &state);
	void CreateSelectivitySubgraph(FilterInfoWithTotalDomains &edge, JoinRelationSet &edge_left_set,
	                               JoinRelationSet &edge_right_set, SelectivityState &state);
	void ExtendSelectivitySubgraph(idx_t subgraph_index, FilterInfoWithTotalDomains &edge,
	                               JoinRelationSet &edge_left_set, JoinRelationSet &edge_right_set,
	                               bool can_increment_existing_join, SelectivityState &state);
	void MergeSelectivitySubgraphs(const vector<idx_t> &subgraph_connections, FilterInfoWithTotalDomains &edge,
	                               SelectivityState &state);
	void MergeDisconnectedSelectivitySubgraphs(SelectivityState &state);
	bool SingleColumnFilter(const FilterInfo &filter_info);

	//! Selectivity calculation
	CardinalityValue CalculateInnerJoinSelectivity(CardinalityValue base_selectivity,
	                                               FilterInfoWithTotalDomains &filter);
	CardinalityValue GetLeftJoinSelectivity(const JoinOrderOperator &join_operator);
	bool ApplyJoinIncrement(CardinalityValue &target_selectivity, FilterInfoWithTotalDomains &edge,
	                        reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                        reference_set_t<JoinRelationSet> &capped_join_pairs, JoinRelationSet &scope,
	                        optional_ptr<JoinRelationSet> join_pair = nullptr);
	bool ApplyJoinPairCap(CardinalityValue &target_selectivity, JoinRelationSet &join_pair,
	                      reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                      reference_set_t<JoinRelationSet> &capped_join_pairs);
	bool ApplyCompositeJoinPairCaps(CardinalityValue &target_selectivity, JoinRelationSet &scope,
	                                reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                                reference_set_t<JoinRelationSet> &capped_join_pairs);
	double GetJoinPairCap(JoinRelationSet &join_pair);

	void AddRelationStats(const FilterInfo &filter_info);
	bool EmptyFilter(const FilterInfo &filter_info);

private:
	unique_ptr<CardinalityEstimatorState> state;
	JoinRelationSetManager &set_manager;
	const JoinPredicateModel &predicate_model;
};

} // namespace duckdb
