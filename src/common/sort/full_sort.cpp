#include "duckdb/common/sorting/full_sort.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/sorting/sorted_run.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// FullSortGroup
//===--------------------------------------------------------------------===//
class FullSortGroup {
public:
	FullSortGroup(ClientContext &client, const Sort &sort);

	atomic<idx_t> count;

	//	Sink
	unique_ptr<GlobalSinkState> sort_global;

	//	Source
	unique_ptr<GlobalSourceState> sort_source;
};

FullSortGroup::FullSortGroup(ClientContext &client, const Sort &sort) : count(0) {
	sort_global = sort.GetGlobalSinkState(client);
}

//===--------------------------------------------------------------------===//
// FullSortGlobalSinkState
//===--------------------------------------------------------------------===//
class FullSortGlobalSinkState : public GlobalSinkState {
public:
	using HashGroupPtr = unique_ptr<FullSortGroup>;

	FullSortGlobalSinkState(ClientContext &client, const FullSort &full_sort);

	// OVER(PARTITION BY...) (hash grouping)
	ProgressData GetSinkProgress(ClientContext &context, const ProgressData source_progress);

	//! System and query state
	ClientContext &client;
	const FullSort &full_sort;
	mutable mutex lock;

	// OVER(...) (sorting)
	HashGroupPtr hash_group;

	// Threading
	atomic<idx_t> count;
	//! Keeps the sink progress monotonic
	MonotonicProgress sink_progress;
};

FullSortGlobalSinkState::FullSortGlobalSinkState(ClientContext &client, const FullSort &full_sort)
    : client(client), full_sort(full_sort), count(0) {
	//	Sort early into a dedicated hash group if we only sort.
	hash_group = make_uniq<FullSortGroup>(client, *full_sort.sort);
}

ProgressData FullSortGlobalSinkState::GetSinkProgress(ClientContext &client, const ProgressData source) {
	lock_guard<mutex> guard(lock);
	const auto sorted_count = hash_group ? full_sort.sort->GetSortedCount(*hash_group->sort_global) : 0;
	return Sort::GetSinkProgress(source, sorted_count, count, sink_progress);
}

SinkFinalizeType FullSort::Finalize(ClientContext &client, OperatorSinkFinalizeInput &finalize) const {
	auto &gsink = finalize.global_state.Cast<FullSortGlobalSinkState>();

	//	Did we get any data?
	if (!gsink.count) {
		return SinkFinalizeType::NO_OUTPUT_POSSIBLE;
	}

	//	OVER(ORDER BY...)
	auto &hash_group = gsink.hash_group;
	auto &global_sink = *hash_group->sort_global;
	OperatorSinkFinalizeInput hfinalize {global_sink, finalize.interrupt_state};
	sort->Finalize(client, hfinalize);
	hash_group->sort_source = sort->GetGlobalSourceState(client, global_sink);

	return SinkFinalizeType::READY;
}

ProgressData FullSort::GetSinkProgress(ClientContext &client, GlobalSinkState &gstate,
                                       const ProgressData source) const {
	auto &gsink = gstate.Cast<FullSortGlobalSinkState>();
	return gsink.GetSinkProgress(client, source);
}

//===--------------------------------------------------------------------===//
// FullSortLocalSinkState
//===--------------------------------------------------------------------===//
// Formerly PartitionLocalSinkState
class FullSortLocalSinkState : public LocalSinkState {
public:
	using LocalSortStatePtr = unique_ptr<LocalSinkState>;

	FullSortLocalSinkState(ExecutionContext &context, const FullSort &full_sort);

	//! Global state
	const FullSort &full_sort;

	//! Shared expression evaluation
	ExpressionExecutor sort_exec;
	DataChunk group_chunk;
	DataChunk sort_chunk;
	DataChunk payload_chunk;

	//! Compute the hash values
	void Hash(DataChunk &input_chunk, Vector &hash_vector);
	//! Merge the state into the global state.
	void Combine(ExecutionContext &context);

	// OVER(ORDER BY...) (only sorting)
	LocalSortStatePtr sort_local;

	// OVER() (no sorting)
	unique_ptr<ColumnDataCollection> unsorted;
	ColumnDataAppendState unsorted_append;
};

