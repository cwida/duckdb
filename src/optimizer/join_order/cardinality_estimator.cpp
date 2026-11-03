#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/function/table/table_scan.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/optimizer/relation_statistics/relation_statistics_helper.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/storage/data_table.hpp"

#include <math.h>

namespace duckdb {

template <>
double CardinalityEstimator::EstimateCardinalityWithSet<double>(JoinRelationSet &new_set);

struct DenomInfo {
public:
	DenomInfo(JoinRelationSet &numerator_relations, double denominator)
	    : numerator_relations(numerator_relations), denominator(denominator) {
	}

public:
	JoinRelationSet &numerator_relations;
	double denominator;
};

struct DomainEstimate {
public:
	DomainEstimate();

public:
	idx_t GetDistinctCount() const;
	bool HasReliableDistinctCount() const;
	idx_t GetReliableDistinctCount() const;
	void Update(const DistinctCount &distinct_count);
	void Restrict(idx_t bound);

private:
	optional<DistinctCount> estimate;
};

DomainEstimate::DomainEstimate() {
}

idx_t DomainEstimate::GetDistinctCount() const {
	return estimate ? estimate->distinct_count : NumericLimits<idx_t>::Maximum();
}

bool DomainEstimate::HasReliableDistinctCount() const {
	return estimate && (estimate->source == DistinctCountSource::HLL || estimate->source == DistinctCountSource::EXACT);
}

idx_t DomainEstimate::GetReliableDistinctCount() const {
	if (!estimate || (estimate->source != DistinctCountSource::HLL && estimate->source != DistinctCountSource::EXACT)) {
		throw InternalException("Attempted to access an unreliable distinct count");
	}
	return estimate->distinct_count;
}

void DomainEstimate::Restrict(idx_t bound) {
	D_ASSERT(estimate);
	estimate->distinct_count = MinValue(estimate->distinct_count, bound);
}

void DomainEstimate::Update(const DistinctCount &distinct_count) {
	if (!estimate) {
		estimate = distinct_count;
		return;
	}
	estimate = RelationStatisticsHelper::SelectTotalDomain({*estimate, distinct_count});
}

struct RelationsSetToStats {
public:
	explicit RelationsSetToStats(const column_binding_set_t &column_binding_set)
	    : equivalent_relations(column_binding_set) {
	}

public:
	//! Column binding sets that are equivalent in a join plan.
	column_binding_set_t equivalent_relations;
	DomainEstimate domain_estimate;
	vector<reference<JoinPredicate>> predicates;
	vector<string> column_names;
};

struct FilterInfoWithTotalDomains {
	FilterInfoWithTotalDomains(JoinPredicate &predicate, RelationsSetToStats &relation_set_to_stats)
	    : predicate(predicate), domain_estimate(relation_set_to_stats.domain_estimate) {
	}

public:
	double GetDistinctCount() const;
	bool HasReliableDistinctCount() const;
	idx_t GetReliableDistinctCount() const;
	ExpressionType GetComparisonType();
	bool IsInnerEquality();
	FilterInfo &GetFilter() const;
	JoinPredicate &GetPredicate() const;

public:
	reference<JoinPredicate> predicate;
	reference<const DomainEstimate> domain_estimate;
};

struct Subgraph2Denominator {
public:
	Subgraph2Denominator() : relations(nullptr), numerator_relations(nullptr), denom(1) {
	}

public:
	optional_ptr<JoinRelationSet> relations;
	optional_ptr<JoinRelationSet> numerator_relations;
	double denom;
};

struct LeftJoinDenomInfo {
public:
	LeftJoinDenomInfo(JoinRelationSet &numerator_relations, double denominator)
	    : numerator_relations(numerator_relations), denominator(denominator) {
	}

public:
	JoinRelationSet &numerator_relations;
	double denominator;
};

struct CompositeJoinPairStats {
public:
	//! The row-count cap is only plausible when the candidate key cardinality is within the same order of magnitude
	//! as an observed single-column domain. Otherwise broad fact-to-fact joins can look like key lookups.
	static constexpr double MAX_CARDINALITY_TO_DISTINCT_RATIO = 8;

	void RegisterDistinctCount(double distinct_count) {
		if (!has_distinct_count) {
			first_distinct_count = distinct_count;
			has_distinct_count = true;
		}
		if (distinct_count > max_distinct_count) {
			max_distinct_count = distinct_count;
		}
	}

