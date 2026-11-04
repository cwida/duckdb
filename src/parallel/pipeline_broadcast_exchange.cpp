#include "duckdb/parallel/pipeline_broadcast_exchange.hpp"

#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/logging/logger.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/pipeline_executor.hpp"
#include "duckdb/parallel/task_scheduler.hpp"
#include "duckdb/storage/standard_buffer_manager.hpp"

namespace duckdb {

static constexpr const idx_t PIPELINE_BROADCAST_HIGH_WATERMARK_CHUNKS = 32;
static constexpr const idx_t PIPELINE_BROADCAST_LOW_WATERMARK_CHUNKS = PIPELINE_BROADCAST_HIGH_WATERMARK_CHUNKS / 2;

static bool RequiresResetForReuse(const vector<LogicalType> &types) {
	for (auto &type : types) {
		if (!TypeIsConstantSize(type.InternalType())) {
			return true;
		}
	}
	return false;
}

struct PipelineBroadcastExchange::ChunkPool : public enable_shared_from_this<PipelineBroadcastExchange::ChunkPool> {
	enum class ResetMode : uint8_t { RESET, CLEAR_CARDINALITY };

	ChunkPool(Allocator &allocator_p, vector<LogicalType> types_p, idx_t max_threads_p)
	    : allocator(allocator_p), types(std::move(types_p)),
	      reset_mode(RequiresResetForReuse(types) ? ResetMode::RESET : ResetMode::CLEAR_CARDINALITY),
	      max_cached_chunks(MaxValue<idx_t>(max_threads_p * 4, 16)) {
	}

	shared_ptr<DataChunk> Acquire(DataChunk &source) {
		unique_ptr<DataChunk> result;
		idx_t acquire_generation;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			acquire_generation = generation;
			if (caching_enabled && !cached_chunks.empty()) {
				result = std::move(cached_chunks.back());
				cached_chunks.pop_back();
			}
		}

		if (!result) {
			result = make_uniq<DataChunk>();
			result->Initialize(allocator, types);
		} else if (reset_mode == ResetMode::RESET) {
			result->Reset();
		} else {
			result->SetChildCardinality(0);
		}

		source.Copy(*result);

		auto self = shared_from_this();
		return shared_ptr<DataChunk>(result.release(), [self, acquire_generation](DataChunk *chunk) {
			unique_ptr<DataChunk> owned(chunk);
			self->Release(std::move(owned), acquire_generation);
		});
	}

	void BeginExecution() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		cached_chunks.clear();
		generation++;
		caching_enabled = true;
	}

	void EndExecution() {
		annotated_lock_guard<annotated_mutex> guard(lock);
		caching_enabled = false;
		cached_chunks.clear();
	}

	void Release(unique_ptr<DataChunk> chunk, idx_t release_generation) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (!caching_enabled || release_generation != generation || cached_chunks.size() >= max_cached_chunks) {
			return;
		}
		cached_chunks.push_back(std::move(chunk));
	}

	Allocator &allocator;
	vector<LogicalType> types;
	ResetMode reset_mode;
	idx_t max_cached_chunks;
	idx_t generation = 0;
	bool caching_enabled = false;

	annotated_mutex lock;
	vector<unique_ptr<DataChunk>> cached_chunks;
};

struct PipelineBroadcastExchange::BroadcastSpoolReader {
	explicit BroadcastSpoolReader(BroadcastSpool &spool);

	bool Contains(idx_t row_idx) const {
		return scan_state.current_row_index <= row_idx && row_idx < scan_state.next_row_index;
	}

	ColumnDataScanState scan_state;
	DataChunk scan_chunk;
	idx_t observed_generation = 0;
};

struct PipelineBroadcastExchange::BroadcastSpool {
	struct ChunkEntry {
		idx_t row_offset;
		idx_t row_count;
		idx_t batch_index;
	};

	BroadcastSpool(ClientContext &context, const vector<LogicalType> &types, idx_t base_position_p)
	    : collection(context, types, ColumnDataAllocatorType::BUFFER_MANAGER_ALLOCATOR), base_position(base_position_p),
	      next_position(base_position_p) {
		collection.InitializeAppend(append_state);
	}

	void Append(DataChunk &chunk, idx_t batch_index) DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		D_ASSERT(chunk.size() > 0);
		auto row_offset = collection.Count();
		collection.Append(append_state, chunk);
		chunks.push_back(ChunkEntry {row_offset, chunk.size(), batch_index});
		next_position++;
		append_generation++;
	}

	void Read(idx_t position, const ChunkEntry &entry, DataChunk &result, BroadcastSpoolReader &reader,
	          bool producer_finished) DUCKDB_EXCLUDES(lock) {
		// Completion is observed under the exchange mutex; collection metadata and append_generation are then
		// immutable.
		annotated_unique_lock<annotated_mutex> guard(lock, std::defer_lock);
		if (!producer_finished) {
			guard.lock();
		}
		result.Reset();

		auto row_idx = entry.row_offset;
		auto offset = Seek(position, entry, row_idx, reader);
		auto available = reader.scan_chunk.size() - offset;
		if (available >= entry.row_count) {
			if (offset == 0 && entry.row_count == reader.scan_chunk.size()) {
				result.Reference(reader.scan_chunk);
			} else {
				result.Slice(reader.scan_chunk, offset, offset + entry.row_count);
			}
			return;
		}

		idx_t rows_read = 0;
		while (rows_read < entry.row_count) {
			row_idx = entry.row_offset + rows_read;
			offset = Seek(position, entry, row_idx, reader);
			auto append_count = MinValue<idx_t>(reader.scan_chunk.size() - offset, entry.row_count - rows_read);
			auto sel = SelectionVector::Incremental(offset, append_count);
			result.Append(reader.scan_chunk, sel, append_count);
			rows_read += append_count;
		}
	}

	idx_t RetireBefore(idx_t position) DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		idx_t retired_chunks = 0;
		while (!chunks.empty() && base_position < position) {
			chunks.pop_front();
			base_position++;
			retired_chunks++;
		}
		return retired_chunks;
	}

	ChunkEntry GetChunkEntry(idx_t position) DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (position < base_position || position >= next_position) {
			throw InternalException("Attempted to inspect retired pipeline broadcast spool chunk");
		}
		return chunks[position - base_position];
	}

	void InitializeReader(BroadcastSpoolReader &reader) DUCKDB_EXCLUDES(lock) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		ResetReader(reader);
		collection.InitializeScanChunk(reader.scan_state, reader.scan_chunk);
	}

	void ResetReader(BroadcastSpoolReader &reader) {
		collection.InitializeScan(reader.scan_state, ColumnDataScanProperties::ALLOW_ZERO_COPY);
		reader.observed_generation = append_generation;
	}

	idx_t Seek(idx_t position, const ChunkEntry &entry, idx_t row_idx, BroadcastSpoolReader &reader) {
		if (reader.observed_generation != append_generation && !reader.Contains(row_idx)) {
			ResetReader(reader);
		}
		if (!collection.Seek(row_idx, reader.scan_state, reader.scan_chunk)) {
			throw InternalException(
			    "Failed to read pipeline broadcast spool chunk (position=%llu, row=%llu, "
			    "row_offset=%llu, row_count=%llu, current=%llu, next_row=%llu, generation=%llu, observed=%llu)",
			    position, row_idx, entry.row_offset, entry.row_count, reader.scan_state.current_row_index,
			    reader.scan_state.next_row_index, append_generation, reader.observed_generation);
		}
		D_ASSERT(reader.scan_state.current_row_index <= row_idx);
		auto offset = row_idx - reader.scan_state.current_row_index;
		D_ASSERT(offset < reader.scan_chunk.size());
		return offset;
	}

	annotated_mutex lock;
	ColumnDataCollection collection;
	ColumnDataAppendState append_state DUCKDB_GUARDED_BY(lock);
	deque<ChunkEntry> chunks DUCKDB_GUARDED_BY(lock);
	idx_t base_position DUCKDB_GUARDED_BY(lock);
	idx_t next_position DUCKDB_GUARDED_BY(lock);
	idx_t append_generation = 0;
};

PipelineBroadcastExchange::BroadcastSpoolReader::BroadcastSpoolReader(BroadcastSpool &spool) {
	spool.InitializeReader(*this);
}

struct PipelineBroadcastExchange::AppendReservation {
	shared_ptr<BroadcastSpool> shared_spool;
	idx_t position = 0;
	idx_t producer_batch_index = DConstants::INVALID_INDEX;
	idx_t exchange_batch_index = DConstants::INVALID_INDEX;
	bool pending = false;
};

struct PipelineBroadcastExchange::SpoolReadReservation {
	shared_ptr<BroadcastSpool> spool;
	shared_ptr<BroadcastSpoolReader> reader;
	BroadcastSpool::ChunkEntry entry;
	idx_t position = 0;
	optional_idx batch_sequence;

	bool IsSet() const {
		return spool != nullptr;
	}
};

struct PipelineBroadcastExchange::BufferedChunk {
	shared_ptr<DataChunk> chunk;
	idx_t batch_index;
};

class PipelineBroadcastExchangeScanState {
	friend class PipelineBroadcastExchange;

public:
	PipelineBroadcastExchangeScanState() = default;

private:
	shared_ptr<DataChunk> current_chunk;
	shared_ptr<PipelineBroadcastExchange::BroadcastSpoolReader> spool_reader;
	optional_idx batch_sequence;
	//! Latest batch index exposed to this local scan
	optional_idx reported_batch_index;
};

struct PipelineBroadcastExchange::BufferState {
	//! A produced batch occupies a contiguous range in the shared chunk stream.
	struct BatchRange {
		idx_t producer_batch_index;
		idx_t batch_index;
		idx_t begin_position;
		idx_t end_position;
		bool closed = false;
	};

	BufferState(ClientContext &context_p, const vector<LogicalType> &types_p, idx_t max_threads)
	    : context(context_p), types(types_p),
	      chunk_pool(make_shared_ptr<ChunkPool>(BufferAllocator::Get(context), types, max_threads)) {
	}

	void BeginExecution() {
		chunk_pool->BeginExecution();
	}

	void EndExecution() {
		chunk_pool->EndExecution();
	}

