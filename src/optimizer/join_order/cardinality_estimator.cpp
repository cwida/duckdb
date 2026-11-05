#include "duckdb/optimizer/join_order/cardinality_estimator.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/optimizer/join_order/join_order_operator.hpp"
#include "duckdb/optimizer/join_order/join_predicate.hpp"
#include "duckdb/planner/expression/bound_comparison_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"

#include <algorithm>

namespace duckdb {

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

struct SelectivitySubgraph {
public:
	SelectivitySubgraph() : relations(nullptr), selectivity(1) {
	}

public:
	optional_ptr<JoinRelationSet> relations;
	CardinalityValue selectivity;
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

struct LogicalJoinStats {
	LogicalJoinStats(const JoinOrderOperator &op, JoinRelationSet &scope, CardinalityValue selectivity)
	    : op(op), scope(scope), selectivity(selectivity) {
	}

	reference<const JoinOrderOperator> op;
	reference<JoinRelationSet> scope;
	CardinalityValue selectivity;
};

struct CardinalityEstimatorState {
	vector<RelationsSetToStats> relation_set_stats;
	vector<CardinalityValue> leaf_cardinalities;
	vector<reference<const JoinOrderOperator>> operators;
	vector<LogicalJoinStats> boundaries;
	reference_map_t<JoinRelationSet, CardinalityValue> inner_selectivities;
	reference_map_t<JoinRelationSet, CardinalityValue> cardinalities;
	vector<reference<FilterInfo>> residual_filters;
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

static bool IsResidualFilter(const FilterInfo &filter) {
	if (filter.join_type != JoinType::INNER ||
	    (filter.set.get().count < 2 && !filter.source_operator_index.IsValid())) {
		return false;
	}
	return filter.must_remain_filter || filter.from_residual_predicate ||
	       (filter.filter && filter.filter->GetExpressionType() == ExpressionType::CONJUNCTION_OR);
}

void CardinalityEstimator::InitEquivalentRelations() {
	state->relation_set_stats.clear();
	state->residual_filters.clear();

	for (auto predicate_ref : predicate_model.GetPredicates()) {
		auto &filter = predicate_ref.get().GetFilter();
		if (IsResidualFilter(filter)) {
			state->residual_filters.push_back(filter);
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
}

CardinalityValue CardinalityEstimator::GetNumerator(JoinRelationSet &set) {
	CardinalityValue numerator;
	for (idx_t i = 0; i < set.count; i++) {
		auto cardinality = state->leaf_cardinalities.at(set.relations[i].index);
		// Logical output handles exact zero separately from the selectivity model.
		numerator *= cardinality.IsZero() ? CardinalityValue(1) : cardinality;
	}
	return numerator;
}

bool CardinalityEstimator::IsFilterApplicable(const FilterInfo &filter, JoinRelationSet &set) const {
	if (!filter.set.get().Empty() && !JoinRelationSet::IsSubset(set, filter.set)) {
		return false;
	}
	if (filter.source_operator_index.IsValid()) {
		auto &op = state->operators.at(filter.source_operator_index.GetIndex()).get();
		if (JoinOrderConflictDetector::MustApplyAsOperator(op)) {
			return JoinOrderConflictDetector::IsCompletedBy(op, set);
		}
	}
	return true;
}

vector<FilterInfoWithTotalDomains> GetEdges(vector<RelationsSetToStats> &relations_to_tdom,
                                            JoinRelationSet &requested_set) {
	vector<FilterInfoWithTotalDomains> res;
	for (auto &relation_2_tdom : relations_to_tdom) {
		for (auto predicate_ref : relation_2_tdom.predicates) {
			auto &predicate = predicate_ref.get();
			auto &filter = predicate.GetFilter();
			if (predicate.GetJoinType() != JoinType::INNER) {
				continue;
			}
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
	auto &filter = edge.GetFilter();
	if (filter.filter && BoundComparisonExpression::IsComparison(*filter.filter)) {
		auto &binding = edge.GetPredicate().GetStatsBinding(left);
		if (binding.table_index.IsValid()) {
			return &set_manager.GetJoinRelation(RelationIndex(binding.table_index.index));
		}
	}
	return left ? edge.GetPredicate().GetLeftSetOptional() : edge.GetPredicate().GetRightSetOptional();
}

bool EdgeConnects(FilterInfoWithTotalDomains &edge, SelectivitySubgraph &subgraph,
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

vector<idx_t> SubgraphsConnectedByEdge(FilterInfoWithTotalDomains &edge, vector<SelectivitySubgraph> &subgraphs,
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

// Apply the selectivity multiplier for a given comparison type and effective distinct count.
static CardinalityValue ApplyComparisonRatio(CardinalityValue base_selectivity, ExpressionType comparison_type,
                                             double effective_d) {
	switch (comparison_type) {
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return base_selectivity / CardinalityValue(effective_d);
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return effective_d > 1 ? base_selectivity * CardinalityValue(effective_d - 1) / CardinalityValue(effective_d)
		                       : base_selectivity;
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
		// Model strict comparisons as half the cross product, including at small domains.
		return base_selectivity / CardinalityValue(2);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return base_selectivity * CardinalityValue(effective_d + 1) / CardinalityValue(2 * effective_d);
	default:
		return base_selectivity;
	}
}

static double GetEffectiveDistinctCount(double distinct_count) {
	return distinct_count <= 0 ? 1 : distinct_count;
}

CardinalityValue CardinalityEstimator::CalculateInnerJoinSelectivity(CardinalityValue base_selectivity,
                                                                     FilterInfoWithTotalDomains &filter) {
	auto effective_d = GetEffectiveDistinctCount(filter.GetDistinctCount());
	auto comparison_type = filter.GetComparisonType();
	if (comparison_type == ExpressionType::INVALID) {
		return base_selectivity / CardinalityValue(effective_d);
	}
	return ApplyComparisonRatio(base_selectivity, comparison_type, effective_d);
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
		auto cardinality = GetNumerator(single_relation).GetDouble();
		if (cardinality <= 0) {
			return 0;
		}
		cap = MinValue(cap, cardinality);
	}
	return cap == NumericLimits<double>::Maximum() ? 0 : cap;
}

bool CardinalityEstimator::ApplyJoinPairCap(CardinalityValue &target_selectivity, JoinRelationSet &join_pair,
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
		// Lower weak same-pair composite evidence to the FK/PK cap.
		target_selectivity = target_selectivity * CardinalityValue(first_d) / CardinalityValue(cap);
		first_d = cap;
	}
	capped_join_pairs.insert(join_pair);
	return true;
}

bool CardinalityEstimator::ApplyJoinIncrement(CardinalityValue &target_selectivity, FilterInfoWithTotalDomains &edge,
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
		return ApplyJoinPairCap(target_selectivity, *target_pair, join_pair_stats, capped_join_pairs);
	}

	return false;
}

bool CardinalityEstimator::ApplyCompositeJoinPairCaps(
    CardinalityValue &target_selectivity, JoinRelationSet &scope,
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
		applied = ApplyJoinPairCap(target_selectivity, join_pair, join_pair_stats, capped_join_pairs) || applied;
	}
	return applied;
}

enum class SelectivityEdgeKind : uint8_t {
	INNER_EQUIVALENCE,
	NON_EQUALITY_SELECTIVITY,
	REDUNDANT_TRANSITIVE_EQUALITY,
	OTHER
};

static bool EquivalenceGroupApplied(FilterInfoWithTotalDomains &edge, const unordered_set<idx_t> &applied_groups) {
	auto equality_class_index = edge.GetPredicate().GetEqualityClassIndex();
	return equality_class_index.IsValid() && applied_groups.count(equality_class_index.GetIndex()) > 0;
}

static SelectivityEdgeKind ClassifySelectivityEdge(FilterInfoWithTotalDomains &edge,
                                                   const unordered_set<idx_t> &applied_groups) {
	if (EquivalenceGroupApplied(edge, applied_groups)) {
		return SelectivityEdgeKind::REDUNDANT_TRANSITIVE_EQUALITY;
	}
	switch (edge.GetPredicate().GetPredicateClass()) {
	case JoinPredicateClass::INNER_EQUALITY:
		return SelectivityEdgeKind::INNER_EQUIVALENCE;
	case JoinPredicateClass::INNER_NON_EQUALITY:
		return SelectivityEdgeKind::NON_EQUALITY_SELECTIVITY;
	default:
		return SelectivityEdgeKind::OTHER;
	}
}

static bool CanIncrementExistingJoin(SelectivityEdgeKind kind) {
	return kind == SelectivityEdgeKind::INNER_EQUIVALENCE;
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

struct SelectivityState {
	vector<SelectivitySubgraph> subgraphs;
	unordered_set<idx_t> applied_equivalence_groups;
	reference_map_t<JoinRelationSet, CompositeJoinPairStats> join_pair_stats;
	reference_set_t<JoinRelationSet> capped_join_pairs;
	unordered_set<idx_t> unused_edge_tdoms;
};

static bool SelectivitySubgraphComplete(SelectivityState &state, JoinRelationSet &set) {
	return state.subgraphs.size() == 1 && state.subgraphs.at(0).relations &&
	       RefersToSameObject(*state.subgraphs.at(0).relations, set);
}

void CardinalityEstimator::ProcessSelectivityEdge(FilterInfoWithTotalDomains &edge, JoinRelationSet &requested_set,
                                                  SelectivityState &state) {
	auto edge_kind = ClassifySelectivityEdge(edge, state.applied_equivalence_groups);
	if (SelectivitySubgraphComplete(state, requested_set)) {
		auto &complete_subgraph = state.subgraphs.at(0);
		ApplyCompositeJoinPairCaps(complete_subgraph.selectivity, *complete_subgraph.relations, state.join_pair_stats,
		                           state.capped_join_pairs);
		if (edge_kind == SelectivityEdgeKind::REDUNDANT_TRANSITIVE_EQUALITY) {
			return;
		}
		if (CanIncrementExistingJoin(edge_kind) &&
		    ApplyJoinIncrement(complete_subgraph.selectivity, edge, state.join_pair_stats, state.capped_join_pairs,
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
		CreateSelectivitySubgraph(edge, *edge_left_set, *edge_right_set, state);
	} else if (subgraph_connections.size() == 1) {
		ExtendSelectivitySubgraph(subgraph_connections.at(0), edge, *edge_left_set, *edge_right_set,
		                          CanIncrementExistingJoin(edge_kind), state);
	} else if (subgraph_connections.size() == 2) {
		MergeSelectivitySubgraphs(subgraph_connections, edge, state);
	}
}

void CardinalityEstimator::CreateSelectivitySubgraph(FilterInfoWithTotalDomains &edge, JoinRelationSet &edge_left_set,
                                                     JoinRelationSet &edge_right_set, SelectivityState &state) {
	auto left_subgraph = SelectivitySubgraph();
	auto selectivity = CalculateInnerJoinSelectivity(CardinalityValue(1), edge);
	left_subgraph.relations = &set_manager.Union(edge_left_set, edge_right_set);
	left_subgraph.selectivity = selectivity;
	ApplyCompositeJoinPairCaps(left_subgraph.selectivity, *left_subgraph.relations, state.join_pair_stats,
	                           state.capped_join_pairs);
	state.subgraphs.push_back(left_subgraph);
}

void CardinalityEstimator::ExtendSelectivitySubgraph(idx_t subgraph_index, FilterInfoWithTotalDomains &edge,
                                                     JoinRelationSet &edge_left_set, JoinRelationSet &edge_right_set,
                                                     bool can_increment_existing_join, SelectivityState &state) {
	auto left_subgraph = &state.subgraphs.at(subgraph_index);
	auto right_subgraph = SelectivitySubgraph();
	right_subgraph.relations = &edge_right_set;
	if (JoinRelationSet::IsSubset(*left_subgraph->relations, *right_subgraph.relations)) {
		right_subgraph.relations = &edge_left_set;
	}

	if (JoinRelationSet::IsSubset(*left_subgraph->relations, edge_left_set) &&
	    JoinRelationSet::IsSubset(*left_subgraph->relations, edge_right_set)) {
		if (can_increment_existing_join) {
			ApplyJoinIncrement(left_subgraph->selectivity, edge, state.join_pair_stats, state.capped_join_pairs,
			                   *left_subgraph->relations);
		}
		ApplyCompositeJoinPairCaps(left_subgraph->selectivity, *left_subgraph->relations, state.join_pair_stats,
		                           state.capped_join_pairs);
		return;
	}

	auto selectivity = CalculateInnerJoinSelectivity(left_subgraph->selectivity, edge);
	left_subgraph->relations = &set_manager.Union(*left_subgraph->relations, *right_subgraph.relations);
	left_subgraph->selectivity = selectivity;
	ApplyCompositeJoinPairCaps(left_subgraph->selectivity, *left_subgraph->relations, state.join_pair_stats,
	                           state.capped_join_pairs);
}

void CardinalityEstimator::MergeSelectivitySubgraphs(const vector<idx_t> &subgraph_connections,
                                                     FilterInfoWithTotalDomains &edge, SelectivityState &state) {
	D_ASSERT(subgraph_connections.size() == 2);
	D_ASSERT(subgraph_connections.at(0) < subgraph_connections.at(1));
	auto subgraph_to_merge_into = &state.subgraphs.at(subgraph_connections.at(0));
	auto subgraph_to_delete = &state.subgraphs.at(subgraph_connections.at(1));
	auto selectivity =
	    CalculateInnerJoinSelectivity(subgraph_to_merge_into->selectivity * subgraph_to_delete->selectivity, edge);
	subgraph_to_merge_into->relations =
	    &set_manager.Union(*subgraph_to_merge_into->relations, *subgraph_to_delete->relations);
	subgraph_to_merge_into->selectivity = selectivity;
	ApplyCompositeJoinPairCaps(subgraph_to_merge_into->selectivity, *subgraph_to_merge_into->relations,
	                           state.join_pair_stats, state.capped_join_pairs);
	subgraph_to_delete->relations = nullptr;
	auto remove_start = std::remove_if(state.subgraphs.begin(), state.subgraphs.end(),
	                                   [](SelectivitySubgraph &s) { return !s.relations; });
	state.subgraphs.erase(remove_start, state.subgraphs.end());
}

void CardinalityEstimator::MergeDisconnectedSelectivitySubgraphs(SelectivityState &state) {
	if (state.subgraphs.size() <= 1) {
		return;
	}
	auto final_subgraph = state.subgraphs.at(0);
	for (auto merge_with = state.subgraphs.begin() + 1; merge_with != state.subgraphs.end(); merge_with++) {
		D_ASSERT(final_subgraph.relations && merge_with->relations);
		final_subgraph.relations = &set_manager.Union(*final_subgraph.relations, *merge_with->relations);
		final_subgraph.selectivity *= merge_with->selectivity;
	}
	state.subgraphs.clear();
	state.subgraphs.push_back(final_subgraph);
}

CardinalityValue CardinalityEstimator::GetSelectivity(JoinRelationSet &set) {
	SelectivityState selectivity_state;
	auto edges = GetEdges(state->relation_set_stats, set);
	for (auto &edge : edges) {
		if (IsFilterApplicable(edge.GetFilter(), set) && !IsResidualFilter(edge.GetFilter())) {
			ProcessSelectivityEdge(edge, set, selectivity_state);
		}
	}
	MergeDisconnectedSelectivitySubgraphs(selectivity_state);
	auto result =
	    selectivity_state.subgraphs.empty() ? CardinalityValue(1) : selectivity_state.subgraphs[0].selectivity;
	return result / CardinalityValue(1.0 + static_cast<double>(selectivity_state.unused_edge_tdoms.size()));
}

CardinalityValue CardinalityEstimator::GetInnerSelectivity(JoinRelationSet &set) {
	auto entry = state->inner_selectivities.find(set);
	if (entry != state->inner_selectivities.end()) {
		return entry->second;
	}
	auto result = GetSelectivity(set);
	for (auto &filter : state->residual_filters) {
		if (IsFilterApplicable(filter, set)) {
			result *= CardinalityValue(RelationStatisticsHelper::DEFAULT_SELECTIVITY);
		}
	}
	state->inner_selectivities.emplace(set, result);
	return result;
}

CardinalityValue CardinalityEstimator::GetJoinSelectivity(JoinRelationSet &set, JoinRelationSet &left,
                                                          JoinRelationSet &right) {
	// Leaf cardinalities cancel because the inputs partition the combined set.
	auto combined = GetInnerSelectivity(set);
	auto left_domains = GetInnerSelectivity(left);
	auto right_domains = GetInnerSelectivity(right);
	D_ASSERT(!left_domains.IsZero() && !right_domains.IsZero());
	return MinValue(CardinalityValue(1), combined / left_domains / right_domains);
}

CardinalityValue CardinalityEstimator::EstimateCardinality(JoinRelationSet &set) {
	D_ASSERT(!state->leaf_cardinalities.empty() && !set.Empty());
	auto entry = state->cardinalities.find(set);
	if (entry != state->cardinalities.end()) {
		return entry->second;
	}
	optional_ptr<const LogicalJoinStats> boundary;
	optional_ptr<JoinRelationSet> region;
	for (auto &candidate : state->boundaries) {
		if (!JoinOrderConflictDetector::IsCompletedBy(candidate.op, set)) {
			continue;
		}
		auto &projected = set_manager.Intersection(set, candidate.scope);
		// Original scopes are nested or disjoint; postorder indexes break ties after projection.
		if (!region || projected.count > region->count ||
		    (projected.count == region->count && candidate.op.get().index > boundary->op.get().index)) {
			boundary = candidate;
			region = projected;
		}
	}
	CardinalityValue result;
	if (!boundary) {
		result = GetNumerator(set) * GetInnerSelectivity(set);
		for (idx_t i = 0; i < set.count; i++) {
			if (state->leaf_cardinalities.at(set.relations[i].index).IsZero()) {
				result = CardinalityValue(0);
				break;
			}
		}
	} else if (region->count < set.count) {
		auto &remaining = set_manager.Difference(set, *region);
		result =
		    EstimateCardinality(*region) * EstimateCardinality(remaining) * GetJoinSelectivity(set, *region, remaining);
	} else {
		auto &op = boundary->op.get();
		auto &left = set_manager.Intersection(set, op.left_relations);
		auto &right = set_manager.Intersection(set, op.right_relations);
		D_ASSERT(!left.Empty() && !right.Empty() && left.count < set.count && right.count < set.count);
		auto left_rows = EstimateCardinality(left);
		auto right_rows = EstimateCardinality(right);
		if (op.type == JoinOrderOperatorType::LEFT) {
			result = MaxValue(left_rows, left_rows * right_rows * boundary->selectivity);
		} else {
			auto type = op.type == JoinOrderOperatorType::SEMI ? JoinType::SEMI : JoinType::ANTI;
			result = RelationStatisticsHelper::EstimateSemiAntiJoinFallbackCardinality(
			    left_rows, right_rows.IsZero() ? 0 : 1, type);
		}
		// Filters above the boundary see its output, including NULL-extended LEFT rows.
		result *= GetJoinSelectivity(set, left, right);
	}
	state->cardinalities.emplace(set, result);
	return result;
}

static CardinalityValue GetResidualOnSelectivity(const Expression &expression) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &value = expression.Cast<BoundConstantExpression>().GetValue();
		return CardinalityValue(!value.IsNull() && value.GetValue<bool>() ? 1 : 0);
	}
	return CardinalityValue(RelationStatisticsHelper::DEFAULT_SELECTIVITY);
}

CardinalityValue CardinalityEstimator::GetLeftJoinSelectivity(const JoinOrderOperator &join_operator) {
	CardinalityValue selectivity;
	unordered_set<idx_t> equality_groups;
	unordered_set<idx_t> accounted_predicates;
	reference_map_t<JoinRelationSet, CardinalityValue> pair_selectivities;
	reference_map_t<JoinRelationSet, CompositeJoinPairStats> pair_stats;
	for (auto &domain : state->relation_set_stats) {
		for (auto predicate_ref : domain.predicates) {
			auto &predicate = predicate_ref.get();
			auto source = predicate.GetFilter().source_operator_index;
			if (!source.IsValid() || source.GetIndex() != join_operator.index) {
				continue;
			}
			accounted_predicates.insert(predicate.GetFilter().filter_index);
			auto group = predicate.GetFilter().left_equality_group;
			if (group.IsValid() && !equality_groups.insert(group.GetIndex()).second) {
				continue;
			}
			FilterInfoWithTotalDomains edge(predicate, domain);
			auto factor = CalculateInnerJoinSelectivity(CardinalityValue(1), edge);
			auto pair = predicate.GetCompositeJoinPair();
			if (pair) {
				pair_stats[*pair].RegisterDistinctCount(edge.GetDistinctCount());
				auto entry = pair_selectivities.emplace(*pair, CardinalityValue(1)).first;
				entry->second *= factor;
			} else {
				selectivity *= factor;
			}
		}
	}
	for (auto &entry : pair_selectivities) {
		auto cap = GetJoinPairCap(entry.first);
		selectivity *=
		    pair_stats.at(entry.first).CanApplyCap(cap) ? CardinalityValue(1) / CardinalityValue(cap) : entry.second;
	}
	for (auto index : join_operator.costing_predicate_indices) {
		if (!accounted_predicates.count(index)) {
			selectivity *= CardinalityValue(RelationStatisticsHelper::DEFAULT_SELECTIVITY);
		}
	}
	for (auto &residual : join_operator.residual_predicates) {
		selectivity *= GetResidualOnSelectivity(residual);
	}
	return selectivity;
}

static bool IsSelectivityComparison(const JoinPredicate &predicate) {
	switch (predicate.GetComparisonType()) {
	case ExpressionType::COMPARE_NOTEQUAL:
	case ExpressionType::COMPARE_DISTINCT_FROM:
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		break;
	default:
		return false;
	}
	auto predicate_class = predicate.GetPredicateClass();
	return predicate_class == JoinPredicateClass::INNER_NON_EQUALITY ||
	       predicate_class == JoinPredicateClass::LEFT_JOIN;
}

static bool IsSelectivityOnlyGroup(const RelationsSetToStats &stats) {
	if (stats.predicates.empty()) {
		return false;
	}
	for (auto &predicate : stats.predicates) {
		if (!IsSelectivityComparison(predicate.get())) {
			return false;
		}
	}
	return true;
}

bool SortTdoms(const RelationsSetToStats &a, const RelationsSetToStats &b) {
	// process pure selectivity (comparison) edges last so a broad comparison domain
	// cannot displace an equality domain from the denominator
	auto a_selectivity_only = IsSelectivityOnlyGroup(a);
	auto b_selectivity_only = IsSelectivityOnlyGroup(b);
	if (a_selectivity_only != b_selectivity_only) {
		return b_selectivity_only;
	}
	return a.domain_estimate.GetDistinctCount() > b.domain_estimate.GetDistinctCount();
}

void CardinalityEstimator::Initialize(const vector<RelationStats> &relations,
                                      const vector<unique_ptr<JoinOrderOperator>> &operators) {
	D_ASSERT(state->leaf_cardinalities.empty());
	InitEquivalentRelations();
	for (idx_t i = 0; i < relations.size(); i++) {
		auto &stats = relations[i];
		D_ASSERT(stats.stats_initialized);
		auto cardinality = CardinalityValue(static_cast<double>(stats.cardinality));
		state->leaf_cardinalities.push_back(cardinality);
		state->cardinalities.emplace(set_manager.GetJoinRelation(RelationIndex(i)), cardinality);
		UpdateTotalDomains(RelationIndex(i), stats);
	}
	RestrictFilterDomains(relations);
	for (auto &op : operators) {
		D_ASSERT(op->index == state->operators.size());
		state->operators.push_back(*op);
		if (op->type == JoinOrderOperatorType::INNER || op->type == JoinOrderOperatorType::CROSS_PRODUCT) {
			continue;
		}
		auto selectivity = op->type == JoinOrderOperatorType::LEFT ? GetLeftJoinSelectivity(*op) : CardinalityValue(1);
		state->boundaries.emplace_back(*op, set_manager.Union(op->left_relations, op->right_relations), selectivity);
	}
}

void CardinalityEstimator::UpdateTotalDomains(RelationIndex relation_id, const RelationStats &stats) {
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

} // namespace duckdb
