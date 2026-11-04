#include "duckdb/execution/physical_operator.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/function/table_function.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/render_tree.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/execution/execution_context.hpp"
#include "duckdb/execution/operator/set/physical_recursive_cte.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/meta_pipeline.hpp"
#include "duckdb/parallel/pipeline.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/storage/buffer/buffer_pool.hpp"
#include "duckdb/storage/buffer_manager.hpp"

namespace duckdb {

PhysicalOperator::PhysicalOperator(PhysicalPlan &physical_plan, PhysicalOperatorType type, vector<LogicalType> types,
                                   idx_t estimated_cardinality)
    : children(physical_plan.ArenaRef()), type(type), types(std::move(types)),
      estimated_cardinality(estimated_cardinality) {
}

string PhysicalOperator::GetName() const {
	return PhysicalOperatorToString(type);
}

string PhysicalOperator::ToString(optional_ptr<ClientContext> context, const ProfilerPrintFormat &format) const {
	auto renderer = context ? TreeRenderer::CreateRenderer(*context, format) : TreeRenderer::CreateRenderer(format);
	if (!renderer) {
		// formats without output (e.g. "no_output") render nothing
		return string();
	}
	StringTreeRenderer ss;
	auto tree = RenderTree::CreateRenderTree(*this);
	renderer->ToStream(*tree, ss);
	return ss.str();
}

// LCOV_EXCL_START
void PhysicalOperator::Print() const {
	Printer::Print(ToString());
}
// LCOV_EXCL_STOP

vector<const_reference<PhysicalOperator>> PhysicalOperator::GetChildren() const {
	vector<const_reference<PhysicalOperator>> result;
	for (auto &child : children) {
		result.push_back(child.get());
	}
	return result;
}

void PhysicalOperator::SetEstimatedCardinality(InsertionOrderPreservingMap<string> &result,
                                               idx_t estimated_cardinality) {
	result[RenderTreeNode::ESTIMATED_CARDINALITY] = StringUtil::Format("%llu", estimated_cardinality);
}

idx_t PhysicalOperator::EstimatedThreadCount() const {
	idx_t result = 0;
	if (children.empty()) {
		// Terminal operator, e.g., base table, these decide the degree of parallelism of pipelines
		result = MaxValue<idx_t>(estimated_cardinality / (DEFAULT_ROW_GROUP_SIZE * 2), 1);
	} else if (type == PhysicalOperatorType::UNION) {
		// We can run union pipelines in parallel, so we sum up the thread count of the children
		for (auto &child : children) {
			result += child.get().EstimatedThreadCount();
		}
	} else {
		// For other operators we take the maximum of the children
		for (auto &child : children) {
			result = MaxValue(child.get().EstimatedThreadCount(), result);
		}
	}
	return result;
}

bool PhysicalOperator::CanSaturateThreads(ClientContext &context) const {
#ifdef DEBUG
	// In debug mode we always return true here so that the code that depends on it is well-tested
	return true;
#else
	const auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	return EstimatedThreadCount() >= num_threads;
#endif
}

//===--------------------------------------------------------------------===//
// Operator
//===--------------------------------------------------------------------===//
// LCOV_EXCL_START
unique_ptr<OperatorState> PhysicalOperator::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<OperatorState>();
}

unique_ptr<GlobalOperatorState> PhysicalOperator::GetGlobalOperatorState(ClientContext &context) const {
	return make_uniq<GlobalOperatorState>();
}

bool PhysicalOperator::ResetGlobalOperatorState(ClientContext &context, GlobalOperatorState &state) const {
	return false;
}

OperatorResultType PhysicalOperator::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                             GlobalOperatorState &gstate, OperatorState &state) const {
	throw InternalException("Calling Execute on a node that is not an operator!");
}

OperatorFinalizeResultType PhysicalOperator::FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                                          GlobalOperatorState &gstate, OperatorState &state) const {
	throw InternalException("Calling FinalExecute on a node that is not an operator!");
}

