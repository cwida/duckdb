#include "duckdb/parallel/task_scheduler_queue.hpp"

#include "duckdb/parallel/task.hpp"

#ifndef DUCKDB_NO_THREADS
#include "concurrentqueue.h"
#endif

namespace duckdb {

TaskSchedulerType TaskSchedulerQueue::GetPoolType() {
	return pool_type;
}

#ifndef DUCKDB_NO_THREADS
struct TaskQueueTraits : public duckdb_moodycamel::ConcurrentQueueDefaultTraits {
	//! Consumers move on to the next producer after this many tasks (default: 256)
	static constexpr uint32_t EXPLICIT_CONSUMER_CONSUMPTION_QUOTA_BEFORE_ROTATE = 16;
};
typedef duckdb_moodycamel::ConcurrentQueue<shared_ptr<Task>, TaskQueueTraits> concurrent_queue_t;

struct ConcurrentQueueWrapper {
	concurrent_queue_t q;
};

struct QueueProducerToken {
public:
	explicit QueueProducerToken(TaskSchedulerQueue &queue) : token(queue.GetQueue().q) {
	}

public:
	duckdb_moodycamel::ProducerToken token;
};

struct QueueConsumerToken {
public:
	explicit QueueConsumerToken(TaskSchedulerQueue &queue) : token(queue.GetQueue().q) {
	}

public:
	duckdb_moodycamel::ConsumerToken token;
};

TaskSchedulerQueue::TaskSchedulerQueue(TaskSchedulerType pool_type_p)
    : pool_type(pool_type_p), queue(make_uniq<ConcurrentQueueWrapper>()) {
}

void TaskSchedulerQueue::Enqueue(ProducerToken &token, shared_ptr<Task> task) {
	const annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	task->token = token;
	if (queue->q.enqueue(token.GetQueueProducerToken(pool_type).token, std::move(task))) {
		++tasks_in_queue;
		token.producer_cv.notify_one();
	} else {
		throw InternalException("Could not schedule task!");
	}
}

void TaskSchedulerQueue::EnqueueBulk(ProducerToken &token, vector<shared_ptr<Task>> &tasks) {
	const annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	for (auto &task : tasks) {
		task->token = token;
	}
	if (queue->q.enqueue_bulk(token.GetQueueProducerToken(pool_type).token, std::make_move_iterator(tasks.begin()),
	                          tasks.size())) {
		tasks_in_queue += tasks.size();
		token.producer_cv.notify_one();
	} else {
		throw InternalException("Could not schedule tasks!");
	}
}

bool TaskSchedulerQueue::DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	const annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	return DequeueFromProducerLocked(token, task);
}

bool TaskSchedulerQueue::DequeueFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) {
	if (!queue->q.try_dequeue_from_producer(token.GetQueueProducerToken(pool_type).token, task)) {
		return false;
	}
	--tasks_in_queue;
	return true;
}

bool TaskSchedulerQueue::Dequeue(ConsumerToken &token, shared_ptr<Task> &task) {
	if (!queue->q.try_dequeue(token.GetQueueConsumerToken(pool_type).token, task)) {
		return false;
	}
	--tasks_in_queue;
	return true;
}

idx_t TaskSchedulerQueue::GetTasksInQueue() const {
	return tasks_in_queue;
}
idx_t TaskSchedulerQueue::GetApproxSize() const {
	return queue->q.size_approx();
}
idx_t TaskSchedulerQueue::GetProducerCount() const {
	return queue->q.size_producers_approx();
}

idx_t TaskSchedulerQueue::GetTaskCountForProducer(ProducerToken &token) const {
	const annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	return queue->q.size_producer_approx(token.GetQueueProducerToken(pool_type).token);
}

ConcurrentQueueWrapper &TaskSchedulerQueue::GetQueue() {
	return *queue;
}

#else

struct QueueProducerToken {
public:
	explicit QueueProducerToken(TaskSchedulerQueue &queue) : queue(&queue) {
	}

	~QueueProducerToken() {
		queue->RemoveToken(*this);
	}

private:
	TaskSchedulerQueue *queue;
};

