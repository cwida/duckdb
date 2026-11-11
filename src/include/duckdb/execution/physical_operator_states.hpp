//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/physical_operator_states.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/parallel/interrupt.hpp"
#include "duckdb/execution/partition_info.hpp"

namespace duckdb {
class Event;
class Executor;
class PhysicalOperator;
class Pipeline;
class PipelineBuildState;
class MetaPipeline;
class InterruptState;

// LCOV_EXCL_START
class OperatorState {
public:
	virtual ~OperatorState() {
	}

	virtual void Finalize(const PhysicalOperator &op, ExecutionContext &context) {
	}

	virtual bool SupportsReuse() const {
		return false;
	}

	virtual void Reset() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class GlobalOperatorState {
public:
	virtual ~GlobalOperatorState() {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	virtual idx_t MaxThreads(idx_t source_max_threads) {
		return source_max_threads;
	}
};

class GlobalSinkState : public StateWithBlockableTasks {
public:
	GlobalSinkState() : state(SinkFinalizeType::READY) {
	}
	virtual ~GlobalSinkState() {
	}

	SinkFinalizeType state;
	//! Upper bound on the workers the next rescheduled execution runs with, set before Reset is called
	optional_idx reschedule_worker_limit;
	//! Whether exactly one producer pipeline feeds this sink in the rescheduled execution
	bool reschedule_single_producer = false;

	virtual bool SupportsReuse() const {
		return false;
	}

	virtual void Reset(ClientContext &context) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		ResetBlocking();
		state = SinkFinalizeType::READY;
	}

	//! Called when the rescheduled execution's actual worker count is known, before any data flows.
	//! Only called for sinks with a single producer pipeline, where no other producer can interfere.
	virtual void AdjustRescheduledWorkers(ClientContext &context, idx_t scheduled_workers) {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

	virtual idx_t MaxThreads(idx_t source_max_threads) {
		return source_max_threads;
	}
};

class LocalSinkState {
public:
	virtual ~LocalSinkState() {
	}

	//! Source partition info
	SourcePartitionInfo partition_info;

	virtual bool SupportsReuse() const {
		return false;
	}

	virtual void Reset(ExecutionContext &context, GlobalSinkState &gstate) {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class GlobalSourceState : public StateWithBlockableTasks {
public:
	virtual ~GlobalSourceState() {
	}

	virtual idx_t MaxThreads() {
		return 1;
	}

	virtual bool SupportsReuse() const {
		return false;
	}

	virtual void Reset(ClientContext &context) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		ResetBlocking();
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

class LocalSourceState {
public:
	virtual ~LocalSourceState() {
	}

	virtual bool SupportsReuse() const {
		return false;
	}

	virtual void Reset(ExecutionContext &context, GlobalSourceState &gstate) {
	}

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

struct OperatorSinkInput {
	GlobalSinkState &global_state;
	LocalSinkState &local_state;
	InterruptState &interrupt_state;
};

struct OperatorSourceInput {
	GlobalSourceState &global_state;
	LocalSourceState &local_state;
	InterruptState &interrupt_state;
	SourceBatchIndexState batch_index_state = SourceBatchIndexState::UNCHANGED;
};

struct OperatorSinkCombineInput {
	GlobalSinkState &global_state;
	LocalSinkState &local_state;
	InterruptState &interrupt_state;
};

struct OperatorSinkFinalizeInput {
	GlobalSinkState &global_state;
	InterruptState &interrupt_state;
};

struct OperatorFinalizeInput {
	GlobalOperatorState &global_state;
	InterruptState &interrupt_state;
};

struct OperatorSinkNextBatchInput {
	GlobalSinkState &global_state;
	LocalSinkState &local_state;
	InterruptState &interrupt_state;
};

// LCOV_EXCL_STOP

} // namespace duckdb
