#include "duckdb/parallel/pipeline_prepare_finish_event.hpp"

namespace duckdb {

PipelinePrepareFinishEvent::PipelinePrepareFinishEvent(shared_ptr<Pipeline> pipeline_p)
    : BasePipelineEvent(std::move(pipeline_p)) {
}

class PipelinePreFinishTask : public ExecutorTask {
public:
	explicit PipelinePreFinishTask(Pipeline &pipeline_p, shared_ptr<Event> event_p)
	    : ExecutorTask(pipeline_p.executor, std::move(event_p)), pipeline(pipeline_p) {
	}

	Pipeline &pipeline;

public:
	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		pipeline.PrepareFinalize();
		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "PipelinePreFinishTask";
	}
};

void PipelinePrepareFinishEvent::Schedule() {
	vector<shared_ptr<Task>> tasks;
	tasks.push_back(make_uniq<PipelinePreFinishTask>(*pipeline, shared_from_this()));
	SetTasks(std::move(tasks));
}

void PipelinePrepareFinishEvent::FinishEvent() {
}

} // namespace duckdb