OperatorFinalResultType PhysicalOperator::OperatorFinalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                                           OperatorFinalizeInput &input) const {
	throw InternalException("Calling FinalExecute on a node that is not an operator!");
}
// LCOV_EXCL_STOP

//===--------------------------------------------------------------------===//
// Source
//===--------------------------------------------------------------------===//
unique_ptr<LocalSourceState> PhysicalOperator::GetLocalSourceState(ExecutionContext &context,
                                                                   GlobalSourceState &gstate) const {
	return make_uniq<LocalSourceState>();
}

unique_ptr<GlobalSourceState> PhysicalOperator::GetGlobalSourceState(ClientContext &context) const {
	return make_uniq<GlobalSourceState>();
}

unique_ptr<GlobalSourceState>
PhysicalOperator::GetGlobalSourceState(ClientContext &context, const OperatorPartitionInfo &partition_info) const {
	(void)partition_info;
	return GetGlobalSourceState(context);
}

// LCOV_EXCL_START
SourceResultType PhysicalOperator::GetData(ExecutionContext &context, DataChunk &chunk,
                                           OperatorSourceInput &input) const {
	return GetDataInternal(context, chunk, input);
}

SourceResultType PhysicalOperator::GetDataInternal(ExecutionContext &context, DataChunk &chunk,
                                                   OperatorSourceInput &input) const {
	throw InternalException("Calling GetDataInternal on a node that is not a source!");
}

OperatorPartitionData PhysicalOperator::GetPartitionData(ExecutionContext &context, DataChunk &chunk,
                                                         GlobalSourceState &gstate, LocalSourceState &lstate,
                                                         const OperatorPartitionInfo &partition_info) const {
	throw InternalException("Calling GetPartitionData on a node that does not support it");
}

TableFunctionParallelism PhysicalOperator::SourceParallelism() const {
	return TableFunctionParallelism::SELF_MANAGED_PARALLELISM;
}

ProgressData PhysicalOperator::GetProgress(ClientContext &context, GlobalSourceState &gstate) const {
	ProgressData res;
	res.SetInvalid();
	return res;
}

void PhysicalOperator::SourceFinished(ClientContext &context, GlobalSourceState &gstate) const {
}
// LCOV_EXCL_STOP

//===--------------------------------------------------------------------===//
// Sink
//===--------------------------------------------------------------------===//
// LCOV_EXCL_START
SinkResultType PhysicalOperator::Sink(ExecutionContext &context, DataChunk &chunk, OperatorSinkInput &input) const {
	throw InternalException("Calling Sink on a node that is not a sink!");
}

// LCOV_EXCL_STOP

SinkCombineResultType PhysicalOperator::Combine(ExecutionContext &context, OperatorSinkCombineInput &input) const {
	return SinkCombineResultType::FINISHED;
}

void PhysicalOperator::PrepareFinalize(ClientContext &context, GlobalSinkState &sink_state) const {
}

SinkFinalizeType PhysicalOperator::Finalize(Pipeline &pipeline, Event &event, ClientContext &context,
                                            OperatorSinkFinalizeInput &input) const {
	return SinkFinalizeType::READY;
}

SinkNextBatchType PhysicalOperator::NextBatch(ExecutionContext &context, OperatorSinkNextBatchInput &input) const {
	return SinkNextBatchType::READY;
}

SinkNextBatchType PhysicalOperator::UpdateMinBatchIndex(ExecutionContext &context,
                                                        OperatorSinkNextBatchInput &input) const {
	return SinkNextBatchType::READY;
}

unique_ptr<LocalSinkState> PhysicalOperator::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<LocalSinkState>();
}

unique_ptr<GlobalSinkState> PhysicalOperator::GetGlobalSinkState(ClientContext &context) const {
	return make_uniq<GlobalSinkState>();
}

