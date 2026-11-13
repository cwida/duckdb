//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/filter/physical_top_key_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/bound_query_node.hpp"

namespace duckdb {
struct DynamicFilterData;

//! PhysicalTopKeyFilter is a streaming filter below a grouped aggregate whose Top-N orders by a prefix of the group
//! keys. Every thread keeps the smallest "limit" distinct key tuples that it has seen. The largest of them is not
//! smaller than the limit-th smallest distinct key tuple of the whole input, and neither is the smallest such bound of
//! all threads. A row passes if its key tuple is not larger than that bound (ties included), so that every group that
//! the Top-N can return still receives all of its rows.
class PhysicalTopKeyFilter : public CachingPhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::TOP_KEY_FILTER;

public:
	PhysicalTopKeyFilter(PhysicalPlan &physical_plan, vector<LogicalType> types, vector<BoundOrderByNode> orders,
	                     idx_t limit, shared_ptr<DynamicFilterData> dynamic_filter, idx_t estimated_cardinality);
	~PhysicalTopKeyFilter() override;

	//! The keys and their order
	vector<BoundOrderByNode> orders;
	//! The number of distinct key tuples that pass
	idx_t limit;
	//! Dynamic table filter on the first key (if any)
	shared_ptr<DynamicFilterData> dynamic_filter;

public:
	void InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const override;
	unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const override;
	unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const override;

	bool ParallelOperator() const override {
		return true;
	}
	PipelineExternalInputSupport GetExternalInputSupport() const override {
		return PipelineExternalInputSupport::SUPPORTED;
	}

	InsertionOrderPreservingMap<string> ParamsToString() const override;

protected:
	OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                   GlobalOperatorState &gstate, OperatorState &state) const override;
};
} // namespace duckdb
