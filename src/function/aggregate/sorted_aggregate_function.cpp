#include "duckdb/common/clustered_aggregate.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/sorting/sort.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/function/aggregate/list_aggregate.hpp"
#include "duckdb/function/aggregate_function.hpp"
#include "duckdb/function/aggregate_state_layout.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/storage/buffer_manager.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

namespace {

//! Maps each ORDER BY key to a buffered column, reusing an argument column when the key matches one. Returns the
//! buffered column types and fills `orders` with the (column, modifiers) per key.
vector<LogicalType> MapSortedColumns(const vector<unique_ptr<Expression>> &children,
                                     const vector<BoundOrderByNode> &order_bys,
                                     vector<SortedAggregateStateOrder> &orders) {
	vector<LogicalType> column_types;
	for (const auto &child : children) {
		column_types.emplace_back(child->GetReturnType());
	}
	for (const auto &order : order_bys) {
		idx_t column = DConstants::INVALID_INDEX;
		for (idx_t arg = 0; arg < children.size(); ++arg) {
			if (children[arg]->Equals(*order.expression)) {
				column = arg;
				break;
			}
		}
		if (column == DConstants::INVALID_INDEX) {
			column = column_types.size();
			column_types.emplace_back(order.expression->GetReturnType());
		}
		orders.push_back(SortedAggregateStateOrder {column, order.type, order.null_order});
	}
	return column_types;
}

//! The struct type of a buffered row: the buffered columns named v0, v1, ...
LogicalType BufferStructType(const vector<LogicalType> &column_types) {
	child_list_t<LogicalType> children;
	for (idx_t i = 0; i < column_types.size(); i++) {
		children.emplace_back("v" + to_string(i), column_types[i]);
	}
	return LogicalType::STRUCT(std::move(children));
}

struct SortedAggregateBindData : public FunctionData {
	using Expressions = vector<unique_ptr<Expression>>;
	using BindInfoPtr = unique_ptr<FunctionData>;
	using OrderBys = vector<BoundOrderByNode>;

	//! fixed_layout: the buffer struct and per-key columns to use instead of mapping the keys onto the arguments
	SortedAggregateBindData(
	    ClientContext &context, Expressions &children, BoundAggregateFunction &aggregate, BindInfoPtr &bind_info,
	    OrderBys &order_bys,
	    optional_ptr<const pair<LogicalType, vector<SortedAggregateStateOrder>>> fixed_layout = nullptr)
	    : context(context), function(aggregate), bind_info(std::move(bind_info)),
	      threshold(Settings::Get<OrderedAggregateThresholdSetting>(context)) {
		vector<SortedAggregateStateOrder> order_spec;
		if (fixed_layout) {
			for (auto &child : StructType::GetChildTypes(fixed_layout->first)) {
				buffered_types.emplace_back(child.second);
			}
			order_spec = fixed_layout->second;
		} else {
			buffered_types = MapSortedColumns(children, order_bys, order_spec);
		}
		const idx_t argument_count = children.size();
		// the arguments are the leading buffered columns (referencing their input slot); appended sort keys follow
		buffered_cols.resize(buffered_types.size());
		for (idx_t i = 0; i < argument_count; i++) {
			buffered_cols[i] = i;
			scan_cols.emplace_back(i + 1);
			scan_types.emplace_back(buffered_types[i]);
		}
		for (idx_t o = 0; o < order_spec.size(); o++) {
			if (order_spec[o].column >= argument_count) {
				buffered_cols[order_spec[o].column] = argument_count + o;
			}
		}
		sorted_on_args = (buffered_types.size() == argument_count);
		BuildSort(order_spec);
	}

	SortedAggregateBindData(
	    ClientContext &context, BoundAggregateExpression &expr,
	    optional_ptr<const pair<LogicalType, vector<SortedAggregateStateOrder>>> fixed_layout = nullptr)
	    : SortedAggregateBindData(context, expr.GetChildrenMutable(), expr.FunctionMutable(), expr.BindInfoMutable(),
	                              expr.GetOrderBysMutable()->orders, fixed_layout) {
	}

	SortedAggregateBindData(ClientContext &context, BoundWindowExpression &expr)
	    : SortedAggregateBindData(context, expr.GetChildrenMutable(), *expr.AggregateFunction(), expr.BindInfoMutable(),
	                              expr.ArgOrdersMutable()) {
	}