idx_t PhysicalOperator::GetMaxThreadMemory(ClientContext &context) {
	// Memory usage per thread should scale with max mem / num threads
	// We take 1/4th of this, to be conservative
	auto max_memory = BufferManager::GetBufferManager(context).GetOperatorMemoryLimit();
	auto num_threads = TaskScheduler::GetScheduler(context).NumberOfThreads();
	return (max_memory / num_threads) / 4;
}

OperatorCachingMode PhysicalOperator::SelectOperatorCachingMode(ExecutionContext &context) {
	if (!Settings::Get<EnableCachingOperatorsSetting>(context.client)) {
		return OperatorCachingMode::NONE;
	} else if (!context.pipeline) {
		return OperatorCachingMode::NONE;
	} else if (context.pipeline->CanStopSourceEarly()) {
		return OperatorCachingMode::NONE;
	} else if (!context.pipeline->GetSink()) {
		return OperatorCachingMode::NONE;
	} else {
		auto partition_info = context.pipeline->GetSink()->RequiredPartitionInfo();
		if (partition_info.AnyRequired()) {
			return OperatorCachingMode::PARTITIONED;
		}
	}
	if (context.pipeline->IsOrderDependent()) {
		return OperatorCachingMode::ORDERED;
	}

	return OperatorCachingMode::UNORDERED;
}

//===--------------------------------------------------------------------===//
// Pipeline Construction
//===--------------------------------------------------------------------===//
void PhysicalOperator::BuildPipelines(Pipeline &current, MetaPipeline &meta_pipeline) {
	op_state.reset();

	auto &state = meta_pipeline.GetState();
	if (!IsSink() && children.empty()) {
		// Operator is a source.
		state.SetPipelineSource(current, *this);
		return;
	}

	if (children.size() != 1) {
		throw InternalException("Operator not supported in BuildPipelines");
	}

	if (IsSink()) {
		// Operator is a sink.
		sink_state.reset();

		// It becomes the data source of the current pipeline.
		state.SetPipelineSource(current, *this);

		// Create a new pipeline starting at the child.
		auto &child_meta_pipeline = meta_pipeline.CreateChildMetaPipeline(current, *this);
		child_meta_pipeline.Build(children[0].get());
		return;
	}

	// Recurse into the child.
	state.AddPipelineOperator(current, *this);
	children[0].get().BuildPipelines(current, meta_pipeline);
}

vector<const_reference<PhysicalOperator>> PhysicalOperator::GetSources() const {
	vector<const_reference<PhysicalOperator>> result;
	if (!IsSink() && children.empty()) {
		// Operator is a source.
		result.push_back(*this);
		return result;
	}

	if (children.size() != 1) {
		throw InternalException("Operator not supported in GetSource");
	}

	if (IsSink()) {
		result.push_back(*this);
		return result;
	}

	// Recurse into the child.
	return children[0].get().GetSources();
}

bool PhysicalOperator::AllSourcesSupportBatchIndex() const {
	auto sources = GetSources();
	for (auto &source : sources) {
		if (!source.get().SupportsPartitioning(OperatorPartitionInfo::BatchIndex())) {
			return false;
		}
	}
	return true;
}

void PhysicalOperator::Verify() {
#ifdef DEBUG
	auto sources = GetSources();
	D_ASSERT(!sources.empty());
	for (auto &child : children) {
		child.get().Verify();
	}
#endif
}

bool CachingPhysicalOperator::CanCacheType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
	case LogicalTypeId::ARRAY:
	case LogicalTypeId::VARIANT:
		return false;
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto &entries = StructType::GetChildTypes(type);
		for (auto &entry : entries) {
			if (!CanCacheType(entry.second)) {
				return false;
			}
		}
		return true;
	}
	default:
		return true;
	}
}

CachingPhysicalOperator::CachingPhysicalOperator(PhysicalPlan &physical_plan, PhysicalOperatorType type,
                                                 vector<LogicalType> types_p, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, type, std::move(types_p), estimated_cardinality) {
	caching_supported = true;
	for (auto &col_type : types) {
		if (!CanCacheType(col_type)) {
			caching_supported = false;
			break;
		}
	}
}

