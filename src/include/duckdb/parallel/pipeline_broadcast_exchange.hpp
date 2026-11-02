//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parallel/pipeline_broadcast_exchange.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/atomic.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/deque.hpp"
#include "duckdb/common/enums/operator_result_type.hpp"
#include "duckdb/common/enums/order_preservation_type.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/execution/partition_info.hpp"
#include "duckdb/execution/progress_data.hpp"
#include "duckdb/parallel/interrupt.hpp"

namespace duckdb {

class ClientContext;
class Pipeline;
class PipelineBroadcastExchange;
class PipelineBroadcastExchangeScanState;
class PipelineExecutor;
class PhysicalOperator;

enum class PipelineBroadcastExchangeCompletionMode : uint8_t { STOP_WHEN_UNCONSUMED, RUN_TO_COMPLETION };
//! THROTTLE_PRODUCER blocks the producer while the consumers cannot keep up. BUFFER_ALL never blocks the producer and
//! buffers the rows instead (spilling to disk if required) - required when the consumer of the exchange cannot run
//! until the producer is done, e.g. because its pipeline depends on a pipeline that the producer feeds
enum class PipelineBroadcastExchangeBufferMode { THROTTLE_PRODUCER, BUFFER_ALL };
enum class PipelineBroadcastExchangeOrderMode : uint8_t { UNORDERED, SEQUENTIAL, BATCH_INDEX };
enum class PipelineBroadcastExchangeLocalMode : uint8_t { DIRECT_ONLY, BUFFERED };
enum class PipelineBroadcastExchangeDirectPushState : uint8_t { NOT_STARTED, RESUMING, ACTIVE, FINISHED };
enum class PipelineBroadcastExchangeConsumerMode : uint8_t { UNRESOLVED, BUFFERED, DIRECT, MATERIALIZED };
enum class PipelineBroadcastExchangeScanMode : uint8_t { CHUNK, BATCH };

struct PipelineBroadcastExchangeConsumerSummary {
	idx_t unresolved = 0;
	idx_t buffered = 0;
	idx_t direct = 0;
	idx_t materialized = 0;

	idx_t ExchangeConsumerCount() const {
		return buffered + direct;
	}
};

class PipelineBroadcastExchangeLocalState {
public:
	PipelineBroadcastExchangeLocalState(ClientContext &context, const PipelineBroadcastExchange &exchange,
	                                    idx_t producer_base_batch_index);
	~PipelineBroadcastExchangeLocalState();

private:
	friend class PipelineBroadcastExchange;
	SinkResultType Push(DataChunk &chunk, const SourcePartitionInfo &partition_info,
	                    const InterruptState &interrupt_state);
	SinkNextBatchType NextBatch(const SourcePartitionInfo &partition_info, const InterruptState &interrupt_state);
	SinkNextBatchType UpdateMinBatchIndex(const SourcePartitionInfo &partition_info,
	                                      const InterruptState &interrupt_state);
	SinkCombineResultType Finish(const SourcePartitionInfo &partition_info, const InterruptState &interrupt_state);
	bool HasDirectConsumers() const;
	bool DirectConsumersFinished() const;
	void ResetPush();
	OperatorPartitionData GetSourcePartitionData(const SourcePartitionInfo &partition_info) const;
	optional_idx GetSourceMinBatchIndex(const SourcePartitionInfo &partition_info) const;

	vector<unique_ptr<PipelineExecutor>> direct_executors;
	vector<unique_ptr<DataChunk>> direct_input_chunks;
	idx_t direct_idx = 0;
	idx_t direct_next_batch_idx = 0;
	idx_t direct_min_batch_idx = 0;
	idx_t direct_finalize_idx = 0;
	idx_t producer_base_batch_index;
	bool supports_batch_index;
	PipelineBroadcastExchangeLocalMode mode = PipelineBroadcastExchangeLocalMode::BUFFERED;
	PipelineBroadcastExchangeDirectPushState direct_push_state = PipelineBroadcastExchangeDirectPushState::NOT_STARTED;
};

class PipelineBroadcastExchange {
	friend class PipelineBroadcastExchangeLocalState;

public:
	PipelineBroadcastExchange(
	    ClientContext &context, vector<LogicalType> types_p, PipelineBroadcastExchangeCompletionMode completion_mode_p,
	    OrderPreservationType source_order_p, bool use_batch_index_p,
	    PipelineBroadcastExchangeBufferMode buffer_mode_p = PipelineBroadcastExchangeBufferMode::THROTTLE_PRODUCER);