	void Reset() {
		chunks.clear();
		pending_batches.clear();
		active_batches.clear();
		batch_ranges.clear();
		shared_spool.reset();
		base_position = 0;
		next_position = 0;
		base_batch_sequence = 0;
		buffered_chunks = 0;
		pending_chunks = 0;
		min_batch_index = 0;
		producer_pipeline_index = DConstants::INVALID_INDEX;
		producer_pipeline_offset = 0;
		max_local_batch_index = 0;
	}

	shared_ptr<DataChunk> Copy(DataChunk &chunk) {
		return chunk_pool->Acquire(chunk);
	}

	idx_t BasePosition() const {
		return base_position;
	}

	idx_t NextPosition() const {
		return next_position;
	}

	idx_t BaseBatchSequence() const {
		return base_batch_sequence;
	}

	idx_t NextBatchSequence() const {
		return base_batch_sequence + batch_ranges.size();
	}

	const BatchRange &GetBatchRange(idx_t batch_sequence) const {
		if (batch_sequence < base_batch_sequence || batch_sequence >= NextBatchSequence()) {
			throw InternalException("Attempted to inspect retired pipeline broadcast batch");
		}
		return batch_ranges[batch_sequence - base_batch_sequence];
	}

	idx_t BufferedCount() const {
		return buffered_chunks;
	}

	idx_t PendingCount() const {
		return pending_chunks;
	}

	idx_t MinBatchIndex() const {
		return min_batch_index;
	}

	bool UpdateMinBatchIndex(idx_t new_min_batch_index) {
		if (new_min_batch_index <= min_batch_index) {
			return false;
		}
		min_batch_index = new_min_batch_index;
		return true;
	}

	bool HasReadyBatch() const {
		return !pending_batches.empty() && pending_batches.begin()->first <= min_batch_index &&
		       (active_batches.empty() || pending_batches.begin()->first <= *active_batches.begin());
	}

	void RegisterActiveBatch(idx_t batch_index) {
		active_batches.insert(batch_index);
	}

	void UnregisterActiveBatch(idx_t batch_index) {
		auto entry = active_batches.find(batch_index);
		D_ASSERT(entry != active_batches.end());
		active_batches.erase(entry);
	}

	bool HasEarlierActiveBatch(idx_t batch_index) const {
		return !active_batches.empty() && *active_batches.begin() < batch_index;
	}

	bool HasSharedSpool() const {
		return shared_spool != nullptr;
	}

	bool Empty() const {
		return chunks.empty() && !shared_spool && batch_ranges.empty();
	}

	idx_t ExchangeBatchIndex(idx_t producer_batch_index) {
		if (producer_batch_index == DConstants::INVALID_INDEX) {
			return DConstants::INVALID_INDEX;
		}
		auto pipeline_index = producer_batch_index / PipelineBuildState::BATCH_INCREMENT;
		auto local_batch_index = producer_batch_index % PipelineBuildState::BATCH_INCREMENT;
		if (local_batch_index == 0) {
			throw InternalException("Pipeline broadcast exchange received an uninitialized batch index");
		}
		if (producer_pipeline_index == DConstants::INVALID_INDEX) {
			producer_pipeline_index = pipeline_index;
		} else if (pipeline_index != producer_pipeline_index) {
			if (pipeline_index < producer_pipeline_index) {
				throw InternalException("Pipeline broadcast producer index decreased from %llu to %llu",
				                        producer_pipeline_index, pipeline_index);
			}
			producer_pipeline_offset += max_local_batch_index;
			producer_pipeline_index = pipeline_index;
			max_local_batch_index = 0;
		}
		if (local_batch_index < max_local_batch_index) {
			throw InternalException("Pipeline broadcast exchange emitted batch %llu after batch %llu",
			                        local_batch_index, max_local_batch_index);
		}
		max_local_batch_index = MaxValue(max_local_batch_index, local_batch_index);
		auto result = producer_pipeline_offset + local_batch_index - 1;
		if (result >= PipelineBuildState::BATCH_INCREMENT - 1) {
			throw InternalException("Pipeline broadcast exchange exceeded the batch index range");
		}
		return result;
	}

	void ReserveAppend(AppendReservation &reservation, idx_t producer_batch_index) {
		reservation.shared_spool = shared_spool;
		reservation.position = next_position;
		reservation.producer_batch_index = producer_batch_index;
		reservation.exchange_batch_index = ExchangeBatchIndex(producer_batch_index);
		reservation.pending = false;
	}

	shared_ptr<DataChunk> ReservePendingAppend(AppendReservation &reservation) {
		D_ASSERT(HasReadyBatch());
		auto &pending = pending_batches.begin()->second.front();
		reservation.shared_spool = shared_spool;
		reservation.position = next_position;
		reservation.producer_batch_index = pending.batch_index;
		reservation.exchange_batch_index = ExchangeBatchIndex(pending.batch_index);
		reservation.pending = true;
		return pending.chunk;
	}

	void Stage(shared_ptr<DataChunk> copy, idx_t batch_index) {
		pending_batches[batch_index].push_back({std::move(copy), batch_index});
		pending_chunks++;
	}

	void CompleteAppend(const AppendReservation &reservation, shared_ptr<DataChunk> copy, bool track_batch_ranges) {
		if (reservation.pending) {
			D_ASSERT(HasReadyBatch());
			auto pending_entry = pending_batches.begin();
			D_ASSERT(pending_entry->first == reservation.producer_batch_index);
			D_ASSERT(pending_entry->second.front().chunk == copy);
			pending_entry->second.pop_front();
			pending_chunks--;
			if (pending_entry->second.empty()) {
				pending_batches.erase(pending_entry);
			}
		}
		if (!reservation.shared_spool) {
			chunks.push_back({std::move(copy), reservation.exchange_batch_index});
		}
		buffered_chunks++;
		D_ASSERT(next_position == reservation.position);
		next_position++;
		if (track_batch_ranges && reservation.exchange_batch_index != DConstants::INVALID_INDEX) {
			if (batch_ranges.empty() || batch_ranges.back().batch_index != reservation.exchange_batch_index) {
				if (!batch_ranges.empty()) {
					D_ASSERT(batch_ranges.back().batch_index < reservation.exchange_batch_index);
					batch_ranges.back().closed = true;
				}
				batch_ranges.push_back({reservation.producer_batch_index, reservation.exchange_batch_index,
				                        reservation.position, reservation.position + 1, false});
			} else {
				D_ASSERT(!batch_ranges.back().closed);
				batch_ranges.back().end_position++;
			}
		}
	}

	void FinishBatches() {
		if (!batch_ranges.empty()) {
			batch_ranges.back().closed = true;
		}
	}

	bool CloseBatchBefore(idx_t producer_batch_index) {
		if (batch_ranges.empty() || batch_ranges.back().closed ||
		    batch_ranges.back().producer_batch_index >= producer_batch_index) {
			return false;
		}
		batch_ranges.back().closed = true;
		return true;
	}

	void ReserveRead(idx_t position, shared_ptr<BroadcastSpoolReader> &reader, shared_ptr<DataChunk> &next_chunk,
	                 optional_idx &batch_index, SpoolReadReservation &spool_read) const {
		D_ASSERT(position < next_position);
		if (shared_spool) {
			spool_read.entry = shared_spool->GetChunkEntry(position);
			if (!reader) {
				reader = make_shared_ptr<BroadcastSpoolReader>(*shared_spool);
			}
			spool_read.spool = shared_spool;
			spool_read.reader = reader;
			spool_read.position = position;
			auto stored_batch_index = spool_read.entry.batch_index;
			if (stored_batch_index != DConstants::INVALID_INDEX) {
				batch_index = stored_batch_index;
			}
			return;
		}
		D_ASSERT(position >= base_position);
		auto chunk_idx = position - base_position;
		D_ASSERT(chunk_idx < chunks.size());
		next_chunk = chunks[chunk_idx].chunk;
		if (chunks[chunk_idx].batch_index != DConstants::INVALID_INDEX) {
			batch_index = chunks[chunk_idx].batch_index;
		}
	}

	void CreateSharedSpool() {
		D_ASSERT(!shared_spool);
		shared_spool = make_shared_ptr<BroadcastSpool>(context, types, base_position);
		for (auto &chunk : chunks) {
			shared_spool->Append(*chunk.chunk, chunk.batch_index);
		}
		chunks.clear();
	}

	void RetireBefore(idx_t position) {
		if (shared_spool) {
			auto retired_chunks = shared_spool->RetireBefore(position);
			buffered_chunks -= MinValue(buffered_chunks, retired_chunks);
			base_position = MaxValue(base_position, position);
			return;
		}
		while (!chunks.empty() && base_position < position) {
			buffered_chunks -= MinValue<idx_t>(buffered_chunks, 1);
			chunks.pop_front();
			base_position++;
		}
	}

	void RetireBatchRangesBefore(idx_t batch_sequence) {
		while (base_batch_sequence < batch_sequence && !batch_ranges.empty() && batch_ranges.front().closed) {
			batch_ranges.pop_front();
			base_batch_sequence++;
		}
	}

	void Release() {
		chunks.clear();
		pending_batches.clear();
		shared_spool.reset();
		base_batch_sequence += batch_ranges.size();
		batch_ranges.clear();
		buffered_chunks = 0;
		pending_chunks = 0;
		base_position = next_position;
	}

	ClientContext &context;
	const vector<LogicalType> &types;
	shared_ptr<ChunkPool> chunk_pool;
	deque<BufferedChunk> chunks;
	map<idx_t, deque<BufferedChunk>> pending_batches;
	multiset<idx_t> active_batches;
	deque<BatchRange> batch_ranges;
	shared_ptr<BroadcastSpool> shared_spool;
	idx_t base_position = 0;
	idx_t next_position = 0;
	idx_t base_batch_sequence = 0;
	idx_t buffered_chunks = 0;
	idx_t pending_chunks = 0;
	idx_t min_batch_index = 0;
	idx_t producer_pipeline_index = DConstants::INVALID_INDEX;
	idx_t producer_pipeline_offset = 0;
	idx_t max_local_batch_index = 0;
};