	//! Reconstruct from an exported buffer state - the buffer struct, the per-key (column, modifiers) and the leading
	//! argument count fully describe the layout, no original expressions are needed.
	SortedAggregateBindData(ClientContext &context, const BoundAggregateFunction &inner_function,
	                        unique_ptr<FunctionData> inner_bind_info, const LogicalType &buffer_struct,
	                        const vector<SortedAggregateStateOrder> &order_spec, idx_t argument_count)
	    : context(context), function(inner_function), bind_info(std::move(inner_bind_info)),
	      threshold(Settings::Get<OrderedAggregateThresholdSetting>(context)) {
		for (auto &child : StructType::GetChildTypes(buffer_struct)) {
			buffered_cols.emplace_back(buffered_cols.size());
			buffered_types.emplace_back(child.second);
		}
		for (idx_t i = 0; i < argument_count; i++) {
			scan_cols.emplace_back(i + 1);
			scan_types.emplace_back(buffered_types[i]);
		}
		sorted_on_args = (argument_count == buffered_types.size());
		BuildSort(order_spec);
	}

	//! Builds the sort once the buffered columns and per-key (column, modifiers) are known: prefixes the group number,
	//! lays out the buffered struct and creates the sort. Sort keys reference buffered columns, offset by the prefix.
	void BuildSort(const vector<SortedAggregateStateOrder> &order_spec) {
		sort_types.emplace_back(LogicalType::USMALLINT);
		orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_FIRST,
		                    make_uniq<BoundReferenceExpression>(LogicalType::USMALLINT, 0U));
		for (const auto &buffered_type : buffered_types) {
			sort_types.emplace_back(buffered_type);
		}
		buffered_struct_type = BufferStructType(buffered_types);
		GetSegmentDataFunctions(buffered_funcs, buffered_struct_type);
		for (const auto &entry : order_spec) {
			orders.emplace_back(entry.order_type, entry.null_order,
			                    make_uniq<BoundReferenceExpression>(buffered_types[entry.column],
			                                                        UnsafeNumericCast<idx_t>(entry.column + 1)));
		}
		sort = make_shared_ptr<Sort>(context, orders, sort_types, scan_cols);
	}

	SortedAggregateBindData(const SortedAggregateBindData &other)
	    : FunctionData(other), context(other.context), function(other.function), sort_types(other.sort_types),
	      scan_cols(other.scan_cols), scan_types(other.scan_types), sort(other.sort),
	      buffered_cols(other.buffered_cols), buffered_types(other.buffered_types),
	      buffered_struct_type(other.buffered_struct_type), buffered_funcs(other.buffered_funcs),
	      sorted_on_args(other.sorted_on_args), threshold(other.threshold) {
		if (other.bind_info) {
			bind_info = other.bind_info->Copy();
		}
		for (auto &order : other.orders) {
			orders.emplace_back(order.Copy());
		}
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<SortedAggregateBindData>(*this);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<SortedAggregateBindData>();
		if (bind_info && other.bind_info) {
			if (!bind_info->Equals(*other.bind_info)) {
				return false;
			}
		} else if (bind_info || other.bind_info) {
			return false;
		}
		if (function != other.function) {
			return false;
		}
		if (orders.size() != other.orders.size()) {
			return false;
		}
		for (size_t i = 0; i < orders.size(); ++i) {
			if (!orders[i].Equals(other.orders[i])) {
				return false;
			}
		}
		return true;
	}

	ClientContext &context;
	BoundAggregateFunction function;
	unique_ptr<FunctionData> bind_info;

	//! The sort expressions (all references as the expressions have been computed)
	vector<BoundOrderByNode> orders;
	//! The types of the sunk columns
	vector<LogicalType> sort_types;
	//! The sorted columns that have the arguments
	vector<column_t> scan_cols;
	//! The types of the sunk columns
	vector<LogicalType> scan_types;
	//! The immutable shared sort specification
	shared_ptr<const Sort> sort;

	//! The mapping from inputs to buffered columns
	vector<column_t> buffered_cols;
	//! The schema of the buffered data
	vector<LogicalType> buffered_types;
	//! The struct type holding one buffered row
	LogicalType buffered_struct_type;
	//! The linked list functions for the buffered rows
	ListSegmentFunctions buffered_funcs;
	//! Can we just use the inputs for sorting?
	bool sorted_on_args = true;