	const vector<LogicalType> &Types() const {
		return types;
	}
	bool RunToCompletion() const {
		return completion_mode == PipelineBroadcastExchangeCompletionMode::RUN_TO_COMPLETION;
	}
	bool PreservesOrder() const {
		return order_mode != PipelineBroadcastExchangeOrderMode::UNORDERED;
	}
	bool SupportsBatchIndex() const {
		return order_mode == PipelineBroadcastExchangeOrderMode::BATCH_INDEX;
	}
	PipelineBroadcastExchangeOrderMode OrderMode() const {
		return order_mode;
	}
	OrderPreservationType SourceOrder() const {
		return source_order;
	}

	unique_ptr<PipelineBroadcastExchangeLocalState> GetLocalState(ClientContext &context,
	                                                              idx_t producer_base_batch_index) const;
	shared_ptr<PipelineBroadcastExchangeScanState> GetScanState() const;

	void SetProducerPipelines(const vector<shared_ptr<Pipeline>> &pipelines);
	idx_t RegisterConsumer();
	bool CanRegisterDirectConsumer(Pipeline &pipeline) const;
	void SelectDirectConsumer(Pipeline &pipeline, idx_t consumer_idx);
	bool TryRegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx);
	vector<reference<Pipeline>> GetProducerPipelines() const;
	void SelectBufferedConsumer(idx_t consumer_idx, PipelineBroadcastExchangeScanMode scan_mode);
	void SelectMaterializedConsumer(idx_t consumer_idx);
	void ResetConsumerRegistrations();
	void Reset();
	void SetLogOperator(const PhysicalOperator &op);

	SinkResultType Push(DataChunk &chunk, PipelineBroadcastExchangeLocalState &lstate,
	                    const SourcePartitionInfo &partition_info, const InterruptState &interrupt_state);
	SinkNextBatchType NextBatch(PipelineBroadcastExchangeLocalState &lstate, const SourcePartitionInfo &partition_info,
	                            const InterruptState &interrupt_state);
	SinkNextBatchType UpdateMinBatchIndex(PipelineBroadcastExchangeLocalState &lstate,
	                                      const SourcePartitionInfo &partition_info,
	                                      const InterruptState &interrupt_state);
	SinkCombineResultType FinishLocal(PipelineBroadcastExchangeLocalState &lstate,
	                                  const SourcePartitionInfo &partition_info, const InterruptState &interrupt_state);
	void Finish();
	void FinishDirectConsumers();
	void Cancel();

	SourceResultType Scan(idx_t consumer_idx, DataChunk &chunk, PipelineBroadcastExchangeScanState &scan_state,
	                      optional_idx &batch_index, SourceBatchIndexState &batch_index_state,
	                      const InterruptState &interrupt_state);
	void UnregisterConsumer(idx_t consumer_idx);

	ProgressData ScanProgress(idx_t consumer_idx, idx_t estimated_cardinality) const;
	ProgressData SinkProgress(const ProgressData &source_progress, idx_t estimated_cardinality) const;
	idx_t MaxThreads() const;
	idx_t RegisteredConsumerCount() const;
	PipelineBroadcastExchangeConsumerMode GetConsumerMode(idx_t consumer_idx) const;
	PipelineBroadcastExchangeScanMode GetConsumerScanMode(idx_t consumer_idx) const;
	PipelineBroadcastExchangeConsumerSummary GetConsumerSummary() const;

private:
	friend class PipelineBroadcastExchangeScanState;

	struct ChunkPool;
	struct BroadcastSpool;
	struct BroadcastSpoolReader;
	struct AppendReservation;
	struct SpoolReadReservation;
	struct BufferedChunk;
	struct BufferState;