PipelineBroadcastExchange::ConsumerState::ConsumerState() = default;
PipelineBroadcastExchange::ConsumerState::~ConsumerState() = default;
PipelineBroadcastExchange::ConsumerState::ConsumerState(ConsumerState &&other) noexcept = default;
PipelineBroadcastExchange::ConsumerState &
PipelineBroadcastExchange::ConsumerState::operator=(ConsumerState &&other) noexcept = default;

PipelineBroadcastExchange::~PipelineBroadcastExchange() = default;

PipelineBroadcastExchangeLocalState::PipelineBroadcastExchangeLocalState(ClientContext &context,
                                                                         const PipelineBroadcastExchange &exchange,
                                                                         idx_t producer_base_batch_index_p)
    : producer_base_batch_index(producer_base_batch_index_p), supports_batch_index(exchange.SupportsBatchIndex()) {
	vector<reference<Pipeline>> direct_pipeline_refs;
	{
		annotated_lock_guard<annotated_mutex> guard(exchange.lock);
		auto summary = exchange.GetConsumerSummaryLocked();
		D_ASSERT(summary.unresolved == 0);
		mode = summary.ExchangeConsumerCount() > 0 && summary.buffered == 0 &&
		               exchange.direct_pipelines.size() == summary.direct
		           ? PipelineBroadcastExchangeLocalMode::DIRECT_ONLY
		           : PipelineBroadcastExchangeLocalMode::BUFFERED;
		direct_pipeline_refs = exchange.direct_pipelines;
	}

	for (auto &pipeline_ref : direct_pipeline_refs) {
		auto &pipeline = pipeline_ref.get();
		pipeline.PrepareExternalInput();
		direct_executors.push_back(make_uniq<PipelineExecutor>(context, pipeline));
		auto input_chunk = make_uniq<DataChunk>();
		input_chunk->InitializeEmpty(exchange.Types());
		direct_input_chunks.push_back(std::move(input_chunk));
	}
}

void PipelineBroadcastExchange::SetLogOperator(const PhysicalOperator &op) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	log_operator = &op;
}

PipelineBroadcastExchangeLocalState::~PipelineBroadcastExchangeLocalState() = default;

PipelineBroadcastExchange::PipelineBroadcastExchange(ClientContext &context, vector<LogicalType> types_p,
                                                     PipelineBroadcastExchangeCompletionMode completion_mode_p,
                                                     OrderPreservationType source_order_p, bool use_batch_index_p,
                                                     PipelineBroadcastExchangeBufferMode buffer_mode_p)
    : context(context), types(std::move(types_p)), completion_mode(completion_mode_p), buffer_mode(buffer_mode_p),
      order_mode(use_batch_index_p                                   ? PipelineBroadcastExchangeOrderMode::BATCH_INDEX
                 : source_order_p == OrderPreservationType::NO_ORDER ? PipelineBroadcastExchangeOrderMode::UNORDERED
                                                                     : PipelineBroadcastExchangeOrderMode::SEQUENTIAL),
      source_order(source_order_p),
      max_threads(NumericCast<idx_t>(TaskScheduler::GetScheduler(context).NumberOfThreads())) {
	D_ASSERT(!use_batch_index_p || source_order_p != OrderPreservationType::NO_ORDER);
	buffer = make_uniq<BufferState>(context, types, max_threads);
}

unique_ptr<PipelineBroadcastExchangeLocalState>
PipelineBroadcastExchange::GetLocalState(ClientContext &context, idx_t producer_base_batch_index) const {
	return make_uniq<PipelineBroadcastExchangeLocalState>(context, *this, producer_base_batch_index);
}

shared_ptr<PipelineBroadcastExchangeScanState> PipelineBroadcastExchange::GetScanState() const {
	return make_shared_ptr<PipelineBroadcastExchangeScanState>();
}

void PipelineBroadcastExchange::SetProducerPipelines(const vector<shared_ptr<Pipeline>> &pipelines) {
	D_ASSERT(!pipelines.empty());
	annotated_lock_guard<annotated_mutex> guard(lock);
	producer_pipelines.clear();
	for (auto &pipeline : pipelines) {
		producer_pipelines.push_back(*pipeline);
	}
}

idx_t PipelineBroadcastExchange::RegisterConsumer() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	ConsumerState state;
	state.position = buffer->BasePosition();
	consumers.push_back(std::move(state));
	active_consumers++;
	return consumers.size() - 1;
}

void PipelineBroadcastExchange::SelectMaterializedConsumer(idx_t consumer_idx) {
	// Materialized consumers are served from CTE storage instead of this exchange.
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(consumer_idx < consumers.size());
	auto &consumer = consumers[consumer_idx];
	D_ASSERT(consumer.mode == PipelineBroadcastExchangeConsumerMode::UNRESOLVED);
	consumer.mode = PipelineBroadcastExchangeConsumerMode::MATERIALIZED;
	DeactivateConsumerLocked(consumer, buffer->NextPosition());
}

bool PipelineBroadcastExchange::CanRegisterDirectConsumer(Pipeline &pipeline) const {
	auto source_partition_info =
	    SupportsBatchIndex() ? OperatorPartitionInfo::BatchIndex() : OperatorPartitionInfo::NoPartitionInfo();
	if (!pipeline.CanUseExternalInput(source_partition_info)) {
		return false;
	}
	annotated_lock_guard<annotated_mutex> guard(lock);
	auto required_partition_info = pipeline.GetSink()->RequiredPartitionInfo();
	if (required_partition_info.RequiresBatchIndex() && producer_pipelines.size() != 1) {
		return false;
	}
	return true;
}

void PipelineBroadcastExchange::SelectDirectConsumer(Pipeline &pipeline, idx_t consumer_idx) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(consumer_idx < consumers.size());
	auto &consumer = consumers[consumer_idx];
	if (consumer.mode == PipelineBroadcastExchangeConsumerMode::DIRECT) {
		return;
	}
	D_ASSERT(consumer.mode == PipelineBroadcastExchangeConsumerMode::UNRESOLVED);
	consumer.mode = PipelineBroadcastExchangeConsumerMode::DIRECT;
	DeactivateConsumerLocked(consumer, buffer->BasePosition());
	direct_pipelines.push_back(pipeline);
}

bool PipelineBroadcastExchange::TryRegisterDirectConsumer(Pipeline &pipeline, idx_t consumer_idx) {
	if (!CanRegisterDirectConsumer(pipeline)) {
		return false;
	}
	pipeline.SetExternalInput(GetProducerPipelines());
	SelectDirectConsumer(pipeline, consumer_idx);
	return true;
}

vector<reference<Pipeline>> PipelineBroadcastExchange::GetProducerPipelines() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return producer_pipelines;
}

void PipelineBroadcastExchange::SelectBufferedConsumer(idx_t consumer_idx,
                                                       PipelineBroadcastExchangeScanMode scan_mode) {
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(consumer_idx < consumers.size());
	auto &consumer = consumers[consumer_idx];
	D_ASSERT(consumer.mode == PipelineBroadcastExchangeConsumerMode::UNRESOLVED);
	D_ASSERT(scan_mode != PipelineBroadcastExchangeScanMode::BATCH || SupportsBatchIndex());
	consumer.mode = PipelineBroadcastExchangeConsumerMode::BUFFERED;
	consumer.scan_mode = scan_mode;
	if (scan_mode == PipelineBroadcastExchangeScanMode::BATCH) {
		has_batch_scan_consumer = true;
	}
}

void PipelineBroadcastExchange::ResetConsumerRegistrations() {
	annotated_lock_guard<annotated_mutex> guard(lock);
	buffer->EndExecution();
	ResetExchangeStateLocked();
	direct_pipelines.clear();
	producer_pipelines.clear();
	blocked_readers.clear();
	blocked_batch_claim_readers.clear();
	blocked_writers.clear();
	blocked_appenders.clear();
	has_batch_scan_consumer = false;
	for (auto &consumer : consumers) {
		ResetConsumerRegistrationLocked(consumer);
	}
}

void PipelineBroadcastExchange::Reset() {
	vector<InterruptState> readers;
	vector<InterruptState> writers;
	vector<InterruptState> appenders;
	vector<ExchangeLogEntry> log_entries;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		buffer->BeginExecution();
		ResetExchangeStateLocked();
		for (auto &consumer : consumers) {
			ResetConsumerExecutionLocked(consumer);
		}
		WakeReadersLocked(readers);
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		WakeAppendersLocked(appenders);
	}
	CallbackAll(readers);
	CallbackAll(writers);
	CallbackAll(appenders);
	LogTransitions(log_entries);
}

void PipelineBroadcastExchange::ResetExchangeStateLocked() {
	buffer->Reset();
	produced_rows.store(0, std::memory_order_relaxed);
	producer_state = ProducerState::ACTIVE;
	watermark_state = WatermarkState::BELOW_HIGH_WATERMARK;
	append_reservation_state = AppendReservationState::IDLE;
	active_consumers = 0;
}

void PipelineBroadcastExchange::ResetConsumerReadStateLocked(ConsumerState &consumer, idx_t position) {
	consumer.position = position;
	consumer.next_batch_sequence = buffer->BaseBatchSequence();
	consumer.batch_index_floor = DConstants::INVALID_INDEX;
	consumer.exhausted = false;
	consumer.in_flight_reads.clear();
	consumer.active_batch_positions.clear();
}

void PipelineBroadcastExchange::ResetConsumerRegistrationLocked(ConsumerState &consumer) {
	consumer.rows_read = 0;
	consumer.lifecycle = ConsumerLifecycle::ACTIVE;
	consumer.mode = PipelineBroadcastExchangeConsumerMode::UNRESOLVED;
	consumer.scan_mode = PipelineBroadcastExchangeScanMode::CHUNK;
	ResetConsumerReadStateLocked(consumer, buffer->BasePosition());
	active_consumers++;
}

void PipelineBroadcastExchange::ResetConsumerExecutionLocked(ConsumerState &consumer) {
	consumer.rows_read = 0;
	D_ASSERT(consumer.mode != PipelineBroadcastExchangeConsumerMode::UNRESOLVED);
	consumer.lifecycle = consumer.mode == PipelineBroadcastExchangeConsumerMode::BUFFERED ? ConsumerLifecycle::ACTIVE
	                                                                                      : ConsumerLifecycle::INACTIVE;
	ResetConsumerReadStateLocked(consumer, buffer->BasePosition());
	if (consumer.lifecycle == ConsumerLifecycle::ACTIVE) {
		active_consumers++;
	}
}

