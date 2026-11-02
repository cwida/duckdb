#include "core_functions/aggregate/nested_functions.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/owning_string_map.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/sql_value_map.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

namespace {

// FIXME: benchmark the use of cluster_update against using update (if applicable)

unique_ptr<FunctionData> ListAggregatesBindFailure(BoundScalarFunction &bound_function) {
	bound_function.GetArguments()[0] = LogicalType::SQLNULL;
	bound_function.SetReturnType(LogicalType::SQLNULL);
	// no bind data, like the other path that binds nothing: the bind data of this function is always a
	// ListAggregatesBindData, so anything else here cannot be told apart from one
	return nullptr;
}

struct ListAggregatesBindData : public FunctionData {
	ListAggregatesBindData(const LogicalType &stype_p, unique_ptr<Expression> aggr_expr_p);
	~ListAggregatesBindData() override;

	LogicalType stype;
	unique_ptr<Expression> aggr_expr;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ListAggregatesBindData>(stype, aggr_expr->Copy());
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ListAggregatesBindData>();
		return stype == other.stype && aggr_expr->Equals(*other.aggr_expr);
	}
	void Serialize(Serializer &serializer) const {
		serializer.WriteProperty(1, "stype", stype);
		serializer.WriteProperty(2, "aggr_expr", aggr_expr);
	}
	static unique_ptr<ListAggregatesBindData> Deserialize(Deserializer &deserializer) {
		auto stype = deserializer.ReadProperty<LogicalType>(1, "stype");
		auto aggr_expr = deserializer.ReadProperty<unique_ptr<Expression>>(2, "aggr_expr");
		auto result = make_uniq<ListAggregatesBindData>(std::move(stype), std::move(aggr_expr));
		return result;
	}

	static void SerializeFunction(Serializer &serializer, const optional_ptr<FunctionData> bind_data_p,
	                              const BoundScalarFunction &function) {
		const ListAggregatesBindData *bind_data = nullptr;
		if (bind_data_p) {
			bind_data = &bind_data_p->Cast<ListAggregatesBindData>();
		}
		serializer.WritePropertyWithDefault(100, "bind_data", bind_data, (const ListAggregatesBindData *)nullptr);
	}

	static unique_ptr<FunctionData> DeserializeFunction(Deserializer &deserializer,
	                                                    BoundScalarFunction &bound_function) {
		auto result = deserializer.ReadPropertyWithExplicitDefault<unique_ptr<ListAggregatesBindData>>(
		    100, "bind_data", unique_ptr<ListAggregatesBindData>(nullptr));
		if (!result) {
			return ListAggregatesBindFailure(bound_function);
		}
		return std::move(result);
	}
};

ListAggregatesBindData::ListAggregatesBindData(const LogicalType &stype_p, unique_ptr<Expression> aggr_expr_p)
    : stype(stype_p), aggr_expr(std::move(aggr_expr_p)) {
}

ListAggregatesBindData::~ListAggregatesBindData() {
}

struct ListAggregatesLocalState : public FunctionLocalState {
	ListAggregatesLocalState(ClientContext &context, optional_ptr<FunctionData> bind_data)
	    : arena_allocator(BufferAllocator::Get(context)) {
		if (!bind_data) {
			return;
		}
		// arguments that the bind of the aggregate derived from its input (e.g. a collation of the compared value) are
		// evaluated over the list elements
		auto &aggr = bind_data->Cast<ListAggregatesBindData>().aggr_expr->Cast<BoundAggregateExpression>();
		auto &children = aggr.GetChildren();
		for (idx_t child_idx = 1; child_idx < children.size(); child_idx++) {
			if (children[child_idx]->GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
				continue;
			}
			if (!derived_executor) {
				derived_executor = make_uniq<ExpressionExecutor>(context);
			}
			derived_executor->AddExpression(*children[child_idx]);
		}
	}

	ArenaAllocator arena_allocator;
	unique_ptr<ExpressionExecutor> derived_executor;
};

unique_ptr<FunctionLocalState> ListAggregatesInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                            FunctionData *bind_data) {
	return make_uniq<ListAggregatesLocalState>(state.GetContext(), bind_data);
}

struct StateVector {
	StateVector(idx_t count_p, unique_ptr<Expression> aggr_expr_p)
	    : count(count_p), aggr_expr(std::move(aggr_expr_p)), state_vector(Vector(LogicalType::POINTER, count_p)) {
	}

