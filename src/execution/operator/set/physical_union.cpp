#include "duckdb/execution/operator/set/physical_union.hpp"

#include "duckdb/main/settings.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"

namespace duckdb {

PhysicalUnion::PhysicalUnion(PhysicalPlan &physical_plan, vector<LogicalType> types_p,
                             const ArenaLinkedList<reference<PhysicalOperator>> &children_p,
                             idx_t estimated_cardinality, bool allow_out_of_order)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::UNION, std::move(types_p), estimated_cardinality),
      allow_out_of_order(allow_out_of_order) {
	for (auto &child : children_p) {
		children.push_back(child);
	}
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
static bool ContainsSink(PhysicalOperator &op) {
	if (op.IsSink()) {
		return true;
	}
	for (auto &child : op.children) {
		if (ContainsSink(child)) {
			return true;
		}
	}
	return false;
}

void PhysicalUnion::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();
	sink_state.reset();

	// order matters if any of the downstream operators are order dependent,
	// or if the sink preserves order, but does not support batch indices to do so
	auto sink = meta_pipeline.GetSink();
	bool order_matters = false;
	if (!allow_out_of_order) {
		order_matters = true;
	}
	if (current.IsOrderDependent()) {
		order_matters = true;
	}
	if (sink) {
		if (Settings::Get<PreserveInsertionOrderSetting>(current.GetClientContext()) && sink->SinkOrderDependent()) {
			order_matters = true;
		}
		auto partition_info = sink->RequiredPartitionInfo();
		if (partition_info.batch_index) {
			order_matters = true;
		}
		if (!sink->ParallelSink()) {
			order_matters = true;
		}
	}

	// create union pipelines that has identical dependencies to 'current'
	vector<reference<Pipeline>> union_pipelines;
	for (idx_t i = 0; i + 1 < children.size(); i++) {
		auto &union_pipeline = meta_pipeline.CreateUnionPipeline(current, order_matters);
		union_pipelines.push_back(union_pipeline);
	}
	// continue with the current pipeline
	children[0].get().BuildPipelines(current, meta_pipeline);
	bool can_saturate_threads =
	    ContainsSink(children[0].get()) && children[0].get().CanSaturateThreads(current.GetClientContext());
	for (idx_t i = 1; i < children.size(); i++) {
		auto &union_pipeline = union_pipelines[children.size() - i - 1].get();
		vector<shared_ptr<Pipeline>> dependencies;
		optional_ptr<MetaPipeline> last_child_ptr;
		if (ContainsSink(children[i - 1].get()) &&
		    children[i - 1].get().CanSaturateThreads(current.GetClientContext())) {
			can_saturate_threads = true;
		}
		if (order_matters || can_saturate_threads) {
			// we add dependencies if order matters: union_pipeline comes after all pipelines created by building
			// current
			auto dependency_type =
			    order_matters ? PipelineDependencyType::REQUIRED : PipelineDependencyType::OPTIONAL_DEPENDENCY;
			dependencies = meta_pipeline.AddDependenciesFrom(union_pipeline, union_pipeline, false, dependency_type);
			if (!allow_out_of_order) {
				// CreateUnionPipeline already orders this pipeline after current. Include current when
				// propagating that ordering to the child sink pipelines as well.
				dependencies.push_back(current.shared_from_this());
			}
			// we also add dependencies if the LHS child can saturate all available threads
			// in that case, we recursively make all RHS children depend on the LHS.
			// This prevents breadth-first plan evaluation
			if (!allow_out_of_order || can_saturate_threads) {
				last_child_ptr = meta_pipeline.GetLastChild();
			}
		}
		// Assign proper batch index to the union pipeline
		meta_pipeline.AssignNextBatchIndex(union_pipeline);
		// build the union pipeline
		children[i].get().BuildPipelines(union_pipeline, meta_pipeline);

		if (last_child_ptr) {
			// Explicitly ordered unions also order their children's sink pipelines.
			auto dependency_mode =
			    !allow_out_of_order ? RecursiveDependencyMode::FORCE : RecursiveDependencyMode::RESPECT_PARALLELISM;
			meta_pipeline.AddRecursiveDependencies(dependencies, *last_child_ptr, dependency_mode);
		}
	}
}

vector<const_reference<PhysicalOperator>> PhysicalUnion::GetSources() const {
	vector<const_reference<PhysicalOperator>> result;
	for (auto &child : children) {
		auto child_sources = child.get().GetSources();
		result.insert(result.end(), child_sources.begin(), child_sources.end());
	}
	return result;
}

} // namespace duckdb
