//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/cte_join_filter_pusher.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/insertion_order_preserving_map.hpp"
#include "duckdb/planner/bound_parameter_map.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

class LogicalOperator;
class LogicalCTERef;
class Optimizer;

class CTEJoinFilterPusher {
public:
	explicit CTEJoinFilterPusher(Optimizer &optimizer);
	void Optimize(LogicalOperator &op);

private:
	struct MaterializedCTEInfo {
		explicit MaterializedCTEInfo(LogicalOperator &cte) : materialized_cte(cte) {
		}
		LogicalOperator &materialized_cte;
		vector<reference<LogicalCTERef>> references;
		vector<TableIndex> ancestors;
	};

	void FindCandidates(LogicalOperator &op);
	bool HasValidDependency(const MaterializedCTEInfo &info);

private:
	Optimizer &optimizer;
	InsertionOrderPreservingMap<unique_ptr<MaterializedCTEInfo>> cte_info_map;
	vector<TableIndex> available_ctes;
	//! Parameter data, in case the plan is part of a prepared statement and a subtree must be copied
	optional_ptr<bound_parameter_map_t> parameter_data;
};

} // namespace duckdb