	bool CanApplyCap(double cap) const {
		return has_distinct_count && max_distinct_count > 0 &&
		       cap <= max_distinct_count * MAX_CARDINALITY_TO_DISTINCT_RATIO;
	}

public:
	double first_distinct_count = 0;
	double max_distinct_count = 0;
	bool has_distinct_count = false;
};

struct CardinalityHelper {
public:
	CardinalityHelper() : CardinalityHelper(0, true) {
	}
	CardinalityHelper(double cardinality_before_filters, bool is_empty)
	    : cardinality_before_filters(cardinality_before_filters), is_empty(is_empty) {
	}

public:
	// Must be a double. Otherwise we can lose significance between different join orders.
	double cardinality_before_filters;
	//! Structural emptiness is independent of the numerator's zero-to-one substitution.
	bool is_empty;
	vector<string> table_names_joined;
	vector<string> column_names;
};

struct CardinalityEstimatorState {
	vector<RelationsSetToStats> relation_set_stats;
	reference_map_t<JoinRelationSet, CardinalityHelper> relation_set_2_cardinality;
	vector<optional_ptr<FilterInfo>> or_filters;
};

CardinalityEstimator::CardinalityEstimator(JoinRelationSetManager &set_manager,
                                           const JoinPredicateModel &predicate_model)
    : state(make_uniq<CardinalityEstimatorState>()), set_manager(set_manager), predicate_model(predicate_model) {
}

CardinalityEstimator::~CardinalityEstimator() {
}

double FilterInfoWithTotalDomains::GetDistinctCount() const {
	return static_cast<double>(domain_estimate.get().GetDistinctCount());
}

bool FilterInfoWithTotalDomains::HasReliableDistinctCount() const {
	return domain_estimate.get().HasReliableDistinctCount();
}

idx_t FilterInfoWithTotalDomains::GetReliableDistinctCount() const {
	return domain_estimate.get().GetReliableDistinctCount();
}

ExpressionType FilterInfoWithTotalDomains::GetComparisonType() {
	return GetPredicate().GetComparisonType();
}

bool FilterInfoWithTotalDomains::IsInnerEquality() {
	return GetPredicate().IsEquivalencePredicate();
}

FilterInfo &FilterInfoWithTotalDomains::GetFilter() const {
	return GetPredicate().GetFilter();
}

JoinPredicate &FilterInfoWithTotalDomains::GetPredicate() const {
	return predicate.get();
}

// The filter was made on top of a logical sample or other projection,
// but no specific columns are referenced. See issue 4978 number 4.
bool CardinalityEstimator::EmptyFilter(const FilterInfo &filter_info) {
	if (!filter_info.left_set && !filter_info.right_set) {
		return true;
	}
	return false;
}

void CardinalityEstimator::AddRelationStats(const FilterInfo &filter_info) {
	D_ASSERT(!filter_info.set.get().Empty());
	// Use whichever binding is valid: prefer left_binding, fall back to right_binding.
	// left_binding may be INVALID_INDEX when left_set is empty (e.g. residual predicates
	// or single-column filters where only the right side references a relation).
	auto binding = filter_info.left_binding;
	if (!binding.table_index.IsValid()) {
		binding = filter_info.right_binding;
	}
	if (!binding.table_index.IsValid()) {
		// No valid binding (EmptyFilter), nothing to record
		return;
	}
	for (const RelationsSetToStats &r2tdom : state->relation_set_stats) {
		auto &i_set = r2tdom.equivalent_relations;
		if (i_set.find(binding) != i_set.end()) {
			// found an equivalent filter
			return;
		}
	}

	auto key = ColumnBinding(binding.table_index, binding.column_index);
	RelationsSetToStats new_r2tdom(column_binding_set_t({key}));

	state->relation_set_stats.emplace_back(new_r2tdom);
}

bool CardinalityEstimator::SingleColumnFilter(const FilterInfo &filter_info) {
	if (filter_info.left_set && filter_info.right_set && filter_info.set.get().count > 1) {
		// Both set and are from different relations
		return false;
	}
	if (EmptyFilter(filter_info)) {
		return false;
	}
	if (filter_info.join_type == JoinType::SEMI || filter_info.join_type == JoinType::ANTI) {
		return false;
	}
	return true;
}

static bool IsJoinOrFilter(const FilterInfo &filter) {
	if (filter.join_type == JoinType::SEMI || filter.join_type == JoinType::ANTI) {
		return false;
	}
	return filter.filter && filter.filter->GetExpressionClass() == ExpressionClass::BOUND_CONJUNCTION &&
	       filter.filter->GetExpressionType() == ExpressionType::CONJUNCTION_OR && filter.set.get().count >= 2;
}

void CardinalityEstimator::InitEquivalentRelations() {
	state->relation_set_stats.clear();
	state->or_filters.clear();

	for (auto predicate_ref : predicate_model.GetPredicates()) {
		auto &filter = predicate_ref.get().GetFilter();
		if (IsJoinOrFilter(filter)) {
			state->or_filters.push_back(filter);
			continue;
		}
		if (SingleColumnFilter(filter)) {
			// Filter on one relation, (i.e. string or range filter on a column).
			// Grab the first relation and add it to  the equivalence_relations
			AddRelationStats(filter);
		}
	}

	for (auto &equality_class : predicate_model.GetEqualityClasses()) {
		if (equality_class.columns.empty()) {
			continue;
		}
		state->relation_set_stats.emplace_back(equality_class.columns);
		for (auto &edge : equality_class.edges) {
			state->relation_set_stats.back().predicates.push_back(edge.predicate);
		}
	}

	for (auto predicate_ref : predicate_model.GetSelectivityPredicates()) {
		auto &predicate = predicate_ref.get();
		D_ASSERT(predicate.HasValidJoinEndpoints());
		D_ASSERT(predicate.HasAnyValidStatsBinding());

		column_binding_set_t domain_group;
		if (predicate.GetStatsBinding(true).table_index.IsValid()) {
			domain_group.insert(predicate.GetStatsBinding(true));
		}
		if (predicate.GetStatsBinding(false).table_index.IsValid()) {
			domain_group.insert(predicate.GetStatsBinding(false));
		}
		D_ASSERT(!domain_group.empty());
		state->relation_set_stats.emplace_back(domain_group);
		state->relation_set_stats.back().predicates.push_back(predicate);
	}
	RemoveEmptyTotalDomains();
}

void CardinalityEstimator::RemoveEmptyTotalDomains() {
	auto remove_start =
	    std::remove_if(state->relation_set_stats.begin(), state->relation_set_stats.end(),
	                   [](RelationsSetToStats &r_2_tdom) { return r_2_tdom.equivalent_relations.empty(); });
	state->relation_set_stats.erase(remove_start, state->relation_set_stats.end());
}

double CardinalityEstimator::GetNumerator(JoinRelationSet &set) {
	double numerator = 1;
	for (idx_t i = 0; i < set.count; i++) {
		auto &single_node_set = set_manager.GetJoinRelation(set.relations[i]);
		auto entry = state->relation_set_2_cardinality.find(single_node_set);
		D_ASSERT(entry != state->relation_set_2_cardinality.end());
		if (entry == state->relation_set_2_cardinality.end()) {
			continue;
		}
		auto cardinality = entry->second.cardinality_before_filters;
		numerator *= cardinality == 0 ? 1 : cardinality;
	}
	return numerator;
}

double CardinalityEstimator::GetSubgraphCardinality(const Subgraph2Denominator &subgraph) {
	D_ASSERT(subgraph.numerator_relations);
	if (subgraph.denom >= NumericLimits<double>::Maximum()) {
		return 0;
	}
	double numerator = 1;
	for (idx_t relation_idx = 0; relation_idx < subgraph.numerator_relations->count; relation_idx++) {
		auto &relation = set_manager.GetJoinRelation(subgraph.numerator_relations->relations[relation_idx]);
		auto entry = state->relation_set_2_cardinality.find(relation);
		D_ASSERT(entry != state->relation_set_2_cardinality.end());
		if (entry == state->relation_set_2_cardinality.end()) {
			continue;
		}
		numerator *= entry->second.cardinality_before_filters;
	}
	return numerator / (subgraph.denom <= 0 ? 1 : subgraph.denom);
}

optional<double> CardinalityEstimator::EstimateTopLevelSemiAnti(JoinRelationSet &set) {
	for (auto predicate_ref : predicate_model.GetPredicates()) {
		auto &predicate = predicate_ref.get();
		if (predicate.GetJoinType() != JoinType::SEMI && predicate.GetJoinType() != JoinType::ANTI) {
			continue;
		}
		auto semantic_left = predicate.GetSemanticLeftSetOptional();
		auto semantic_right = predicate.GetSemanticRightSetOptional();
		if (!semantic_left || !semantic_right) {
			continue;
		}
		auto &operator_set = set_manager.Union(*semantic_left, *semantic_right);
		if (!RefersToSameObject(operator_set, set)) {
			continue;
		}
		D_ASSERT(semantic_left->count < set.count && semantic_right->count < set.count);
		auto &preserved = GetCardinalityBeforeFilters(*semantic_left);
		auto preserved_cardinality = preserved.is_empty ? 0 : preserved.cardinality_before_filters;
		auto rhs_cardinality = GetCardinalityBeforeFilters(*semantic_right).is_empty ? 0 : 1;
		return RelationStatisticsHelper::EstimateSemiAntiJoinFallback(preserved_cardinality, rhs_cardinality,
		                                                              predicate.GetJoinType())
		    .cardinality;
	}
	return {};
}

vector<FilterInfoWithTotalDomains> GetEdges(vector<RelationsSetToStats> &relations_to_tdom,
                                            JoinRelationSet &requested_set) {
	vector<FilterInfoWithTotalDomains> res;
	for (auto &relation_2_tdom : relations_to_tdom) {
		for (auto predicate_ref : relation_2_tdom.predicates) {
			auto &predicate = predicate_ref.get();
			auto &filter = predicate.GetFilter();
			if (JoinRelationSet::IsSubset(requested_set, filter.set.get()) && filter.left_set != filter.right_set) {
				FilterInfoWithTotalDomains new_edge(predicate, relation_2_tdom);
				res.push_back(new_edge);
			}
		}
	}
	return res;
}

static optional_ptr<JoinRelationSet> GetEdgeEndpoint(FilterInfoWithTotalDomains &edge,
                                                     JoinRelationSetManager &set_manager, bool left) {
	auto join_type = edge.GetPredicate().GetJoinType();
	if (join_type == JoinType::SEMI || join_type == JoinType::ANTI) {
		auto semantic_set =
		    left ? edge.GetPredicate().GetSemanticLeftSetOptional() : edge.GetPredicate().GetSemanticRightSetOptional();
		if (semantic_set) {
			return semantic_set;
		}
	}
	auto &filter = edge.GetFilter();
	if (filter.filter && BoundComparisonExpression::IsComparison(*filter.filter)) {
		auto &binding = edge.GetPredicate().GetStatsBinding(left);
		if (binding.table_index.IsValid()) {
			return &set_manager.GetJoinRelation(RelationIndex(binding.table_index.index));
		}
	}
	return left ? edge.GetPredicate().GetLeftSetOptional() : edge.GetPredicate().GetRightSetOptional();
}

bool EdgeConnects(FilterInfoWithTotalDomains &edge, Subgraph2Denominator &subgraph,
                  JoinRelationSetManager &set_manager) {
	auto left_endpoint = GetEdgeEndpoint(edge, set_manager, true);
	if (left_endpoint && JoinRelationSet::IsSubset(*subgraph.relations, *left_endpoint)) {
		return true;
	}
	auto right_endpoint = GetEdgeEndpoint(edge, set_manager, false);
	if (right_endpoint && JoinRelationSet::IsSubset(*subgraph.relations, *right_endpoint)) {
		return true;
	}
	return false;
}

vector<idx_t> SubgraphsConnectedByEdge(FilterInfoWithTotalDomains &edge, vector<Subgraph2Denominator> &subgraphs,
                                       JoinRelationSetManager &set_manager) {
	vector<idx_t> res;
	if (subgraphs.empty()) {
		return res;
	} else {
		// check the combinations of subgraphs and see if the edge connects two of them,
		// if so, return the indexes of the two subgraphs within the vector
		for (idx_t outer = 0; outer != subgraphs.size(); outer++) {
			// check if the edge connects two subgraphs.
			for (idx_t inner = outer + 1; inner != subgraphs.size(); inner++) {
				if (EdgeConnects(edge, subgraphs.at(outer), set_manager) &&
				    EdgeConnects(edge, subgraphs.at(inner), set_manager)) {
					// order is important because we will delete the inner subgraph later
					res.push_back(outer);
					res.push_back(inner);
					return res;
				}
			}
			// if the edge does not connect two subgraphs, see if the edge connects with just outer
			// merge subgraph.at(outer) with the RelationSet(s) that edge connects
			if (EdgeConnects(edge, subgraphs.at(outer), set_manager)) {
				res.push_back(outer);
				return res;
			}
		}
	}
	// this edge connects only the relations it connects. Return an empty result so a new subgraph is created.
	return res;
}

JoinRelationSet &CardinalityEstimator::UpdateNumeratorRelations(Subgraph2Denominator left, Subgraph2Denominator right,
                                                                FilterInfoWithTotalDomains &filter) {
	auto &predicate = filter.GetPredicate();
	switch (predicate.GetJoinType()) {
	case JoinType::LEFT: {
		return CalculateLeftJoinDenomInfo(left, right, filter).numerator_relations;
	}
	case JoinType::SEMI:
	case JoinType::ANTI: {
		if (JoinRelationSet::IsSubset(*left.relations, predicate.GetLeftSet()) &&
		    JoinRelationSet::IsSubset(*right.relations, predicate.GetRightSet())) {
			return *left.numerator_relations;
		}
		return *right.numerator_relations;
	}
	default:
		// cross product or inner join
		return set_manager.Union(*left.numerator_relations, *right.numerator_relations);
	}
}

// Apply the denominator multiplier for a given comparison type and effective distinct count.
static double ApplyComparisonRatio(double base_denom, ExpressionType comparison_type, double effective_d) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return base_denom * effective_d;
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return effective_d > 1 ? base_denom * (effective_d / (effective_d - 1)) : base_denom;
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
		// Assume this blows up, but use the tdom to bound it a bit.
		return base_denom * pow(effective_d, 2.0 / 3.0);
	default:
		return base_denom;
	}
}

