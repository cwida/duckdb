#include "duckdb/execution/operator/join/physical_range_join.hpp"

#include "duckdb/common/row_operations/row_operations.hpp"
#include "duckdb/common/sorting/sort_key.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parallel/base_pipeline_event.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/parallel/executor_task.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

PhysicalRangeJoin::LocalSortedTable::LocalSortedTable(ExecutionContext &context, GlobalSortedTable &global_table,
                                                      const idx_t child)
    : global_table(global_table), executor(context.client), has_null(0), count(0) {
	// Initialize order clause expression executor and key DataChunk
	const auto &op = global_table.op;
	vector<LogicalType> types;
	for (const auto &cond : op.conditions) {
		const auto &expr = child ? cond.GetRHS() : cond.GetLHS();
		executor.AddExpression(expr);

		types.push_back(expr.GetReturnType());
	}
	auto &allocator = Allocator::Get(context.client);
	keys.Initialize(allocator, types);

	local_sink = global_table.sort->GetLocalSinkState(context);

	//	Only sort the primary key
	types.resize(1);
	const auto &payload_types = op.children[child].get().types;
	types.insert(types.end(), payload_types.begin(), payload_types.end());
	sort_chunk.InitializeEmpty(types);
}

void PhysicalRangeJoin::LocalSortedTable::ResetForReuse(ExecutionContext &context) {
	local_sink = global_table.sort->GetLocalSinkState(context);
	has_null = 0;
	count = 0;
	keys.Reset();
	sort_chunk.Reset();
}

void PhysicalRangeJoin::LocalSortedTable::Sink(ExecutionContext &context, DataChunk &input) {
	// Obtain sorting columns
	keys.Reset();
	executor.Execute(input, keys);

	Vector primary(keys.data[0].GetType());
	if (keys.data[0].GetVectorType() == VectorType::CONSTANT_VECTOR && ConstantVector::IsNull(keys.data[0])) {
		// Primary is already NULL - no need to merge NULLs
		primary.Reference(keys.data[0]);
		has_null += keys.size();
	} else {
		VectorOperations::Copy(keys.data[0], primary, keys.size(), 0, 0);
		FlatVector::SetSize(primary, count_t(keys.size()));
		// Count the NULLs so we can exclude them later
		has_null += MergeNulls(primary, global_table.op.conditions);
	}
	count += keys.size();

	//	Only sort the primary key
	sort_chunk.data[0].Reference(primary);
	for (column_t col_idx = 0; col_idx < input.ColumnCount(); ++col_idx) {
		sort_chunk.data[col_idx + 1].Reference(input.data[col_idx]);
	}
	sort_chunk.SetChildCardinality(input.size());
	// Sink the data into the local sort state
	InterruptState interrupt;
	OperatorSinkInput sink {*global_table.global_sink, *local_sink, interrupt};
	global_table.sort->Sink(context, sort_chunk, sink);
}

PhysicalRangeJoin::GlobalSortedTable::GlobalSortedTable(ClientContext &client,
                                                        const vector<BoundOrderByNode> &order_bys,
                                                        const vector<LogicalType> &payload_types,
                                                        const PhysicalComparisonJoin &op)
    : op(op), has_null(0), count(0), tasks_completed(0) {
	// Set up the sort. We will materialize keys ourselves, so just set up references.
	vector<BoundOrderByNode> orders;
	vector<LogicalType> input_types;
	for (const auto &order_by : order_bys) {
		auto order = order_by.Copy();
		const auto type = order.expression->GetReturnType();
		input_types.emplace_back(type);
		order.expression = make_uniq<BoundReferenceExpression>(type, orders.size());
		orders.emplace_back(std::move(order));
	}

	vector<idx_t> projection_map;
	for (const auto &type : payload_types) {
		projection_map.emplace_back(input_types.size());
		input_types.emplace_back(type);
	}

	sort = make_uniq<Sort>(client, orders, input_types, projection_map);

	global_sink = sort->GetGlobalSinkState(client);
}

void PhysicalRangeJoin::GlobalSortedTable::Combine(ExecutionContext &context, LocalSortedTable &ltable) {
	InterruptState interrupt;
	OperatorSinkCombineInput combine {*global_sink, *ltable.local_sink, interrupt};
	sort->Combine(context, combine);
	has_null += ltable.has_null;
	count += ltable.count;
}