enum class CachingPhysicalOperatorExecuteMode : uint8_t {
	RETURN_CACHED_APPEND_CHUNK,
	RETURN_CACHED_PLUS_CHUNK,
	RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION,
	RETURN_CHUNK,
	APPEND_CHUNK,
	RETURN_CACHED
};

static CachingPhysicalOperatorExecuteMode
SelectExecutionMode(const DataChunk &chunk, const OperatorResultType child_result, CachingOperatorState &state) {
	if (state.can_cache_chunk == OperatorCachingMode::NONE) {
		return CachingPhysicalOperatorExecuteMode::RETURN_CHUNK;
	}
	const bool needs_continuation_chunk = (state.can_cache_chunk == OperatorCachingMode::PARTITIONED &&
	                                       child_result != OperatorResultType::HAVE_MORE_OUTPUT) ||
	                                      (child_result == OperatorResultType::FINISHED);
	const bool has_non_empty_cached_chunk = state.cached_chunk && state.cached_chunk->size() > 0;
	const bool has_space_for_chunk_in_cache =
	    !state.cached_chunk || (state.cached_chunk->size() + chunk.size() <= STANDARD_VECTOR_SIZE);

	if (has_non_empty_cached_chunk && needs_continuation_chunk) {
		if (chunk.size() == 0) {
			if (child_result == OperatorResultType::BLOCKED) {
				// First return cached, then empty chunk via continuation that will BLOCK
				return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION;
			}

			// Return cached, and the current result
			return CachingPhysicalOperatorExecuteMode::RETURN_CACHED;
		}
		if (chunk.size() <= CachingPhysicalOperator::CACHE_THRESHOLD && has_space_for_chunk_in_cache) {
			// chunk is small, both fit
			return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_PLUS_CHUNK;
		}

		// First return cached, then chunk via continuation
		return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION;
	} else if (chunk.size() == 0) {
		// Nothing required to be done, this also means that BLOCKED is properly passed through
		// Note that this case works also for unordered cases, given no rows are there

		return CachingPhysicalOperatorExecuteMode::RETURN_CHUNK;
	} else if (chunk.size() <= CachingPhysicalOperator::CACHE_THRESHOLD && !needs_continuation_chunk) {
		// We have filtered out a significant amount of tuples
		// The cache is materialised lazily by AppendToCache on first use

		if (has_space_for_chunk_in_cache) {
			// We can just append, do and return empty chunk
			return CachingPhysicalOperatorExecuteMode::APPEND_CHUNK;
		}

		// Return what is now cached, and append chunk (via tmp)
		return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_APPEND_CHUNK;
	} else if (state.can_cache_chunk == OperatorCachingMode::UNORDERED) {
		// Chunk is too big to considering caching, order is not required, just return it
		return CachingPhysicalOperatorExecuteMode::RETURN_CHUNK;
	} else if (has_non_empty_cached_chunk) {
		// We need first to return (*state.cached_chunk), then chunk on the continuation
		// NOTE: Both are not empty
		D_ASSERT(chunk.size() > 0);
		D_ASSERT(state.cached_chunk->size() > 0);

		if (chunk.size() <= CachingPhysicalOperator::CACHE_THRESHOLD) {
			// We can consider appending
			if (chunk.size() + state.cached_chunk->size() <= STANDARD_VECTOR_SIZE) {
				// Both fit together, append then return
				return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_PLUS_CHUNK;
			}
			if (needs_continuation_chunk) {
				// Both needs to be returned in this step, but cached before current chunk
				return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION;
			}

			// Return now cached, and append chunk (via tmp)
			return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_APPEND_CHUNK;
		}

		// Both needs to be returned in this step, but cached before current chunk
		return CachingPhysicalOperatorExecuteMode::RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION;
	}
	return CachingPhysicalOperatorExecuteMode::RETURN_CHUNK;
}