	//! The sort flush threshold
	const idx_t threshold;
};

//! The sorted aggregate buffers its input rows in a linked list of structs, sharing the "list" callbacks
struct SortedAggregateState : ListAggState {};

//! Caches the chunks, contexts and inner aggregate state used while finalizing the groups of a sorted aggregate.
//! When the caller provides a local state slot (e.g. the hash table scan), this state survives across finalize
//! calls instead of being re-instantiated for every result chunk.
struct SortedAggregateFinalizeState : FunctionLocalState {
	static idx_t StateSize(const SortedAggregateBindData &order_bind) {
		AggregateStateInput input(order_bind.function, order_bind.bind_info.get());
		return order_bind.function.GetCallbacks().GetStateSizeCallback()(input);
	}

	explicit SortedAggregateFinalizeState(const SortedAggregateBindData &order_bind)
	    : thread(order_bind.context), context(order_bind.context, thread, nullptr), agg_state(StateSize(order_bind)),
	      agg_state_vec(Value::POINTER(CastPointerToValue(agg_state.data())), count_t(1)) {
		auto &buffer_allocator = BufferManager::GetBufferManager(order_bind.context).GetBufferAllocator();
		rows.Initialize(buffer_allocator, {order_bind.buffered_struct_type});
		scanned.Initialize(buffer_allocator, order_bind.scan_types);
		sliced.Initialize(buffer_allocator, order_bind.scan_types);
		prefixed.Initialize(buffer_allocator, order_bind.sort_types);

		//	The local state of the inner aggregate's finalize is kept alive across finalize calls as well
		const auto &callbacks = order_bind.function.GetCallbacks();
		if (callbacks.HasInitLocalStateFinalizeCallback()) {
			inner_local_state =
			    callbacks.GetInitLocalStateFinalizeCallback()(order_bind.function, order_bind.bind_info.get());
		}
	}

	static unique_ptr<FunctionLocalState> Init(const BoundAggregateFunction &, optional_ptr<FunctionData> bind_data) {
		return make_uniq<SortedAggregateFinalizeState>(bind_data->Cast<SortedAggregateBindData>());
	}

	//! The execution context for the sort operator
	ThreadContext thread;
	ExecutionContext context;
	InterruptState interrupt;
	//! The buffered rows of (possibly many) groups, accumulated before they are sunk into the sort
	DataChunk rows;
	//! The chunk for scanning the sorted data
	DataChunk scanned;
	//! The scanned data sliced to the rows of a single group
	DataChunk sliced;
	//! The sink chunk holding the buffered rows prefixed with the group number
	DataChunk prefixed;
	//! The state of the inner aggregate
	vector<data_t> agg_state;
	//! A vector pointing to the inner aggregate state
	Vector agg_state_vec;
	//! The local state used by the inner aggregate's finalize (may be null)
	unique_ptr<FunctionLocalState> inner_local_state;
};

struct SortedAggregateFunction {
	static LogicalType GetElementType(AggregateInputData &aggr_input_data) {
		return aggr_input_data.bind_data->Cast<SortedAggregateBindData>().buffered_struct_type;
	}