void PipelineBroadcastExchange::DeactivateConsumerLocked(ConsumerState &consumer, idx_t position) {
	if (consumer.lifecycle == ConsumerLifecycle::ACTIVE) {
		D_ASSERT(active_consumers > 0);
		active_consumers--;
	}
	consumer.lifecycle = ConsumerLifecycle::INACTIVE;
	ResetConsumerReadStateLocked(consumer, position);
}

SinkResultType PipelineBroadcastExchange::Push(DataChunk &chunk, PipelineBroadcastExchangeLocalState &lstate,
                                               const SourcePartitionInfo &partition_info,
                                               const InterruptState &interrupt_state) {
	if (lstate.HasDirectConsumers() &&
	    (lstate.direct_push_state == PipelineBroadcastExchangeDirectPushState::NOT_STARTED ||
	     lstate.direct_push_state == PipelineBroadcastExchangeDirectPushState::RESUMING)) {
		auto direct_result = lstate.Push(chunk, partition_info, interrupt_state);
		if (direct_result == SinkResultType::BLOCKED) {
			return SinkResultType::BLOCKED;
		}
	}

	if (lstate.mode == PipelineBroadcastExchangeLocalMode::BUFFERED) {
		idx_t batch_index = DConstants::INVALID_INDEX;
		idx_t min_batch_index = DConstants::INVALID_INDEX;
		if (SupportsBatchIndex()) {
			D_ASSERT(partition_info.batch_index.IsValid());
			D_ASSERT(partition_info.min_batch_index.IsValid());
			batch_index = partition_info.batch_index.GetIndex();
			min_batch_index = partition_info.min_batch_index.GetIndex();
		}
		auto append_result = Append(chunk, batch_index, min_batch_index, interrupt_state);
		if (append_result == BufferedPushState::BLOCKED) {
			return SinkResultType::BLOCKED;
		}
		return CompletePush(chunk, lstate, append_result);
	}
	return CompletePush(chunk, lstate, BufferedPushState::NOT_REQUIRED);
}

