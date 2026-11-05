#include "duckdb/execution/operator/helper/physical_result_collector.hpp"

#include "duckdb/execution/operator/helper/physical_result_sink.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/main/prepared_statement_data.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

PhysicalResultCollector::PhysicalResultCollector(PhysicalPlan &physical_plan, PreparedStatementData &data)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::RESULT_COLLECTOR, {LogicalType::BOOLEAN}, 0),
      statement_type(data.statement_type), properties(data.properties), plan(data.physical_plan->Root()),
      names(data.names) {
	types = data.types;
}

unique_ptr<PhysicalOperator> PhysicalResultCollector::GetResultCollector(ClientContext &context,
                                                                         PreparedStatementData &data) {
	auto &physical_plan = *data.physical_plan;
	auto &root = physical_plan.Root();

	if (!PhysicalPlanGenerator::PreserveInsertionOrder(context, root)) {
		return make_uniq<PhysicalResultSink>(physical_plan, data, ResultOrdering::UNORDERED);
	}
	if (!PhysicalPlanGenerator::UseBatchIndex(context, root)) {
		return make_uniq<PhysicalResultSink>(physical_plan, data, ResultOrdering::SOURCE_ORDERED);
	}
	return make_uniq<PhysicalResultSink>(physical_plan, data, ResultOrdering::BATCH_INDEX_ORDERED);
}

vector<const_reference<PhysicalOperator>> PhysicalResultCollector::GetChildren() const {
	return {plan};
}

void PhysicalResultCollector::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	// operator is a sink, build a pipeline
	sink_state.reset();

	D_ASSERT(children.empty());

	// single operator: the operator becomes the data source of the current pipeline
	auto &state = meta_pipeline.GetState();
	state.SetPipelineSource(current, *this);

	// we create a new pipeline starting from the child
	auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
	child_meta_pipeline.Build(plan);
}

} // namespace duckdb