static double GetEffectiveDenom(double denom) {
	return denom <= 0 ? 1 : denom;
}

double CardinalityEstimator::CalculateInnerJoinDenom(double base_denom, FilterInfoWithTotalDomains &filter) {
	auto effective_d = GetEffectiveDenom(filter.GetDistinctCount());
	auto comparison_type = filter.GetComparisonType();
	if (comparison_type == ExpressionType::INVALID) {
		return base_denom * effective_d;
	}
	return ApplyComparisonRatio(base_denom, comparison_type, effective_d);
}

LeftJoinDenomInfo CardinalityEstimator::CalculateLeftJoinDenomInfo(Subgraph2Denominator &left,
                                                                   Subgraph2Denominator &right,
                                                                   FilterInfoWithTotalDomains &filter) {
	auto &predicate = filter.GetPredicate();
	D_ASSERT(left.relations && right.relations);
	D_ASSERT(left.numerator_relations && right.numerator_relations);
	auto left_is_preserved = JoinRelationSet::IsSubset(*left.relations, predicate.GetLeftSet()) &&
	                         JoinRelationSet::IsSubset(*right.relations, predicate.GetRightSet());
	auto &preserved_numerator = left_is_preserved ? *left.numerator_relations : *right.numerator_relations;
	auto preserved_denom = GetEffectiveDenom(left_is_preserved ? left.denom : right.denom);

	auto &inner_numerator = set_manager.Union(*left.numerator_relations, *right.numerator_relations);
	auto inner_denom = CalculateInnerJoinDenom(GetEffectiveDenom(left.denom) * GetEffectiveDenom(right.denom), filter);
	auto composite_pair = predicate.GetCompositeJoinPair();
	if (composite_pair) {
		auto cap = GetJoinPairCap(*composite_pair);
		CompositeJoinPairStats stats;
		stats.RegisterDistinctCount(filter.GetDistinctCount());
		if (stats.CanApplyCap(cap) && filter.GetDistinctCount() > 0 && cap > filter.GetDistinctCount()) {
			inner_denom = inner_denom / filter.GetDistinctCount() * cap;
		}
	}
	if (inner_denom <= 0) {
		return LeftJoinDenomInfo(preserved_numerator, preserved_denom);
	}
	auto inner_cardinality = GetNumerator(inner_numerator) / inner_denom;
	auto preserved_cardinality = GetNumerator(preserved_numerator) / preserved_denom;
	if (inner_cardinality > preserved_cardinality) {
		return LeftJoinDenomInfo(inner_numerator, inner_denom);
	}
	return LeftJoinDenomInfo(preserved_numerator, preserved_denom);
}

