#include "duckdb/execution/operator/helper/physical_execute.hpp"

#include "duckdb/parallel/meta_pipeline.hpp"

namespace duckdb {

PhysicalExecute::PhysicalExecute(PhysicalPlan &physical_plan, PhysicalOperator &plan,
                                 shared_ptr<PreparedStatementData> prepared)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::EXECUTE, plan.types, idx_t(-1)), plan(plan),
      prepared(std::move(prepared)) {
}

vector<const_reference<PhysicalOperator>> PhysicalExecute::GetChildren() const {
	return {plan};
}

vector<const_reference<PhysicalOperator>> PhysicalExecute::GetSources() const {
	return plan.GetSources();
}

void PhysicalExecute::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	// EXECUTE statement: build pipeline on child
	meta_pipeline.Build(plan);
}

} // namespace duckdb