	enum class ConsumerLifecycle : uint8_t { ACTIVE, INACTIVE };
	enum class ProducerState : uint8_t { ACTIVE, FINISHED, CANCELLED };
	enum class AppendReservationState : uint8_t { IDLE, RESERVED };
	enum class WatermarkState : uint8_t { BELOW_HIGH_WATERMARK, ABOVE_HIGH_WATERMARK };
	enum class ReaderWakeMode : uint8_t { DATA_AVAILABLE, ALL };
	enum class WriterWakeMode : uint8_t { LOW_WATERMARK, FORCE };
	enum class AppendAdmission : uint8_t { READY, BLOCKED, UNCONSUMED, CANCELLED };
	enum class BufferedPushState : uint8_t { NOT_REQUIRED, APPENDED, STAGED, BLOCKED, UNCONSUMED, CANCELLED };
	enum class ExchangeLogEvent : uint8_t {
		SPOOL_CREATED,
		HIGH_WATERMARK_BLOCKED,
		LOW_WATERMARK_WAKE,
		CONSUMER_UNREGISTERED
	};

	struct ExchangeLogEntry {
		ExchangeLogEvent event;
		idx_t consumer_count;
		idx_t buffered_chunks;
	};

	struct ConsumerState {
		ConsumerState();
		~ConsumerState();
		ConsumerState(ConsumerState &&other) noexcept;
		ConsumerState &operator=(ConsumerState &&other) noexcept;
		ConsumerState(const ConsumerState &other) = delete;
		ConsumerState &operator=(const ConsumerState &other) = delete;

