//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/operator/helper/physical_external_resource.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/planner/operator/logical_external_resource.hpp"

namespace duckdb {

//! PhysicalExternalResource executes CREATE / REGISTER / DESTROY EXTERNAL RESOURCE: it provisions a resource
//! and registers it in the ExternalResourcesManager, records a handle, or tears one down and deregisters it.
//! Returns one (status, name, type, handle) row. With a child it is REGISTER EXTERNAL RESOURCES FROM <source>:
//! the child's (type, handle, name) rows are sunk, recorded together, and returned each with its status.
class PhysicalExternalResource : public PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::EXTERNAL_RESOURCE;

public:
	PhysicalExternalResource(PhysicalPlan &physical_plan, BoundExternalResource data, vector<LogicalType> types,
	                         idx_t estimated_cardinality)
	    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXTERNAL_RESOURCE, std::move(types),
	                       estimated_cardinality),
	      data(std::move(data)) {
	}

	BoundExternalResource data;

public:
	// Source interface
	unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const override;
	SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                 OperatorSourceInput &input) const override;

	bool IsSource() const override {
		return true;
	}

public:
	// Sink interface (REGISTER EXTERNAL RESOURCES FROM <source>)
	unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const override;
	SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const override;

	bool IsSink() const override {
		return !children.empty();
	}
};

} // namespace duckdb
