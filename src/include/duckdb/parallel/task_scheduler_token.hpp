//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/task_scheduler_token.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/task_scheduler_type.hpp"
#include "duckdb/common/mutex.hpp"

#include <condition_variable>

namespace duckdb {

// Forward declarations.
struct QueueProducerToken;
struct QueueConsumerToken;
class TaskSchedulerQueue;

struct ProducerToken {
public:
	explicit ProducerToken(array<unique_ptr<TaskSchedulerQueue>, TASK_SCHEDULER_TYPE_COUNT> &queues);
	~ProducerToken();

public:
	QueueProducerToken &GetQueueProducerToken(TaskSchedulerType pool_type);

public:
	annotated_mutex producer_lock;
	std::condition_variable producer_cv;

private:
	array<unique_ptr<QueueProducerToken>, TASK_SCHEDULER_TYPE_COUNT> tokens;
};

//! Dequeues tasks from any producer, rotating across producers so that every query gets a turn
struct ConsumerToken {
public:
	explicit ConsumerToken(array<unique_ptr<TaskSchedulerQueue>, TASK_SCHEDULER_TYPE_COUNT> &queues);
	~ConsumerToken();

public:
	QueueConsumerToken &GetQueueConsumerToken(TaskSchedulerType pool_type);

private:
	array<unique_ptr<QueueConsumerToken>, TASK_SCHEDULER_TYPE_COUNT> tokens;
};

} // namespace duckdb