//! Only global dictionaries may be accumulated: the cache holds the entry by reference across child Execute calls and
//! materializes its strings lazily (on rotation/flush), so the child data must outlive the producing chunk. A global
//! dictionary is wrapped by the producer for its whole lifetime; a non-global (per-chunk) dict points into storage that
//! is recycled once its chunk is done, so accumulating it by reference would read freed data - fall back to flat
//! caching. Empty-id dicts (e.g. slice-of-flat) mint a fresh entry per slice and are never global.
static inline bool IsAccumulableDictionary(const Vector &vector) {
	return vector.GetVectorType() == VectorType::DICTIONARY_VECTOR && !DictionaryVector::DictionaryId(vector).empty() &&
	       DictionaryVector::IsGlobalDictionary(vector);
}

static bool ChunkHasAccumulableDictionary(const DataChunk &chunk) {
	for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
		if (IsAccumulableDictionary(chunk.data[col_idx])) {
			return true;
		}
	}
	return false;
}

//! Switch the empty cache to dictionary mode: pin each accumulable dictionary column's upstream entry and allocate
//! its sel accumulator. Columns keep a real (resettable) cache so a flushed dict column flattens on the next Reset.
static void SeedDictCache(CachingOperatorState &state, DataChunk &source) {
	const idx_t col_count = source.ColumnCount();
	state.dict_columns.clear();
	state.dict_columns.resize(col_count);
	for (idx_t col_idx = 0; col_idx < col_count; col_idx++) {
		if (!IsAccumulableDictionary(source.data[col_idx])) {
			continue;
		}
		auto &slot = state.dict_columns[col_idx];
		slot.entry = source.data[col_idx].BufferMutable().Cast<DictionaryBuffer>().GetEntryPtr();
		slot.accumulated_sel.Initialize(STANDARD_VECTOR_SIZE);
	}
	state.dict_cache_active = true;
}

//! On identity change, demote just this column to flat (materializing the rows so far) instead of flushing the
//! whole cache, so sibling columns keep accumulating and the chunk still coalesces.
static void FlattenDictColumn(CachingOperatorState &state, DataChunk &cache, idx_t col_idx, idx_t base) {
	auto &slot = state.dict_columns[col_idx];
	D_ASSERT(slot.entry);
	FlatVector::SetSize(cache.data[col_idx], 0);
	if (base > 0) {
		// materialize into the cache's own slot so the possibly-shared upstream child is never flattened in place
		cache.data[col_idx].Append(slot.entry->data, slot.accumulated_sel, base, VectorAppendMode::ERROR_ON_NO_SPACE);
	}
	slot.entry = nullptr;
}