	static void ScatterUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states,
	                          idx_t count) {
		if (!count) {
			return;
		}
		// Pack the buffered columns into a single struct vector and append the rows through the list update
		const auto &order_bind = aggr_input_data.bind_data->Cast<SortedAggregateBindData>();
		Vector packed(order_bind.buffered_struct_type, count);
		auto &entries = StructVector::GetEntries(packed);
		const auto &buffered_cols = order_bind.buffered_cols;
		for (idx_t b = 0; b < buffered_cols.size(); ++b) {
			D_ASSERT(buffered_cols[b] < input_count);
			entries[b].Reference(inputs[buffered_cols[b]]);
		}
		FlatVector::SetSize(packed, count_t(count));
		ListUpdateFunction(&packed, aggr_input_data, 1, states, count);
	}

	static void Window(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
	                   const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames &subframes, Vector &result,
	                   idx_t rid) {
		throw InternalException("Sorted aggregates should not be generated for window clauses");
	}

	static void WindowBatch(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
	                        const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames *subframes_per_row,
	                        idx_t count, Vector &result, idx_t row_idx) {
		for (idx_t rid = 0; rid < count; ++rid) {
			Window(aggr_input_data, partition, g_state, l_state, subframes_per_row[rid], result, rid);
		}
	}

	//! Sinks the rows accumulated in the rows chunk into the sort, prefixed with their group numbers
	static void FlushAccumulated(const SortedAggregateBindData &order_bind, idx_t &accumulated,
	                             SortedAggregateFinalizeState &finalize_state, ExecutionContext &context,
	                             OperatorSinkInput &sink) {
		if (!accumulated) {
			return;
		}
		auto &prefixed = finalize_state.prefixed;
		FlatVector::SetSize(prefixed.data[0], count_t(accumulated));
		auto &entries = StructVector::GetEntries(finalize_state.rows.data[0]);
		for (column_t col_idx = 0; col_idx < entries.size(); ++col_idx) {
			prefixed.data[col_idx + 1].Reference(entries[col_idx]);
			FlatVector::SetSize(prefixed.data[col_idx + 1], count_t(accumulated));
		}
		order_bind.sort->Sink(context, prefixed, sink);
		finalize_state.rows.Reset();
		accumulated = 0;
	}

	//! Buffers the rows of the state into the cached rows chunk, prefixed with the group number, flushing into
	//! the sort whenever the chunk fills up - this batches many small groups into a single sink call
	static void SinkState(const SortedAggregateBindData &order_bind, SortedAggregateState &state,
	                      const idx_t group_number, idx_t &accumulated, SortedAggregateFinalizeState &finalize_state,
	                      ExecutionContext &context, OperatorSinkInput &sink) {
		const auto group_count = state.linked_list.total_capacity;
		if (!group_count) {
			return;
		}
		auto &rows = finalize_state.rows.data[0];
		auto group_numbers = FlatVector::GetDataMutable<uint16_t>(finalize_state.prefixed.data[0]);
		if (group_count <= STANDARD_VECTOR_SIZE) {
			//	The group fits in the rows chunk - flush first if there is not enough space left
			if (accumulated + group_count > STANDARD_VECTOR_SIZE) {
				FlushAccumulated(order_bind, accumulated, finalize_state, context, sink);
			}
			//	Append the group's rows to the accumulated rows
			order_bind.buffered_funcs.BuildListVector(state.linked_list, rows, accumulated);
			for (idx_t i = 0; i < group_count; ++i) {
				group_numbers[accumulated + i] = UnsafeNumericCast<uint16_t>(group_number);
			}
			accumulated += group_count;
		} else {
			//	The group does not fit in a single chunk - flush, then stream it chunk at a time
			FlushAccumulated(order_bind, accumulated, finalize_state, context, sink);
			ListSegmentScanState scan_state;
			order_bind.buffered_funcs.InitializeScan(state.linked_list, scan_state);
			for (;;) {
				const auto chunk_count = order_bind.buffered_funcs.Scan(scan_state, rows);
				if (!chunk_count) {
					break;
				}
				for (idx_t i = 0; i < chunk_count; ++i) {
					group_numbers[i] = UnsafeNumericCast<uint16_t>(group_number);
				}
				accumulated = chunk_count;
				FlushAccumulated(order_bind, accumulated, finalize_state, context, sink);
			}
		}
		//	Release the state - the rows are freed with the arena allocator
		state.linked_list = LinkedList();
	}

	static void FinalizeGroups(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result,
	                           idx_t count, const idx_t offset) {
		auto &order_bind = finalize_input_data.bind_data->Cast<SortedAggregateBindData>();
		auto &client = order_bind.context;

		//	The local state holds the chunks and contexts - callers can keep it alive across finalize calls
		//	so they do not have to be re-instantiated for every finalize call
		D_ASSERT(finalize_input_data.local_state);
		auto &finalize_state = finalize_input_data.local_state->Cast<SortedAggregateFinalizeState>();
		auto &scanned = finalize_state.scanned;
		auto &sliced = finalize_state.sliced;
		auto &agg_state = finalize_state.agg_state;
		auto &agg_state_vec = finalize_state.agg_state_vec;
		auto &context = finalize_state.context;
		auto &interrupt = finalize_state.interrupt;

		// State variables
		auto &aggr = order_bind.function;
		auto bind_info = order_bind.bind_info.get();
		AggregateFinalizeInputData aggr_bind_info(aggr, bind_info, finalize_input_data.allocator,
		                                          finalize_state.inner_local_state.get());

		// Inner aggregate APIs
		auto initialize = aggr.GetCallbacks().GetStateInitCallback();
		auto destructor = aggr.GetCallbacks().GetStateDestructorCallback();
		auto cluster_update = aggr.GetCallbacks().GetStateClusterUpdateCallback();
		auto update = aggr.GetCallbacks().GetStateUpdateCallback();
		auto finalize = aggr.GetCallbacks().GetStateFinalizeCallback();
		AggregateStateInput state_input(aggr, bind_info);
		data_ptr_t agg_state_ptr = agg_state.data();

		//	The inner aggregate (or the sort) can throw while its state is initialized - destroy it in that case
		struct InnerStateGuard {
			~InnerStateGuard() {
				if (live && destructor) {
					try {
						destructor(state_vec, input, 1);
					} catch (...) { // NOLINT
					}
				}
			}
			aggregate_destructor_t destructor;
			Vector &state_vec;
			AggregateInputData &input;
			bool live;
		};
		InnerStateGuard inner_state_guard {destructor, agg_state_vec, aggr_bind_info, false};
		auto initialize_inner_state = [&]() {
			initialize(state_input, &agg_state_ptr, 1);
			inner_state_guard.live = true;
		};
		auto finalize_inner_state = [&](idx_t result_idx) {
			agg_state_vec.SetVectorType(states.GetVectorType());
			finalize(agg_state_vec, aggr_bind_info, result, 1, result_idx);
			if (destructor) {
				destructor(agg_state_vec, aggr_bind_info, 1);
			}
			inner_state_guard.live = false;
		};

		auto sdata = states.Values<SortedAggregateState *>();

		vector<idx_t> state_unprocessed(count, 0);
		for (idx_t i = 0; i < count; ++i) {
			state_unprocessed[i] = sdata[i].GetValueUnsafe()->linked_list.total_capacity;
		}

		auto &sort = order_bind.sort;
		auto global_sink = sort->GetGlobalSinkState(client);
		auto local_sink = sort->GetLocalSinkState(context);

		//	Go through the states accumulating values to sort until we hit the sort threshold
		idx_t unsorted_count = 0;
		idx_t sorted = 0;
		idx_t accumulated = 0;
		for (idx_t finalized = 0; finalized < count;) {
			if (unsorted_count < order_bind.threshold) {
				auto state = sdata[finalized].GetValueUnsafe();
				OperatorSinkInput sink {*global_sink, *local_sink, interrupt};
				SinkState(order_bind, *state, finalized, accumulated, finalize_state, context, sink);
				unsorted_count += state_unprocessed[finalized];

				// Go to the next aggregate unless this is the last one
				if (++finalized < count) {
					continue;
				}
			}

			//	Sink any remaining accumulated rows before sorting
			{
				OperatorSinkInput sink {*global_sink, *local_sink, interrupt};
				FlushAccumulated(order_bind, accumulated, finalize_state, context, sink);
			}

			//	If they were all empty (filtering) flush them
			//	(This can only happen on the last range)
			if (!unsorted_count) {
				break;
			}

			//	Sort all the data
			OperatorSinkCombineInput combine {*global_sink, *local_sink, interrupt};
			order_bind.sort->Combine(context, combine);

			OperatorSinkFinalizeInput finalize_input {*global_sink, interrupt};
			order_bind.sort->Finalize(client, finalize_input);

			auto global_source = sort->GetGlobalSourceState(client, *global_sink);
			auto local_source = sort->GetLocalSourceState(context, *global_source);

			initialize_inner_state();
			for (;;) {
				OperatorSourceInput source {*global_source, *local_source, interrupt};
				scanned.Reset();
				if (sort->GetData(context, scanned, source) == SourceResultType::FINISHED) {
					break;
				}
				idx_t consumed = 0;

				// Distribute the scanned chunk to the aggregates
				while (consumed < scanned.size()) {
					//	Find the next aggregate that needs data
					for (; !state_unprocessed[sorted]; ++sorted) {
						// Finalize a single value at the next offset
						finalize_inner_state(sorted + offset);
						initialize_inner_state();
					}
					const auto input_count = MinValue(state_unprocessed[sorted], scanned.size() - consumed);
					for (column_t col_idx = 0; col_idx < scanned.ColumnCount(); ++col_idx) {
						sliced.data[col_idx].Slice(scanned.data[col_idx], consumed, consumed + input_count);
					}

					if (cluster_update) {
						ClusteredAggr clustered;
						clustered.SetSingleRun(agg_state.data(), sliced.size());
						aggr_bind_info.clustered = &clustered;
						cluster_update(sliced.data.data(), aggr_bind_info, sliced.data.size(), clustered,
						               sliced.size());
						aggr_bind_info.clustered = nullptr;
					} else {
						// We are only updating a constant state
						agg_state_vec.SetVectorType(VectorType::CONSTANT_VECTOR);
						update(sliced.data.data(), aggr_bind_info, sliced.data.size(), agg_state_vec, sliced.size());
					}

					consumed += input_count;
					state_unprocessed[sorted] -= input_count;
				}
			}

			//	Finalize the last state for this sort
			finalize_inner_state(sorted + offset);
			++sorted;

			//	Stop if we are done
			if (finalized >= count) {
				break;
			}

			//	Create a new sort
			global_sink = sort->GetGlobalSinkState(client);
			local_sink = sort->GetLocalSinkState(context);
			unsorted_count = 0;
		}

		for (; sorted < count; ++sorted) {
			initialize_inner_state();
			// Finalize a single value at the next offset
			finalize_inner_state(sorted + offset);
		}

		result.Verify();
	}

	static void Finalize(Vector &states, AggregateFinalizeInputData &finalize_input_data, Vector &result, idx_t count,
	                     const idx_t offset) {
		//	Internally we can only handle 2^16 rows, so chunk them up
		const idx_t group_limit = NumericLimits<uint16_t>::Maximum();
		for (auto group_offset = offset; group_offset < count; group_offset += group_limit) {
			const auto group_count = MinValue<idx_t>(group_limit, count - group_offset);
			FinalizeGroups(states, finalize_input_data, result, group_count, group_offset);
		}
	}
};