void PhysicalRangeJoin::GlobalSortedTable::ResetForReuse(ClientContext &client) {
	global_sink = sort->GetGlobalSinkState(client);
	has_null = 0;
	count = 0;
	tasks_completed = 0;
	global_source.reset();
	sorted.reset();
	found_match.reset();
}

void PhysicalRangeJoin::GlobalSortedTable::Finalize(ClientContext &client, InterruptState &interrupt) {
	OperatorSinkFinalizeInput finalize {*global_sink, interrupt};
	sort->Finalize(client, finalize);
	global_source = sort->GetGlobalSourceState(client, *global_sink);
}

void PhysicalRangeJoin::GlobalSortedTable::InitializeMatches() {
	found_match = make_unsafe_uniq_array_uninitialized<bool>(Count());
	memset(found_match.get(), 0, sizeof(bool) * Count());
}

void PhysicalRangeJoin::GlobalSortedTable::MaterializeEmpty(ClientContext &client) {
	D_ASSERT(!sorted);
	sorted = make_uniq<SortedRun>(client, *sort, false);
}

void PhysicalRangeJoin::GlobalSortedTable::Print() {
	D_ASSERT(sorted);
	auto &collection = *sorted->payload_data;
	TupleDataScanState scanner;
	collection.InitializeScan(scanner);

	DataChunk payload;
	collection.InitializeScanChunk(scanner, payload);

	while (collection.Scan(scanner, payload)) {
		payload.Print();
	}
}

//===--------------------------------------------------------------------===//
// RangeJoinMaterializeTask
//===--------------------------------------------------------------------===//
class RangeJoinMaterializeTask : public ExecutorTask {
public:
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;

public:
	RangeJoinMaterializeTask(Pipeline &pipeline, shared_ptr<Event> event, ClientContext &client,
	                         GlobalSortedTable &table, idx_t tasks_scheduled)
	    : ExecutorTask(client, std::move(event), table.op), pipeline(pipeline), table(table),
	      tasks_scheduled(tasks_scheduled) {
	}

	TaskExecutionResult ExecuteTask(TaskExecutionMode mode) override {
		ExecutionContext execution(pipeline.GetClientContext(), *thread_context, &pipeline);
		auto &sort = *table.sort;
		auto &sort_global = *table.global_source;
		auto sort_local = sort.GetLocalSourceState(execution, sort_global);
		InterruptState interrupt((weak_ptr<Task>(shared_from_this())));
		OperatorSourceInput input {sort_global, *sort_local, interrupt};
		sort.MaterializeSortedRun(execution, input);
		if (++table.tasks_completed == tasks_scheduled) {
			table.sorted = sort.GetSortedRun(sort_global);
			if (!table.sorted) {
				table.MaterializeEmpty(execution.client);
			}
			table.global_source.reset();
		}

		event->FinishTask();
		return TaskExecutionResult::TASK_FINISHED;
	}

	string TaskType() const override {
		return "RangeJoinMaterializeTask";
	}

private:
	Pipeline &pipeline;
	GlobalSortedTable &table;
	const idx_t tasks_scheduled;
};

//===--------------------------------------------------------------------===//
// RangeJoinMaterializeEvent
//===--------------------------------------------------------------------===//
class RangeJoinMaterializeEvent : public BasePipelineEvent {
public:
	using GlobalSortedTable = PhysicalRangeJoin::GlobalSortedTable;

public:
	RangeJoinMaterializeEvent(GlobalSortedTable &table, Pipeline &pipeline)
	    : BasePipelineEvent(pipeline), table(table) {
	}

	GlobalSortedTable &table;

public:
	void Schedule() override {
		auto &client = pipeline->GetClientContext();

		// Schedule as many tasks as the sort will allow
		auto &ts = TaskScheduler::GetScheduler(client);
		auto num_threads = ts.NumberOfThreads();
		vector<shared_ptr<Task>> tasks;

		const auto tasks_scheduled = MinValue<idx_t>(num_threads, table.global_source->MaxThreads());
		for (idx_t tnum = 0; tnum < tasks_scheduled; ++tnum) {
			tasks.push_back(
			    make_uniq<RangeJoinMaterializeTask>(*pipeline, shared_from_this(), client, table, tasks_scheduled));
		}

		SetTasks(std::move(tasks));
	}
};