//! Append source into a lazily created cache, retaining its largest capacity across flushes.
//! Accumulable dictionary columns concatenate selection indices instead of flattening.
static void AppendToCache(CachingOperatorState &state, DataChunk &source, ClientContext &client_context) {
	if (!state.cached_chunk) {
		// dictionary columns accumulate selection indices in place and cannot grow, so they need the full capacity
		const idx_t initial_capacity =
		    ChunkHasAccumulableDictionary(source) ? STANDARD_VECTOR_SIZE : CachingPhysicalOperator::CACHE_THRESHOLD;
		state.cached_chunk_capacity = MaxValue(state.cached_chunk_capacity, initial_capacity);
		state.cached_chunk = make_uniq<DataChunk>();
		state.cached_chunk->Initialize(Allocator::Get(client_context), source.GetTypes(), state.cached_chunk_capacity);
	}
	auto &cache = *state.cached_chunk;
	if (cache.size() == 0 && !state.dict_cache_active && ChunkHasAccumulableDictionary(source)) {
		SeedDictCache(state, source);
	}
	if (!state.dict_cache_active) {
		// no dict columns: plain flat append
		cache.Append(source, VectorAppendMode::ALLOW_RESIZE);
		if (cache.size() > state.cached_chunk_capacity) {
			state.cached_chunk_capacity = VectorBuffer::GetReserveSize(cache.size());
		}
		return;
	}
	const idx_t base = cache.size();
	const idx_t added = source.size();
	// accumulated_sel is sized STANDARD_VECTOR_SIZE and the caching state machine guarantees base + added stays
	// within it. Index accumulation has no overrun guard of its own (unlike the flat Append), so assert it.
	D_ASSERT(base + added <= STANDARD_VECTOR_SIZE);
	for (idx_t col_idx = 0; col_idx < cache.ColumnCount(); col_idx++) {
		auto &slot = state.dict_columns[col_idx];
		if (slot.entry) {
			auto &source_col = source.data[col_idx];
			// Compare the pinned entry by pointer: cheaper than the string id and exact, since pinning keeps the
			// address stable (no ABA). The type check must come first to guard the Cast against a flat vector.
			bool same_dict = false;
			if (source_col.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
				auto &source_entry = source_col.Buffer().Cast<DictionaryBuffer>().GetEntry();
				// Reinterpret (e.g. a type-changing function) re-wraps a global dictionary in a new entry per chunk
				// that keeps its id, so a global dictionary is also matched by id
				same_dict = &source_entry == slot.entry.get() ||
				            (slot.entry->global_dictionary && source_entry.global_dictionary &&
				             !slot.entry->id.empty() && source_entry.id == slot.entry->id);
			}
			if (same_dict) {
				const auto &source_sel = DictionaryVector::SelVector(source_col);
				for (idx_t row = 0; row < added; row++) {
					slot.accumulated_sel.set_index(base + row, source_sel.get_index(row));
				}
				continue;
			}
			// A global dictionary wraps the same entry for the producer's lifetime, so a change is a producer bug
			// that would desync the downstream sink layout -> fail loudly. A non-global dict rotating is normal.
			if (slot.entry->global_dictionary) {
				throw InternalException("dict-surviving cache: global dictionary column %llu changed identity "
				                        "after being seeded for dictionary caching",
				                        static_cast<uint64_t>(col_idx));
			}
			FlattenDictColumn(state, cache, col_idx, base);
		}
		// reached by originally-flat and just-demoted columns
		D_ASSERT(!slot.entry);
		FlatVector::SetSize(cache.data[col_idx], base);
		cache.data[col_idx].Append(source.data[col_idx], added, VectorAppendMode::ALLOW_RESIZE);
	}
	// dict columns are rewrapped on flush, flat columns already sized; this only sets the cardinality
	cache.SetChildCardinality(base + added);
}

//! After moving the cache into chunk, re-wrap each dict column as a DICTIONARY_VECTOR over the
//! pinned upstream entry, carrying its id and global_dictionary flag through unchanged.
static void RewrapDictColumns(CachingOperatorState &state, DataChunk &chunk, idx_t count) {
	for (idx_t col_idx = 0; col_idx < chunk.ColumnCount(); col_idx++) {
		auto &slot = state.dict_columns[col_idx];
		if (!slot.entry) {
			continue;
		}
		chunk.data[col_idx].Dictionary(slot.entry, slot.accumulated_sel, count);
	}
}

//! Hand the cached rows to chunk (reconstructing dict columns) and drop the cache; the next append creates a new
//! one. Referencing keeps the output chunk's own vectors, so its buffer layout is preserved.
static void FlushCacheToChunk(CachingOperatorState &state, DataChunk &chunk) {
	const idx_t count = state.cached_chunk->size();
	if (chunk.ColumnCount() == 0) {
		// the output chunk was moved out to hold a continuation chunk: take over the cache instead
		chunk.Move(*state.cached_chunk);
	} else {
		chunk.Reference(*state.cached_chunk);
	}
	state.cached_chunk.reset();
	if (!state.dict_cache_active) {
		return;
	}
	RewrapDictColumns(state, chunk, count);
	state.ResetDictCache();
}