FullSortLocalSinkState::FullSortLocalSinkState(ExecutionContext &context, const FullSort &full_sort)
    : full_sort(full_sort), sort_exec(context.client) {
	vector<LogicalType> sort_types;
	for (const auto &expr : full_sort.sort_exprs) {
		sort_types.emplace_back(expr->GetReturnType());
		sort_exec.AddExpression(*expr);
	}
	sort_chunk.Initialize(context.client, sort_types);

	auto payload_types = full_sort.payload_types;

	// OVER(ORDER BY...)
	auto &sort = *full_sort.sort;
	sort_local = sort.GetLocalSinkState(context);
	payload_chunk.Initialize(context.client, payload_types);
}

SinkResultType FullSort::Sink(ExecutionContext &context, DataChunk &input_chunk, OperatorSinkInput &sink) const {
	auto &gstate = sink.global_state.Cast<FullSortGlobalSinkState>();
	auto &lstate = sink.local_state.Cast<FullSortLocalSinkState>();
	gstate.count += input_chunk.size();

	//	Payload prefix is the input data
	auto &payload_chunk = lstate.payload_chunk;
	payload_chunk.Reset();
	for (column_t i = 0; i < input_chunk.ColumnCount(); ++i) {
		payload_chunk.data[i].Reference(input_chunk.data[i]);
	}

	//	Compute any sort columns that are not references and append them to the end of the payload
	auto &sort_chunk = lstate.sort_chunk;
	auto &sort_exec = lstate.sort_exec;
	if (!sort_exprs.empty()) {
		sort_chunk.Reset();
		sort_exec.Execute(input_chunk, sort_chunk);
		for (column_t i = 0; i < sort_chunk.ColumnCount(); ++i) {
			payload_chunk.data[input_chunk.ColumnCount() + i].Reference(sort_chunk.data[i]);
		}
	}

	//	Append a forced payload column
	if (force_payload) {
		auto &vec = payload_chunk.data[input_chunk.ColumnCount() + sort_chunk.ColumnCount()];
		D_ASSERT(vec.GetType().id() == LogicalTypeId::BOOLEAN);
		ConstantVector::SetNull(vec, count_t(input_chunk.size()));
	}

	//	OVER(ORDER BY...)
	auto &sort_local = lstate.sort_local;
	D_ASSERT(sort_local);
	auto &hash_group = *gstate.hash_group;
	OperatorSinkInput input {*hash_group.sort_global, *sort_local, sink.interrupt_state};
	sort->Sink(context, payload_chunk, input);
	hash_group.count += payload_chunk.size();

	return SinkResultType::NEED_MORE_INPUT;
}

SinkCombineResultType FullSort::Combine(ExecutionContext &context, OperatorSinkCombineInput &combine) const {
	auto &gstate = combine.global_state.Cast<FullSortGlobalSinkState>();
	auto &lstate = combine.local_state.Cast<FullSortLocalSinkState>();

	//	OVER(ORDER BY...)
	D_ASSERT(lstate.sort_local);
	auto &hash_group = *gstate.hash_group;
	OperatorSinkCombineInput input {*hash_group.sort_global, *lstate.sort_local, combine.interrupt_state};
	sort->Combine(context, input);
	lstate.sort_local.reset();
	return SinkCombineResultType::FINISHED;
}

//===--------------------------------------------------------------------===//
// FullSortGlobalSourceState
//===--------------------------------------------------------------------===//
class FullSortGlobalSourceState : public GlobalSourceState {
public:
	using ChunkRow = FullSort::ChunkRow;
	using ChunkRows = FullSort::ChunkRows;

	FullSortGlobalSourceState(ClientContext &client, FullSortGlobalSinkState &gsink);

	FullSortGlobalSinkState &gsink;
	ChunkRows chunk_rows;
};

FullSortGlobalSourceState::FullSortGlobalSourceState(ClientContext &client, FullSortGlobalSinkState &gsink)
    : gsink(gsink) {
	if (!gsink.count) {
		return;
	}

	//	One sorted group
	ChunkRow chunk_row;

	auto &hash_group = gsink.hash_group;
	if (hash_group) {
		chunk_row.count = hash_group->count;
		chunk_row.chunks = (chunk_row.count + STANDARD_VECTOR_SIZE - 1) / STANDARD_VECTOR_SIZE;
	}

	chunk_rows.emplace_back(chunk_row);
}

