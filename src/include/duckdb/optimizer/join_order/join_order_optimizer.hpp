//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_order_optimizer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/optimizer/join_order/query_graph_manager.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class JoinOrderOptimizer {
public:
	explicit JoinOrderOptimizer(ClientContext &context);

public:
	//! Deepest nesting of child optimizers (one per non-reorderable operator, projection, aggregate, ...) that is still
	//! join-order optimized. Every level recurses through Optimize/Build/ExtractJoinRelations, so a very deep plan
	//! (e.g. a long chain of EXCEPT/INTERSECT) would otherwise overflow the stack long before the plan depth limit is
	//! hit; subplans below this depth keep their original join order, which is always a valid plan.
	static constexpr idx_t MAX_RECURSION_DEPTH = 128;

	JoinOrderOptimizer CreateChildOptimizer();
	//! Perform join reordering inside a plan
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> plan, optional_ptr<RelationStats> stats = nullptr);
	//! Adds/gets materialized CTE stats
	void AddMaterializedCTEStats(TableIndex index, RelationStats &&stats);
	RelationStats GetMaterializedCTEStats(TableIndex index);
	//! Adds/gets delim scan stats
	void AddDelimScanStats(RelationStats &stats);
	RelationStats GetDelimScanStats();

private:
	ClientContext &context;

	//! manages the query graph, relations, and edges between relations
	QueryGraphManager query_graph_manager;

	//! The set of filters extracted from the query graph
	vector<unique_ptr<Expression>> filters;
	//! The set of filter infos created from the extracted filters
	vector<unique_ptr<FilterInfo>> filter_infos;
	//! A map of all expressions a given expression has to be equivalent to. This is used to add "implied join edges".
	//! i.e. in the join A=B AND B=C, the equivalence set of {B} is {A, C}, thus we can add an implied join edge {A = C}
	expression_map_t<vector<FilterInfo *>> equivalence_sets;

	unordered_set<std::string> join_nodes_in_full_plan;

	//! Mapping from materialized CTE index to stats
	unordered_map<TableIndex, RelationStats> materialized_cte_stats;
	//! Stats of Delim Scans of the Delim Join that is currently being optimized
	optional_ptr<RelationStats> delim_scan_stats;
	idx_t depth;

public:
	unordered_set<TableIndex> recursive_cte_indexes;
};

} // namespace duckdb