SinkResultType PipelineBroadcastExchange::CompletePush(DataChunk &chunk, PipelineBroadcastExchangeLocalState &lstate,
                                                       BufferedPushState buffered_state) {
	if (buffered_state != BufferedPushState::APPENDED && buffered_state != BufferedPushState::STAGED &&
	    buffered_state != BufferedPushState::CANCELLED) {
		RecordProducedRows(chunk.size());
	}
	const auto direct_consumers_finished = lstate.DirectConsumersFinished();
	lstate.ResetPush();
	if (buffered_state == BufferedPushState::CANCELLED ||
	    (!RunToCompletion() && direct_consumers_finished &&
	     (buffered_state == BufferedPushState::UNCONSUMED || buffered_state == BufferedPushState::NOT_REQUIRED))) {
		return SinkResultType::FINISHED;
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkNextBatchType PipelineBroadcastExchange::NextBatch(PipelineBroadcastExchangeLocalState &lstate,
                                                       const SourcePartitionInfo &partition_info,
                                                       const InterruptState &interrupt_state) {
	if (!SupportsBatchIndex()) {
		return SinkNextBatchType::READY;
	}
	D_ASSERT(partition_info.min_batch_index.IsValid());
	auto result = FlushReadyBatches(partition_info.min_batch_index.GetIndex(), interrupt_state);
	if (result == BufferedPushState::BLOCKED) {
		return SinkNextBatchType::BLOCKED;
	}
	return lstate.NextBatch(partition_info, interrupt_state);
}

SinkNextBatchType PipelineBroadcastExchange::UpdateMinBatchIndex(PipelineBroadcastExchangeLocalState &lstate,
                                                                 const SourcePartitionInfo &partition_info,
                                                                 const InterruptState &interrupt_state) {
	if (!SupportsBatchIndex()) {
		return SinkNextBatchType::READY;
	}
	D_ASSERT(partition_info.min_batch_index.IsValid());
	auto result = FlushReadyBatches(partition_info.min_batch_index.GetIndex(), interrupt_state);
	if (result == BufferedPushState::BLOCKED) {
		return SinkNextBatchType::BLOCKED;
	}
	return lstate.UpdateMinBatchIndex(partition_info, interrupt_state);
}

SinkCombineResultType PipelineBroadcastExchange::FinishLocal(PipelineBroadcastExchangeLocalState &lstate,
                                                             const SourcePartitionInfo &partition_info,
                                                             const InterruptState &interrupt_state) {
	if (SupportsBatchIndex()) {
		D_ASSERT(partition_info.min_batch_index.IsValid());
		auto result = FlushReadyBatches(partition_info.min_batch_index.GetIndex(), interrupt_state);
		if (result == BufferedPushState::BLOCKED) {
			return SinkCombineResultType::BLOCKED;
		}
	}
	return lstate.Finish(partition_info, interrupt_state);
}

SinkResultType PipelineBroadcastExchangeLocalState::Push(DataChunk &chunk, const SourcePartitionInfo &partition_info,
                                                         const InterruptState &interrupt_state) {
	auto resuming = direct_push_state == PipelineBroadcastExchangeDirectPushState::RESUMING;
	if (!resuming) {
		direct_idx = 0;
	}
	auto source_partition_data = GetSourcePartitionData(partition_info);
	auto source_min_batch_index = GetSourceMinBatchIndex(partition_info);
	for (; direct_idx < direct_executors.size(); direct_idx++) {
		auto &executor = *direct_executors[direct_idx];
		executor.SetInterruptState(interrupt_state);
		if (executor.IsFinishedProcessing()) {
			resuming = false;
			continue;
		}
		auto &input_chunk = *direct_input_chunks[direct_idx];
		if (!resuming) {
			// Operators may slice their input chunk. Keep each direct consumer's wrapper independent.
			input_chunk.Reference(chunk);
		}
		auto result = executor.PushExternal(input_chunk, source_partition_data, source_min_batch_index);
		if (result == PipelineExecuteResult::INTERRUPTED) {
			direct_push_state = PipelineBroadcastExchangeDirectPushState::RESUMING;
			return SinkResultType::BLOCKED;
		}
		resuming = false;
	}

	direct_push_state = PipelineBroadcastExchangeDirectPushState::FINISHED;
	for (auto &executor : direct_executors) {
		if (!executor->IsFinishedProcessing()) {
			direct_push_state = PipelineBroadcastExchangeDirectPushState::ACTIVE;
			break;
		}
	}
	return SinkResultType::NEED_MORE_INPUT;
}

SinkNextBatchType PipelineBroadcastExchangeLocalState::NextBatch(const SourcePartitionInfo &partition_info,
                                                                 const InterruptState &interrupt_state) {
	auto source_min_batch_index = GetSourceMinBatchIndex(partition_info);
	if (!partition_info.batch_index.IsValid()) {
		throw InternalException("Batch-indexed pipeline broadcast exchange received no batch index");
	}
	auto batch_index = partition_info.batch_index.GetIndex();
	auto max_batch_index = producer_base_batch_index + PipelineBuildState::BATCH_INCREMENT - 1;
	const bool finalize = batch_index == max_batch_index;
	OperatorPartitionData source_partition_data(0);
	if (!finalize) {
		source_partition_data = GetSourcePartitionData(partition_info);
	}

	for (; direct_next_batch_idx < direct_executors.size(); direct_next_batch_idx++) {
		auto &executor = *direct_executors[direct_next_batch_idx];
		executor.SetInterruptState(interrupt_state);
		PipelineExecuteResult result;
		if (finalize || executor.IsFinishedProcessing()) {
			result = executor.FinishBatchExternal(source_min_batch_index);
		} else {
			result = executor.NextBatchExternal(source_partition_data, source_min_batch_index);
		}
		if (result == PipelineExecuteResult::INTERRUPTED) {
			return SinkNextBatchType::BLOCKED;
		}
	}
	direct_next_batch_idx = 0;
	return SinkNextBatchType::READY;
}

SinkNextBatchType PipelineBroadcastExchangeLocalState::UpdateMinBatchIndex(const SourcePartitionInfo &partition_info,
                                                                           const InterruptState &interrupt_state) {
	auto source_min_batch_index = GetSourceMinBatchIndex(partition_info);
	for (; direct_min_batch_idx < direct_executors.size(); direct_min_batch_idx++) {
		auto &executor = *direct_executors[direct_min_batch_idx];
		executor.SetInterruptState(interrupt_state);
		if (executor.IsFinishedProcessing()) {
			continue;
		}
		auto result = executor.UpdateMinBatchIndexExternal(source_min_batch_index);
		if (result == PipelineExecuteResult::INTERRUPTED) {
			return SinkNextBatchType::BLOCKED;
		}
	}
	direct_min_batch_idx = 0;
	return SinkNextBatchType::READY;
}

SinkCombineResultType PipelineBroadcastExchangeLocalState::Finish(const SourcePartitionInfo &partition_info,
                                                                  const InterruptState &interrupt_state) {
	auto source_min_batch_index = GetSourceMinBatchIndex(partition_info);
	for (; direct_finalize_idx < direct_executors.size(); direct_finalize_idx++) {
		auto &executor = *direct_executors[direct_finalize_idx];
		executor.SetInterruptState(interrupt_state);
		auto result = PipelineExecuteResult::NOT_FINISHED;
		while (result == PipelineExecuteResult::NOT_FINISHED) {
			result = executor.FinishExternal(source_min_batch_index);
		}
		if (result == PipelineExecuteResult::INTERRUPTED) {
			return SinkCombineResultType::BLOCKED;
		}
	}
	return SinkCombineResultType::FINISHED;
}

OperatorPartitionData
PipelineBroadcastExchangeLocalState::GetSourcePartitionData(const SourcePartitionInfo &partition_info) const {
	OperatorPartitionData result(0);
	if (!supports_batch_index) {
		return result;
	}
	if (!partition_info.batch_index.IsValid()) {
		throw InternalException("Batch-indexed pipeline broadcast exchange received no batch index");
	}
	auto batch_index = partition_info.batch_index.GetIndex();
	if (batch_index <= producer_base_batch_index) {
		throw InternalException("Pipeline broadcast exchange received invalid producer batch index %llu", batch_index);
	}
	result.batch_index = batch_index - producer_base_batch_index - 1;
	if (result.batch_index >= PipelineBuildState::BATCH_INCREMENT - 2) {
		throw InternalException("Pipeline broadcast exchange received producer batch index outside its pipeline");
	}
	return result;
}

optional_idx
PipelineBroadcastExchangeLocalState::GetSourceMinBatchIndex(const SourcePartitionInfo &partition_info) const {
	if (!supports_batch_index) {
		return optional_idx();
	}
	if (!partition_info.min_batch_index.IsValid()) {
		throw InternalException("Batch-indexed pipeline broadcast exchange received no minimum batch index");
	}
	auto producer_min_batch_index = partition_info.min_batch_index.GetIndex();
	if (producer_min_batch_index < producer_base_batch_index) {
		throw InternalException("Pipeline broadcast exchange received invalid producer minimum batch index %llu",
		                        producer_min_batch_index);
	}
	if (partition_info.batch_index.IsValid() && producer_min_batch_index > partition_info.batch_index.GetIndex()) {
		throw InternalException("Pipeline broadcast exchange received invalid producer minimum batch index %llu",
		                        producer_min_batch_index);
	}
	auto result = producer_min_batch_index - producer_base_batch_index;
	if (result >= PipelineBuildState::BATCH_INCREMENT) {
		throw InternalException("Pipeline broadcast exchange received producer minimum batch index outside its "
		                        "pipeline");
	}
	return optional_idx(result);
}

bool PipelineBroadcastExchangeLocalState::HasDirectConsumers() const {
	return !direct_executors.empty();
}

bool PipelineBroadcastExchangeLocalState::DirectConsumersFinished() const {
	return direct_executors.empty() || direct_push_state == PipelineBroadcastExchangeDirectPushState::FINISHED;
}

void PipelineBroadcastExchangeLocalState::ResetPush() {
	direct_push_state = PipelineBroadcastExchangeDirectPushState::NOT_STARTED;
}

PipelineBroadcastExchange::AppendAdmission
PipelineBroadcastExchange::PrepareAppendLocked(const InterruptState &interrupt_state,
                                               vector<ExchangeLogEntry> &log_entries) {
	if (producer_state != ProducerState::ACTIVE) {
		return AppendAdmission::CANCELLED;
	}
	if (active_consumers == 0) {
		return AppendAdmission::UNCONSUMED;
	}
	if (append_reservation_state == AppendReservationState::RESERVED) {
		blocked_appenders.push_back(interrupt_state);
		return AppendAdmission::BLOCKED;
	}
	if (ShouldCreateSharedSpoolLocked()) {
		CreateSharedSpoolLocked(log_entries);
	}
	if (ShouldThrottleProducerLocked()) {
		if (watermark_state == WatermarkState::BELOW_HIGH_WATERMARK) {
			watermark_state = WatermarkState::ABOVE_HIGH_WATERMARK;
			log_entries.push_back(
			    {ExchangeLogEvent::HIGH_WATERMARK_BLOCKED, active_consumers, buffer->BufferedCount()});
		}
		blocked_writers.push_back(interrupt_state);
		return AppendAdmission::BLOCKED;
	}
	return AppendAdmission::READY;
}

PipelineBroadcastExchange::AppendAdmission
PipelineBroadcastExchange::ReserveAppendLocked(idx_t batch_index, const InterruptState &interrupt_state,
                                               AppendReservation &reservation, vector<ExchangeLogEntry> &log_entries) {
	auto admission = PrepareAppendLocked(interrupt_state, log_entries);
	if (admission != AppendAdmission::READY) {
		return admission;
	}
	append_reservation_state = AppendReservationState::RESERVED;
	buffer->ReserveAppend(reservation, batch_index);
	return AppendAdmission::READY;
}

PipelineBroadcastExchange::AppendAdmission
PipelineBroadcastExchange::PrepareStageLocked(idx_t batch_index, const InterruptState &interrupt_state,
                                              vector<ExchangeLogEntry> &log_entries) {
	if (producer_state != ProducerState::ACTIVE) {
		return AppendAdmission::CANCELLED;
	}
	if (active_consumers == 0) {
		return AppendAdmission::UNCONSUMED;
	}
	if (batch_index > buffer->MinBatchIndex() && buffer->PendingCount() >= PIPELINE_BROADCAST_HIGH_WATERMARK_CHUNKS) {
		if (watermark_state == WatermarkState::BELOW_HIGH_WATERMARK) {
			watermark_state = WatermarkState::ABOVE_HIGH_WATERMARK;
			log_entries.push_back({ExchangeLogEvent::HIGH_WATERMARK_BLOCKED, active_consumers, buffer->PendingCount()});
		}
		blocked_writers.push_back(interrupt_state);
		return AppendAdmission::BLOCKED;
	}
	return AppendAdmission::READY;
}

PipelineBroadcastExchange::BufferedPushState PipelineBroadcastExchange::CompleteAppendLocked(
    const AppendReservation &reservation, shared_ptr<DataChunk> copy, idx_t row_count, bool record_produced_rows,
    vector<InterruptState> &readers, vector<InterruptState> &appenders, vector<ExchangeLogEntry> &log_entries) {
	D_ASSERT(append_reservation_state == AppendReservationState::RESERVED);
	append_reservation_state = AppendReservationState::IDLE;
	if (producer_state == ProducerState::CANCELLED) {
		TryReleaseBufferedStorageLocked();
		WakeReadersLocked(readers);
		WakeAppendersLocked(appenders);
		return BufferedPushState::CANCELLED;
	}
	D_ASSERT(producer_state == ProducerState::ACTIVE);
	if (active_consumers == 0) {
		TryReleaseBufferedStorageLocked();
		WakeAppendersLocked(appenders);
		return BufferedPushState::UNCONSUMED;
	}
	auto next_batch_sequence = buffer->NextBatchSequence();
	buffer->CompleteAppend(reservation, std::move(copy), has_batch_scan_consumer);
	auto reader_wake_mode =
	    buffer->NextBatchSequence() > next_batch_sequence ? ReaderWakeMode::ALL : ReaderWakeMode::DATA_AVAILABLE;
	WakeReadersLocked(readers, reader_wake_mode);
	if (record_produced_rows) {
		RecordProducedRows(row_count);
	}
	WakeAppendersLocked(appenders);
	return BufferedPushState::APPENDED;
}

void PipelineBroadcastExchange::AbortAppendReservation(vector<InterruptState> &readers, vector<InterruptState> &writers,
                                                       vector<InterruptState> &appenders) {
	D_ASSERT(append_reservation_state == AppendReservationState::RESERVED);
	append_reservation_state = AppendReservationState::IDLE;
	producer_state = ProducerState::CANCELLED;
	DeactivateAllConsumersLocked();
	TryReleaseBufferedStorageLocked();
	WakeReadersLocked(readers);
	vector<ExchangeLogEntry> ignored_log_entries;
	WakeWritersLocked(writers, ignored_log_entries, WriterWakeMode::FORCE);
	WakeAppendersLocked(appenders);
}

PipelineBroadcastExchange::BufferedPushState
PipelineBroadcastExchange::FlushReadyBatches(idx_t min_batch_index, const InterruptState &interrupt_state) {
	D_ASSERT(SupportsBatchIndex());
	bool appended = false;
	while (true) {
		vector<ExchangeLogEntry> log_entries;
		vector<InterruptState> readers;
		vector<InterruptState> writers;
		AppendAdmission admission;
		AppendReservation reservation;
		shared_ptr<DataChunk> copy;
		bool has_ready_batch;
		try {
			annotated_lock_guard<annotated_mutex> guard(lock);
			if (buffer->UpdateMinBatchIndex(min_batch_index)) {
				WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
			}
			has_ready_batch = buffer->HasReadyBatch();
			if (has_ready_batch) {
				admission = PrepareAppendLocked(interrupt_state, log_entries);
				if (admission == AppendAdmission::READY) {
					append_reservation_state = AppendReservationState::RESERVED;
					copy = buffer->ReservePendingAppend(reservation);
				}
			} else if (buffer->CloseBatchBefore(min_batch_index)) {
				WakeReadersLocked(readers, ReaderWakeMode::DATA_AVAILABLE);
			}
		} catch (...) {
			Cancel();
			throw;
		}
		CallbackAll(readers);
		CallbackAll(writers);
		LogTransitions(log_entries);
		if (!has_ready_batch) {
			return appended ? BufferedPushState::APPENDED : BufferedPushState::NOT_REQUIRED;
		}
		if (admission == AppendAdmission::BLOCKED) {
			return BufferedPushState::BLOCKED;
		}
		if (admission == AppendAdmission::UNCONSUMED) {
			return BufferedPushState::UNCONSUMED;
		}
		if (admission == AppendAdmission::CANCELLED) {
			return BufferedPushState::CANCELLED;
		}

		try {
			if (reservation.shared_spool) {
				reservation.shared_spool->Append(*copy, reservation.exchange_batch_index);
			}
		} catch (...) {
			vector<InterruptState> readers;
			vector<InterruptState> writers;
			vector<InterruptState> appenders;
			{
				annotated_lock_guard<annotated_mutex> guard(lock);
				AbortAppendReservation(readers, writers, appenders);
			}
			CallbackAll(readers);
			CallbackAll(writers);
			CallbackAll(appenders);
			buffer->EndExecution();
			throw;
		}

		readers.clear();
		writers.clear();
		vector<InterruptState> appenders;
		log_entries.clear();
		BufferedPushState result;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			result = CompleteAppendLocked(reservation, std::move(copy), 0, false, readers, appenders, log_entries);
			WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		}
		CallbackAll(readers);
		CallbackAll(writers);
		CallbackAll(appenders);
		LogTransitions(log_entries);
		if (result != BufferedPushState::APPENDED) {
			return result;
		}
		appended = true;
	}
}

PipelineBroadcastExchange::BufferedPushState PipelineBroadcastExchange::Append(DataChunk &chunk, idx_t batch_index,
                                                                               idx_t min_batch_index,
                                                                               const InterruptState &interrupt_state) {
	if (SupportsBatchIndex()) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		buffer->RegisterActiveBatch(batch_index);
	}
	auto unregister_active_batch = [&](idx_t *) {
		if (!SupportsBatchIndex()) {
			return;
		}
		annotated_lock_guard<annotated_mutex> guard(lock);
		buffer->UnregisterActiveBatch(batch_index);
	};
	unique_ptr<idx_t, decltype(unregister_active_batch)> active_batch_guard(
	    SupportsBatchIndex() ? &batch_index : nullptr, unregister_active_batch);

	if (SupportsBatchIndex()) {
		auto flush_result = FlushReadyBatches(min_batch_index, interrupt_state);
		if (flush_result == BufferedPushState::BLOCKED || flush_result == BufferedPushState::UNCONSUMED ||
		    flush_result == BufferedPushState::CANCELLED) {
			return flush_result;
		}
	}

	vector<ExchangeLogEntry> log_entries;
	AppendAdmission admission;
	try {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (SupportsBatchIndex()) {
			buffer->UpdateMinBatchIndex(min_batch_index);
		}
		admission = SupportsBatchIndex() && batch_index > buffer->MinBatchIndex()
		                ? PrepareStageLocked(batch_index, interrupt_state, log_entries)
		                : PrepareAppendLocked(interrupt_state, log_entries);
	} catch (...) {
		Cancel();
		throw;
	}
	LogTransitions(log_entries);
	if (admission == AppendAdmission::BLOCKED) {
		return BufferedPushState::BLOCKED;
	}
	if (admission == AppendAdmission::UNCONSUMED) {
		return BufferedPushState::UNCONSUMED;
	}
	if (admission == AppendAdmission::CANCELLED) {
		return BufferedPushState::CANCELLED;
	}

	shared_ptr<DataChunk> copy;
	try {
		copy = buffer->Copy(chunk);
	} catch (...) {
		Cancel();
		throw;
	}
	AppendReservation reservation;
	log_entries.clear();
	bool staged = false;
	try {
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (SupportsBatchIndex()) {
			buffer->UpdateMinBatchIndex(min_batch_index);
		}
		if (SupportsBatchIndex() && (batch_index > buffer->MinBatchIndex() ||
		                             buffer->HasEarlierActiveBatch(batch_index) || buffer->HasReadyBatch())) {
			admission = PrepareStageLocked(batch_index, interrupt_state, log_entries);
			if (admission == AppendAdmission::READY) {
				buffer->Stage(copy, batch_index);
				RecordProducedRows(chunk.size());
				staged = true;
			}
		} else {
			admission = ReserveAppendLocked(batch_index, interrupt_state, reservation, log_entries);
		}
	} catch (...) {
		Cancel();
		throw;
	}
	LogTransitions(log_entries);
	if (admission == AppendAdmission::BLOCKED) {
		return BufferedPushState::BLOCKED;
	}
	if (admission == AppendAdmission::UNCONSUMED) {
		return BufferedPushState::UNCONSUMED;
	}
	if (admission == AppendAdmission::CANCELLED) {
		return BufferedPushState::CANCELLED;
	}
	if (staged) {
		return BufferedPushState::STAGED;
	}

	try {
		if (reservation.shared_spool) {
			reservation.shared_spool->Append(*copy, reservation.exchange_batch_index);
		}
	} catch (...) {
		vector<InterruptState> readers;
		vector<InterruptState> writers;
		vector<InterruptState> appenders;
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			AbortAppendReservation(readers, writers, appenders);
		}
		CallbackAll(readers);
		CallbackAll(writers);
		CallbackAll(appenders);
		buffer->EndExecution();
		throw;
	}

	vector<InterruptState> readers;
	vector<InterruptState> appenders;
	log_entries.clear();
	BufferedPushState result;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		result =
		    CompleteAppendLocked(reservation, std::move(copy), chunk.size(), true, readers, appenders, log_entries);
	}
	CallbackAll(readers);
	CallbackAll(appenders);
	LogTransitions(log_entries);
	return result;
}