//! The exported state of a sorted aggregate is its buffer of values: a LIST<buffered_struct>.
AggregateStateLayout SortedAggregateGetStateType(AggregateLayoutInput &input) {
	auto &bind_data = input.bind_data->Cast<SortedAggregateBindData>();
	AggregateStateLayout layout;
	layout.type = LogicalType::LIST(bind_data.buffered_struct_type);
	layout.total_state_size = AlignValue<idx_t>(sizeof(SortedAggregateState));
	layout.field = BuildStateField<StateListType<StateReturnType>>();
	AggregateStateField::PopulateListFunctions(layout.type, layout.field);
	// an empty buffer is an empty group: export it as a non-NULL empty list so finalize runs the wrapped
	// aggregate's empty-group finalize (e.g. count -> 0) instead of returning NULL on a NULL state
	layout.field.list_empty_as_empty = true;
	return layout;
}

//! Builds the sorted aggregate wrapper AggregateFunction (shared by the forward export path and the re-bind path).
AggregateFunction CreateSortedAggregateWrapper(const Identifier &name, const vector<LogicalType> &arguments,
                                               const LogicalType &return_type,
                                               const FunctionProperties &inner_properties) {
	AggregateFunction ordered_aggregate(
	    name, arguments, return_type, AggregateFunction::StateSize<SortedAggregateState>,
	    AggregateFunction::StateInitialize<SortedAggregateState, ListFunction>, SortedAggregateFunction::ScatterUpdate,
	    ListCombineFunction<SortedAggregateFunction>, SortedAggregateFunction::Finalize,
	    inner_properties.GetNullHandling(), nullptr, nullptr, nullptr, nullptr, SortedAggregateFunction::WindowBatch);
	// the wrapper only buffers and sorts: it throws and is volatile exactly when the inner aggregate is
	ordered_aggregate.SetErrorMode(inner_properties.GetErrorMode());
	ordered_aggregate.SetStability(inner_properties.GetStability());
	ordered_aggregate.SetInitLocalStateFinalizeCallback(SortedAggregateFinalizeState::Init);
	ordered_aggregate.SetStructStateExport(SortedAggregateGetStateType);
	return ordered_aggregate;
}

} // namespace