struct QueueConsumerToken {
public:
	explicit QueueConsumerToken(TaskSchedulerQueue &) {
	}
};

TaskSchedulerQueue::TaskSchedulerQueue(TaskSchedulerType pool_type_p) : pool_type(pool_type_p) {
}

void TaskSchedulerQueue::Enqueue(ProducerToken &token, shared_ptr<Task> task) {
	annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	lock_guard<mutex> lock(qlock);
	task->token = token;
	q[token.GetQueueProducerToken(pool_type)].push(std::move(task));
	token.producer_cv.notify_one();
}

void TaskSchedulerQueue::EnqueueBulk(ProducerToken &token, vector<shared_ptr<Task>> &tasks) {
	const annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	lock_guard<mutex> lock(qlock);
	for (auto &task : tasks) {
		task->token = token;
		q[token.GetQueueProducerToken(pool_type)].push(std::move(task));
	}
	token.producer_cv.notify_one();
}

bool TaskSchedulerQueue::DequeueFromProducer(ProducerToken &token, shared_ptr<Task> &task) {
	annotated_lock_guard<annotated_mutex> producer_lock(token.producer_lock);
	return DequeueFromProducerLocked(token, task);
}

bool TaskSchedulerQueue::DequeueFromProducerLocked(ProducerToken &token, shared_ptr<Task> &task) {
	lock_guard<mutex> lock(qlock);

	const auto it = q.find(token.GetQueueProducerToken(pool_type));
	if (it == q.end() || it->second.empty()) {
		return false;
	}

	task = std::move(it->second.front());
	it->second.pop();

	return true;
}

bool TaskSchedulerQueue::Dequeue(ConsumerToken &token, shared_ptr<Task> &task) {
	throw InternalException("Global dequeue not supported for no threads queue");
}

idx_t TaskSchedulerQueue::GetTasksInQueue() const {
	lock_guard<mutex> lock(qlock);
	idx_t task_count = 0;
	for (auto &producer : q) {
		task_count += producer.second.size();
	}
	return task_count;
}

idx_t TaskSchedulerQueue::GetApproxSize() const {
	return GetTasksInQueue();
}

idx_t TaskSchedulerQueue::GetProducerCount() const {
	lock_guard<mutex> lock(qlock);
	return q.size();
}

idx_t TaskSchedulerQueue::GetTaskCountForProducer(ProducerToken &token) const {
	lock_guard<mutex> lock(qlock);
	const auto it = q.find(token.GetQueueProducerToken(pool_type));
	if (it == q.end()) {
		return 0;
	}
	return it->second.size();
}

void TaskSchedulerQueue::RemoveToken(QueueProducerToken &token) {
	lock_guard<mutex> lock(qlock);
	q.erase(token);
}

#endif

TaskSchedulerQueue::~TaskSchedulerQueue() {
}

ProducerToken::ProducerToken(array<unique_ptr<TaskSchedulerQueue>, TASK_SCHEDULER_TYPE_COUNT> &queues) {
	for (uint8_t i = 0; i < TASK_SCHEDULER_TYPE_COUNT; i++) {
		tokens[i] = make_uniq<QueueProducerToken>(*queues[i]);
	}
}

ProducerToken::~ProducerToken() {
}

QueueProducerToken &ProducerToken::GetQueueProducerToken(TaskSchedulerType pool_type) {
	return *tokens[static_cast<uint8_t>(pool_type)];
}

ConsumerToken::ConsumerToken(array<unique_ptr<TaskSchedulerQueue>, TASK_SCHEDULER_TYPE_COUNT> &queues) {
	for (uint8_t i = 0; i < TASK_SCHEDULER_TYPE_COUNT; i++) {
		tokens[i] = make_uniq<QueueConsumerToken>(*queues[i]);
	}
}

ConsumerToken::~ConsumerToken() {
}

QueueConsumerToken &ConsumerToken::GetQueueConsumerToken(TaskSchedulerType pool_type) {
	return *tokens[static_cast<uint8_t>(pool_type)];
}

} // namespace duckdb
