//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_filter_pusher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/common/table_index.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {

class LogicalOperator;
class LogicalCTERef;
class Optimizer;

class CTEFilterPusher {
public:
	explicit CTEFilterPusher(Optimizer &optimizer);
	//! The materialized CTEs for which the filter of a reference may be dropped from the pushed disjunction when it
	//! includes the filter of another reference (see SemiJoinKeyFilterTransfer)
	void SetKeyFilteredCTEs(unordered_set<TableIndex> key_filtered_ctes_p) {
		key_filtered_ctes = std::move(key_filtered_ctes_p);
	}
	//! Push consumer predicates and eligible join-key restrictions into materialized CTEs.
	unique_ptr<LogicalOperator> Optimize(unique_ptr<LogicalOperator> op);
	//! Discard planner guarantees before transformations that do not preserve consumer identity.
	static void ClearDependencies(LogicalOperator &op);

private:
	//! CTE info needed for creating OR filters that can be pushed down
	struct MaterializedCTEInfo {
		explicit MaterializedCTEInfo(LogicalOperator &materialized_cte);
		LogicalOperator &materialized_cte;
		vector<reference<LogicalOperator>> filters;
		vector<reference<LogicalCTERef>> references;
		bool all_cte_refs_are_filtered;
		bool has_filter_dependency;
	};

private:
	//! Find all materialized CTEs and their refs
	void FindCandidates(LogicalOperator &op);
	bool CanPushFilter(const MaterializedCTEInfo &info);
	bool HasValidDependency(const MaterializedCTEInfo &info);
	//! Creates an OR filter and pushes it into a materialized CTE
	void PushFilterIntoCTE(MaterializedCTEInfo &info);

private:
	//! The optimizer
	Optimizer &optimizer;
	//! The materialized CTEs that have a reference filtered by SemiJoinKeyFilterTransfer
	unordered_set<TableIndex> key_filtered_ctes;
	//! Mapping from CTE index to CTE info, order preserving so deepest CTEs are done first
	InsertionOrderPreservingMap<unique_ptr<MaterializedCTEInfo>> cte_info_map;
};

} // namespace duckdb