double CardinalityEstimator::CalculateLeftJoinDenom(Subgraph2Denominator &left, Subgraph2Denominator &right,
                                                    FilterInfoWithTotalDomains &filter) {
	return CalculateLeftJoinDenomInfo(left, right, filter).denominator;
}

double CardinalityEstimator::CalculateSemiAntiJoinDenom(Subgraph2Denominator &left, Subgraph2Denominator &right,
                                                        FilterInfoWithTotalDomains &filter) {
	auto &predicate = filter.GetPredicate();
	auto left_is_preserved = JoinRelationSet::IsSubset(*left.relations, predicate.GetLeftSet()) &&
	                         JoinRelationSet::IsSubset(*right.relations, predicate.GetRightSet());
	auto &preserved = left_is_preserved ? left : right;
	auto &rhs = left_is_preserved ? right : left;
	auto preserved_cardinality = GetSubgraphCardinality(preserved);
	auto semantic_rhs = predicate.GetSemanticRightSetOptional();
	auto rhs_is_empty =
	    semantic_rhs ? GetCardinalityBeforeFilters(*semantic_rhs).is_empty : GetSubgraphCardinality(rhs) <= 0;
	auto rhs_cardinality = rhs_is_empty ? 0 : 1;
	auto estimate = RelationStatisticsHelper::EstimateSemiAntiJoinFallback(preserved_cardinality, rhs_cardinality,
	                                                                       predicate.GetJoinType());
	if (estimate.cardinality <= 0) {
		return NumericLimits<double>::Maximum();
	}
	return preserved.denom * preserved_cardinality / estimate.cardinality;
}

// Given two subgraphs, compute the updated denominator for the join between them.
double CardinalityEstimator::CalculateUpdatedDenom(Subgraph2Denominator left, Subgraph2Denominator right,
                                                   FilterInfoWithTotalDomains &filter) {
	double base_denom = GetEffectiveDenom(left.denom) * GetEffectiveDenom(right.denom);
	switch (filter.GetPredicate().GetJoinType()) {
	case JoinType::LEFT:
		return CalculateLeftJoinDenom(left, right, filter);
	case JoinType::INNER:
		return CalculateInnerJoinDenom(base_denom, filter);
	case JoinType::SEMI:
	case JoinType::ANTI:
		return CalculateSemiAntiJoinDenom(left, right, filter);
	default:
		// Cross product: no join condition reduces the denominator.
		return base_denom;
	}
}

static bool GetEqualityEdgeRelations(FilterInfoWithTotalDomains &edge, RelationIndex &left, RelationIndex &right) {
	if (!edge.IsInnerEquality()) {
		return false;
	}
	auto &left_binding = edge.GetPredicate().GetEqualityBinding(true);
	auto &right_binding = edge.GetPredicate().GetEqualityBinding(false);
	if (!left_binding.table_index.IsValid() || !right_binding.table_index.IsValid()) {
		return false;
	}
	left = RelationIndex(left_binding.table_index.index);
	right = RelationIndex(right_binding.table_index.index);
	return left != right;
}