void FunctionBinder::GetSortedAggregateStateLayout(const BoundAggregateExpression &expr, LogicalType &buffer_struct,
                                                   vector<SortedAggregateStateOrder> &orders, idx_t &argument_count) {
	D_ASSERT(expr.GetOrderBys());
	argument_count = expr.GetChildren().size();
	buffer_struct = BufferStructType(MapSortedColumns(expr.GetChildren(), expr.GetOrderBys()->orders, orders));
}

pair<AggregateFunction, unique_ptr<FunctionData>>
FunctionBinder::BindSortedAggregateState(ClientContext &context, const BoundAggregateFunction &inner_function,
                                         unique_ptr<FunctionData> inner_bind_info, const LogicalType &buffer_struct,
                                         const vector<SortedAggregateStateOrder> &orders, idx_t argument_count) {
	// the leading buffered columns are passed to the inner aggregate as-is - they must have its argument types
	auto &buffer_columns = StructType::GetChildTypes(buffer_struct);
	auto &inner_arguments = inner_function.GetArguments();
	if (argument_count != inner_arguments.size() || argument_count > buffer_columns.size()) {
		throw BinderException("Aggregate state for %s has %llu state columns, expected at least %llu",
		                      inner_function.GetName(), (uint64_t)buffer_columns.size(),
		                      (uint64_t)inner_arguments.size());
	}
	for (idx_t i = 0; i < argument_count; i++) {
		if (inner_arguments[i].IsComplete() && buffer_columns[i].second != inner_arguments[i]) {
			throw BinderException("Aggregate state for %s has state column %llu of type %s, expected %s",
			                      inner_function.GetName(), (uint64_t)i, buffer_columns[i].second.ToString(),
			                      inner_arguments[i].ToString());
		}
	}
	auto bind_data = make_uniq<SortedAggregateBindData>(context, inner_function, std::move(inner_bind_info),
	                                                    buffer_struct, orders, argument_count);
	// the wrapper consumes the buffered columns (the struct fields)
	vector<LogicalType> arguments;
	for (const auto &child : StructType::GetChildTypes(buffer_struct)) {
		arguments.emplace_back(child.second);
	}
	auto result_function = CreateSortedAggregateWrapper(inner_function.GetName(), arguments,
	                                                    inner_function.GetReturnType(), inner_function.GetProperties());
	return make_pair(std::move(result_function), std::move(bind_data));
}

