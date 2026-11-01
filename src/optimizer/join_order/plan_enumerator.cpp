#include "duckdb/optimizer/join_order/plan_enumerator.hpp"

#include "duckdb/main/client_context.hpp"
#include "duckdb/optimizer/join_order/join_node.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/main/settings.hpp"

#include <cmath>

namespace duckdb {

PlanEnumerator::PlanEnumerator(QueryGraphManager &query_graph_manager, CostModel &cost_model,
                               const QueryGraphEdges &query_graph)
    : query_graph(query_graph), query_graph_manager(query_graph_manager), cost_model(cost_model) {
}

static vector<unordered_set<RelationIndex>> AddSuperSets(const vector<unordered_set<RelationIndex>> &current,
                                                         const vector<RelationIndex> &all_neighbors) {
	vector<unordered_set<RelationIndex>> ret;

	for (const auto &neighbor_set : current) {
		auto max_val = std::max_element(neighbor_set.begin(), neighbor_set.end());
		for (const auto &neighbor : all_neighbors) {
			if (*max_val >= neighbor) {
				continue;
			}
			if (neighbor_set.count(neighbor) == 0) {
				unordered_set<RelationIndex> new_set;
				for (auto &n : neighbor_set) {
					new_set.insert(n);
				}
				new_set.insert(neighbor);
				ret.push_back(new_set);
			}
		}
	}

	return ret;
}

//! Update the exclusion set with all entries in the subgraph
static void UpdateExclusionSet(optional_ptr<JoinRelationSet> node, unordered_set<RelationIndex> &exclusion_set) {
	for (idx_t i = 0; i < node->count; i++) {
		exclusion_set.insert(node->relations[i]);
	}
}

// works by first creating all sets with cardinality 1
// then iterates over each previously created group of subsets and will only add a neighbor if the neighbor
// is greater than all relations in the set.
static vector<unordered_set<RelationIndex>> GetAllNeighborSets(vector<RelationIndex> neighbors) {
	vector<unordered_set<RelationIndex>> ret;
	sort(neighbors.begin(), neighbors.end());
	vector<unordered_set<RelationIndex>> added;
	for (auto &neighbor : neighbors) {
		added.push_back(unordered_set<RelationIndex>({neighbor}));
		ret.push_back(unordered_set<RelationIndex>({neighbor}));
	}
	do {
		added = AddSuperSets(added, neighbors);
		for (auto &d : added) {
			ret.push_back(d);
		}
	} while (!added.empty());
#if DEBUG
	// drive by test to make sure we have an accurate amount of
	// subsets, and that each neighbor is in a correct amount
	// of those subsets.
	D_ASSERT(ret.size() == static_cast<size_t>(std::pow(2, neighbors.size())) - 1);
	for (auto &n : neighbors) {
		idx_t count = 0;
		for (auto &set : ret) {
			if (set.count(n) >= 1) {
				count += 1;
			}
		}
		D_ASSERT(count == static_cast<size_t>(std::pow(2, neighbors.size() - 1)));
	}
#endif
	return ret;
}

const reference_map_t<JoinRelationSet, unique_ptr<DPJoinNode>> &PlanEnumerator::GetPlans() const {
	return plans;
}

const vector<reference<NeighborInfo>> &PlanEnumerator::GetConnections(JoinRelationSet &left, JoinRelationSet &right) {
	auto &left_cache = connection_cache[left];
	auto entry = left_cache.find(right);
	if (entry != left_cache.end()) {
		return entry->second;
	}
	auto connections = query_graph.GetConnections(left, right);
	auto inserted = left_cache.insert(make_pair(reference<JoinRelationSet>(right), std::move(connections)));
	return inserted.first->second;
}

static idx_t GetNeighborSetCacheKey(const vector<RelationIndex> &neighbors) {
	idx_t key = 0;
	for (auto &neighbor : neighbors) {
		D_ASSERT(neighbor.index < sizeof(idx_t) * 8);
		key |= idx_t(1) << neighbor.index;
	}
	return key;
}

const vector<reference<JoinRelationSet>> &PlanEnumerator::GetAllNeighborRelationSets(vector<RelationIndex> neighbors) {
	auto key = GetNeighborSetCacheKey(neighbors);
	auto entry = neighbor_set_cache.find(key);
	if (entry != neighbor_set_cache.end()) {
		return entry->second;
	}

	auto all_subset = GetAllNeighborSets(std::move(neighbors));
	vector<reference<JoinRelationSet>> relation_sets;
	relation_sets.reserve(all_subset.size());
	for (const auto &rel_set : all_subset) {
		relation_sets.push_back(query_graph_manager.set_manager.GetJoinRelation(rel_set));
	}
	auto inserted = neighbor_set_cache.insert(make_pair(key, std::move(relation_sets)));
	return inserted.first->second;
}

//! Create a new JoinTree node by joining together two previous JoinTree nodes
unique_ptr<DPJoinNode> PlanEnumerator::CreateJoinTree(JoinRelationSet &set,
                                                      const vector<reference<NeighborInfo>> &possible_connections,
                                                      DPJoinNode &left, DPJoinNode &right) {
	// FIXME: should consider different join algorithms, should we pick a join algorithm here as well? (probably)
	vector<reference<NeighborInfo>> applicable_connections;
	struct JoinCandidate {
		optional_ptr<JoinOrderOperator> join_operator;
		vector<reference<JoinPredicate>> predicates;
		bool generated_cross_product = false;
		idx_t priority = 0;
	};
	vector<JoinCandidate> candidates;
	for (auto &connection_ref : possible_connections) {
		auto &connection = connection_ref.get();
		if (!query_graph_manager.IsConnectionApplicable(connection, left.set, right.set)) {
			continue;
		}
		applicable_connections.push_back(connection);
		optional_ptr<JoinCandidate> candidate;
		for (auto &entry : candidates) {
			if (entry.join_operator == connection.join_operator &&
			    entry.generated_cross_product == connection.generated_cross_product) {
				candidate = entry;
				break;
			}
		}
		if (!candidate) {
			candidates.emplace_back();
			candidate = candidates.back();
			candidate->join_operator = connection.join_operator;
			candidate->generated_cross_product = connection.generated_cross_product;
		}
		candidate->predicates.insert(candidate->predicates.end(), connection.predicates.begin(),
		                             connection.predicates.end());
		idx_t priority = 0;
		if (!connection.predicates.empty()) {
			priority = 3;
		} else if (connection.join_operator) {
			priority = 2;
		} else if (connection.generated_cross_product) {
			priority = 1;
		}
		candidate->priority = MaxValue(candidate->priority, priority);
	}
	if (applicable_connections.empty()) {
		return nullptr;
	}
	optional_ptr<JoinCandidate> best_candidate;
	for (auto &candidate : candidates) {
		if (!query_graph_manager.IsJoinOrderCandidate(candidate.join_operator, candidate.generated_cross_product,
		                                              left.set, right.set)) {
			continue;
		}
		if (!best_candidate || candidate.priority > best_candidate->priority) {
			best_candidate = candidate;
		}
	}
	if (!best_candidate) {
		return nullptr;
	}
	auto cost = cost_model.ComputeCost(left, right, set, applicable_connections);
	auto result = make_uniq<DPJoinNode>(set, best_candidate->join_operator, std::move(best_candidate->predicates),
	                                    best_candidate->generated_cross_product, left.set, right.set, cost);
	result->cardinality = cost_model.GetCardinalityEstimator().EstimateCardinalityWithSet<idx_t>(set);
	return result;
}

optional_ptr<DPJoinNode> PlanEnumerator::EmitPair(JoinRelationSet &left, JoinRelationSet &right,
                                                  const vector<reference<NeighborInfo>> &info) {
	// get the left and right join plans
	auto left_plan = plans.find(left);
	auto right_plan = plans.find(right);
	if (left_plan == plans.end() || right_plan == plans.end()) {
		throw InternalException("No left or right plan: internal error in join order optimizer");
	}
	auto &new_set = query_graph_manager.set_manager.Union(left, right);
	// create the join tree based on combining the two plans
	auto new_plan = CreateJoinTree(new_set, info, *left_plan->second, *right_plan->second);
	if (!new_plan) {
		return nullptr;
	}
	// check if this plan is the optimal plan we found for this set of relations
	auto entry = plans.find(new_set);
	auto new_cost = new_plan->cost;
	double old_cost = NumericLimits<double>::Maximum();
	if (entry != plans.end()) {
		old_cost = entry->second->cost;
	}
	if (entry == plans.end() || new_cost < old_cost) {
		// the new plan costs less than the old plan. Update our DP table.
		plans[new_set] = std::move(new_plan);
		return plans[new_set].get();
	}
	// Tiebreaker for equal-cost plans: needed for LEFT JOINs,
	// because all orderings preserve the LHS cardinality and always tie.
	// This tiebreaker causes less joins to be flipped from LEFT to RIGHT
	// later by the BuildProbeSideOptimizer, and we strongly prefer LEFT
	if (new_cost == old_cost) {
		auto new_right_cardinality = right_plan->second->cardinality;
		auto existing_right = plans.find(entry->second->right_set);
		if (existing_right != plans.end()) {
			auto old_right_cardinality = existing_right->second->cardinality;
			if (new_right_cardinality > old_right_cardinality) {
				plans[new_set] = std::move(new_plan);
				return plans[new_set].get();
			}
		}
	}
	// Create join node from the plan currently in the DP table.
	return entry->second.get();
}

bool PlanEnumerator::TryEmitPair(JoinRelationSet &left, JoinRelationSet &right,
                                 const vector<reference<NeighborInfo>> &info) {
	pairs++;
	// If a full plan is created, it's possible a node in the plan gets updated. When this happens, make sure you keep
	// emitting pairs until you emit another final plan. Another final plan is guaranteed to be produced because of
	// our symmetry guarantees.
	if (pairs >= 10000) {
		// when the amount of pairs gets too large we exit the dynamic programming and resort to a greedy algorithm
		// FIXME: simple heuristic currently
		// at 10K pairs stop searching exactly and switch to heuristic
		return false;
	}
	EmitPair(left, right, info);
	return true;
}

bool PlanEnumerator::EmitCSG(JoinRelationSet &node) {
	if (node.count == query_graph_manager.relation_manager.NumRelations()) {
		return true;
	}
	// create the exclusion set as everything inside the subgraph AND anything with members BELOW it
	unordered_set<RelationIndex> exclusion_set;
	for (idx_t i = 0; i < node.relations[0].index; i++) {
		exclusion_set.emplace(i);
	}
	UpdateExclusionSet(&node, exclusion_set);
	// find the neighbors given this exclusion set
	auto neighbors = query_graph.GetNeighbors(node, exclusion_set);
	if (neighbors.empty()) {
		return true;
	}

	//! Neighbors should be reversed when iterating over them.
	std::sort(neighbors.begin(), neighbors.end(), std::greater<RelationIndex>());
	for (idx_t i = 0; i < neighbors.size() - 1; i++) {
		D_ASSERT(neighbors[i] > neighbors[i + 1]);
	}

	// Dphyp paper missing this.
	// Because we are traversing in reverse order, we need to add neighbors whose number is smaller than the current
	// node to exclusion_set
	// This avoids duplicated enumeration
	unordered_set<RelationIndex> new_exclusion_set = exclusion_set;
	for (idx_t i = 0; i < neighbors.size(); ++i) {
		D_ASSERT(new_exclusion_set.find(neighbors[i]) == new_exclusion_set.end());
		new_exclusion_set.insert(neighbors[i]);
	}

	for (auto neighbor : neighbors) {
		// since the GetNeighbors only returns the smallest element in a list, the entry might not be connected to
		// (only!) this neighbor,  hence we have to do a connectedness check before we can emit it
		auto &neighbor_relation = query_graph_manager.set_manager.GetJoinRelation(neighbor);
		auto &connections = GetConnections(node, neighbor_relation);
		if (!connections.empty()) {
			if (!TryEmitPair(node, neighbor_relation, connections)) {
				return false;
			}
		}

		if (!EnumerateCmpRecursive(node, neighbor_relation, new_exclusion_set)) {
			return false;
		}

		new_exclusion_set.erase(neighbor);
	}
	return true;
}

bool PlanEnumerator::EnumerateCmpRecursive(JoinRelationSet &left, JoinRelationSet &right,
                                           unordered_set<RelationIndex> &exclusion_set) {
	// get the neighbors of the second relation under the exclusion set
	auto neighbors = query_graph.GetNeighbors(right, exclusion_set);
	if (neighbors.empty()) {
		return true;
	}

	auto &all_subset = GetAllNeighborRelationSets(neighbors);
	vector<reference<JoinRelationSet>> union_sets;
	union_sets.reserve(all_subset.size());
	for (auto &neighbor : all_subset) {
		// emit the combinations of this node and its neighbors
		auto &combined_set = query_graph_manager.set_manager.Union(right, neighbor.get());
		// If combined_set.count == right.count, This means we found a neighbor that has been present before
		// This means we didn't set exclusion_set correctly.
		D_ASSERT(combined_set.count > right.count);
		if (plans.find(combined_set) != plans.end()) {
			auto &connections = GetConnections(left, combined_set);
			if (!connections.empty()) {
				if (!TryEmitPair(left, combined_set, connections)) {
					return false;
				}
			}
		}
		union_sets.push_back(combined_set);
	}

	unordered_set<RelationIndex> new_exclusion_set = exclusion_set;
	for (const auto &neighbor : neighbors) {
		new_exclusion_set.insert(neighbor);
	}

	// recursively enumerate the sets
	for (idx_t i = 0; i < union_sets.size(); i++) {
		// updated the set of excluded entries with this neighbor
		if (!EnumerateCmpRecursive(left, union_sets[i], new_exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool PlanEnumerator::EnumerateCSGRecursive(JoinRelationSet &node, unordered_set<RelationIndex> &exclusion_set) {
	// find neighbors of S under the exclusion set
	auto neighbors = query_graph.GetNeighbors(node, exclusion_set);
	if (neighbors.empty()) {
		return true;
	}

	auto &all_subset = GetAllNeighborRelationSets(neighbors);
	vector<reference<JoinRelationSet>> union_sets;
	union_sets.reserve(all_subset.size());
	for (auto &neighbor : all_subset) {
		// emit the combinations of this node and its neighbors
		auto &new_set = query_graph_manager.set_manager.Union(node, neighbor.get());
		D_ASSERT(new_set.count > node.count);
		if (plans.find(new_set) != plans.end()) {
			if (!EmitCSG(new_set)) {
				return false;
			}
		}
		union_sets.push_back(new_set);
	}

	unordered_set<RelationIndex> new_exclusion_set = exclusion_set;
	for (const auto &neighbor : neighbors) {
		new_exclusion_set.insert(neighbor);
	}

	// recursively enumerate the sets
	for (idx_t i = 0; i < union_sets.size(); i++) {
		// updated the set of excluded entries with this neighbor
		if (!EnumerateCSGRecursive(union_sets[i], new_exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool PlanEnumerator::SolveJoinOrderExactly() {
	// now we perform the actual dynamic programming to compute the final result
	// we enumerate over all the possible pairs in the neighborhood
	for (idx_t i = query_graph_manager.relation_manager.NumRelations(); i > 0; i--) {
		// for every node in the set, we consider it as the start node once
		auto &start_node = query_graph_manager.set_manager.GetJoinRelation(RelationIndex(i - 1));
		// emit the start node
		if (!EmitCSG(start_node)) {
			return false;
		}
		// initialize the set of exclusion_set as all the nodes with a number below this
		unordered_set<RelationIndex> exclusion_set;
		for (idx_t j = 0; j < i; j++) {
			exclusion_set.emplace(j);
		}
		// then we recursively search for neighbors that do not belong to the banned entries
		if (!EnumerateCSGRecursive(start_node, exclusion_set)) {
			return false;
		}
	}
	return true;
}

bool PlanEnumerator::SolveJoinOrderApproximately() {
	// at this point, we exited the dynamic programming but did not compute the final join order because it took too
	// long instead, we use a greedy heuristic to obtain a join ordering now we use Greedy Operator Ordering to
	// construct the result tree first we start out with all the base relations (the to-be-joined relations)
	vector<reference<JoinRelationSet>> join_relations; // T in the paper
	for (idx_t i = 0; i < query_graph_manager.relation_manager.NumRelations(); i++) {
		join_relations.push_back(query_graph_manager.set_manager.GetJoinRelation(RelationIndex(i)));
	}

	// in every step of the algorithm, we greedily pick the join between the to-be-joined relations that has the
	// smallest cost. Pair costs are deterministic while the query graph is unchanged, so we evaluate each pair once
	// and keep the viable candidates in a list, instead of re-emitting every pair in every step
	struct GreedyCandidate {
		optional_ptr<JoinRelationSet> left;
		optional_ptr<JoinRelationSet> right;
		double cost;
	};
	vector<GreedyCandidate> candidates;
	auto EmitCandidate = [&](JoinRelationSet &left, JoinRelationSet &right) {
		auto &connection = GetConnections(left, right);
		if (connection.empty()) {
			return;
		}
		auto node = EmitPair(left, right, connection);
		if (node) {
			candidates.push_back({&left, &right, node->cost});
		}
	};
	auto InitializeCandidates = [&]() {
		candidates.clear();
		for (idx_t i = 0; i < join_relations.size(); i++) {
			for (idx_t j = i + 1; j < join_relations.size(); j++) {
				EmitCandidate(join_relations[i], join_relations[j]);
			}
		}
	};
	InitializeCandidates();

	while (join_relations.size() > 1) {
		optional_ptr<JoinRelationSet> best_left;
		optional_ptr<JoinRelationSet> best_right;
		double best_cost = NumericLimits<double>::Maximum();
		for (auto &candidate : candidates) {
			if (!best_left || candidate.cost < best_cost) {
				best_cost = candidate.cost;
				best_left = candidate.left;
				best_right = candidate.right;
			}
		}
		if (!best_left) {
			if (ActivateRequiredCrossProducts()) {
				// the query graph gained edges, which invalidates the evaluated candidates
				InitializeCandidates();
				continue;
			}
			// Optimizer-introduced cross products are valid only inside an inner-join companion set (Section 6.2).
			for (idx_t i = 0; i < join_relations.size(); i++) {
				auto &left = join_relations[i].get();
				for (idx_t j = i + 1; j < join_relations.size(); j++) {
					auto &right = join_relations[j].get();
					if (!query_graph_manager.CanCreateCrossProduct(left, right)) {
						continue;
					}
					auto left_plan = plans.find(left);
					auto right_plan = plans.find(right);
					D_ASSERT(left_plan != plans.end() && right_plan != plans.end());
					auto cost = left_plan->second->cost + right_plan->second->cost;
					if (!best_left || cost < best_cost) {
						best_cost = cost;
						best_left = left;
						best_right = right;
					}
				}
			}
			if (!best_left) {
				return false;
			}
			// create a cross product edge (i.e. edge with empty filter) between these two sets in the query graph
			query_graph_manager.CreateQueryGraphCrossProduct(*best_left, *best_right);
			// the new edge only connects this pair, which is merged below, so the other candidates remain valid
			connection_cache.clear();
			// now emit the pair and continue with the algorithm
			auto &connections = GetConnections(*best_left, *best_right);
			D_ASSERT(!connections.empty());

			auto cross_product = EmitPair(*best_left, *best_right, connections);
			if (!cross_product) {
				return false;
			}
		}
		// now update the to-be-checked pairs: remove left and right, and add the combination
		auto &new_set = query_graph_manager.set_manager.Union(*best_left, *best_right);
		for (idx_t i = join_relations.size(); i > 0; i--) {
			auto &relation = join_relations[i - 1].get();
			if (&relation == best_left.get() || &relation == best_right.get()) {
				join_relations.erase(join_relations.begin() + NumericCast<int64_t>(i - 1));
			}
		}
		idx_t kept_candidates = 0;
		for (idx_t candidate_idx = 0; candidate_idx < candidates.size(); candidate_idx++) {
			auto &candidate = candidates[candidate_idx];
			if (candidate.left == best_left || candidate.left == best_right || candidate.right == best_left ||
			    candidate.right == best_right) {
				continue;
			}
			candidates[kept_candidates++] = candidate;
		}
		candidates.resize(kept_candidates);
		for (auto &relation : join_relations) {
			EmitCandidate(relation, new_set);
		}
		join_relations.push_back(new_set);
	}
	return true;
}

bool PlanEnumerator::HasCompletePlan() const {
	unordered_set<RelationIndex> bindings;
	for (idx_t relation_idx = 0; relation_idx < query_graph_manager.relation_manager.NumRelations(); relation_idx++) {
		bindings.emplace(relation_idx);
	}
	auto &total_relation = query_graph_manager.set_manager.GetJoinRelation(bindings);
	return plans.find(total_relation) != plans.end();
}

bool PlanEnumerator::ActivateRequiredCrossProducts() {
	if (!query_graph_manager.ActivateRequiredCrossProducts()) {
		return false;
	}
	pairs = 0;
	connection_cache.clear();
	neighbor_set_cache.clear();
	return true;
}

bool PlanEnumerator::PlanUsesCrossProduct(const DPJoinNode &node) const {
	if (node.is_leaf) {
		return false;
	}
	if (node.generated_cross_product ||
	    (node.join_operator && node.join_operator->type == JoinOrderOperatorType::CROSS_PRODUCT)) {
		return true;
	}
	auto left = plans.find(node.left_set);
	auto right = plans.find(node.right_set);
	D_ASSERT(left != plans.end() && right != plans.end());
	return PlanUsesCrossProduct(*left->second) || PlanUsesCrossProduct(*right->second);
}

void PlanEnumerator::InitLeafPlans() {
	// First we initialize each of the single-node plans with themselves and with their cardinalities these are the leaf
	// nodes of the join tree NOTE: we can just use pointers to JoinRelationSet* here because the GetJoinRelation
	// function ensures that a unique combination of relations will have a unique JoinRelationSet object.
	// first initialize equivalent relations based on the filters
	auto relation_stats = query_graph_manager.relation_manager.GetRelationStats();

	cost_model.GetCardinalityEstimator().InitEquivalentRelations();
	cost_model.GetCardinalityEstimator().AddRelationNamesToRelationStats(relation_stats);

	// then update the total domains based on the cardinalities of each relation.
	for (idx_t i = 0; i < relation_stats.size(); i++) {
		auto stats = relation_stats.at(i);
		auto &relation_set = query_graph_manager.set_manager.GetJoinRelation(RelationIndex(i));
		auto join_node = make_uniq<DPJoinNode>(relation_set);
		join_node->cost = 0;
		join_node->cardinality = stats.cardinality;
		D_ASSERT(join_node->set.count == 1);
		plans[relation_set] = std::move(join_node);
		cost_model.GetCardinalityEstimator().InitCardinalityEstimatorProps(&relation_set, stats);
	}
	cost_model.GetCardinalityEstimator().RestrictFilterDomains(relation_stats);
}

// the plan enumeration is a straight implementation of the paper "Dynamic Programming Strikes Back" by Guido
// Moerkotte and Thomas Neumannn, see that paper for additional info/documentation bonus slides:
// https://db.in.tum.de/teaching/ws1415/queryopt/chapter3.pdf?lang=de
bool PlanEnumerator::SolveJoinOrder() {
	bool force_no_cross_product = Settings::Get<DebugForceNoCrossProductSetting>(query_graph_manager.context);
	auto swap_to_approximate_threshold =
	    Settings::Get<ApproximateJoinOrderThresholdSetting>(query_graph_manager.context);

	// first try to solve the join order exactly
	bool solved;
	if (query_graph_manager.relation_manager.NumRelations() >= swap_to_approximate_threshold) {
		solved = SolveJoinOrderApproximately();
	} else {
		auto completed_exactly = SolveJoinOrderExactly();
		if (completed_exactly && !HasCompletePlan() && ActivateRequiredCrossProducts()) {
			completed_exactly = SolveJoinOrderExactly();
		}
		if (!completed_exactly || !HasCompletePlan()) {
			// Exact enumeration either reached its pair budget or could not connect the graph.
			solved = SolveJoinOrderApproximately();
		} else {
			solved = true;
		}
	}

	// now the optimal join path should have been found
	// get it from the node
	unordered_set<RelationIndex> bindings;
	for (idx_t i = 0; i < query_graph_manager.relation_manager.NumRelations(); i++) {
		bindings.emplace(i);
	}
	auto &total_relation = query_graph_manager.set_manager.GetJoinRelation(bindings);
	auto final_plan = plans.find(total_relation);
	if (!solved) {
		if (force_no_cross_product && query_graph_manager.RequiresCrossProduct()) {
			throw InvalidInputException(
			    "Query requires a cross-product, but 'force_no_cross_product' PRAGMA is enabled");
		}
		return false;
	}
	if (final_plan == plans.end()) {
		if (force_no_cross_product) {
			throw InvalidInputException(
			    "Query requires a cross-product, but 'force_no_cross_product' PRAGMA is enabled");
		}
		return false;
	}
	if (force_no_cross_product && PlanUsesCrossProduct(*final_plan->second)) {
		throw InvalidInputException("Query requires a cross-product, but 'force_no_cross_product' PRAGMA is enabled");
	}
	return true;
}

} // namespace duckdb
