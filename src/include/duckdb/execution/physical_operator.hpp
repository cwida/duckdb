//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/physical_operator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arena_linked_list.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/main/profiler/profiler_print_format.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/order_preservation_type.hpp"
#include "duckdb/common/enums/physical_operator_type.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/execution/physical_operator_states.hpp"
#include "duckdb/execution/progress_data.hpp"

namespace duckdb {

class DictionaryEntry;
class Event;
class Executor;
class PhysicalOperator;
class Pipeline;
class PipelineBuildState;
class MetaPipeline;
class PhysicalPlan;

enum class TableFunctionParallelism : uint8_t;
enum class OperatorCachingMode : uint8_t { NONE, PARTITIONED, ORDERED, UNORDERED };
enum class PipelineExternalInputSupport : uint8_t { UNSUPPORTED, SUPPORTED };
enum class PipelineExternalInputCost : uint8_t { PIPELINED, SERIALIZED_FANOUT };
enum class PipelineSourceConsumption : uint8_t { ALL_INPUT, MAY_STOP_EARLY };

//! PhysicalOperator is the base class of the physical operators present in the execution plan.
class PhysicalOperator {
public:
	static constexpr const PhysicalOperatorType TYPE = PhysicalOperatorType::INVALID;

public:
	PhysicalOperator(PhysicalPlan &physical_plan, PhysicalOperatorType type, vector<LogicalType> types,
	                 idx_t estimated_cardinality);
	virtual ~PhysicalOperator() {
	}

	//! Deleted copy constructors.
	PhysicalOperator(const PhysicalOperator &other) = delete;
	PhysicalOperator &operator=(const PhysicalOperator &) = delete;

	//! The child operators.
	ArenaLinkedList<reference<PhysicalOperator>> children;
	//! The physical operator type.
	PhysicalOperatorType type;
	//! The return types.
	vector<LogicalType> types;
	//! The estimated cardinality.
	idx_t estimated_cardinality;

	//! The global sink state. Published under `lock` by Pipeline::ResetSink on a worker;
	//! a reader racing pipeline initialization must hold `lock` to observe it safely
	unique_ptr<GlobalSinkState> sink_state;
	//! The global operator state.
	unique_ptr<GlobalOperatorState> op_state;
	//! Lock for (re)setting any of the operator states.
	mutex lock;

public:
	virtual string GetName() const;
	virtual InsertionOrderPreservingMap<string> ParamsToString() const {
		return InsertionOrderPreservingMap<string>();
	}
	static void SetEstimatedCardinality(InsertionOrderPreservingMap<string> &result, idx_t estimated_cardinality);
	virtual string ToString(optional_ptr<ClientContext> context = nullptr,
	                        const ProfilerPrintFormat &format = ProfilerPrintFormat::Default()) const;
	void Print() const;
	virtual vector<const_reference<PhysicalOperator>> GetChildren() const;

	//! Return a vector of the types that will be returned by this operator
	const vector<LogicalType> &GetTypes() const {
		return types;
	}

	virtual bool Equals(const PhysicalOperator &other) const {
		return false;
	}

	//! Initialize the vectors this operator uses for its output
	virtual void InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
		chunk.Initialize(allocator, types);
	}

	//! Functions to help decide how to set up pipeline dependencies
	idx_t EstimatedThreadCount() const;
	bool CanSaturateThreads(ClientContext &context) const;

	virtual void Verify();

public:
	// Operator interface
	virtual unique_ptr<OperatorState> GetOperatorState(ExecutionContext &context) const;
	//! Construct global operator state. Runs under the operator's lock and must not rely on a
	//! previous global operator state. On the execute scheduling paths it runs after the
	//! containing pipeline's dependencies have finished and may consume completed
	//! dependency-produced sink state; the external-input initialization path constructs state
	//! before dependency completion and does not support sink-consuming constructors.
	virtual unique_ptr<GlobalOperatorState> GetGlobalOperatorState(ClientContext &context) const;
	//! Make existing global operator state reusable for a repeated execution. May run before the
	//! operator's dependencies have produced output for the next round, so it must not consume
	//! sink state: return true only when the retained state is ready for reuse as-is, and false
	//! when the state is safe to destroy and rebuild through GetGlobalOperatorState.
	virtual bool ResetGlobalOperatorState(ClientContext &context, GlobalOperatorState &state) const;
	virtual OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                   GlobalOperatorState &gstate, OperatorState &state) const;
	virtual OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk,
	                                                GlobalOperatorState &gstate, OperatorState &state) const;
	virtual OperatorFinalResultType OperatorFinalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                                                 OperatorFinalizeInput &input) const;

	virtual bool ParallelOperator() const {
		return false;
	}

	virtual PipelineExternalInputSupport GetExternalInputSupport() const {
		return PipelineExternalInputSupport::UNSUPPORTED;
	}
	virtual PipelineExternalInputCost GetExternalInputCost() const {
		return PipelineExternalInputCost::PIPELINED;
	}

	virtual PipelineSourceConsumption GetSourceConsumption() const {
		return PipelineSourceConsumption::ALL_INPUT;
	}

	virtual bool RequiresFinalExecute() const {
		return false;
	}

	virtual bool RequiresOperatorFinalize() const {
		return false;
	}

	//! The influence the operator has on order (insertion order means no influence)
	virtual OrderPreservationType OperatorOrder() const {
		return OrderPreservationType::INSERTION_ORDER;
	}