	~StateVector() { // NOLINT
		// destroy objects within the aggregate states
		auto &aggr = aggr_expr->Cast<BoundAggregateExpression>();
		if (initialized_count != 0 && aggr.Function().HasStateDestructorCallback()) {
			ArenaAllocator allocator(Allocator::DefaultAllocator());
			AggregateInputData aggr_input_data(aggr, allocator);
			aggr.Function().GetStateDestructorCallback()(state_vector, aggr_input_data, initialized_count);
		}
	}

	idx_t count;
	//! The number of states that have been initialized so far. The vector holds uninitialized memory until then,
	//! so an exception during initialization must not leave the destructor reading it as state pointers.
	idx_t initialized_count = 0;
	unique_ptr<Expression> aggr_expr;
	Vector state_vector;
};

struct FinalizeValueFunctor {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		FlatVector::GetDataMutable<T>(result)[offset] = value;
	}
};

struct FinalizeStringValueFunctor {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		FlatVector::GetDataMutable<string_t>(result)[offset] = StringVector::AddStringOrBlob(result, value);
	}
};

struct FinalizeGenericValueFunctor {
	template <class T>
	static void HistogramFinalize(T value, Vector &result, idx_t offset) {
		CreateSortKeyHelpers::DecodeSortKey(value, result, offset,
		                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
	}
};

struct AggregateFunctor {
	template <class OP, class T, class MAP_TYPE = sql_value_map_t<T, idx_t>>
	static void ListExecuteFunction(Vector &result, const Vector &state_vector, idx_t count) {
	}
};

struct DistinctFunctor {
	template <class OP, class T, class MAP_TYPE = sql_value_map_t<T, idx_t>>
	static void ListExecuteFunction(Vector &result, const Vector &state_vector, idx_t count) {
		UnifiedVectorFormat sdata;
		state_vector.ToUnifiedFormat(sdata);
		auto states = UnifiedVectorFormat::GetData<HistogramAggState<T, MAP_TYPE> *>(sdata);

		auto old_len = ListVector::GetListSize(result);
		idx_t new_entries = 0;
		// figure out how much space we need
		for (idx_t i = 0; i < count; i++) {
			auto &state = *states[sdata.sel->get_index(i)];
			if (!state.hist) {
				continue;
			}
			new_entries += state.hist->size();
		}
		// reserve space in the list vector
		ListVector::Reserve(result, old_len + new_entries);
		auto &child_elements = ListVector::GetChildMutable(result);
		auto list_entries = FlatVector::Writer<list_entry_t>(result, count);

		idx_t current_offset = old_len;
		for (idx_t i = 0; i < count; i++) {
			auto &state = *states[sdata.sel->get_index(i)];
			const idx_t entry_offset = current_offset;
			if (!state.hist) {
				list_entries.WriteValue(list_entry_t(entry_offset, 0));
				continue;
			}

			for (auto &entry : *state.hist) {
				OP::template HistogramFinalize<T>(entry.first, child_elements, current_offset);
				current_offset++;
			}
			list_entries.WriteValue(list_entry_t(entry_offset, current_offset - entry_offset));
		}
		D_ASSERT(current_offset == old_len + new_entries);
		ListVector::SetListSize(result, current_offset);
		result.Verify();
	}
};

struct UniqueFunctor {
	template <class OP, class T, class MAP_TYPE = sql_value_map_t<T, idx_t>>
	static void ListExecuteFunction(Vector &result, const Vector &state_vector, idx_t count) {
		UnifiedVectorFormat sdata;
		state_vector.ToUnifiedFormat(sdata);
		auto states = UnifiedVectorFormat::GetData<HistogramAggState<T, MAP_TYPE> *>(sdata);

		auto result_data = FlatVector::Writer<uint64_t>(result, count);
		for (idx_t i = 0; i < count; i++) {
			auto state = states[sdata.sel->get_index(i)];

			if (!state->hist) {
				result_data.WriteValue(0);
				continue;
			}
			result_data.WriteValue(state->hist->size());
		}
		result.Verify();
	}
};

template <class FUNCTION_FUNCTOR, bool IS_AGGR = false>
void ListAggregatesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	Vector &lists = args.data[0];

	// set the result vector
	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto &result_validity = FlatVector::ValidityMutable(result);