static optional_ptr<JoinRelationSet> GetEqualityJoinPair(FilterInfoWithTotalDomains &edge,
                                                         JoinRelationSetManager &set_manager) {
	RelationIndex left;
	RelationIndex right;
	if (!GetEqualityEdgeRelations(edge, left, right)) {
		return nullptr;
	}
	auto &left_set = set_manager.GetJoinRelation(left);
	auto &right_set = set_manager.GetJoinRelation(right);
	return set_manager.Union(left_set, right_set);
}

double CardinalityEstimator::GetJoinPairCap(JoinRelationSet &join_pair) {
	D_ASSERT(join_pair.count == 2);
	double cap = NumericLimits<double>::Maximum();
	for (idx_t relation_index = 0; relation_index < join_pair.count; relation_index++) {
		auto &single_relation = set_manager.GetJoinRelation(join_pair.relations[relation_index]);
		auto cardinality = GetNumerator(single_relation);
		if (cardinality <= 0) {
			return 0;
		}
		cap = MinValue(cap, cardinality);
	}
	return cap == NumericLimits<double>::Maximum() ? 0 : cap;
}

bool CardinalityEstimator::ApplyJoinPairCap(double &target_denom, JoinRelationSet &join_pair,
                                            reference_map_t<JoinRelationSet, CompositeJoinPairStats> &join_pair_stats,
                                            reference_set_t<JoinRelationSet> &capped_join_pairs) {
	if (capped_join_pairs.find(join_pair) != capped_join_pairs.end()) {
		return false;
	}
	const auto it = join_pair_stats.find(join_pair);
	if (it == join_pair_stats.end()) {
		return false;
	}
	auto &stats = it->second;
	auto cap = GetJoinPairCap(join_pair);
	// The cap is an FK/PK heuristic. Avoid applying it to broad fact-to-fact joins where the base row count is much
	// larger than any observed single-column join domain; in that case row count is a poor proxy for composite NDV.
	if (!stats.CanApplyCap(cap)) {
		return false;
	}
	auto &first_d = stats.first_distinct_count;
	if (cap <= 0 || first_d <= 0 || first_d > cap) {
		return false;
	}
	if (cap > 0 && first_d < cap && first_d > 0) {
		// Raise weak same-pair composite evidence to the FK/PK denominator floor.
		target_denom = target_denom / first_d * cap;
		first_d = cap;
	}
	capped_join_pairs.insert(join_pair);
	return true;
}

bool CardinalityEstimator::ApplyJoinIncrement(double &target_denom, FilterInfoWithTotalDomains &edge,
                                              reference_map_t<JoinRelationSet, CompositeJoinPairStats> &join_pair_stats,
                                              reference_set_t<JoinRelationSet> &capped_join_pairs,
                                              JoinRelationSet &scope, optional_ptr<JoinRelationSet> join_pair) {
	if (edge.IsInnerEquality()) {
		auto target_pair = join_pair;
		if (!target_pair) {
			target_pair = GetEqualityJoinPair(edge, set_manager);
		}
		if (!target_pair) {
			return false;
		}
		if (!JoinRelationSet::IsSubset(scope, *target_pair) ||
		    !predicate_model.HasDirectCompositeEquality(*target_pair)) {
			return false;
		}
		if (capped_join_pairs.find(*target_pair) != capped_join_pairs.end()) {
			// The composite-key cap already accounts for same-pair equality predicates.
			return true;
		}
		return ApplyJoinPairCap(target_denom, *target_pair, join_pair_stats, capped_join_pairs);
	}

	if (edge.GetPredicate().GetJoinType() == JoinType::LEFT) {
		// LEFT joins preserve the LHS cardinality in this estimator. Additional LEFT conditions
		// should not reduce the output or add unused-edge penalties.
		return true;
	}

	return false;
}

bool CardinalityEstimator::ApplyCompositeJoinPairCaps(
    double &target_denom, JoinRelationSet &scope,
    reference_map_t<JoinRelationSet, CompositeJoinPairStats> &join_pair_stats,
    reference_set_t<JoinRelationSet> &capped_join_pairs) {
	bool applied = false;
	for (auto &entry : join_pair_stats) {
		auto &join_pair = entry.first.get();
		if (capped_join_pairs.find(join_pair) != capped_join_pairs.end()) {
			continue;
		}
		if (!JoinRelationSet::IsSubset(scope, join_pair)) {
			continue;
		}
		if (!predicate_model.HasDirectCompositeEquality(join_pair)) {
			continue;
		}
		applied = ApplyJoinPairCap(target_denom, join_pair, join_pair_stats, capped_join_pairs) || applied;
	}
	return applied;
}

enum class DenominatorEdgeKind : uint8_t {
	INNER_EQUIVALENCE,
	LEFT_JOIN,
	SEMI_ANTI_JOIN,
	NON_EQUALITY_SELECTIVITY,
	REDUNDANT_TRANSITIVE_EQUALITY,
	OTHER
};

static bool EquivalenceGroupApplied(FilterInfoWithTotalDomains &edge, const unordered_set<idx_t> &applied_groups) {
	auto equality_class_index = edge.GetPredicate().GetEqualityClassIndex();
	return equality_class_index.IsValid() && applied_groups.count(equality_class_index.GetIndex()) > 0;
}

static DenominatorEdgeKind ClassifyDenominatorEdge(FilterInfoWithTotalDomains &edge,
                                                   const unordered_set<idx_t> &applied_groups) {
	if (EquivalenceGroupApplied(edge, applied_groups)) {
		return DenominatorEdgeKind::REDUNDANT_TRANSITIVE_EQUALITY;
	}
	switch (edge.GetPredicate().GetPredicateClass()) {
	case JoinPredicateClass::INNER_EQUALITY:
		return DenominatorEdgeKind::INNER_EQUIVALENCE;
	case JoinPredicateClass::INNER_NON_EQUALITY:
		return DenominatorEdgeKind::NON_EQUALITY_SELECTIVITY;
	case JoinPredicateClass::LEFT_JOIN:
		return DenominatorEdgeKind::LEFT_JOIN;
	case JoinPredicateClass::SEMI_ANTI_JOIN:
		return DenominatorEdgeKind::SEMI_ANTI_JOIN;
	default:
		return DenominatorEdgeKind::OTHER;
	}
}