void FunctionBinder::BindSortedAggregate(ClientContext &context, BoundAggregateExpression &expr,
                                         const vector<unique_ptr<Expression>> &groups,
                                         optional_ptr<vector<GroupingSet>> grouping_sets) {
	if (!expr.GetOrderBys() || expr.GetOrderBys()->orders.empty() || expr.GetChildren().empty()) {
		// not a sorted aggregate: return
		return;
	}
	// the exported state buffers the values, so the ORDER BY must be preserved verbatim - skip the simplification
	const bool state_export = expr.StateExportMode() == AggregateStateExportMode::STATE_EXPORT;
	if (!state_export && Settings::Get<EnableOptimizerSetting>(context)) {
		if (expr.GetOrderBysMutable()->Simplify(groups, grouping_sets)) {
			expr.GetOrderBysMutable().reset();
			return;
		}
	}
	auto &bound_function = expr.Function();
	auto &children = expr.GetChildrenMutable();
	auto &order_bys = *expr.GetOrderBysMutable();

	// the layout of an exported state was fixed at bind time and is part of its type - keep it, as the optimizer may
	// have changed the expressions since (e.g. so that an ORDER BY key no longer matches the argument it matched)
	pair<LogicalType, vector<SortedAggregateStateOrder>> export_layout;
	if (state_export) {
		export_layout.first = ListType::GetChildType(expr.GetReturnType());
		auto &logical_fields = StructType::GetChildTypes(export_layout.first);
		ExportAggregateFunction::GetSortedStateOrders(expr.GetReturnType(), export_layout.second);
		if (export_layout.second.size() != order_bys.orders.size() || children.size() > logical_fields.size()) {
			throw InternalException("Exported state layout of ordered aggregate %s does not match its ORDER BY",
			                        bound_function.GetName());
		}
		// the statistics optimizer may have narrowed the buffered column types since the exported type was fixed at
		// bind time (e.g. a small-range group column INTEGER->TINYINT) - cast them back so the buffer matches its
		// declared type. The widening preserves sort order.
		auto cast_to = [&](unique_ptr<Expression> &e, const LogicalType &type) {
			if (e->GetReturnType() != type) {
				e = BoundCastExpression::AddCastToType(context, std::move(e), type);
			}
		};
		for (idx_t i = 0; i < children.size(); i++) {
			cast_to(children[i], logical_fields[i].second);
		}
		for (idx_t o = 0; o < order_bys.orders.size(); o++) {
			cast_to(order_bys.orders[o].expression, logical_fields[export_layout.second[o].column].second);
		}
	}

	auto sorted_bind = make_uniq<SortedAggregateBindData>(context, expr, state_export ? &export_layout : nullptr);

	if (!sorted_bind->sorted_on_args) {
		// The arguments are the children plus the sort columns.
		for (auto &order : order_bys.orders) {
			children.emplace_back(std::move(order.expression));
		}
	}

	vector<LogicalType> arguments;
	arguments.reserve(children.size());
	for (const auto &child : children) {
		arguments.emplace_back(child->GetReturnType());
	}

	// Replace the aggregate with the wrapper
	auto ordered_aggregate = CreateSortedAggregateWrapper(
	    bound_function.GetName(), arguments, bound_function.GetReturnType(), bound_function.GetProperties());

	expr.FunctionMutable().ReplaceImplementation(ordered_aggregate);
	expr.BindInfoMutable() = std::move(sorted_bind);
	expr.GetOrderBysMutable().reset();

	if (state_export) {
		// wire the export onto the wrapper - the AGGREGATE_STATE return type was already set at bind time
		ExportAggregateFunction::SetStateExport(expr, expr.GetReturnType());
	}
}