public:
	// Source interface
	virtual unique_ptr<LocalSourceState> GetLocalSourceState(ExecutionContext &context,
	                                                         GlobalSourceState &gstate) const;
	virtual unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context) const;
	virtual unique_ptr<GlobalSourceState> GetGlobalSourceState(ClientContext &context,
	                                                           const OperatorPartitionInfo &partition_info) const;

protected:
	virtual SourceResultType GetDataInternal(ExecutionContext &context, DataChunk &chunk,
	                                         OperatorSourceInput &input) const;

public:
	SourceResultType GetData(ExecutionContext &context, DataChunk &chunk, OperatorSourceInput &input) const;

	virtual OperatorPartitionData GetPartitionData(ExecutionContext &context, DataChunk &chunk,
	                                               GlobalSourceState &gstate, LocalSourceState &lstate,
	                                               const OperatorPartitionInfo &partition_info) const;

	virtual bool IsSource() const {
		return false;
	}

	virtual bool ParallelSource() const {
		return false;
	}

	//! Whether this source creates partitioned work that is not bounded by its input chunks
	virtual bool HasSourceTasks() const {
		return false;
	}

	//! How this source manages parallelism
	virtual TableFunctionParallelism SourceParallelism() const;

	virtual bool SupportsPartitioning(const OperatorPartitionInfo &partition_info) const {
		if (partition_info.AnyRequired()) {
			return false;
		}
		return true;
	}

	//! The type of order emitted by the operator (as a source)
	virtual OrderPreservationType SourceOrder() const {
		return OrderPreservationType::INSERTION_ORDER;
	}

	//! Returns the current progress percentage, or a negative value if progress bars are not supported
	virtual ProgressData GetProgress(ClientContext &context, GlobalSourceState &gstate) const;
	virtual void SourceFinished(ClientContext &context, GlobalSourceState &gstate) const;

	//! Returns the current progress percentage, or a negative value if progress bars are not supported
	virtual ProgressData GetSinkProgress(ClientContext &context, GlobalSinkState &gstate,
	                                     const ProgressData source_progress) const {
		return source_progress;
	}

public:
	// Sink interface

	//! The sink method is called constantly with new input, as long as new input is available. Note that this method
	//! CAN be called in parallel, proper locking is needed when accessing dat
	//! a inside the GlobalSinkState.
	virtual SinkResultType Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const;
	//! The combine is called when a single thread has completed execution of its part of the pipeline, it is the final
	//! time that a specific LocalSinkState is accessible. This method can be called in parallel while other Sink() or
	//! Combine() calls are active on the same GlobalSinkState.
	virtual SinkCombineResultType Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const;
	//! (optional) function that will be called before Finalize
	//! For now, its only use is to to communicate memory usage in multi-join pipelines through TemporaryMemoryManager
	virtual void PrepareFinalize(ClientContext &context, GlobalSinkState &sink_state) const;
	//! The finalize is called when ALL threads are finished execution. It is called only once per pipeline, and is
	//! entirely single threaded.
	//! If Finalize returns SinkResultType::Finished, the sink is marked as finished
	virtual SinkFinalizeType Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
	                                  OperatorSinkFinalizeInput &input) const;
	//! For sinks with RequiresBatchIndex set to true, when a new batch starts being processed this method is called
	//! This allows flushing of the current batch (e.g. to disk)
	virtual SinkNextBatchType NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const;
	//! Called after NextBatch when the pipeline minimum advances without subsequent input for this local sink state
	virtual SinkNextBatchType UpdateMinBatchIndex(ExecutionContext &context, OperatorSinkNextBatchInput &input) const;

	virtual unique_ptr<LocalSinkState> GetLocalSinkState(ExecutionContext &context) const;
	virtual unique_ptr<GlobalSinkState> GetGlobalSinkState(ClientContext &context) const;

	//! The maximum amount of memory the operator should use per thread.
	static idx_t GetMaxThreadMemory(ClientContext &context);

	//! Whether operator caching is allowed in the current execution context
	static OperatorCachingMode SelectOperatorCachingMode(ExecutionContext &context);

	virtual bool IsSink() const {
		return false;
	}

	virtual bool ParallelSink() const {
		return false;
	}

	virtual OperatorPartitionInfo RequiredPartitionInfo() const {
		return OperatorPartitionInfo::NoPartitionInfo();
	}

	//! Whether or not the sink operator depends on the order of the input chunks
	//! If this is set to true, we cannot do things like caching intermediate vectors
	virtual bool SinkOrderDependent() const {
		return false;
	}