void PhysicalRangeJoin::GlobalSortedTable::Materialize(Pipeline &pipeline, Event &event) {
	// Schedule all the sorts for maximum thread utilisation
	auto sort_event = make_shared_ptr<RangeJoinMaterializeEvent>(*this, pipeline);
	event.InsertEvent(std::move(sort_event));
}

void PhysicalRangeJoin::GlobalSortedTable::MaterializeSortedRun(ExecutionContext &context, InterruptState &interrupt) {
	auto local_source = sort->GetLocalSourceState(context, *global_source);
	OperatorSourceInput source {*global_source, *local_source, interrupt};
	sort->MaterializeSortedRun(context, source);
}

void PhysicalRangeJoin::GlobalSortedTable::GetSortedRun(ClientContext &client) {
	sorted = sort->GetSortedRun(*global_source);
	if (!sorted) {
		MaterializeEmpty(client);
	}
	global_source.reset();
}

void PhysicalRangeJoin::GlobalSortedTable::Materialize(ExecutionContext &context, InterruptState &interrupt) {
	MaterializeSortedRun(context, interrupt);
	GetSortedRun(context.client);
}

bool PhysicalRangeJoin::LessThan(const JoinCondition &a, const JoinCondition &b) {
	//	Comparisons come before non-comparisons
	if (!b.IsComparison()) {
		return a.IsComparison();
	} else if (!a.IsComparison()) {
		return false;
	}

	//	Both are comparisons, so use distinct counts to compare selectivities
	//	(higher is more selective, zero is unknown/completely unselective)
	const auto a_left = a.GetLeftStats() ? a.GetLeftStats()->GetDistinctCount() : 0;
	const auto a_right = a.GetRightStats() ? a.GetRightStats()->GetDistinctCount() : 0;
	const auto a_min = MinValue(a_left, a_right);
	const auto a_type = a.GetRHS().GetReturnType().InternalType();

	const auto b_left = b.GetLeftStats() ? b.GetLeftStats()->GetDistinctCount() : 0;
	const auto b_right = b.GetRightStats() ? b.GetRightStats()->GetDistinctCount() : 0;
	const auto b_min = MinValue(b_left, b_right);
	const auto b_type = b.GetRHS().GetReturnType().InternalType();

	switch (a.GetComparisonType()) {
	case ExpressionType::COMPARE_LESSTHAN:
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
	case ExpressionType::COMPARE_GREATERTHAN:
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		switch (b.GetComparisonType()) {
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			//	Both inequalities
			//	Prefer higher selectivities, but use the minimum of both sides
			if (a_min != b_min) {
				return a_min > b_min;
			}
			//	Prefer narrower types for faster comparisons
			if (!TypeIsConstantSize(b_type)) {
				return TypeIsConstantSize(a_type);
			} else if (!TypeIsConstantSize(a_type)) {
				return false;
			}
			return GetTypeIdSize(a_type) < GetTypeIdSize(b_type);
		default:
			//	Inequalities first, so a < b
			return true;
		}
	case ExpressionType::COMPARE_EQUAL:
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		switch (b.GetComparisonType()) {
		case ExpressionType::COMPARE_LESSTHAN:
		case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		case ExpressionType::COMPARE_GREATERTHAN:
		case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
			//	Inequalities first, so a > b
			return false;
		case ExpressionType::COMPARE_EQUAL:
		case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
			//	Both equalities
			//	Prefer higher rhs selectivities for equalities
			if (a_right != b_right) {
				return a_right > b_right;
			}
			//	Prefer narrower types for faster comparisons
			if (!TypeIsConstantSize(b_type)) {
				return TypeIsConstantSize(a_type);
			} else if (!TypeIsConstantSize(a_type)) {
				return false;
			}
			return GetTypeIdSize(a_type) < GetTypeIdSize(b_type);
		default:
			//	Inequalities first, so a > b
			return false;
		}
	default:
		//	Some other comparison
		return false;
	}
}