void PipelineBroadcastExchange::RecordProducedRows(idx_t count) {
	produced_rows.fetch_add(count, std::memory_order_relaxed);
}

void PipelineBroadcastExchange::Finish() {
	vector<InterruptState> readers;
	vector<InterruptState> writers;
	vector<ExchangeLogEntry> log_entries;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (active_consumers > 0 && buffer->PendingCount() > 0) {
			throw InternalException("Finishing ordered pipeline broadcast exchange with %llu pending chunks",
			                        buffer->PendingCount());
		}
		buffer->FinishBatches();
		if (producer_state == ProducerState::ACTIVE) {
			D_ASSERT(append_reservation_state == AppendReservationState::IDLE);
			producer_state = ProducerState::FINISHED;
		}
		TryReleaseBufferedStorageLocked();
		WakeReadersLocked(readers);
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
	}
	buffer->EndExecution();
	CallbackAll(readers);
	CallbackAll(writers);
	LogTransitions(log_entries);
}

void PipelineBroadcastExchange::FinishDirectConsumers() {
	for (auto &pipeline_ref : direct_pipelines) {
		pipeline_ref.get().CompleteExternalInput();
	}
}

void PipelineBroadcastExchange::Cancel() {
	vector<InterruptState> readers;
	vector<InterruptState> writers;
	vector<InterruptState> appenders;
	vector<ExchangeLogEntry> log_entries;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (producer_state != ProducerState::CANCELLED) {
			producer_state = ProducerState::CANCELLED;
			DeactivateAllConsumersLocked();
			TryReleaseBufferedStorageLocked();
		}
		WakeReadersLocked(readers);
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		WakeAppendersLocked(appenders);
	}
	buffer->EndExecution();
	CallbackAll(readers);
	CallbackAll(writers);
	CallbackAll(appenders);
	LogTransitions(log_entries);
}

SourceResultType PipelineBroadcastExchange::Scan(idx_t consumer_idx, DataChunk &chunk,
                                                 PipelineBroadcastExchangeScanState &scan_state,
                                                 optional_idx &batch_index, SourceBatchIndexState &batch_index_state,
                                                 const InterruptState &interrupt_state) {
	vector<InterruptState> writers;
	vector<InterruptState> readers;
	vector<ExchangeLogEntry> log_entries;
	shared_ptr<DataChunk> next_chunk;
	SpoolReadReservation spool_read;
	bool producer_finished;
	SourceResultType result;
	batch_index = optional_idx();
	batch_index_state = SourceBatchIndexState::UNCHANGED;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		result = ReserveScanLocked(consumer_idx, interrupt_state, scan_state, next_chunk, batch_index,
		                           batch_index_state, spool_read, readers, writers, log_entries);
		producer_finished = producer_state == ProducerState::FINISHED;
	}

	if (spool_read.IsSet()) {
		try {
			spool_read.spool->Read(spool_read.position, spool_read.entry, chunk, *spool_read.reader, producer_finished);
		} catch (...) {
			vector<InterruptState> failed_readers;
			vector<InterruptState> failed_writers;
			vector<InterruptState> appenders;
			{
				annotated_lock_guard<annotated_mutex> guard(lock);
				auto &consumer = consumers[consumer_idx];
				consumer.in_flight_reads.erase(spool_read.position);
				producer_state = ProducerState::CANCELLED;
				DeactivateAllConsumersLocked();
				TryReleaseBufferedStorageLocked();
				WakeReadersLocked(failed_readers);
				vector<ExchangeLogEntry> ignored_log_entries;
				WakeWritersLocked(failed_writers, ignored_log_entries, WriterWakeMode::FORCE);
				WakeAppendersLocked(appenders);
			}
			CallbackAll(failed_readers);
			CallbackAll(failed_writers);
			CallbackAll(appenders);
			buffer->EndExecution();
			throw;
		}
		{
			annotated_lock_guard<annotated_mutex> guard(lock);
			CompleteSpoolReadLocked(consumer_idx, spool_read, chunk, readers, writers, log_entries);
		}
	}

	CallbackAll(readers);
	CallbackAll(writers);
	LogTransitions(log_entries);
	if (next_chunk) {
		scan_state.current_chunk = std::move(next_chunk);
		chunk.Reference(*scan_state.current_chunk);
	}
	return result;
}

SourceResultType PipelineBroadcastExchange::ReserveScanLocked(
    idx_t consumer_idx, const InterruptState &interrupt_state, PipelineBroadcastExchangeScanState &scan_state,
    shared_ptr<DataChunk> &next_chunk, optional_idx &batch_index, SourceBatchIndexState &batch_index_state,
    SpoolReadReservation &spool_read, vector<InterruptState> &readers, vector<InterruptState> &writers,
    vector<ExchangeLogEntry> &log_entries) {
	D_ASSERT(consumer_idx < consumers.size());
	auto &consumer = consumers[consumer_idx];
	if (consumer.lifecycle != ConsumerLifecycle::ACTIVE || producer_state == ProducerState::CANCELLED) {
		return SourceResultType::FINISHED;
	}
	if (consumer.scan_mode == PipelineBroadcastExchangeScanMode::BATCH) {
		return ReserveBatchScanLocked(consumer_idx, interrupt_state, scan_state, next_chunk, batch_index,
		                              batch_index_state, spool_read, readers, writers, log_entries);
	}
	if (consumer.position < buffer->NextPosition()) {
		auto position = consumer.position++;
		buffer->ReserveRead(position, scan_state.spool_reader, next_chunk, batch_index, spool_read);
		if (spool_read.IsSet()) {
#ifdef D_ASSERT_IS_ENABLED
			auto inserted = consumer.in_flight_reads.insert(position);
			D_ASSERT(inserted.second);
#else
			consumer.in_flight_reads.insert(position);
#endif
		} else {
			consumer.rows_read += next_chunk->size();
			RetireChunksLocked();
			WakeWritersLocked(writers, log_entries);
		}
		return SourceResultType::HAVE_MORE_OUTPUT;
	}
	if (producer_state == ProducerState::FINISHED) {
		consumer.exhausted = true;
		if (consumer.in_flight_reads.empty()) {
			consumer.lifecycle = ConsumerLifecycle::INACTIVE;
			D_ASSERT(active_consumers > 0);
			active_consumers--;
		}
		RetireChunksLocked();
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		return SourceResultType::FINISHED;
	}
	WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
	blocked_readers.push_back(interrupt_state);
	return SourceResultType::BLOCKED;
}

