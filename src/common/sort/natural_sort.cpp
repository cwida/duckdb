#include "duckdb/common/sorting/natural_sort.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// NaturalSortGroup
//===--------------------------------------------------------------------===//
class NaturalSortGroup {
public:
	explicit NaturalSortGroup(ClientContext &client);

	atomic<idx_t> count;

	unique_ptr<ColumnDataCollection> columns;
	atomic<idx_t> get_columns;
};

NaturalSortGroup::NaturalSortGroup(ClientContext &client) : count(0), get_columns(0) {
}

//===--------------------------------------------------------------------===//
// NaturalSortGlobalSinkState
//===--------------------------------------------------------------------===//
class NaturalSortGlobalSinkState : public GlobalSinkState {
public:
	using HashGroupPtr = unique_ptr<NaturalSortGroup>;

	NaturalSortGlobalSinkState(ClientContext &client, const NaturalSort &natural_sort);

	ProgressData GetSinkProgress(ClientContext &context, const ProgressData source_progress) const;

	//! System and query state
	const NaturalSort &natural_sort;

	//! Combined rows
	mutable annotated_mutex lock;
	HashGroupPtr hash_group;

	// Threading
	atomic<idx_t> count;
};

NaturalSortGlobalSinkState::NaturalSortGlobalSinkState(ClientContext &client, const NaturalSort &natural_sort)
    : natural_sort(natural_sort), count(0) {
}

ProgressData NaturalSortGlobalSinkState::GetSinkProgress(ClientContext &client, const ProgressData source) const {
	// there is nothing to sort
	return source;
}

SinkFinalizeType NaturalSort::Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const {
	auto &gsink = finalize.global_state.Cast<NaturalSortGlobalSinkState>();

	//	Did we get any data?
	return gsink.count ? SinkFinalizeType::READY : SinkFinalizeType::NO_OUTPUT_POSSIBLE;
}

ProgressData NaturalSort::GetSinkProgress(ClientContext &client, GlobalSinkState &gstate,
                                          const ProgressData source) const {
	auto &gsink = gstate.Cast<NaturalSortGlobalSinkState>();
	return gsink.GetSinkProgress(client, source);
}

//===--------------------------------------------------------------------===//
// NaturalSortLocalSinkState
//===--------------------------------------------------------------------===//
class NaturalSortLocalSinkState : public LocalSinkState {
public:
	NaturalSortLocalSinkState(ExecutionContext &context, const NaturalSort &natural_sort);

	//! Global state
	const NaturalSort &natural_sort;

	//! Merge the state into the global state.
	void Combine(ExecutionContext &context);

	// OVER() (no sorting)
	unique_ptr<ColumnDataCollection> unsorted;
	ColumnDataAppendState unsorted_append;
};

NaturalSortLocalSinkState::NaturalSortLocalSinkState(ExecutionContext &context, const NaturalSort &natural_sort)
    : natural_sort(natural_sort) {
	unsorted = make_uniq<ColumnDataCollection>(context.client, natural_sort.payload_types);
	unsorted->InitializeAppend(unsorted_append);
}

SinkResultType NaturalSort::Sink(ExecutionContext &context, DataChunk &input_chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<NaturalSortGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<NaturalSortLocalSinkState>();
	gstate.count += input_chunk.size();

	lstate.unsorted->Append(lstate.unsorted_append, input_chunk);
	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType NaturalSort::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<NaturalSortGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<NaturalSortLocalSinkState>();

	// Only one partition, so need a global lock.
	annotated_lock_guard<annotated_mutex> glock(gstate.lock);
	auto &hash_group = gstate.hash_group;
	if (hash_group) {
		auto &unsorted = *hash_group->columns;
		if (lstate.unsorted) {
			hash_group->count += lstate.unsorted->Count();
			unsorted.Combine(*lstate.unsorted);
			lstate.unsorted.reset();
		}
	} else {
		hash_group = make_uniq<NaturalSortGroup>(context.client);
		hash_group->columns = std::move(lstate.unsorted);
		hash_group->count += hash_group->columns->Count();
	}
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// NaturalSortGlobalSourceState
//===--------------------------------------------------------------------===//
class NaturalSortGlobalSourceState : public GlobalSourceState {
public:
	using ChunkRow = NaturalSort::ChunkRow;
	using ChunkRows = NaturalSort::ChunkRows;

	NaturalSortGlobalSourceState(ClientContext &client, NaturalSortGlobalSinkState &gsink);

	NaturalSortGlobalSinkState &gsink;
	ChunkRows chunk_rows;
};

NaturalSortGlobalSourceState::NaturalSortGlobalSourceState(ClientContext &client, NaturalSortGlobalSinkState &gsink)
    : gsink(gsink) {
	if (!gsink.count) {
		return;
	}

	//	One unsorted group. We have the count and chunks.
	ChunkRow chunk_row;

	auto &hash_group = gsink.hash_group;
	if (hash_group) {
		chunk_row.count = hash_group->count;
		chunk_row.chunks = hash_group->columns->ChunkCount();
	}

	chunk_rows.emplace_back(chunk_row);
}

//===--------------------------------------------------------------------===//
// NaturalSort
//===--------------------------------------------------------------------===//
NaturalSort::NaturalSort(const Types &input_types) : SortStrategy(input_types) {
}

unique_ptr<GlobalSinkState> NaturalSort::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<NaturalSortGlobalSinkState>(client, *this);
}

unique_ptr<LocalSinkState> NaturalSort::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<NaturalSortLocalSinkState>(context, *this);
}

unique_ptr<GlobalSourceState> NaturalSort::GetGlobalSourceState(ClientContext &client, GlobalSinkState &sink) const {
	return make_uniq<NaturalSortGlobalSourceState>(client, sink.Cast<NaturalSortGlobalSinkState>());
}

unique_ptr<LocalSourceState> NaturalSort::GetLocalSourceState(ExecutionContext &context,
                                                              GlobalSourceState &gstate) const {
	return make_uniq<LocalSourceState>();
}

const NaturalSort::ChunkRows &NaturalSort::GetHashGroups(GlobalSourceState &gstate) const {
	auto &gsource = gstate.Cast<NaturalSortGlobalSourceState>();
	return gsource.chunk_rows;
}

SourceResultType NaturalSort::MaterializeColumnData(ExecutionContext &execution, idx_t hash_bin,
                                                    OperatorSourceInput &source) const {
	D_ASSERT(hash_bin == 0);
	auto &gsource = source.global_state.Cast<NaturalSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_group;

	// Hack: Only report finished for the first call
	return hash_group.get_columns++ ? SourceResultType::HAVE_MORE_OUTPUT : SourceResultType::FINISHED;
}

NaturalSort::HashGroupPtr NaturalSort::GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<NaturalSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_group;

	// OVER()
	D_ASSERT(hash_bin == 0);
	return std::move(hash_group.columns);
}

SourceResultType NaturalSort::MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
                                                   OperatorSourceInput &source) const {
	throw InternalException("NaturalSort does not implement sorted runs.");
}

NaturalSort::SortedRunPtr NaturalSort::GetSortedRun(ClientContext &client, idx_t hash_bin,
                                                    OperatorSourceInput &source) const {
	throw InternalException("NaturalSort does not implement sorted runs.");
}

} // namespace duckdb
