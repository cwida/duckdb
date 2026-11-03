//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/cardinality_estimator.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/reference_map.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"

namespace duckdb {

class CardinalityValue;
class FilterInfo;
class JoinPredicateModel;
struct CardinalityEstimatorState;
struct CardinalityHelper;
struct CompositeJoinPairStats;
struct SelectivityInfo;
struct SelectivityState;
struct FilterInfoWithTotalDomains;
struct LeftJoinSelectivity;
struct SelectivitySubgraph;

class CardinalityEstimator {
public:
	CardinalityEstimator(JoinRelationSetManager &set_manager, const JoinPredicateModel &predicate_model);
	~CardinalityEstimator();

public:
	void RemoveEmptyTotalDomains();
	void UpdateTotalDomains(optional_ptr<JoinRelationSet> set, RelationStats &stats);
	void InitEquivalentRelations();

	void InitCardinalityEstimatorProps(optional_ptr<JoinRelationSet> set, RelationStats &stats);
	void RestrictFilterDomains(const vector<RelationStats> &relation_stats);

	//! cost model needs estimated cardinalities to the fraction since the formula captures
	//! distinct count selectivities and multiplicities. Hence the template
	template <class T>
	T EstimateCardinalityWithSet(JoinRelationSet &new_set);

	//! used for debugging.
	void AddRelationNamesToRelationStats(vector<RelationStats> &stats);
	void PrintRelationStats();

private:
	CardinalityValue GetNumerator(JoinRelationSet &set);
	CardinalityValue GetSubgraphCardinality(const SelectivitySubgraph &subgraph);
	const CardinalityHelper &GetCardinalityBeforeFilters(JoinRelationSet &set);
	optional<CardinalityValue> EstimateTopLevelSemiAnti(JoinRelationSet &set);
	SelectivityInfo GetSelectivity(JoinRelationSet &set);
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
	void AddCrossProductRelations(JoinRelationSet &set, SelectivityState &state);
	SelectivityInfo CreateSelectivityResult(JoinRelationSet &set, SelectivityState &state);
	//! Applied outside the cardinality cache so stored values stay pre-OR.
	CardinalityValue ApplyOrFilterSelectivities(JoinRelationSet &new_set, CardinalityValue cardinality) const;

	bool SingleColumnFilter(const FilterInfo &filter_info);

	//! Selectivity calculation
	CardinalityValue CalculateUpdatedSelectivity(SelectivitySubgraph left, SelectivitySubgraph right,
	                                             FilterInfoWithTotalDomains &filter);
	CardinalityValue CalculateInnerJoinSelectivity(CardinalityValue base_selectivity,
	                                               FilterInfoWithTotalDomains &filter);
	LeftJoinSelectivity CalculateLeftJoinSelectivityInfo(SelectivitySubgraph &left, SelectivitySubgraph &right,
	                                                     FilterInfoWithTotalDomains &filter);
	CardinalityValue CalculateLeftJoinSelectivity(SelectivitySubgraph &left, SelectivitySubgraph &right,
	                                              FilterInfoWithTotalDomains &filter);
	CardinalityValue CalculateSemiAntiJoinSelectivity(SelectivitySubgraph &left, SelectivitySubgraph &right,
	                                                  FilterInfoWithTotalDomains &filter);
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

	JoinRelationSet &UpdateNumeratorRelations(SelectivitySubgraph left, SelectivitySubgraph right,
	                                          FilterInfoWithTotalDomains &filter);

	void AddRelationStats(const FilterInfo &filter_info);
	bool EmptyFilter(const FilterInfo &filter_info);

private:
	unique_ptr<CardinalityEstimatorState> state;
	JoinRelationSetManager &set_manager;
	const JoinPredicateModel &predicate_model;
};

} // namespace duckdb