SourceResultType PipelineBroadcastExchange::ReserveBatchScanLocked(
    idx_t consumer_idx, const InterruptState &interrupt_state, PipelineBroadcastExchangeScanState &scan_state,
    shared_ptr<DataChunk> &next_chunk, optional_idx &batch_index, SourceBatchIndexState &batch_index_state,
    SpoolReadReservation &spool_read, vector<InterruptState> &readers, vector<InterruptState> &writers,
    vector<ExchangeLogEntry> &log_entries) {
	auto &consumer = consumers[consumer_idx];
	while (true) {
		if (scan_state.batch_sequence.IsValid()) {
			auto batch_sequence = scan_state.batch_sequence.GetIndex();
			auto position_entry = consumer.active_batch_positions.find(batch_sequence);
			D_ASSERT(position_entry != consumer.active_batch_positions.end());
			auto &batch_range = buffer->GetBatchRange(batch_sequence);
			D_ASSERT(position_entry->second >= batch_range.begin_position);
			D_ASSERT(position_entry->second <= batch_range.end_position);
			if (position_entry->second < batch_range.end_position) {
				auto position = position_entry->second;
				buffer->ReserveRead(position, scan_state.spool_reader, next_chunk, batch_index, spool_read);
				D_ASSERT(batch_index.IsValid() && batch_index.GetIndex() == batch_range.batch_index);
				D_ASSERT(!scan_state.reported_batch_index.IsValid() ||
				         scan_state.reported_batch_index.GetIndex() <= batch_range.batch_index);
				scan_state.reported_batch_index = batch_range.batch_index;
				if (spool_read.IsSet()) {
					spool_read.batch_sequence = batch_sequence;
#ifdef D_ASSERT_IS_ENABLED
					auto inserted = consumer.in_flight_reads.insert(position);
					D_ASSERT(inserted.second);
#else
					consumer.in_flight_reads.insert(position);
#endif
				} else {
					position_entry->second++;
					consumer.rows_read += next_chunk->size();
					RetireChunksLocked();
					WakeWritersLocked(writers, log_entries);
				}
				return SourceResultType::HAVE_MORE_OUTPUT;
			}
			if (!batch_range.closed) {
				WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
				blocked_readers.push_back(interrupt_state);
				return SourceResultType::BLOCKED;
			}
			const bool has_next_batch = consumer.next_batch_sequence < buffer->NextBatchSequence();
			auto completed_batch_index = batch_range.batch_index;
			consumer.active_batch_positions.erase(position_entry);
			scan_state.batch_sequence = optional_idx();
			if (!has_next_batch) {
				if (producer_state == ProducerState::ACTIVE) {
					auto successor_batch_index = completed_batch_index + 1;
					auto batch_floor = GetConsumerBatchFloorLocked(consumer);
					if (batch_floor != DConstants::INVALID_INDEX) {
						successor_batch_index = MaxValue(successor_batch_index, batch_floor);
					}
					if (consumer.batch_index_floor == DConstants::INVALID_INDEX) {
						consumer.batch_index_floor = successor_batch_index;
					} else {
						consumer.batch_index_floor = MaxValue(consumer.batch_index_floor, successor_batch_index);
					}
					scan_state.reported_batch_index = successor_batch_index;
					batch_index = successor_batch_index;
					batch_index_state = SourceBatchIndexState::ADVANCED;
					RetireChunksLocked();
					WakeReadersLocked(readers);
					WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
					return SourceResultType::BLOCKED;
				}
				D_ASSERT(producer_state == ProducerState::FINISHED);
				consumer.exhausted = true;
				if (consumer.active_batch_positions.empty() && consumer.in_flight_reads.empty()) {
					consumer.lifecycle = ConsumerLifecycle::INACTIVE;
					D_ASSERT(active_consumers > 0);
					active_consumers--;
				}
				RetireChunksLocked();
				WakeReadersLocked(readers);
				WakeWritersLocked(writers, log_entries);
				return SourceResultType::FINISHED;
			}
			RetireChunksLocked();
			WakeReadersLocked(readers, ReaderWakeMode::DATA_AVAILABLE);
			WakeWritersLocked(writers, log_entries);
			continue;
		}

		if (consumer.next_batch_sequence < buffer->NextBatchSequence()) {
			auto batch_sequence = consumer.next_batch_sequence++;
			auto &batch_range = buffer->GetBatchRange(batch_sequence);
#ifdef D_ASSERT_IS_ENABLED
			auto inserted = consumer.active_batch_positions.emplace(batch_sequence, batch_range.begin_position);
			D_ASSERT(inserted.second);
#else
			consumer.active_batch_positions.emplace(batch_sequence, batch_range.begin_position);
#endif
			scan_state.batch_sequence = batch_sequence;
			continue;
		}

		if (producer_state == ProducerState::FINISHED) {
			consumer.exhausted = true;
			if (consumer.active_batch_positions.empty() && consumer.in_flight_reads.empty()) {
				consumer.lifecycle = ConsumerLifecycle::INACTIVE;
				D_ASSERT(active_consumers > 0);
				active_consumers--;
			}
			RetireChunksLocked();
			WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
			return SourceResultType::FINISHED;
		}
		auto batch_floor = GetConsumerBatchFloorLocked(consumer);
		if (batch_floor != DConstants::INVALID_INDEX &&
		    (!scan_state.reported_batch_index.IsValid() || scan_state.reported_batch_index.GetIndex() < batch_floor)) {
			scan_state.reported_batch_index = batch_floor;
			batch_index = batch_floor;
			batch_index_state = SourceBatchIndexState::ADVANCED;
			WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
			return SourceResultType::BLOCKED;
		}
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		blocked_batch_claim_readers.push_back(interrupt_state);
		return SourceResultType::BLOCKED;
	}
}

idx_t PipelineBroadcastExchange::GetConsumerBatchFloorLocked(const ConsumerState &consumer) const {
	idx_t min_batch_index = consumer.batch_index_floor;
	if (consumer.next_batch_sequence < buffer->NextBatchSequence()) {
		auto next_batch_index = buffer->GetBatchRange(consumer.next_batch_sequence).batch_index;
		min_batch_index = min_batch_index == DConstants::INVALID_INDEX ? next_batch_index
		                                                               : MaxValue(min_batch_index, next_batch_index);
	}
	idx_t active_min_batch_index = DConstants::INVALID_INDEX;
	for (auto &batch_position : consumer.active_batch_positions) {
		active_min_batch_index =
		    MinValue(active_min_batch_index, buffer->GetBatchRange(batch_position.first).batch_index);
	}
	if (active_min_batch_index != DConstants::INVALID_INDEX) {
		min_batch_index = min_batch_index == DConstants::INVALID_INDEX
		                      ? active_min_batch_index
		                      : MaxValue(min_batch_index, active_min_batch_index);
	}
	return min_batch_index;
}

void PipelineBroadcastExchange::CompleteSpoolReadLocked(idx_t consumer_idx, const SpoolReadReservation &spool_read,
                                                        DataChunk &chunk, vector<InterruptState> &readers,
                                                        vector<InterruptState> &writers,
                                                        vector<ExchangeLogEntry> &log_entries) {
	D_ASSERT(consumer_idx < consumers.size());
	auto &consumer = consumers[consumer_idx];
	auto entry = consumer.in_flight_reads.find(spool_read.position);
	D_ASSERT(entry != consumer.in_flight_reads.end());
	consumer.in_flight_reads.erase(entry);
	if (consumer.lifecycle != ConsumerLifecycle::ACTIVE || producer_state == ProducerState::CANCELLED) {
		chunk.Reset();
		RetireChunksLocked();
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		WakeReadersLocked(readers);
		return;
	}
	if (spool_read.batch_sequence.IsValid()) {
		auto position_entry = consumer.active_batch_positions.find(spool_read.batch_sequence.GetIndex());
		D_ASSERT(position_entry != consumer.active_batch_positions.end());
		D_ASSERT(position_entry->second == spool_read.position);
		position_entry->second++;
	}
	consumer.rows_read += chunk.size();
	if (consumer.exhausted && consumer.in_flight_reads.empty() && consumer.active_batch_positions.empty()) {
		consumer.lifecycle = ConsumerLifecycle::INACTIVE;
		D_ASSERT(active_consumers > 0);
		active_consumers--;
	}
	RetireChunksLocked();
	WakeWritersLocked(writers, log_entries);
	WakeReadersLocked(readers, ReaderWakeMode::DATA_AVAILABLE);
}

void PipelineBroadcastExchange::UnregisterConsumer(idx_t consumer_idx) {
	vector<InterruptState> readers;
	vector<InterruptState> writers;
	vector<ExchangeLogEntry> log_entries;
	{
		annotated_lock_guard<annotated_mutex> guard(lock);
		if (consumer_idx >= consumers.size()) {
			return;
		}
		auto &consumer = consumers[consumer_idx];
		if (consumer.lifecycle != ConsumerLifecycle::ACTIVE) {
			return;
		}
		if (consumer.exhausted) {
			return;
		}
		consumer.lifecycle = ConsumerLifecycle::INACTIVE;
		consumer.position = buffer->NextPosition();
		consumer.next_batch_sequence = buffer->NextBatchSequence();
		consumer.active_batch_positions.clear();
		D_ASSERT(active_consumers > 0);
		active_consumers--;
		RetireChunksLocked();
		WakeReadersLocked(readers);
		WakeWritersLocked(writers, log_entries, WriterWakeMode::FORCE);
		log_entries.push_back({ExchangeLogEvent::CONSUMER_UNREGISTERED, active_consumers, buffer->BufferedCount()});
	}
	CallbackAll(readers);
	CallbackAll(writers);
	LogTransitions(log_entries);
}