	if (lists.GetType().id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(count));
		return;
	}

	// get the aggregate function
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ListAggregatesBindData>();
	auto &aggr = info.aggr_expr->Cast<BoundAggregateExpression>();
	auto &local_state = ExecuteFunctionState::GetFunctionState(state)->Cast<ListAggregatesLocalState>();
	auto &allocator = local_state.arena_allocator;
	allocator.Reset();
	AggregateFinalizeInputData aggr_input_data(aggr, allocator);

	D_ASSERT(aggr.Function().HasStateUpdateCallback());

	auto &child_vector = ListVector::GetChildMutable(lists);
	child_vector.Flatten();

	UnifiedVectorFormat child_data;
	child_vector.ToUnifiedFormat(child_data);

	UnifiedVectorFormat lists_data;
	lists.ToUnifiedFormat(lists_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(lists_data);

	// state_buffer holds the state for each list of this chunk
	AggregateStateInput state_input(aggr.Function(), aggr.BindInfo().get());
	idx_t size = aggr.Function().GetStateSizeCallback()(state_input);
	auto state_buffer = make_unsafe_uniq_array_uninitialized<data_t>(size * count);

	// state vector for initialize and finalize
	StateVector state_vector(count, info.aggr_expr->Copy());
	auto states = FlatVector::GetDataMutable<data_ptr_t>(state_vector.state_vector);

	// state vector of STANDARD_VECTOR_SIZE holds the pointers to the states
	Vector state_vector_update = Vector(LogicalType::POINTER);
	auto states_update = FlatVector::GetDataMutable<data_ptr_t>(state_vector_update);

	// selection vector pointing to the data
	SelectionVector sel_vector(STANDARD_VECTOR_SIZE);

	// the aggregate's trailing arguments (e.g. the separator of string_agg) are constants that its bind folded into
	// the bind data - they are still part of its argument list, so they are passed along as constant vectors
	DataChunk derived_input;
	DataChunk derived_result;
	if (local_state.derived_executor) {
		derived_input.InitializeEmpty({child_vector.GetType()});
		vector<LogicalType> derived_types;
		for (auto &derived_expr : local_state.derived_executor->expressions) {
			derived_types.push_back(derived_expr->GetReturnType());
		}
		derived_result.Initialize(Allocator::DefaultAllocator(), derived_types);
	}
	auto update_states = [&](idx_t update_count) {
		vector<Vector> inputs;
		inputs.reserve(aggr.GetChildren().size());
		inputs.emplace_back(child_vector, sel_vector, update_count);
		if (local_state.derived_executor) {
			derived_input.data[0].Slice(child_vector, sel_vector, update_count);
			derived_input.SetChildCardinality(update_count);
			derived_result.Reset();
			local_state.derived_executor->Execute(derived_input, derived_result);
		}
		idx_t derived_idx = 0;
		for (idx_t child_idx = 1; child_idx < aggr.GetChildren().size(); child_idx++) {
			auto &child = *aggr.GetChildren()[child_idx];
			if (child.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
				inputs.emplace_back(Vector::Ref(derived_result.data[derived_idx++]));
				continue;
			}
			auto &constant = child.Cast<BoundConstantExpression>().GetValue();
			inputs.emplace_back(constant, count_t(update_count));
		}
		aggr.Function().GetStateUpdateCallback()(inputs.data(), aggr_input_data, inputs.size(), state_vector_update,
		                                         update_count);
	};
	idx_t states_idx = 0;

	for (idx_t i = 0; i < count; i++) {
		// initialize the state for this list
		auto state_ptr = state_buffer.get() + size * i;
		states[i] = state_ptr;
		aggr.Function().GetStateInitCallback()(state_input, &states[i], 1);
		state_vector.initialized_count = i + 1;

		auto lists_index = lists_data.sel->get_index(i);
		const auto &list_entry = list_entries[lists_index];

		// nothing to do for this list
		if (!lists_data.validity.RowIsValid(lists_index)) {
			result_validity.SetInvalid(i);
			continue;
		}

		// skip empty list
		if (list_entry.length == 0) {
			continue;
		}

		for (idx_t child_idx = 0; child_idx < list_entry.length; child_idx++) {
			// states vector is full, update
			if (states_idx == STANDARD_VECTOR_SIZE) {
				// update the aggregate state(s)
				update_states(states_idx);

				// reset values
				states_idx = 0;
			}

			auto source_idx = child_data.sel->get_index(list_entry.offset + child_idx);
			sel_vector.set_index(states_idx, source_idx);
			states_update[states_idx] = state_ptr;
			states_idx++;
		}
	}

	// update the remaining elements of the last list(s)
	if (states_idx != 0) {
		update_states(states_idx);
	}

	if (IS_AGGR) {
		// finalize all the aggregate states
		aggr.Function().GetStateFinalizeCallback()(state_vector.state_vector, aggr_input_data, result, count, 0);

	} else {
		// finalize manually to use the map
		D_ASSERT(aggr.Function().GetArguments().size() == 1);
		auto key_type = aggr.Function().GetArguments()[0];

		switch (key_type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(list_aggregate_types)
		case PhysicalType::BOOL:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, bool>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::UINT8:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, uint8_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::UINT16:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, uint16_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::UINT32:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, uint32_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::UINT64:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, uint64_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::INT8:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, int8_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::INT16:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, int16_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::INT32:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, int32_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::INT64:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, int64_t>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::FLOAT:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, float>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::DOUBLE:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeValueFunctor, double>(
			    result, state_vector.state_vector, count);
			break;
		case PhysicalType::VARCHAR:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeStringValueFunctor, string_t,
			                                               OwningStringMap<idx_t>>(result, state_vector.state_vector,
			                                                                       count);
			break;
#endif
		default:
			FUNCTION_FUNCTOR::template ListExecuteFunction<FinalizeGenericValueFunctor, string_t,
			                                               OwningStringMap<idx_t>>(result, state_vector.state_vector,
			                                                                       count);
			break;
		}
	}
}

void ListAggregateFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() >= 2);
	ListAggregatesFunction<AggregateFunctor, true>(args, state, result);
}

void ListDistinctFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);
	ListAggregatesFunction<DistinctFunctor>(args, state, result);
}

void ListUniqueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);
	ListAggregatesFunction<UniqueFunctor>(args, state, result);
}

AggregateFunctionCatalogEntry &GetListAggregateEntry(ClientContext &context, const string &function_name) {
	auto &func = Catalog::GetSystemCatalog(context).GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(Catalog::GetSystemCatalog(context).GetName(), Identifier::DefaultSchema(),
	                           Identifier(function_name)));
	D_ASSERT(func.type == CatalogType::AGGREGATE_FUNCTION_ENTRY);
	return func;
}

//! Binds the aggregate that is applied to the elements of the list
template <bool IS_AGGR>
unique_ptr<BoundAggregateExpression> BindListAggregate(ClientContext &context, LogicalType child_type,
                                                       const string &function_name,
                                                       vector<unique_ptr<Expression>> extra_arguments) {
	auto &func = GetListAggregateEntry(context, function_name);

	// the child expression refers to the list elements
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(child_type, idx_t(0)));
	for (auto &argument : extra_arguments) {
		children.push_back(std::move(argument));
	}

	FunctionBinder function_binder(context);
	if (!IS_AGGR) {
		// the histogram is computed as an unordered map, which takes any type
		return function_binder.BindAggregateFunction(HistogramFun::GetHistogramUnorderedMap(child_type),
		                                             std::move(children));
	}
	ErrorData error;
	auto result = function_binder.BindAggregateFunction(func, std::move(children), error);
	if (!result) {
		throw BinderException("No matching aggregate function\n%s", error.Message());
	}
	return result;
}

template <bool IS_AGGR = false>
void ListAggregatesResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto list_type = input.GetArgumentType(0);
	if (list_type.id() == LogicalTypeId::ARRAY) {
		list_type = LogicalType::LIST(ArrayType::GetChildType(list_type));
	}

	if (list_type.id() == LogicalTypeId::SQLNULL) {
		ListAggregatesBindFailure(bound_function);
		return;
	}
	auto function_name = IS_AGGR ? input.GetConstant(1).ToString() : string("histogram");
	if (list_type.id() == LogicalTypeId::UNKNOWN) {
		// verify the aggregate exists, even if we cannot bind it yet
		GetListAggregateEntry(context, function_name);
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return;
	}
	if (list_type.id() != LogicalTypeId::LIST && list_type.id() != LogicalTypeId::MAP) {
		// Unreachable
		throw InvalidInputException("First argument of list aggregate must be a list, map or array");
	}

	vector<unique_ptr<Expression>> extra_arguments;
	for (idx_t i = 2; i < input.GetArgumentCount(); i++) {
		extra_arguments.push_back(input.GetArgument(i).Copy());
	}
	auto bound_aggr_function = BindListAggregate<IS_AGGR>(context, ListType::GetChildType(list_type), function_name,
	                                                      std::move(extra_arguments));

	bound_function.GetArguments()[0] = LogicalType::LIST(bound_aggr_function->Function().GetArguments()[0]);
	if (IS_AGGR) {
		if (bound_aggr_function->Function().GetErrorMode() == FunctionErrors::CAN_THROW_RUNTIME_ERROR) {
			// never clear the error mode here - executing the aggregate can throw regardless of how it is declared
			bound_function.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
		}
		bound_function.SetReturnType(bound_aggr_function->Function().GetReturnType());
	}
}

