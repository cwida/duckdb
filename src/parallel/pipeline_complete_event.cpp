#include "duckdb/parallel/pipeline_complete_event.hpp"
#include "duckdb/execution/executor.hpp"
#include "duckdb/parallel/pipeline_tracer.hpp"
#include "duckdb/parallel/task_scheduler.hpp"

namespace duckdb {

PipelineCompleteEvent::PipelineCompleteEvent(Executor &executor, bool complete_pipeline_p,
                                             shared_ptr<PipelineTraceEntry> trace_p)
    : Event(executor), complete_pipeline(complete_pipeline_p), trace(std::move(trace_p)) {
	if (trace) {
		trace->has_complete_event = true;
	}
}

void PipelineCompleteEvent::Schedule() {
}

void PipelineCompleteEvent::FinalizeFinish() {
	if (trace) {
		trace->SetOnce(trace->complete_ns, TaskScheduler::GetScheduler(executor.context).GetTracer().Now());
	}
	if (complete_pipeline) {
		executor.CompletePipeline();
	}
}

} // namespace duckdb