OperatorResultType CachingPhysicalOperator::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                    GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<CachingOperatorState>();

	if (state.initialized && state.must_return_continuation_chunk) {
		chunk.Move(*state.cached_chunk);
		state.cached_chunk.reset();
		if (state.cached_result == OperatorResultType::BLOCKED && chunk.size() > 0) {
			// In case of BLOCKED, first the chunk + HAVE_MORE_OUTPUT, then blocking
			// This should currently be forbidden, so the assertion, but HAVE_MORE_OUTPUT is also a valid solution
			D_ASSERT(false);
			return OperatorResultType::HAVE_MORE_OUTPUT;
		}
		state.must_return_continuation_chunk = false;
		return state.cached_result;
	}

	// Execute child operator
	auto child_result = ExecuteInternal(context, input, chunk, gstate, state);

	if (!state.initialized) {
		state.initialized = true;
		state.must_return_continuation_chunk = false;
		if (caching_supported) {
			state.can_cache_chunk = PhysicalOperator::SelectOperatorCachingMode(context);
		} else {
			state.can_cache_chunk = OperatorCachingMode::NONE;
		}
	}

	const auto execution_mode = SelectExecutionMode(chunk, child_result, state);

	// Appends and flushes MUST route through AppendToCache / FlushCacheToChunk: a raw Append flattens a zero-width
	// dict placeholder and a raw flush drops the dict. (The continuation case below Moves a fresh chunk in -- a
	// full replacement, not an append -- so raw dict columns pass through verbatim.)
	switch (execution_mode) {
	case CachingPhysicalOperatorExecuteMode::RETURN_CACHED_APPEND_CHUNK: {
		auto tmp = make_uniq<DataChunk>();
		tmp->InitializeEmpty(chunk.GetTypes());
		tmp->Reference(chunk);
		FlushCacheToChunk(state, chunk);
		AppendToCache(state, *tmp, context.client);
		break;
	}
	case CachingPhysicalOperatorExecuteMode::RETURN_CACHED_PLUS_CHUNK:
		AppendToCache(state, chunk, context.client);
		FlushCacheToChunk(state, chunk);
		break;
	case CachingPhysicalOperatorExecuteMode::RETURN_CACHED:
		D_ASSERT(chunk.size() == 0);
		FlushCacheToChunk(state, chunk);
		break;
	case CachingPhysicalOperatorExecuteMode::RETURN_CACHED_THEN_CHUNK_VIA_CONTINUATION: {
		// Swap chunk and *state.cached_chunk
		auto tmp = make_uniq<DataChunk>();
		tmp->Move(chunk);
		FlushCacheToChunk(state, chunk);
		state.cached_chunk = make_uniq<DataChunk>();
		state.cached_chunk->Move(*tmp);

		// Now chunk holds what was in (*state.cached_chunk), and it's returned directly
		// While what was in chunk will be returned at next iteration via continuation
		state.must_return_continuation_chunk = true;
		state.cached_result = child_result;
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
	case CachingPhysicalOperatorExecuteMode::APPEND_CHUNK: {
		AppendToCache(state, chunk, context.client);
		chunk.Reset();
		break;
	}
	case CachingPhysicalOperatorExecuteMode::RETURN_CHUNK:
		break;
	}

	return child_result;
}

OperatorFinalizeResultType CachingPhysicalOperator::FinalExecute(ExecutionContext &context, DataChunk &chunk,
                                                                 GlobalOperatorState &gstate,
                                                                 OperatorState &state_p) const {
	auto &state = state_p.Cast<CachingOperatorState>();
	if (state.cached_chunk) {
		const idx_t count = state.cached_chunk->size();
		const bool dict_cache_active = state.dict_cache_active;
		chunk.Reference(*state.cached_chunk);
		if (dict_cache_active) {
			RewrapDictColumns(state, chunk, count);
			state.ResetDictCache();
		}
		state.cached_chunk.reset();
	}
	return OperatorFinalizeResultType::FINISHED;
}

} // namespace duckdb