static bool CanIncrementExistingJoin(DenominatorEdgeKind kind) {
	return kind == DenominatorEdgeKind::INNER_EQUIVALENCE || kind == DenominatorEdgeKind::LEFT_JOIN;
}

static void RegisterEqualityJoinPair(FilterInfoWithTotalDomains &edge, JoinRelationSetManager &set_manager,
                                     reference_map_t<JoinRelationSet, CompositeJoinPairStats> &join_pair_stats) {
	if (!edge.IsInnerEquality()) {
		return;
	}
	auto join_pair = GetEqualityJoinPair(edge, set_manager);
	if (!join_pair) {
		return;
	}
	join_pair_stats[*join_pair].RegisterDistinctCount(edge.GetDistinctCount());
}

struct DenominatorState {
	vector<Subgraph2Denominator> subgraphs;
	unordered_set<idx_t> applied_equivalence_groups;
	reference_map_t<JoinRelationSet, CompositeJoinPairStats> join_pair_stats;
	reference_set_t<JoinRelationSet> capped_join_pairs;
	unordered_set<idx_t> unused_edge_tdoms;
	unordered_set<idx_t> applied_semi_anti_operators;
	unordered_set<idx_t> applied_semi_anti_predicates;
};

static bool DenominatorSubgraphComplete(DenominatorState &state, JoinRelationSet &set) {
	return state.subgraphs.size() == 1 && state.subgraphs.at(0).relations &&
	       RefersToSameObject(*state.subgraphs.at(0).relations, set);
}

void CardinalityEstimator::ProcessDenominatorEdge(FilterInfoWithTotalDomains &edge, JoinRelationSet &requested_set,
                                                  DenominatorState &state) {
	auto edge_kind = ClassifyDenominatorEdge(edge, state.applied_equivalence_groups);
	if (edge_kind == DenominatorEdgeKind::SEMI_ANTI_JOIN) {
		auto source_index = edge.GetFilter().source_operator_index;
		// Predicates without an operator descriptor have a separate identity namespace.
		auto &applied = source_index.IsValid() ? state.applied_semi_anti_operators : state.applied_semi_anti_predicates;
		auto identity = source_index.IsValid() ? source_index.GetIndex() : edge.GetPredicate().GetIndex();
		if (!applied.insert(identity).second) {
			return;
		}
	}
	if (DenominatorSubgraphComplete(state, requested_set)) {
		auto &complete_subgraph = state.subgraphs.at(0);
		ApplyCompositeJoinPairCaps(complete_subgraph.denom, *complete_subgraph.relations, state.join_pair_stats,
		                           state.capped_join_pairs);
		if (edge_kind == DenominatorEdgeKind::REDUNDANT_TRANSITIVE_EQUALITY) {
			return;
		}
		if (CanIncrementExistingJoin(edge_kind) &&
		    ApplyJoinIncrement(complete_subgraph.denom, edge, state.join_pair_stats, state.capped_join_pairs,
		                       *complete_subgraph.relations)) {
			return;
		}
		if (edge.HasReliableDistinctCount()) {
			state.unused_edge_tdoms.insert(edge.GetReliableDistinctCount());
		}
		return;
	}

	auto equality_class_index = edge.GetPredicate().GetEqualityClassIndex();
	if (equality_class_index.IsValid()) {
		state.applied_equivalence_groups.insert(equality_class_index.GetIndex());
	}
	RegisterEqualityJoinPair(edge, set_manager, state.join_pair_stats);

	auto edge_left_set = GetEdgeEndpoint(edge, set_manager, true);
	auto edge_right_set = GetEdgeEndpoint(edge, set_manager, false);
	if (!edge_left_set || !edge_right_set) {
		return;
	}

	auto subgraph_connections = SubgraphsConnectedByEdge(edge, state.subgraphs, set_manager);
	if (subgraph_connections.empty()) {
		CreateDenominatorSubgraph(edge, *edge_left_set, *edge_right_set, state);
	} else if (subgraph_connections.size() == 1) {
		ExtendDenominatorSubgraph(subgraph_connections.at(0), edge, *edge_left_set, *edge_right_set,
		                          CanIncrementExistingJoin(edge_kind), state);
	} else if (subgraph_connections.size() == 2) {
		MergeDenominatorSubgraphs(subgraph_connections, edge, state);
	}
}

void CardinalityEstimator::CreateDenominatorSubgraph(FilterInfoWithTotalDomains &edge, JoinRelationSet &edge_left_set,
                                                     JoinRelationSet &edge_right_set, DenominatorState &state) {
	auto left_subgraph = Subgraph2Denominator();
	auto right_subgraph = Subgraph2Denominator();
	left_subgraph.relations = &edge_left_set;
	left_subgraph.numerator_relations = &edge_left_set;
	right_subgraph.relations = &edge_right_set;
	right_subgraph.numerator_relations = &edge_right_set;
	auto &numerator_relations = UpdateNumeratorRelations(left_subgraph, right_subgraph, edge);
	auto denom = CalculateUpdatedDenom(left_subgraph, right_subgraph, edge);
	left_subgraph.numerator_relations = &numerator_relations;
	left_subgraph.relations = &set_manager.Union(edge_left_set, edge_right_set);
	left_subgraph.denom = denom;
	ApplyCompositeJoinPairCaps(left_subgraph.denom, *left_subgraph.relations, state.join_pair_stats,
	                           state.capped_join_pairs);
	state.subgraphs.push_back(left_subgraph);
}