		idx_t position = 0;
		idx_t next_batch_sequence = 0;
		//! Lower bound for idle scan tasks after earlier ranges have all been claimed
		idx_t batch_index_floor = DConstants::INVALID_INDEX;
		idx_t rows_read = 0;
		PipelineBroadcastExchangeConsumerMode mode = PipelineBroadcastExchangeConsumerMode::UNRESOLVED;
		PipelineBroadcastExchangeScanMode scan_mode = PipelineBroadcastExchangeScanMode::CHUNK;
		ConsumerLifecycle lifecycle = ConsumerLifecycle::ACTIVE;
		bool exhausted = false;
		set<idx_t> in_flight_reads;
		//! Claimed batch sequence to next unread chunk position
		map<idx_t, idx_t> active_batch_positions;
	};

public:
	~PipelineBroadcastExchange();

private:
	BufferedPushState Append(DataChunk &chunk, idx_t batch_index, idx_t min_batch_index,
	                         const InterruptState &interrupt_state);
	BufferedPushState FlushReadyBatches(idx_t min_batch_index, const InterruptState &interrupt_state);
	SinkResultType CompletePush(DataChunk &chunk, PipelineBroadcastExchangeLocalState &lstate,
	                            BufferedPushState buffered_state);
	void RecordProducedRows(idx_t count);
	void ResetExchangeStateLocked() DUCKDB_REQUIRES(lock);
	void ResetConsumerReadStateLocked(ConsumerState &consumer, idx_t position) DUCKDB_REQUIRES(lock);
	void ResetConsumerRegistrationLocked(ConsumerState &consumer) DUCKDB_REQUIRES(lock);
	void ResetConsumerExecutionLocked(ConsumerState &consumer) DUCKDB_REQUIRES(lock);
	void DeactivateConsumerLocked(ConsumerState &consumer, idx_t position) DUCKDB_REQUIRES(lock);
	PipelineBroadcastExchangeConsumerSummary GetConsumerSummaryLocked() const DUCKDB_REQUIRES(lock);
	AppendAdmission PrepareAppendLocked(const InterruptState &interrupt_state, vector<ExchangeLogEntry> &log_entries)
	    DUCKDB_REQUIRES(lock);
	AppendAdmission ReserveAppendLocked(idx_t batch_index, const InterruptState &interrupt_state,
	                                    AppendReservation &reservation, vector<ExchangeLogEntry> &log_entries)
	    DUCKDB_REQUIRES(lock);
	AppendAdmission PrepareStageLocked(idx_t batch_index, const InterruptState &interrupt_state,
	                                   vector<ExchangeLogEntry> &log_entries) DUCKDB_REQUIRES(lock);
	BufferedPushState CompleteAppendLocked(const AppendReservation &reservation, shared_ptr<DataChunk> copy,
	                                       idx_t row_count, bool record_produced_rows, vector<InterruptState> &readers,
	                                       vector<InterruptState> &appenders, vector<ExchangeLogEntry> &log_entries)
	    DUCKDB_REQUIRES(lock);
	void AbortAppendReservation(vector<InterruptState> &readers, vector<InterruptState> &writers,
	                            vector<InterruptState> &appenders) DUCKDB_REQUIRES(lock);
	SourceResultType ReserveScanLocked(idx_t consumer_idx, const InterruptState &interrupt_state,
	                                   PipelineBroadcastExchangeScanState &scan_state,
	                                   shared_ptr<DataChunk> &next_chunk, optional_idx &batch_index,
	                                   SourceBatchIndexState &batch_index_state, SpoolReadReservation &spool_read,
	                                   vector<InterruptState> &readers, vector<InterruptState> &writers,
	                                   vector<ExchangeLogEntry> &log_entries) DUCKDB_REQUIRES(lock);
	SourceResultType ReserveBatchScanLocked(idx_t consumer_idx, const InterruptState &interrupt_state,
	                                        PipelineBroadcastExchangeScanState &scan_state,
	                                        shared_ptr<DataChunk> &next_chunk, optional_idx &batch_index,
	                                        SourceBatchIndexState &batch_index_state, SpoolReadReservation &spool_read,
	                                        vector<InterruptState> &readers, vector<InterruptState> &writers,
	                                        vector<ExchangeLogEntry> &log_entries) DUCKDB_REQUIRES(lock);
	void CompleteSpoolReadLocked(idx_t consumer_idx, const SpoolReadReservation &spool_read, DataChunk &chunk,
	                             vector<InterruptState> &readers, vector<InterruptState> &writers,
	                             vector<ExchangeLogEntry> &log_entries) DUCKDB_REQUIRES(lock);
	idx_t GetConsumerBatchFloorLocked(const ConsumerState &consumer) const DUCKDB_REQUIRES(lock);
	bool ShouldStopProducerLocked() const DUCKDB_REQUIRES(lock);
	bool ShouldThrottleProducerLocked() const DUCKDB_REQUIRES(lock);
	bool ShouldCreateSharedSpoolLocked() const DUCKDB_REQUIRES(lock);
	void CreateSharedSpoolLocked(vector<ExchangeLogEntry> &log_entries) DUCKDB_REQUIRES(lock);
	void RetireChunksLocked() DUCKDB_REQUIRES(lock);
	void TryReleaseBufferedStorageLocked() DUCKDB_REQUIRES(lock);
	void DeactivateAllConsumersLocked() DUCKDB_REQUIRES(lock);
	void WakeReadersLocked(vector<InterruptState> &readers, ReaderWakeMode mode = ReaderWakeMode::ALL)
	    DUCKDB_REQUIRES(lock);
	void WakeWritersLocked(vector<InterruptState> &writers, vector<ExchangeLogEntry> &log_entries,
	                       WriterWakeMode mode = WriterWakeMode::LOW_WATERMARK) DUCKDB_REQUIRES(lock);
	void WakeAppendersLocked(vector<InterruptState> &appenders) DUCKDB_REQUIRES(lock);
	void LogTransitions(const vector<ExchangeLogEntry> &log_entries) const;
	static void CallbackAll(vector<InterruptState> &interrupts);

private:
	ClientContext &context;
	vector<LogicalType> types;
	PipelineBroadcastExchangeCompletionMode completion_mode;
	PipelineBroadcastExchangeBufferMode buffer_mode;
	PipelineBroadcastExchangeOrderMode order_mode;
	OrderPreservationType source_order;
	idx_t max_threads;
	vector<reference<Pipeline>> producer_pipelines;
	unique_ptr<BufferState> buffer;

	mutable annotated_mutex lock;
	vector<ConsumerState> consumers;
	vector<reference<Pipeline>> direct_pipelines;
	//! Readers waiting for data in the shared stream or their claimed batch
	vector<InterruptState> blocked_readers;
	//! Batch readers waiting for a new range to claim
	vector<InterruptState> blocked_batch_claim_readers;
	vector<InterruptState> blocked_writers;
	vector<InterruptState> blocked_appenders;
	AppendReservationState append_reservation_state DUCKDB_GUARDED_BY(lock) = AppendReservationState::IDLE;
	idx_t active_consumers = 0;
	bool has_batch_scan_consumer DUCKDB_GUARDED_BY(lock) = false;
	atomic<idx_t> produced_rows {0};
	ProducerState producer_state DUCKDB_GUARDED_BY(lock) = ProducerState::ACTIVE;
	WatermarkState watermark_state = WatermarkState::BELOW_HIGH_WATERMARK;
	optional_ptr<const PhysicalOperator> log_operator;
};

} // namespace duckdb
