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

class FilterInfo;
class JoinPredicateModel;
struct CardinalityEstimatorState;
struct CardinalityHelper;
struct CompositeJoinPairStats;
struct DenomInfo;
struct DenominatorState;
struct FilterInfoWithTotalDomains;
struct LeftJoinDenomInfo;
struct Subgraph2Denominator;

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
	double GetNumerator(JoinRelationSet &set);
	double GetSubgraphCardinality(const Subgraph2Denominator &subgraph);
	const CardinalityHelper &GetCardinalityBeforeFilters(JoinRelationSet &set);
	optional<double> EstimateTopLevelSemiAnti(JoinRelationSet &set);
	DenomInfo GetDenominator(JoinRelationSet &set);
	void ProcessDenominatorEdge(FilterInfoWithTotalDomains &edge, JoinRelationSet &requested_set,
	                            DenominatorState &state);
	void CreateDenominatorSubgraph(FilterInfoWithTotalDomains &edge, JoinRelationSet &edge_left_set,
	                               JoinRelationSet &edge_right_set, DenominatorState &state);
	void ExtendDenominatorSubgraph(idx_t subgraph_index, FilterInfoWithTotalDomains &edge,
	                               JoinRelationSet &edge_left_set, JoinRelationSet &edge_right_set,
	                               bool can_increment_existing_join, DenominatorState &state);
	void MergeDenominatorSubgraphs(const vector<idx_t> &subgraph_connections, FilterInfoWithTotalDomains &edge,
	                               DenominatorState &state);
	void MergeDisconnectedDenominatorSubgraphs(DenominatorState &state);
	void AddCrossProductRelations(JoinRelationSet &set, DenominatorState &state);
	DenomInfo CreateDenominatorResult(JoinRelationSet &set, DenominatorState &state);
	//! Applied outside the cardinality cache so stored values stay pre-OR.
	double ApplyOrFilterSelectivities(JoinRelationSet &new_set, double cardinality) const;

	bool SingleColumnFilter(const FilterInfo &filter_info);

	//! Denom calculation
	double CalculateUpdatedDenom(Subgraph2Denominator left, Subgraph2Denominator right,
	                             FilterInfoWithTotalDomains &filter);
	double CalculateInnerJoinDenom(double base_denom, FilterInfoWithTotalDomains &filter);
	LeftJoinDenomInfo CalculateLeftJoinDenomInfo(Subgraph2Denominator &left, Subgraph2Denominator &right,
	                                             FilterInfoWithTotalDomains &filter);
	double CalculateLeftJoinDenom(Subgraph2Denominator &left, Subgraph2Denominator &right,
	                              FilterInfoWithTotalDomains &filter);
	double CalculateSemiAntiJoinDenom(Subgraph2Denominator &left, Subgraph2Denominator &right,
	                                  FilterInfoWithTotalDomains &filter);
	bool ApplyJoinIncrement(double &target_denom, FilterInfoWithTotalDomains &edge,
	                        reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                        reference_set_t<JoinRelationSet> &capped_join_pairs, JoinRelationSet &scope,
	                        optional_ptr<JoinRelationSet> join_pair = nullptr);
	bool ApplyJoinPairCap(double &target_denom, JoinRelationSet &join_pair,
	                      reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                      reference_set_t<JoinRelationSet> &capped_join_pairs);
	bool ApplyCompositeJoinPairCaps(double &target_denom, JoinRelationSet &scope,
	                                reference_map_t<JoinRelationSet, CompositeJoinPairStats> &inner_join_pair_stats,
	                                reference_set_t<JoinRelationSet> &capped_join_pairs);
	double GetJoinPairCap(JoinRelationSet &join_pair);

	JoinRelationSet &UpdateNumeratorRelations(Subgraph2Denominator left, Subgraph2Denominator right,
	                                          FilterInfoWithTotalDomains &filter);

	void AddRelationStats(const FilterInfo &filter_info);
	bool EmptyFilter(const FilterInfo &filter_info);

private:
	unique_ptr<CardinalityEstimatorState> state;
	JoinRelationSetManager &set_manager;
	const JoinPredicateModel &predicate_model;
};

} // namespace duckdb