void CardinalityEstimator::ExtendDenominatorSubgraph(idx_t subgraph_index, FilterInfoWithTotalDomains &edge,
                                                     JoinRelationSet &edge_left_set, JoinRelationSet &edge_right_set,
                                                     bool can_increment_existing_join, DenominatorState &state) {
	auto left_subgraph = &state.subgraphs.at(subgraph_index);
	auto right_subgraph = Subgraph2Denominator();
	right_subgraph.relations = &edge_right_set;
	right_subgraph.numerator_relations = &edge_right_set;
	if (JoinRelationSet::IsSubset(*left_subgraph->relations, *right_subgraph.relations)) {
		right_subgraph.relations = &edge_left_set;
		right_subgraph.numerator_relations = &edge_left_set;
	}

	if (JoinRelationSet::IsSubset(*left_subgraph->relations, edge_left_set) &&
	    JoinRelationSet::IsSubset(*left_subgraph->relations, edge_right_set)) {
		if (can_increment_existing_join) {
			ApplyJoinIncrement(left_subgraph->denom, edge, state.join_pair_stats, state.capped_join_pairs,
			                   *left_subgraph->relations);
		}
		ApplyCompositeJoinPairCaps(left_subgraph->denom, *left_subgraph->relations, state.join_pair_stats,
		                           state.capped_join_pairs);
		return;
	}

	auto &numerator_relations = UpdateNumeratorRelations(*left_subgraph, right_subgraph, edge);
	auto denom = CalculateUpdatedDenom(*left_subgraph, right_subgraph, edge);
	left_subgraph->numerator_relations = &numerator_relations;
	left_subgraph->relations = &set_manager.Union(*left_subgraph->relations, *right_subgraph.relations);
	left_subgraph->denom = denom;
	ApplyCompositeJoinPairCaps(left_subgraph->denom, *left_subgraph->relations, state.join_pair_stats,
	                           state.capped_join_pairs);
}

void CardinalityEstimator::MergeDenominatorSubgraphs(const vector<idx_t> &subgraph_connections,
                                                     FilterInfoWithTotalDomains &edge, DenominatorState &state) {
	D_ASSERT(subgraph_connections.size() == 2);
	D_ASSERT(subgraph_connections.at(0) < subgraph_connections.at(1));
	auto subgraph_to_merge_into = &state.subgraphs.at(subgraph_connections.at(0));
	auto subgraph_to_delete = &state.subgraphs.at(subgraph_connections.at(1));
	auto &numerator_relations = UpdateNumeratorRelations(*subgraph_to_merge_into, *subgraph_to_delete, edge);
	auto denom = CalculateUpdatedDenom(*subgraph_to_merge_into, *subgraph_to_delete, edge);
	subgraph_to_merge_into->relations =
	    &set_manager.Union(*subgraph_to_merge_into->relations, *subgraph_to_delete->relations);
	subgraph_to_merge_into->numerator_relations = &numerator_relations;
	subgraph_to_merge_into->denom = denom;
	ApplyCompositeJoinPairCaps(subgraph_to_merge_into->denom, *subgraph_to_merge_into->relations, state.join_pair_stats,
	                           state.capped_join_pairs);
	subgraph_to_delete->relations = nullptr;
	auto remove_start = std::remove_if(state.subgraphs.begin(), state.subgraphs.end(),
	                                   [](Subgraph2Denominator &s) { return !s.relations; });
	state.subgraphs.erase(remove_start, state.subgraphs.end());
}

void CardinalityEstimator::MergeDisconnectedDenominatorSubgraphs(DenominatorState &state) {
	if (state.subgraphs.size() <= 1) {
		return;
	}
	auto final_subgraph = state.subgraphs.at(0);
	for (auto merge_with = state.subgraphs.begin() + 1; merge_with != state.subgraphs.end(); merge_with++) {
		D_ASSERT(final_subgraph.relations && merge_with->relations);
		final_subgraph.relations = &set_manager.Union(*final_subgraph.relations, *merge_with->relations);
		D_ASSERT(final_subgraph.numerator_relations && merge_with->numerator_relations);
		final_subgraph.numerator_relations =
		    &set_manager.Union(*final_subgraph.numerator_relations, *merge_with->numerator_relations);
		final_subgraph.denom *= merge_with->denom;
	}
	state.subgraphs.clear();
	state.subgraphs.push_back(final_subgraph);
}

void CardinalityEstimator::AddCrossProductRelations(JoinRelationSet &set, DenominatorState &state) {
	if (state.subgraphs.empty()) {
		return;
	}
	auto &returning_subgraph = state.subgraphs.at(0);
	if (returning_subgraph.relations->count == set.count) {
		return;
	}
	for (idx_t rel_index = 0; rel_index < set.count; rel_index++) {
		auto relation_id = set.relations[rel_index];
		auto &rel = set_manager.GetJoinRelation(relation_id);
		if (!JoinRelationSet::IsSubset(*returning_subgraph.relations, rel)) {
			returning_subgraph.numerator_relations = &set_manager.Union(*returning_subgraph.numerator_relations, rel);
			returning_subgraph.relations = &set_manager.Union(*returning_subgraph.relations, rel);
		}
	}
}

DenomInfo CardinalityEstimator::CreateDenominatorResult(JoinRelationSet &set, DenominatorState &state) {
	auto denom_multiplier = 1.0 + static_cast<double>(state.unused_edge_tdoms.size());
	MergeDisconnectedDenominatorSubgraphs(state);
	AddCrossProductRelations(set, state);

	if (state.subgraphs.empty() || state.subgraphs.at(0).denom == 0) {
		return DenomInfo(set, 1);
	}
	return DenomInfo(*state.subgraphs.at(0).numerator_relations, state.subgraphs.at(0).denom * denom_multiplier);
}

DenomInfo CardinalityEstimator::GetDenominator(JoinRelationSet &set) {
	DenominatorState state;
	auto edges = GetEdges(this->state->relation_set_stats, set);
	for (auto &edge : edges) {
		ProcessDenominatorEdge(edge, set, state);
	}
	return CreateDenominatorResult(set, state);
}

