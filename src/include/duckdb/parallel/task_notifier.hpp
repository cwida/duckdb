//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_notifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"

namespace duckdb {
class ExecutionServices;

//! The TaskNotifier notifies ClientContextState listener about started / stopped tasks
class TaskNotifier {
public:
	explicit TaskNotifier(optional_ptr<ExecutionServices> services_p);

	~TaskNotifier();

private:
	optional_ptr<ExecutionServices> services;
};

} // namespace duckdb