ProgressData PipelineBroadcastExchange::ScanProgress(idx_t consumer_idx, idx_t estimated_cardinality) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	ProgressData progress;
	if (consumer_idx >= consumers.size()) {
		progress.SetInvalid();
		return progress;
	}
	auto total = produced_rows.load(std::memory_order_relaxed);
	if (producer_state == ProducerState::ACTIVE) {
		static constexpr const idx_t MAX_PROGRESS_CARDINALITY = 1ULL << 48ULL;
		if (estimated_cardinality > 0 && estimated_cardinality < MAX_PROGRESS_CARDINALITY) {
			total = MaxValue<idx_t>(total, estimated_cardinality);
		} else {
			total = MaxValue<idx_t>(total, consumers[consumer_idx].rows_read + 1);
		}
	}
	total = MaxValue<idx_t>(total, 1);
	progress.done = MinValue<double>(double(consumers[consumer_idx].rows_read), double(total));
	progress.total = double(total);
	return progress;
}

ProgressData PipelineBroadcastExchange::SinkProgress(const ProgressData &source_progress,
                                                     idx_t estimated_cardinality) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	ProgressData progress;
	if (producer_state != ProducerState::ACTIVE) {
		progress.done = 1.0;
		progress.total = 1.0;
		return progress;
	}
	if (source_progress.IsValid()) {
		// the producing pipeline is done once its source is exhausted
		return source_progress;
	}
	auto produced_count = produced_rows.load(std::memory_order_relaxed);
	auto produced = double(produced_count);
	progress.done = produced;
	static constexpr const idx_t MAX_PROGRESS_CARDINALITY = 1ULL << 48ULL;
	if (estimated_cardinality > 0 && estimated_cardinality < MAX_PROGRESS_CARDINALITY) {
		progress.total = double(MaxValue<idx_t>(estimated_cardinality, produced_count + 1));
	} else {
		progress.total = produced + 1.0;
	}
	if (progress.done > progress.total) {
		progress.total = progress.done;
	}
	return progress;
}

idx_t PipelineBroadcastExchange::MaxThreads() const {
	return order_mode == PipelineBroadcastExchangeOrderMode::SEQUENTIAL ? 1 : MaxValue<idx_t>(max_threads, 1);
}

idx_t PipelineBroadcastExchange::RegisteredConsumerCount() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return consumers.size();
}

PipelineBroadcastExchangeConsumerMode PipelineBroadcastExchange::GetConsumerMode(idx_t consumer_idx) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(consumer_idx < consumers.size());
	return consumers[consumer_idx].mode;
}

PipelineBroadcastExchangeScanMode PipelineBroadcastExchange::GetConsumerScanMode(idx_t consumer_idx) const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	D_ASSERT(consumer_idx < consumers.size());
	return consumers[consumer_idx].scan_mode;
}

PipelineBroadcastExchangeConsumerSummary PipelineBroadcastExchange::GetConsumerSummaryLocked() const {
	PipelineBroadcastExchangeConsumerSummary result;
	for (auto &consumer : consumers) {
		switch (consumer.mode) {
		case PipelineBroadcastExchangeConsumerMode::UNRESOLVED:
			result.unresolved++;
			break;
		case PipelineBroadcastExchangeConsumerMode::BUFFERED:
			result.buffered++;
			break;
		case PipelineBroadcastExchangeConsumerMode::DIRECT:
			result.direct++;
			break;
		case PipelineBroadcastExchangeConsumerMode::MATERIALIZED:
			result.materialized++;
			break;
		default:
			throw InternalException("Unknown pipeline broadcast exchange consumer mode");
		}
	}
	return result;
}

PipelineBroadcastExchangeConsumerSummary PipelineBroadcastExchange::GetConsumerSummary() const {
	annotated_lock_guard<annotated_mutex> guard(lock);
	return GetConsumerSummaryLocked();
}

bool PipelineBroadcastExchange::ShouldStopProducerLocked() const {
	return completion_mode == PipelineBroadcastExchangeCompletionMode::STOP_WHEN_UNCONSUMED && active_consumers == 0;
}

bool PipelineBroadcastExchange::ShouldThrottleProducerLocked() const {
	if (buffer_mode == PipelineBroadcastExchangeBufferMode::BUFFER_ALL) {
		// the rows are buffered until the consumer runs - the producer is never blocked
		return false;
	}
	if (buffer->HasSharedSpool()) {
		return false;
	}
	return active_consumers > 0 && buffer->BufferedCount() >= PIPELINE_BROADCAST_HIGH_WATERMARK_CHUNKS;
}

bool PipelineBroadcastExchange::ShouldCreateSharedSpoolLocked() const {
	if (buffer->HasSharedSpool() || buffer->BufferedCount() < PIPELINE_BROADCAST_HIGH_WATERMARK_CHUNKS) {
		return false;
	}
	// the spool holds the rows outside of the in-memory buffer, spilling to disk if required
	return buffer_mode == PipelineBroadcastExchangeBufferMode::BUFFER_ALL || !direct_pipelines.empty() ||
	       active_consumers > 1;
}

void PipelineBroadcastExchange::CreateSharedSpoolLocked(vector<ExchangeLogEntry> &log_entries) {
	buffer->CreateSharedSpool();
	log_entries.push_back({ExchangeLogEvent::SPOOL_CREATED, active_consumers, buffer->BufferedCount()});
}

void PipelineBroadcastExchange::RetireChunksLocked() {
	if (buffer->Empty()) {
		return;
	}
	idx_t min_position = buffer->NextPosition();
	idx_t min_batch_sequence = buffer->NextBatchSequence();
	bool found_reader = false;
	for (auto &consumer : consumers) {
		if (!consumer.in_flight_reads.empty()) {
			found_reader = true;
			min_position = MinValue(min_position, *consumer.in_flight_reads.begin());
		}
		if (consumer.lifecycle == ConsumerLifecycle::ACTIVE) {
			found_reader = true;
			if (consumer.scan_mode == PipelineBroadcastExchangeScanMode::BATCH) {
				D_ASSERT(consumer.next_batch_sequence >= buffer->BaseBatchSequence());
				min_batch_sequence = MinValue(min_batch_sequence, consumer.next_batch_sequence);
				if (consumer.next_batch_sequence < buffer->NextBatchSequence()) {
					auto &next_batch = buffer->GetBatchRange(consumer.next_batch_sequence);
					min_position = MinValue(min_position, next_batch.begin_position);
				}
				for (auto &batch_position : consumer.active_batch_positions) {
					min_batch_sequence = MinValue(min_batch_sequence, batch_position.first);
					min_position = MinValue(min_position, batch_position.second);
				}
			} else {
				min_position = MinValue(min_position, consumer.position);
			}
		}
	}
	if (!found_reader) {
		min_position = buffer->NextPosition();
	}
	buffer->RetireBefore(min_position);
	buffer->RetireBatchRangesBefore(min_batch_sequence);
	TryReleaseBufferedStorageLocked();
}

void PipelineBroadcastExchange::TryReleaseBufferedStorageLocked() {
	if (active_consumers > 0 || append_reservation_state == AppendReservationState::RESERVED) {
		return;
	}
	for (auto &consumer : consumers) {
		if (!consumer.in_flight_reads.empty()) {
			return;
		}
	}
	buffer->Release();
}

void PipelineBroadcastExchange::DeactivateAllConsumersLocked() {
	for (auto &consumer : consumers) {
		if (consumer.lifecycle != ConsumerLifecycle::ACTIVE) {
			continue;
		}
		consumer.lifecycle = ConsumerLifecycle::INACTIVE;
		consumer.position = buffer->NextPosition();
		consumer.next_batch_sequence = buffer->NextBatchSequence();
		consumer.active_batch_positions.clear();
	}
	active_consumers = 0;
}

void PipelineBroadcastExchange::WakeReadersLocked(vector<InterruptState> &readers, ReaderWakeMode mode) {
	readers.insert(readers.end(), blocked_readers.begin(), blocked_readers.end());
	blocked_readers.clear();
	if (mode == ReaderWakeMode::ALL) {
		readers.insert(readers.end(), blocked_batch_claim_readers.begin(), blocked_batch_claim_readers.end());
		blocked_batch_claim_readers.clear();
	}
}

void PipelineBroadcastExchange::WakeWritersLocked(vector<InterruptState> &writers,
                                                  vector<ExchangeLogEntry> &log_entries, WriterWakeMode mode) {
	if (mode == WriterWakeMode::LOW_WATERMARK && buffer->BufferedCount() > PIPELINE_BROADCAST_LOW_WATERMARK_CHUNKS &&
	    producer_state == ProducerState::ACTIVE && !ShouldStopProducerLocked()) {
		return;
	}
	if (mode == WriterWakeMode::LOW_WATERMARK && watermark_state == WatermarkState::ABOVE_HIGH_WATERMARK) {
		watermark_state = WatermarkState::BELOW_HIGH_WATERMARK;
		log_entries.push_back({ExchangeLogEvent::LOW_WATERMARK_WAKE, active_consumers, buffer->BufferedCount()});
	}
	writers.insert(writers.end(), blocked_writers.begin(), blocked_writers.end());
	blocked_writers.clear();
}

void PipelineBroadcastExchange::WakeAppendersLocked(vector<InterruptState> &appenders) {
	appenders.insert(appenders.end(), blocked_appenders.begin(), blocked_appenders.end());
	blocked_appenders.clear();
}

void PipelineBroadcastExchange::LogTransitions(const vector<ExchangeLogEntry> &log_entries) const {
	if (!log_operator) {
		return;
	}
	for (auto &entry : log_entries) {
		const char *event;
		switch (entry.event) {
		case ExchangeLogEvent::SPOOL_CREATED:
			event = "SpoolCreated";
			break;
		case ExchangeLogEvent::HIGH_WATERMARK_BLOCKED:
			event = "HighWatermarkBlocked";
			break;
		case ExchangeLogEvent::LOW_WATERMARK_WAKE:
			event = "LowWatermarkWake";
			break;
		case ExchangeLogEvent::CONSUMER_UNREGISTERED:
			event = "ConsumerUnregistered";
			break;
		default:
			throw InternalException("Unknown pipeline broadcast exchange log event");
		}
		DUCKDB_LOG(context, PhysicalOperatorLogType, *log_operator, "PipelineBroadcastExchange", event,
		           {{"active_consumers", to_string(entry.consumer_count)},
		            {"buffered_chunks", to_string(entry.buffered_chunks)}});
	}
}

void PipelineBroadcastExchange::CallbackAll(vector<InterruptState> &interrupts) {
	for (auto &interrupt : interrupts) {
		interrupt.Callback();
	}
}

} // namespace duckdb
