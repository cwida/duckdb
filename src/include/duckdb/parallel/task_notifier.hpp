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
class RuntimeServices;

//! The TaskNotifier notifies ClientContextState listener about started / stopped tasks
class TaskNotifier {
public:
	explicit TaskNotifier(optional_ptr<RuntimeServices> services_p);

	~TaskNotifier();

private:
	optional_ptr<RuntimeServices> services;
};

} // namespace duckdb