//===--------------------------------------------------------------------===//
// FullSort
//===--------------------------------------------------------------------===//
FullSort::FullSort(ClientContext &client, const vector<BoundOrderByNode> &order_bys, const Types &input_types,
                   bool require_payload)
    : SortStrategy(input_types) {
	//	We have to compute ordering expressions ourselves and materialise them.
	//	To do this, we scan the orders and add generate extra payload columns that we can reference.
	for (const auto &order : order_bys) {
		orders.emplace_back(order.Copy());
	}

	for (auto &order : orders) {
		auto &expr = *order.expression;
		if (expr.GetExpressionClass() == ExpressionClass::BOUND_REF) {
			auto &ref = expr.Cast<BoundReferenceExpression>();
			sort_ids.emplace_back(ref.Index());
			continue;
		}

		//	Real expression - replace with a ref and save the expression
		auto saved = std::move(order.expression);
		const auto type = saved->GetReturnType();
		const auto idx = payload_types.size();
		order.expression = make_uniq<BoundReferenceExpression>(type, idx);
		sort_ids.emplace_back(idx);
		payload_types.emplace_back(type);
		sort_exprs.emplace_back(std::move(saved));
	}

	// If a payload column is required, check whether there is one already
	if (require_payload) {
		//	Watch out for duplicate sort keys!
		unordered_set<column_t> sort_set(sort_ids.begin(), sort_ids.end());
		force_payload = (sort_set.size() >= payload_types.size());
		if (force_payload) {
			payload_types.emplace_back(LogicalType::BOOLEAN);
		}
	}
	vector<idx_t> projection_map;
	sort = make_uniq<Sort>(client, orders, payload_types, projection_map);
}

unique_ptr<GlobalSinkState> FullSort::GetGlobalSinkState(ClientContext &client) const {
	return make_uniq<FullSortGlobalSinkState>(client, *this);
}

unique_ptr<LocalSinkState> FullSort::GetLocalSinkState(ExecutionContext &context) const {
	return make_uniq<FullSortLocalSinkState>(context, *this);
}

unique_ptr<GlobalSourceState> FullSort::GetGlobalSourceState(ClientContext &client, GlobalSinkState &sink) const {
	return make_uniq<FullSortGlobalSourceState>(client, sink.Cast<FullSortGlobalSinkState>());
}

const FullSort::ChunkRows &FullSort::GetHashGroups(GlobalSourceState &gstate) const {
	auto &gsource = gstate.Cast<FullSortGlobalSourceState>();
	return gsource.chunk_rows;
}

SourceResultType FullSort::MaterializeSortedData(ExecutionContext &context, bool build_runs,
                                                 OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<FullSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_group;

	auto &sort_global = *hash_group.sort_source;
	auto sort_local = sort->GetLocalSourceState(context, sort_global);

	OperatorSourceInput input {sort_global, *sort_local, source.interrupt_state};
	if (build_runs) {
		return sort->MaterializeSortedRun(context, input);
	} else {
		return sort->MaterializeColumnData(context, input);
	}
}

SourceResultType FullSort::MaterializeColumnData(ExecutionContext &execution, idx_t hash_bin,
                                                 OperatorSourceInput &source) const {
	return MaterializeSortedData(execution, false, source);
}

FullSort::HashGroupPtr FullSort::GetColumnData(idx_t hash_bin, OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<FullSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_group;

	D_ASSERT(hash_bin == 0);

	auto &sort_global = *hash_group.sort_source;

	OperatorSourceInput input {sort_global, source.local_state, source.interrupt_state};
	auto result = sort->GetColumnData(input);
	hash_group.sort_source.reset();

	//	Just because MaterializeColumnData returned FINISHED doesn't mean that the same thread will
	//	get the result...
	if (result && result->Count() == hash_group.count) {
		return result;
	}

	return nullptr;
}

SourceResultType FullSort::MaterializeSortedRun(ExecutionContext &context, idx_t hash_bin,
                                                OperatorSourceInput &source) const {
	return MaterializeSortedData(context, true, source);
}

FullSort::SortedRunPtr FullSort::GetSortedRun(ClientContext &client, idx_t hash_bin,
                                              OperatorSourceInput &source) const {
	auto &gsource = source.global_state.Cast<FullSortGlobalSourceState>();
	auto &gsink = gsource.gsink;
	auto &hash_group = *gsink.hash_group;

	D_ASSERT(hash_bin == 0);

	auto &sort_global = *hash_group.sort_source;

	auto result = sort->GetSortedRun(sort_global);
	if (!result) {
		D_ASSERT(hash_group.count == 0);
		result = make_uniq<SortedRun>(client, *sort, false);
	}

	hash_group.sort_source.reset();

	return result;
}

} // namespace duckdb