public:
	// Pipeline construction
	virtual vector<const_reference<PhysicalOperator>> GetSources() const;
	bool AllSourcesSupportBatchIndex() const;

	virtual void BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline);

public:
	template <class TARGET>
	TARGET &Cast() {
		if (TARGET::TYPE != PhysicalOperatorType::INVALID && type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast physical operator to type - physical operator type mismatch");
		}
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		if (TARGET::TYPE != PhysicalOperatorType::INVALID && type != TARGET::TYPE) {
			InternalException::Throw("Failed to cast physical operator to type - physical operator type mismatch");
		}
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Accumulator that lets a dictionary column survive the cache: pinned entry + concatenated per-chunk sels
struct CachedDictColumn {
	buffer_ptr<DictionaryEntry> entry;
	SelectionVector accumulated_sel;
};

//! Contains state for the CachingPhysicalOperator
class CachingOperatorState : public OperatorState {
public:
	~CachingOperatorState() override {
	}

	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
	}

	void ResetCachingState() {
		cached_chunk.reset();
		initialized = false;
		can_cache_chunk = OperatorCachingMode::NONE;
		must_return_continuation_chunk = false;
		cached_result = OperatorResultType::NEED_MORE_INPUT;
		ResetDictCache();
	}

	//! Drop the dictionary accumulators, returning the cache to plain flat caching
	void ResetDictCache() {
		dict_columns.clear();
		dict_cache_active = false;
	}

	unique_ptr<DataChunk> cached_chunk;
	bool initialized = false;
	//! Whether or not the chunk can be cached
	OperatorCachingMode can_cache_chunk = OperatorCachingMode::NONE;
	bool must_return_continuation_chunk = false;
	OperatorResultType cached_result;

	//! One slot per cached column. Invariant: entry != null iff the column is accumulating a dictionary
	//! (pinned by entry pointer identity); entry == null iff plain flat caching (the common case)
	vector<CachedDictColumn> dict_columns;
	bool dict_cache_active = false;
};

//! Base class that caches output from child Operator class. Note that Operators inheriting from this class should also
//! inherit their state class from the CachingOperatorState.
class CachingPhysicalOperator : public PhysicalOperator {
public:
	static constexpr const idx_t CACHE_THRESHOLD = 64;
	CachingPhysicalOperator(PhysicalPlan &physical_plan, PhysicalOperatorType type, vector<LogicalType> types,
	                        idx_t estimated_cardinality);

	bool caching_supported;

public:
	//! This Execute will prevent small chunks from entering the pipeline, buffering them until a bigger chunk is
	//! created.
	OperatorResultType Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                           GlobalOperatorState &gstate, OperatorState &state) const final;
	//! FinalExecute is used here to send out the remainder of the chunk (< STANDARD_VECTOR_SIZE) that we still had
	//! cached.
	OperatorFinalizeResultType FinalExecute(ExecutionContext &context, DataChunk &chunk, GlobalOperatorState &gstate,
	                                        OperatorState &state) const final;

	bool RequiresFinalExecute() const final {
		return caching_supported;
	}

protected:
	//! Child classes need to implement the ExecuteInternal method instead of the Execute
	virtual OperatorResultType ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
	                                           GlobalOperatorState &gstate, OperatorState &state) const = 0;

private:
	bool CanCacheType(const LogicalType &type);
};

} // namespace duckdb