void FunctionBinder::BindSortedAggregate(ClientContext &context, BoundWindowExpression &expr) {
	//	Make implicit orderings explicit
	auto &aggregate = *expr.AggregateFunction();
	if (aggregate.GetOrderDependent() == AggregateOrderDependent::ORDER_DEPENDENT && expr.ArgOrders().empty()) {
		for (auto &order : expr.OrderBy()) {
			const auto type = order.type;
			const auto null_order = order.null_order;
			auto expression = order.expression->Copy();
			expr.ArgOrdersMutable().emplace_back(type, null_order, std::move(expression));
		}
	}

	if (expr.ArgOrders().empty() || expr.GetChildren().empty()) {
		// not a sorted aggregate: return
		return;
	}
	// Remove unnecessary ORDER BY clauses and return if nothing remains
	if (Settings::Get<EnableOptimizerSetting>(context)) {
		if (BoundOrderModifier::Simplify(expr.ArgOrdersMutable(), expr.PartitionsMutable(), nullptr)) {
			expr.ArgOrdersMutable().clear();
			return;
		}
	}
	auto &children = expr.GetChildrenMutable();
	auto &arg_orders = expr.ArgOrdersMutable();
	auto sorted_bind = make_uniq<SortedAggregateBindData>(context, expr);

	if (!sorted_bind->sorted_on_args) {
		// The arguments are the children plus the sort columns.
		for (auto &order : arg_orders) {
			children.emplace_back(std::move(order.expression));
		}
	}

	vector<LogicalType> arguments;
	arguments.reserve(children.size());
	for (const auto &child : children) {
		arguments.emplace_back(child->GetReturnType());
	}

	// Replace the aggregate with the wrapper
	AggregateFunction ordered_aggregate(
	    aggregate.GetName(), arguments, aggregate.GetReturnType(), AggregateFunction::StateSize<SortedAggregateState>,
	    AggregateFunction::StateInitialize<SortedAggregateState, ListFunction>, SortedAggregateFunction::ScatterUpdate,
	    ListCombineFunction<SortedAggregateFunction>, SortedAggregateFunction::Finalize,
	    aggregate.GetProperties().GetNullHandling(), nullptr, nullptr, nullptr, nullptr,
	    SortedAggregateFunction::WindowBatch);
	// the wrapper only buffers and sorts: it throws and is volatile exactly when the inner aggregate is
	ordered_aggregate.SetErrorMode(aggregate.GetErrorMode());
	ordered_aggregate.SetStability(aggregate.GetStability());
	ordered_aggregate.SetWindowCallback(SortedAggregateFunction::Window);
	ordered_aggregate.SetInitLocalStateFinalizeCallback(SortedAggregateFinalizeState::Init);

	aggregate.ReplaceImplementation(ordered_aggregate);
	expr.BindInfoMutable() = std::move(sorted_bind);
	expr.ArgOrdersMutable().clear();
}

} // namespace duckdb
