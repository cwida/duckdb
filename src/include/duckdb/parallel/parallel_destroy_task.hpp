//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/parallel_destroy_task.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/parallel/task_executor.hpp"

namespace duckdb {
template <class ITERABLE>
class ParallelDestroyTask : public BaseExecutorTask {
	using VALUE_TYPE = typename ITERABLE::value_type;

public:
	ParallelDestroyTask(TaskExecutor &executor, VALUE_TYPE &&elem_p)
	    : BaseExecutorTask(executor), elem(std::move(elem_p)) {
	}

	void ExecuteTask() override {
		elem.reset();
	}

	static void Schedule(TaskScheduler &scheduler, ITERABLE &elements) {
		// This is used in destructors and is therefore not allowed to throw
		// Similar approach to AttachedDatabase::Close(), just swallow the exception
		try {
			TaskExecutor executor(scheduler);
			vector<unique_ptr<BaseExecutorTask>> tasks;
			tasks.reserve(elements.size());
			for (auto &segment : elements) {
				auto destroy_task = make_uniq<ParallelDestroyTask>(executor, std::move(segment));
				tasks.push_back(std::move(destroy_task));
			}
			executor.ScheduleTasks(std::move(tasks));
			executor.WorkOnTasks();
		} catch (...) { // NOLINT
		}
	}

private:
	VALUE_TYPE elem;
};

} // namespace duckdb