PhysicalRangeJoin::PhysicalRangeJoin(PhysicalPlan &physical_plan, LogicalComparisonJoin &op, PhysicalOperatorType type,
                                     PhysicalOperator &left, PhysicalOperator &right, vector<JoinCondition> cond,
                                     JoinType join_type, idx_t estimated_cardinality,
                                     unique_ptr<JoinFilterPushdownInfo> pushdown_info)
    : PhysicalComparisonJoin(physical_plan, op, type, std::move(cond), join_type, estimated_cardinality) {
	filter_pushdown = std::move(pushdown_info);
	// Reorder the conditions so that selective ranges are at the front.
	if (conditions.size() > 1) {
		//	Do an indirect sort so we can remap any positional references to the conditions.
		vector<idx_t> cond_idx;
		for (idx_t i = 0; i < conditions.size(); ++i) {
			cond_idx.emplace_back(i);
		}
		std::sort(cond_idx.begin(), cond_idx.end(),
		          [&](const idx_t &a, const idx_t &b) { return LessThan(conditions[a], conditions[b]); });

		//	Move the conditions into the new order
		vector<JoinCondition> reordered(conditions.size());
		for (idx_t i = 0; i < cond_idx.size(); ++i) {
			reordered[i] = std::move(conditions[cond_idx[i]]);
		}
		std::swap(reordered, conditions);

		//	Remap any filter pushdown references.
		if (filter_pushdown) {
			map<idx_t, idx_t> inverse;
			for (idx_t i = 0; i < cond_idx.size(); ++i) {
				inverse[cond_idx[i]] = i;
			}
			for (auto &idx : filter_pushdown->join_condition) {
				if (inverse.find(idx) != inverse.end()) {
					idx = inverse[idx];
				}
			}
		}
	}

	children.push_back(left);
	children.push_back(right);

	//	Fill out the left projection map.
	left_projection_map = FillProjectionMap(children[0].get(), op.left_projection_map);
	right_projection_map = FillProjectionMap(children[1].get(), op.right_projection_map);

	//	Construct the unprojected type layout from the children's types
	unprojected_types = children[0].get().GetTypes();
	auto &types = children[1].get().GetTypes();
	unprojected_types.insert(unprojected_types.end(), types.begin(), types.end());
}

idx_t PhysicalRangeJoin::LocalSortedTable::MergeNulls(Vector &primary, const vector<JoinCondition> &conditions) {
	// Merge the validity masks of the comparison keys into the primary
	// Return the number of NULLs in the resulting chunk
	D_ASSERT(keys.ColumnCount() > 0);
	const auto count = keys.size();

	size_t all_constant = 0;
	for (const auto &v : keys.data) {
		if (v.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			++all_constant;
		}
	}

	if (all_constant == keys.data.size()) {
		for (size_t c = 1; c < keys.data.size(); ++c) {
			// Skip comparisons that accept NULLs
			if (conditions[c].GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM) {
				continue;
			}
			const auto &v = keys.data[c];
			if (ConstantVector::IsNull(v)) {
				ConstantVector::SetNull(primary, count_t(count));
				return count;
			}
		}
		return 0;
	} else if (keys.ColumnCount() > 1) {
		//	Flatten the primary, as it will need to merge arbitrary validity masks
		primary.Flatten();
		auto &pvalidity = FlatVector::ValidityMutable(primary);

		D_ASSERT(keys.ColumnCount() == conditions.size());
		for (size_t c = 1; c < keys.data.size(); ++c) {
			// Skip comparisons that accept NULLs
			if (conditions[c].GetComparisonType() == ExpressionType::COMPARE_DISTINCT_FROM) {
				continue;
			}
			//	ToUnifiedFormat the rest, as the sort code will do this anyway.
			const auto &v = keys.data[c];
			UnifiedVectorFormat vdata;
			v.ToUnifiedFormat(vdata);
			auto &vvalidity = vdata.validity;
			if (vvalidity.CannotHaveNull()) {
				continue;
			}
			pvalidity.EnsureWritable();
			switch (v.GetVectorType()) {
			case VectorType::FLAT_VECTOR: {
				// Merge entire entries
				auto pmask = pvalidity.GetData();
				const auto entry_count = pvalidity.EntryCount(count);
				for (idx_t entry_idx = 0; entry_idx < entry_count; ++entry_idx) {
					pmask[entry_idx] &= vvalidity.GetValidityEntry(entry_idx);
				}
				break;
			}
			case VectorType::CONSTANT_VECTOR:
				// All or nothing
				if (ConstantVector::IsNull(v)) {
					pvalidity.SetAllInvalid(count);
					return count;
				}
				break;
			default:
				// One by one
				for (idx_t i = 0; i < count; ++i) {
					const auto idx = vdata.sel->get_index(i);
					if (!vvalidity.RowIsValidUnsafe(idx)) {
						pvalidity.SetInvalidUnsafe(i);
					}
				}
				break;
			}
		}
		return count - pvalidity.CountValid(count);
	} else {
		return count - VectorOperations::CountNotNull(primary);
	}
}