// Cardinality is calculated using logic based on
// https://blobs.duckdb.org/papers/tom-ebergen-msc-thesis-join-order-optimization-with-almost-no-statistics.pdf
//
// The estimator starts with base relation cardinalities and divides by a denominator assembled from predicate domain
// groups. INNER equality predicates use transitive equality classes, composite same-pair equalities can apply an FK/PK
// cap, disconnected predicate subgraphs are merged by cross product, and LEFT/SEMI/ANTI joins adjust the numerator side
// according to their output semantics. Non-equality predicates still use a heuristic total-domain penalty.
const CardinalityHelper &CardinalityEstimator::GetCardinalityBeforeFilters(JoinRelationSet &new_set) {
	auto it = state->relation_set_2_cardinality.find(new_set);
	if (it != state->relation_set_2_cardinality.end()) {
		return it->second;
	}
	double result;
	bool is_empty;
	auto semi_anti_result = EstimateTopLevelSemiAnti(new_set);
	if (semi_anti_result) {
		result = *semi_anti_result;
		is_empty = result <= 0;
	} else {
		// SEMI/ANTI denominators exclude RHS relations from the numerator.
		auto denom = GetDenominator(new_set);
		auto numerator = GetNumerator(denom.numerator_relations);
		result = denom.denominator >= NumericLimits<double>::Maximum() ? 0 : numerator / denom.denominator;
		Subgraph2Denominator subgraph;
		subgraph.numerator_relations = &denom.numerator_relations;
		subgraph.denom = denom.denominator;
		is_empty = GetSubgraphCardinality(subgraph) <= 0;
	}
	return state->relation_set_2_cardinality.emplace(new_set, CardinalityHelper(result, is_empty)).first->second;
}

template <>
double CardinalityEstimator::EstimateCardinalityWithSet(JoinRelationSet &new_set) {
	return ApplyOrFilterSelectivities(new_set, GetCardinalityBeforeFilters(new_set).cardinality_before_filters);
}

double CardinalityEstimator::ApplyOrFilterSelectivities(JoinRelationSet &new_set, double cardinality) const {
	for (auto &filter : state->or_filters) {
		if (JoinRelationSet::IsSubset(new_set, filter->set.get())) {
			cardinality *= RelationStatisticsHelper::DEFAULT_SELECTIVITY;
		}
	}
	return cardinality;
}

template <>
idx_t CardinalityEstimator::EstimateCardinalityWithSet(JoinRelationSet &new_set) {
	auto cardinality_as_double = EstimateCardinalityWithSet<double>(new_set);
	auto max = NumericLimits<idx_t>::Maximum();
	if (cardinality_as_double >= (double)max) {
		return max;
	}
	return (idx_t)cardinality_as_double;
}

bool SortTdoms(const RelationsSetToStats &a, const RelationsSetToStats &b) {
	return a.domain_estimate.GetDistinctCount() > b.domain_estimate.GetDistinctCount();
}

void CardinalityEstimator::InitCardinalityEstimatorProps(optional_ptr<JoinRelationSet> set, RelationStats &stats) {
	// Get the join relation set
	D_ASSERT(stats.stats_initialized);
	auto relation_cardinality = stats.cardinality;

	auto card_helper = CardinalityHelper((double)relation_cardinality, relation_cardinality == 0);
	state->relation_set_2_cardinality[*set] = card_helper;

	UpdateTotalDomains(set, stats);

	// sort relations from greatest tdom to lowest tdom.
	std::sort(state->relation_set_stats.begin(), state->relation_set_stats.end(), SortTdoms);
}

void CardinalityEstimator::UpdateTotalDomains(optional_ptr<JoinRelationSet> set, RelationStats &stats) {
	D_ASSERT(set->count == 1);
	auto relation_id = set->relations[0];
	//! Initialize the distinct count for all columns used in joins with the current relation.
	for (idx_t i = 0; i < stats.columns.size(); i++) {
		//! for every column used in a filter in the relation, get the distinct count via HLL, or assume it to be
		//! the cardinality
		// Update the relation_to_tdom set with the estimated distinct count (or tdom) calculated above
		auto key = ColumnBinding(TableIndex(relation_id.index), ProjectionIndex(i));
		auto distinct_count = stats.columns[i].total_domain;
		for (auto &relation_to_tdom : state->relation_set_stats) {
			const auto &i_set = relation_to_tdom.equivalent_relations;
			if (i_set.find(key) == i_set.end()) {
				continue;
			}
			relation_to_tdom.domain_estimate.Update(distinct_count);
		}
	}
}

void CardinalityEstimator::RestrictFilterDomains(const vector<RelationStats> &relation_stats) {
	for (auto &equivalence : state->relation_set_stats) {
		bool all_filtered = !equivalence.predicates.empty();
		for (auto &predicate : equivalence.predicates) {
			all_filtered &= predicate.get().IsEquivalencePredicate();
		}
		idx_t retained_domain = 0;
		for (auto &binding : equivalence.equivalent_relations) {
			if (!all_filtered || binding.table_index.index >= relation_stats.size() ||
			    binding.column_index.GetIndex() >= relation_stats[binding.table_index.index].columns.size()) {
				all_filtered = false;
				break;
			}
			auto &column = relation_stats[binding.table_index.index].columns[binding.column_index.GetIndex()];
			auto bound = column.semi_anti_join_domain_evidence.GetFilterDomainBound();
			// Unknown-NDV filters need their scan selectivities refined before shrinking the join domain.
			if (!bound.IsValid() || column.total_domain.source == DistinctCountSource::CARDINALITY) {
				all_filtered = false;
				break;
			}
			retained_domain = MaxValue(retained_domain, bound.GetIndex());
		}
		if (all_filtered) {
			equivalence.domain_estimate.Restrict(retained_domain);
		}
	}
	std::sort(state->relation_set_stats.begin(), state->relation_set_stats.end(), SortTdoms);
}

// LCOV_EXCL_START

void CardinalityEstimator::AddRelationNamesToRelationStats(vector<RelationStats> &stats) {
#ifdef DEBUG
	for (auto &total_domain : state->relation_set_stats) {
		for (auto &binding : total_domain.equivalent_relations) {
			D_ASSERT(binding.table_index.index < stats.size());
			string column_name;
			if (binding.column_index < stats[binding.table_index.index].columns.size()) {
				column_name = stats[binding.table_index.index].columns[binding.column_index].name.GetIdentifierName();
			} else {
				column_name = "[unknown]";
			}
			total_domain.column_names.push_back(column_name);
		}
	}
#endif
}

void CardinalityEstimator::PrintRelationStats() {
	for (auto &total_domain : state->relation_set_stats) {
		string domain = "Following columns have the same distinct count: ";
		for (auto &column_name : total_domain.column_names) {
			domain += column_name + ", ";
		}
		domain += "\n TOTAL DOMAIN = " + to_string(total_domain.domain_estimate.GetDistinctCount());
		Printer::Print(domain);
	}
}

// LCOV_EXCL_STOP

} // namespace duckdb
