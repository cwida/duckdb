#include "duckdb/function/window/window_merge_sort_tree.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"

#include <thread>
#include <utility>

namespace duckdb {

WindowMergeSortTree::WindowMergeSortTree(ClientContext &client, const vector<BoundOrderByNode> &orders_p,
                                         const vector<column_t> &order_idx, const idx_t count, bool unique)
    : order_idx(order_idx), build_stage(WindowMergeSortStage::INIT), tasks_completed(0) {
	// Sort the unfiltered indices by the orders
	const auto force_external = Settings::Get<DebugForceExternalSetting>(client);
	LogicalType index_type;
	if (count < std::numeric_limits<uint32_t>::max() && !force_external) {
		index_type = LogicalType::INTEGER;
		mst32 = make_uniq<MergeSortTree32>(BufferAllocator::Get(client));
	} else {
		index_type = LogicalType::BIGINT;
		mst64 = make_uniq<MergeSortTree64>(BufferAllocator::Get(client));
	}

	vector<BoundOrderByNode> orders;
	for (const auto &order_p : orders_p) {
		auto order = order_p.Copy();
		const auto &type = order.expression->GetReturnType();
		scan_types.emplace_back(type);
		order.expression = make_uniq<BoundReferenceExpression>(type, orders.size());
		orders.emplace_back(std::move(order));
		scan_cols.emplace_back(scan_cols.size());
		key_cols.emplace_back(key_cols.size());
	}

	//	Also track the index type
	scan_types.emplace_back(index_type);
	scan_cols.emplace_back(scan_cols.size());

	// 	If the caller wants disambiguation, also sort by the index column
	if (unique) {
		auto unique_expr = make_uniq<BoundReferenceExpression>(scan_types.back(), orders.size());
		const auto order_type = OrderType::ASCENDING;
		const auto order_by_type = OrderByNullType::NULLS_LAST;
		orders.emplace_back(order_type, order_by_type, std::move(unique_expr));
		key_cols.emplace_back(key_cols.size());
	}

	sort = make_uniq<Sort>(client, orders, scan_types, scan_cols);

	global_sink = sort->GetGlobalSinkState(client);
}

optional_ptr<LocalSinkState> WindowMergeSortTree::InitializeLocalSort(ExecutionContext &context) const {
	lock_guard<mutex> local_sort_guard(lock);
	auto local_sink = sort->GetLocalSinkState(context);
	local_sinks.emplace_back(std::move(local_sink));

	return local_sinks.back().get();
}

WindowMergeSortTreeLocalState::WindowMergeSortTreeLocalState(ExecutionContext &context,
                                                             WindowMergeSortTree &window_tree)
    : window_tree(window_tree) {
	sort_chunk.Initialize(context.client, window_tree.scan_types);
	local_sink = window_tree.InitializeLocalSort(context);
}

void WindowMergeSortTreeLocalState::Sink(ExecutionContext &context, DataChunk &chunk, const idx_t row_idx,
                                         optional_ptr<SelectionVector> filter_sel, idx_t filtered,
                                         InterruptState &interrupt) {
	//	Sequence the payload column
	sort_chunk.Reset();
	auto &indices = sort_chunk.data.back();
	indices.Sequence(int64_t(row_idx), 1, chunk.size());

	//	Reference the ORDER BY columns
	auto &order_idx = window_tree.order_idx;
	for (column_t c = 0; c < order_idx.size(); ++c) {
		sort_chunk.data[c].Reference(chunk.data[order_idx[c]]);
	}

	//	Apply FILTER clause, if any
	if (filter_sel) {
		sort_chunk.Slice(*filter_sel, filtered);
	}

	OperatorSinkInput sink {*window_tree.global_sink, *local_sink, interrupt};
	window_tree.sort->Sink(context, sort_chunk, sink);
}

void WindowMergeSortTreeLocalState::ExecuteSortTask(ExecutionContext &context, InterruptState &interrupt) {
	switch (build_stage) {
	case WindowMergeSortStage::COMBINE: {
		auto &local_sink = *window_tree.local_sinks[build_task];
		OperatorSinkCombineInput combine {*window_tree.global_sink, local_sink, interrupt};
		window_tree.sort->Combine(context, combine);
		break;
	}
	case WindowMergeSortStage::FINALIZE: {
		auto &sort = *window_tree.sort;
		OperatorSinkFinalizeInput finalize {*window_tree.global_sink, interrupt};
		sort.Finalize(context.client, finalize);
		auto sort_global = sort.GetGlobalSourceState(context.client, *window_tree.global_sink);
		auto sort_local = sort.GetLocalSourceState(context, *sort_global);
		OperatorSourceInput source {*sort_global, *sort_local, interrupt};
		sort.MaterializeColumnData(context, source);
		window_tree.sorted = sort.GetColumnData(source);
		//	Allocate the tree from the sorted data as part of this task,
		//	so that a failure here does not leave a half-allocated tree for another thread to retry.
		window_tree.MeasurePayloadBlocks();
		break;
	}
	case WindowMergeSortStage::SORTED:
		BuildLeaves();
		break;
	case WindowMergeSortStage::FINISH:
		window_tree.Finished();
		break;
	default:
		break;
	}

	//	Completion means success: a failed task never completes its stage,
	//	and the threads waiting for the stage leave through the executor's interrupt.
	++window_tree.tasks_completed;
}

idx_t WindowMergeSortTree::MeasurePayloadBlocks() {
	const auto count = sorted->Count();

	// Allocate the leaves.
	if (mst32) {
		mst32->Allocate(count);
		mst32->LowestLevel().resize(count);
	} else if (mst64) {
		mst64->Allocate(count);
		mst64->LowestLevel().resize(count);
	}

	return count;
}

void WindowMergeSortTree::Finished() {
	global_sink.reset();
	local_sinks.clear();
	sorted.reset();
}

bool WindowMergeSortTree::TryPrepareSortStage(WindowMergeSortTreeLocalState &lstate) {
	lock_guard<mutex> stage_guard(lock);

	switch (build_stage.load()) {
	case WindowMergeSortStage::INIT:
		total_tasks = local_sinks.size();
		tasks_assigned = 0;
		tasks_completed = 0;
		lstate.build_stage = build_stage = WindowMergeSortStage::COMBINE;
		lstate.build_task = tasks_assigned++;
		return true;
	case WindowMergeSortStage::COMBINE:
		// Process all the local sorts
		if (tasks_assigned < total_tasks) {
			lstate.build_stage = WindowMergeSortStage::COMBINE;
			lstate.build_task = tasks_assigned++;
			return true;
		} else if (tasks_completed < tasks_assigned) {
			return false;
		}
		// All combines are done, so move on to materialising the sorted data (1 task)
		total_tasks = 1;
		tasks_completed = 0;
		tasks_assigned = 0;
		lstate.build_stage = build_stage = WindowMergeSortStage::FINALIZE;
		lstate.build_task = tasks_assigned++;
		return true;
	case WindowMergeSortStage::FINALIZE:
		if (tasks_completed < tasks_assigned) {
			//	Wait for the single task to finish
			return false;
		}
		//	Move on to building the tree in parallel
		total_tasks = local_sinks.size();
		tasks_completed = 0;
		tasks_assigned = 0;
		lstate.build_stage = build_stage = WindowMergeSortStage::SORTED;
		lstate.build_task = tasks_assigned++;
		return true;
	case WindowMergeSortStage::SORTED:
		if (tasks_assigned < total_tasks) {
			lstate.build_stage = WindowMergeSortStage::SORTED;
			lstate.build_task = tasks_assigned++;
			return true;
		} else if (tasks_completed < tasks_assigned) {
			// Sleep while other tasks finish
			return false;
		}
		// All leaves are built, so move on to finishing the tree (1 task)
		total_tasks = 1;
		tasks_completed = 0;
		tasks_assigned = 0;
		lstate.build_stage = build_stage = WindowMergeSortStage::FINISH;
		lstate.build_task = tasks_assigned++;
		return true;
	case WindowMergeSortStage::FINISH:
		if (tasks_completed < tasks_assigned) {
			//	Wait for the single task to finish
			return false;
		}
		break;
	case WindowMergeSortStage::FINISHED:
		break;
	}

	lstate.build_stage = build_stage = WindowMergeSortStage::FINISHED;

	return true;
}

void WindowMergeSortTreeLocalState::Finalize(ExecutionContext &context, InterruptState &interrupt) {
	// Sort, merge and build the tree in parallel
	while (window_tree.build_stage.load() != WindowMergeSortStage::FINISHED) {
		//	A failed task never completes its stage, so the waiting threads
		//	leave through the interrupt that the executor raises for the failure
		context.client.InterruptCheck();
		if (window_tree.TryPrepareSortStage(*this)) {
			ExecuteSortTask(context, interrupt);
		} else {
			std::this_thread::yield();
		}
	}
}

void WindowMergeSortTree::Build(ClientContext &context) {
	if (mst32) {
		mst32->Build(context);
	} else {
		mst64->Build(context);
	}
}

} // namespace duckdb