void PhysicalRangeJoin::ProjectResult(DataChunk &chunk, DataChunk &result) const {
	const auto left_projected = left_projection_map.size();
	for (idx_t i = 0; i < left_projected; ++i) {
		result.data[i].Reference(chunk.data[left_projection_map[i]]);
	}
	const auto left_width = children[0].get().GetTypes().size();
	for (idx_t i = 0; i < right_projection_map.size(); ++i) {
		result.data[left_projected + i].Reference(chunk.data[left_width + right_projection_map[i]]);
	}
}

template <SortKeyType SORT_KEY_TYPE>
static void TemplatedSliceSortedPayload(DataChunk &chunk, const SortedRun &sorted_run,
                                        ExternalBlockIteratorState &state, Vector &sort_key_pointers,
                                        SortedRunScanState &scan_state, const idx_t chunk_idx,
                                        const unsafe_vector<idx_t> &result) {
	using SORT_KEY = SortKey<SORT_KEY_TYPE>;
	using BLOCK_ITERATOR = block_iterator_t<ExternalBlockIteratorState, SORT_KEY>;
	BLOCK_ITERATOR itr(state, chunk_idx, 0);

	const auto result_size = NumericCast<idx_t>(result.size());
	{
		auto writer = FlatVector::Writer<SORT_KEY *>(sort_key_pointers, result_size);
		for (idx_t i = 0; i < result_size; ++i) {
			const auto idx = state.GetIndex(chunk_idx, result[i]);
			writer.WriteValue(&itr[idx]);
		}
	}

	// Scan
	chunk.Reset();
	scan_state.Scan(sorted_run, sort_key_pointers, chunk);
}

void PhysicalRangeJoin::SliceSortedPayload(DataChunk &chunk, GlobalSortedTable &table,
                                           ExternalBlockIteratorState &state, TupleDataChunkState &chunk_state,
                                           const idx_t chunk_idx, const unsafe_vector<idx_t> &result,
                                           SortedRunScanState &scan_state) {
	auto &sorted = *table.sorted;
	auto &sort_keys = chunk_state.row_locations;
	const auto sort_key_type = table.GetSortKeyType();

	switch (sort_key_type) {
#define DUCKDB_SORT_KEY_CASE(SORT_KEY_TYPE)                                                                            \
	case SortKeyType::SORT_KEY_TYPE:                                                                                   \
		TemplatedSliceSortedPayload<SortKeyType::SORT_KEY_TYPE>(chunk, sorted, state, sort_keys, scan_state,           \
		                                                        chunk_idx, result);                                    \
		break;
		DUCKDB_FOR_EACH_SORT_KEY_TYPE(DUCKDB_SORT_KEY_CASE)
#undef DUCKDB_SORT_KEY_CASE
	default:
		throw NotImplementedException("MergeJoinSimpleBlocks for %s", EnumUtil::ToString(sort_key_type));
	}
}

idx_t PhysicalRangeJoin::SelectJoinTail(const ExpressionType &condition, const Vector &left, const Vector &right,
                                        const SelectionVector *sel, idx_t count, SelectionVector *true_sel) {
	switch (condition) {
	case ExpressionType::COMPARE_NOTEQUAL:
		return VectorOperations::NotEquals(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_LESSTHAN:
		return VectorOperations::LessThan(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_GREATERTHAN:
		return VectorOperations::GreaterThan(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_LESSTHANOREQUALTO:
		return VectorOperations::LessThanEquals(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_GREATERTHANOREQUALTO:
		return VectorOperations::GreaterThanEquals(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_DISTINCT_FROM:
		return VectorOperations::DistinctFrom(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_NOT_DISTINCT_FROM:
		return VectorOperations::NotDistinctFrom(left, right, sel, count, true_sel, nullptr);
	case ExpressionType::COMPARE_EQUAL:
		return VectorOperations::Equals(left, right, sel, count, true_sel, nullptr);
	default:
		throw InternalException("Unsupported comparison type for PhysicalRangeJoin");
	}

	return count;
}

} // namespace duckdb