template <bool IS_AGGR = false>
unique_ptr<FunctionData> ListAggregatesBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto list_type_id = arguments[0]->GetReturnType().id();
	if (list_type_id == LogicalTypeId::SQLNULL || list_type_id == LogicalTypeId::UNKNOWN) {
		// no bind data, the bind data of this function is always a ListAggregatesBindData
		return nullptr;
	}

	// the extra arguments are pushed into the list aggregate bind
	vector<unique_ptr<Expression>> extra_arguments;
	for (idx_t i = 2; i < arguments.size(); i++) {
		extra_arguments.push_back(std::move(arguments[i]));
	}
	const idx_t extra_argument_count = extra_arguments.size();
	if (arguments.size() > 2) {
		arguments.resize(2);
	}
	auto function_name = IS_AGGR ? input.GetConstant(1).ToString() : string("histogram");
	auto child_type = ListType::GetChildType(arguments[0]->GetReturnType());
	auto bound_aggr_function =
	    BindListAggregate<IS_AGGR>(context, child_type, function_name, std::move(extra_arguments));

	// the extra arguments are passed to the aggregate as constant vectors, so they have to be constant - arguments
	// that the bind of the aggregate added after them are derived from the list elements
	auto &aggr_children = bound_aggr_function->GetChildrenMutable();
	for (idx_t child_idx = 1; child_idx < aggr_children.size() && child_idx <= extra_argument_count; child_idx++) {
		auto &child = aggr_children[child_idx];
		if (!child->IsFoldable()) {
			throw InvalidInputException(
			    "Aggregate function %s is not supported for list_aggr: extra arguments must be constant",
			    bound_aggr_function->ToString());
		}
		child = make_uniq<BoundConstantExpression>(ExpressionExecutor::EvaluateScalar(context, *child));
	}

	// Preserve folded SQL arguments without evaluating them again.
	if (extra_argument_count + 2 == bound_function.GetLogicalArguments().size()) {
		for (idx_t child_idx = 1; child_idx <= extra_argument_count && child_idx < aggr_children.size(); child_idx++) {
			arguments.push_back(aggr_children[child_idx]->Copy());
		}
	}

	return make_uniq<ListAggregatesBindData>(bound_function.GetReturnType(), std::move(bound_aggr_function));
}

unique_ptr<FunctionData> ListAggregateBind(BindScalarFunctionInput &input) {
	// the list column and the name of the aggregate function
	D_ASSERT(input.GetBoundFunction().GetArguments().size() >= 2);
	D_ASSERT(input.GetArguments().size() >= 2);

	return ListAggregatesBind<true>(input);
}

} // namespace

ScalarFunction ListAggregateFun::GetFunction() {
	auto result = ScalarFunction({}, LogicalType::ANY, ListAggregateFunction, ListAggregateBind, nullptr,
	                             ListAggregatesInitLocalState);
	result.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("function_name", LogicalType::VARCHAR);
	result.SetFallible();
	result.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	result.GetSignature().AddArgs("args", LogicalType::ANY);
	result.SetResolveTypesCallback(ListAggregatesResolveTypes<true>);
	result.SetSerializeCallback(ListAggregatesBindData::SerializeFunction);
	result.SetDeserializeCallback(ListAggregatesBindData::DeserializeFunction);
	return result;
}

ScalarFunction ListDistinctFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), ListDistinctFunction,
	                   ListAggregatesBind<false>, nullptr, ListAggregatesInitLocalState);
	fun.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::TEMPLATE("T")));
	fun.SetResolveTypesCallback(ListAggregatesResolveTypes<false>);
	return fun;
}

ScalarFunction ListUniqueFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::UBIGINT, ListUniqueFunction, ListAggregatesBind<false>, nullptr,
	                   ListAggregatesInitLocalState);
	fun.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));
	fun.SetResolveTypesCallback(ListAggregatesResolveTypes<false>);
	return fun;
}

} // namespace duckdb
