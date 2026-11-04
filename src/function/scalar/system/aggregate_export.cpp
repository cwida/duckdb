#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/list_segment.hpp"
#include "duckdb/function/aggregate_state_serialization.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/extension_type_info.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_common.hpp"
#include "duckdb/function/scalar/system_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/parser/expression/type_expression.hpp"

namespace duckdb {

namespace {

// aggregate state export
struct ExportAggregateBindData : public FunctionData {
	BoundAggregateFunction aggr;
	unique_ptr<FunctionData> bind_data;
	idx_t state_size;

	explicit ExportAggregateBindData(BoundAggregateFunction aggr_p, unique_ptr<FunctionData> bind_data_p,
	                                 idx_t state_size_p)
	    : aggr(std::move(aggr_p)), bind_data(std::move(bind_data_p)), state_size(state_size_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ExportAggregateBindData>(aggr, bind_data ? bind_data->Copy() : nullptr, state_size);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<ExportAggregateBindData>();
		if (bind_data.get() != other.bind_data.get()) {
			if (!bind_data || !other.bind_data) {
				return false;
			}
			if (!bind_data->Equals(*other.bind_data)) {
				return false;
			}
		}
		return aggr == other.aggr && state_size == other.state_size;
	}

	static ExportAggregateBindData &GetFrom(ExpressionState &state) {
		auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
		return func_expr.BindInfo()->Cast<ExportAggregateBindData>();
	}
};

static AggregateStateLayout GetLayout(const BoundAggregateFunction &aggr, optional_ptr<FunctionData> bind_data) {
	return aggr.GetStateType(bind_data);
}

// destroys the temporary underlying-aggregate states referenced by `states` (no-op if the aggregate has no
// destructor). the combine/finalize paths below initialize underlying states into scratch buffers - without this
// their owned heap (e.g. approx_top_k's hash map, approx_quantile's t-digest) would leak.
static void DestroyExportStates(const BoundAggregateFunction &aggr, optional_ptr<FunctionData> bind_data,
                                Vector &states, idx_t count, ArenaAllocator &allocator) {
	if (count == 0 || !aggr.HasStateDestructorCallback()) {
		return;
	}
	AggregateInputData aggr_input_data(aggr, bind_data, allocator);
	aggr.GetStateDestructorCallback()(states, aggr_input_data, count);
}

struct CombineState : public FunctionLocalState {
	//! The state layout, including the segment functions when the state is a linked list
	AggregateStateLayout layout;

	unsafe_unique_array<data_t> state_buffer0, state_buffer1;
	Vector addresses0, addresses1;

	ArenaAllocator allocator;

	explicit CombineState(const ExportAggregateBindData &bind_data)
	    : layout(GetLayout(bind_data.aggr, bind_data.bind_data.get())),
	      state_buffer0(make_unsafe_uniq_array<data_t>(STANDARD_VECTOR_SIZE * layout.total_state_size)),
	      state_buffer1(make_unsafe_uniq_array<data_t>(STANDARD_VECTOR_SIZE * layout.total_state_size)),
	      addresses0(LogicalType::POINTER), addresses1(LogicalType::POINTER), allocator(Allocator::DefaultAllocator()) {
	}
};

unique_ptr<FunctionLocalState> InitCombineState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ExportAggregateBindData>();
	return make_uniq<CombineState>(bind_data);
}

struct FinalizeState : public FunctionLocalState {
	//! The state layout, including the segment functions when the state is a linked list
	AggregateStateLayout layout;

	unsafe_unique_array<data_t> state_buffer;
	Vector addresses;

	ArenaAllocator allocator;

	explicit FinalizeState(const ExportAggregateBindData &bind_data)
	    : layout(GetLayout(bind_data.aggr, bind_data.bind_data.get())),
	      state_buffer(make_unsafe_uniq_array<data_t>(STANDARD_VECTOR_SIZE * layout.total_state_size)),
	      addresses(LogicalType::POINTER), allocator(Allocator::DefaultAllocator()) {
	}
};

unique_ptr<FunctionLocalState> InitFinalizeState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                 FunctionData *bind_data_p) {
	auto &bind_data = bind_data_p->Cast<ExportAggregateBindData>();
	return make_uniq<FinalizeState>(bind_data);
}

void AggregateStateFinalize(DataChunk &input, ExpressionState &state_p, Vector &result) {
	auto &bind_data = ExportAggregateBindData::GetFrom(state_p);
	auto &local_state = ExecuteFunctionState::GetFunctionState(state_p)->Cast<FinalizeState>();
	local_state.allocator.Reset();

	D_ASSERT(bind_data.state_size == bind_data.aggr.GetStateSize(bind_data.bind_data.get()));
	D_ASSERT(input.data.size() == 1);

	auto &layout = local_state.layout;

	auto count = input.size();
	// initialize the states - fields that are NULL in the input are not written by the deserialization
	AggregateStateInput state_input(bind_data.aggr, bind_data.bind_data.get());
	auto state_vec_writer = FlatVector::Writer<data_ptr_t>(local_state.addresses, count);
	for (idx_t i = 0; i < count; i++) {
		data_ptr_t state_ptr = local_state.state_buffer.get() + i * layout.total_state_size;
		bind_data.aggr.GetStateInitCallback()(state_input, &state_ptr, 1);
		state_vec_writer.WriteValue(state_ptr);
	}

	AggregateStateSerialization::DeserializeStates(bind_data.aggr, layout, input.data[0], count,
	                                               local_state.state_buffer.get(), local_state.allocator,
	                                               StateMemoryOwnership::BORROWED);

	AggregateFinalizeInputData aggr_input_data(bind_data.aggr, bind_data.bind_data.get(), local_state.allocator);
	bind_data.aggr.GetStateFinalizeCallback()(local_state.addresses, aggr_input_data, result, count, 0);
	DestroyExportStates(bind_data.aggr, bind_data.bind_data.get(), local_state.addresses, count, local_state.allocator);

	auto validity = input.data[0].Validity();
	for (idx_t i = 0; i < count; i++) {
		if (!validity.IsValid(i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

void AggregateStateCombine(DataChunk &input, ExpressionState &state_p, Vector &result) {
	auto &bind_data = ExportAggregateBindData::GetFrom(state_p);
	auto &local_state = ExecuteFunctionState::GetFunctionState(state_p)->Cast<CombineState>();
	local_state.allocator.Reset();

	D_ASSERT(bind_data.state_size == bind_data.aggr.GetStateSize(bind_data.bind_data.get()));
	D_ASSERT(input.data.size() == 2);
	D_ASSERT(input.data[0].GetType() == result.GetType());

	if (input.data[0].GetType().InternalType() != input.data[1].GetType().InternalType()) {
		throw IOException("Aggregate state combine type mismatch, expect %s, got %s",
		                  input.data[0].GetType().ToString(), input.data[1].GetType().ToString());
	}

	auto &layout = local_state.layout;
	auto count = input.size();

	result.Flatten();

	auto validity0 = input.data[0].Validity();
	auto validity1 = input.data[1].Validity();

	// Initialize both state buffers and build address vectors for all rows
	AggregateStateInput state_input(bind_data.aggr, bind_data.bind_data.get());
	auto state0_writer = FlatVector::Writer<data_ptr_t>(local_state.addresses0, count);
	auto state1_writer = FlatVector::Writer<data_ptr_t>(local_state.addresses1, count);
	for (idx_t i = 0; i < count; i++) {
		data_ptr_t ptr0 = local_state.state_buffer0.get() + i * layout.total_state_size;
		data_ptr_t ptr1 = local_state.state_buffer1.get() + i * layout.total_state_size;
		bind_data.aggr.GetStateInitCallback()(state_input, &ptr0, 1);
		bind_data.aggr.GetStateInitCallback()(state_input, &ptr1, 1);
		state0_writer.WriteValue(ptr0);
		state1_writer.WriteValue(ptr1);
	}

	// Deserialize both inputs — null rows are skipped by LoadOp, keeping the initialized empty state
	AggregateStateSerialization::DeserializeStates(bind_data.aggr, layout, input.data[0], count,
	                                               local_state.state_buffer0.get(), local_state.allocator,
	                                               StateMemoryOwnership::BORROWED);
	AggregateStateSerialization::DeserializeStates(bind_data.aggr, layout, input.data[1], count,
	                                               local_state.state_buffer1.get(), local_state.allocator,
	                                               StateMemoryOwnership::BORROWED);

	AggregateInputData aggr_input_data(bind_data.aggr, bind_data.bind_data.get(), local_state.allocator,
	                                   AggregateCombineType::ALLOW_DESTRUCTIVE);
	// the combine/serialize may throw (e.g. combining approx_top_k states with different k) - always destroy the
	// scratch states so their owned heap is not leaked
	auto destroy_states = [&]() {
		DestroyExportStates(bind_data.aggr, bind_data.bind_data.get(), local_state.addresses0, count,
		                    local_state.allocator);
		DestroyExportStates(bind_data.aggr, bind_data.bind_data.get(), local_state.addresses1, count,
		                    local_state.allocator);
	};
	try {
		bind_data.aggr.GetStateCombineCallback()(local_state.addresses0, local_state.addresses1, aggr_input_data,
		                                         count);
		AggregateStateSerialization::SerializeStates(bind_data.aggr, bind_data.bind_data.get(), layout,
		                                             local_state.addresses1, count, result, local_state.allocator, 0);
	} catch (...) {
		destroy_states();
		throw;
	}
	destroy_states();

	// Rows where both inputs were NULL produce no meaningful combined state — mark result as NULL
	for (idx_t i = 0; i < count; i++) {
		if (!validity0.IsValid(i) && !validity1.IsValid(i)) {
			FlatVector::SetNull(result, i, true);
		}
	}
}

// looks up the aggregate function with the given name in the catalog and binds it with the given argument types
// constant_parameters holds the values of arguments that must be re-bound with a specific constant
// (e.g. string_agg's separator), keyed by argument index - all other arguments are bound with a NULL value
unique_ptr<ExportAggregateBindData> BindExportedAggregate(ClientContext &context, const string &function_name,
                                                          const vector<LogicalType> &argument_types,
                                                          const map<idx_t, Value> &constant_parameters) {
	auto &func = Catalog::GetSystemCatalog(context).GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(Catalog::GetSystemCatalog(context).GetName(), Identifier::DefaultSchema(),
	                           Identifier(function_name)));
	if (func.type != CatalogType::AGGREGATE_FUNCTION_ENTRY) {
		throw BinderException("Could not find aggregate %s", function_name);
	}
	auto &aggr_entry = func.Cast<AggregateFunctionCatalogEntry>();

	FunctionBinder function_binder(context);
	auto best_function = FunctionBinder::SelectFunction(context, aggr_entry.functions, argument_types);
	if (!best_function.IsSuccess()) {
		throw BinderException("Could not re-bind exported aggregate %s: %s", function_name,
		                      best_function.GetError().Message());
	}
	const auto aggr = best_function.GetBinding().GetFunction<AggregateFunction>();
	const auto &signature = aggr->GetSignature();
	for (idx_t arg_idx = 0; arg_idx < argument_types.size(); arg_idx++) {
		if (argument_types[arg_idx].id() != LogicalTypeId::ANY) {
			continue;
		}
		auto parameter = arg_idx < signature.GetPositionalParameterCount()
		                     ? optional_ptr<const FunctionParameter>(signature.GetParameter(arg_idx))
		                     : signature.GetArgs();
		// ANY must describe a state-capable implementation, not a placeholder for type specialization.
		const bool canonical_any = parameter && parameter->GetType().id() == LogicalTypeId::ANY &&
		                           AnyType::GetTargetType(parameter->GetType()) == LogicalType::ANY;
		if (!canonical_any || !aggr->HasGetStateTypeCallback()) {
			throw BinderException("Aggregate %s requires a concrete type for signature argument %llu", function_name,
			                      arg_idx + 1);
		}
	}

	// Bind callbacks can use NULL placeholders for parameters carried by the serialized state.
	// Parameters required by the binding itself must retain their recorded constant values.
	vector<unique_ptr<Expression>> args;
	args.reserve(argument_types.size());
	for (idx_t arg_idx = 0; arg_idx < argument_types.size(); arg_idx++) {
		auto constant_entry = constant_parameters.find(arg_idx);
		if (constant_entry != constant_parameters.end()) {
			args.push_back(make_uniq<BoundConstantExpression>(constant_entry->second));
		} else {
			args.push_back(make_uniq<BoundConstantExpression>(Value(argument_types[arg_idx])));
		}
	}

	vector<pair<Identifier, unique_ptr<Expression>>> keyword_args;
	auto [bound_aggr, bind_info] =
	    function_binder.ResolveAggregateFunction(best_function.GetBinding(), args, keyword_args);

	const auto &bound_args = bound_aggr.GetArguments();
	bool signature_matches = bound_args.size() == argument_types.size();
	for (idx_t arg_idx = 0; signature_matches && arg_idx < bound_args.size(); arg_idx++) {
		// an ANY argument in the function signature (e.g. string_agg's data argument) matches any requested type
		if (bound_args[arg_idx].id() != LogicalTypeId::ANY &&
		    !bound_args[arg_idx].EqualsIncludingCollation(argument_types[arg_idx])) {
			signature_matches = false;
		}
	}
	if (!signature_matches) {
		throw BinderException("Type mismatch for exported aggregate %s: bound=[%s] requested=[%s]", function_name,
		                      StringUtil::ToString(bound_args, ", "), StringUtil::ToString(argument_types, ", "));
	}

	// The signature can retain ANY, but materialized result and state values must have complete types.
	if (!bound_aggr.GetReturnType().IsComplete()) {
		throw BinderException("Aggregate %s signature does not resolve a complete result type", function_name);
	}
	if (bound_aggr.HasGetStateTypeCallback() && !bound_aggr.GetStateType(bind_info.get()).type.IsComplete()) {
		throw BinderException("Aggregate %s signature does not resolve a complete state type", function_name);
	}
	const auto state_size = bound_aggr.GetStateSize(bind_info.get());
	return make_uniq<ExportAggregateBindData>(bound_aggr, std::move(bind_info), state_size);
}

// parses the "parameters" property of an AGGREGATE_STATE type
// each parameter is either a plain type, or a (type, value) pair for parameters that were bound to a constant
// (e.g. string_agg's separator) - the latter are returned in constant_parameters, keyed by argument index
void ParseStateParameters(const Value &parameters, vector<LogicalType> &argument_types,
                          map<idx_t, Value> &constant_parameters) {
	for (auto &val : ListValue::GetChildren(parameters)) {
		const idx_t arg_idx = argument_types.size();
		if (!val.IsNull() && val.type().id() == LogicalTypeId::TYPE) {
			// plain type
			argument_types.push_back(TypeValue::GetType(val));
			continue;
		}
		if (!val.IsNull() && val.type().id() == LogicalTypeId::STRUCT) {
			// (type, value) pair
			auto &children = StructValue::GetChildren(val);
			if (children.size() != 2 || children[0].IsNull() || children[0].type().id() != LogicalTypeId::TYPE ||
			    children[1].type().id() != LogicalTypeId::VARIANT) {
				throw InternalException("Aggregate state parameter entry should be a (type, value) pair");
			}
			argument_types.push_back(TypeValue::GetType(children[0]));
			if (!children[1].IsNull()) {
				// the parameter is bound to a constant - decode it as-is, without casting it to the declared
				// argument type, so that re-binding sees the same (pre-cast) constant as the original bind
				// (e.g. a DECIMAL quantile parameter must stay DECIMAL even though the signature says DOUBLE)
				constant_parameters.emplace(arg_idx, VariantValue::GetValue(children[1]));
			}
			continue;
		}
		throw InternalException(
		    "Aggregate state object should have a property called parameters that is a list of types");
	}
}

// parses an ORDER BY spec (a list of {column, order} structs) - shared by the re-bind path and to_aggregate_state
void ParseOrderBys(const Value &order_value, idx_t column_count, vector<SortedAggregateStateOrder> &orders);

unique_ptr<ExportAggregateBindData> BindAggregateStateInternal(ClientContext &context, BoundSimpleFunction &function,
                                                               vector<unique_ptr<Expression>> &arguments) {
	auto &arg_return_type = arguments[0]->GetReturnType();
	if (!arg_return_type.IsAggregateState()) {
		throw BinderException("Can only %s %s, not AGGREGATE_STATE", function.GetName(), arg_return_type.ToString());
	}
	//
	// now we can look up the function in the catalog again and bind it
	auto ext_info = arg_return_type.GetExtensionInfo();
	auto entry = ext_info->properties.find("function_name");
	if (entry == ext_info->properties.end() || entry->second.type().id() != LogicalTypeId::VARCHAR ||
	    entry->second.IsNull()) {
		throw InternalException("Aggregate state object should have a property called function_name that is a string");
	}
	auto &function_name = StringValue::Get(entry->second);

	entry = ext_info->properties.find("parameters");
	if (entry == ext_info->properties.end() || entry->second.type().id() != LogicalTypeId::LIST ||
	    entry->second.IsNull()) {
		throw InternalException(
		    "Aggregate state object should have a property called parameters that is a list of types");
	}
	vector<LogicalType> argument_types;
	map<idx_t, Value> constant_parameters;
	ParseStateParameters(entry->second, argument_types, constant_parameters);

	auto inner = BindExportedAggregate(context, function_name, argument_types, constant_parameters);

	auto order_entry = ext_info->properties.find("order_bys");
	if (order_entry == ext_info->properties.end()) {
		// a plain (non-ordered) aggregate state
		return inner;
	}
	// an ordered aggregate state: the value is the buffer (LIST<buffered_struct>) - reconstruct the sorted wrapper
	// around the inner aggregate so finalize sorts the buffer and combine concatenates buffers
	const auto buffer_struct = ListType::GetChildType(arg_return_type);
	const idx_t column_count = StructType::GetChildTypes(buffer_struct).size();
	vector<SortedAggregateStateOrder> orders;
	ParseOrderBys(order_entry->second, column_count, orders);
	// the leading buffered columns are the inner aggregate's bound arguments (post constant-erasure)
	const idx_t argument_count = inner->aggr.GetArguments().size();
	if (argument_count == 0) {
		throw BinderException("Aggregate \"%s\" has no arguments and cannot have an ordered aggregate state",
		                      function_name);
	}
	if (argument_count > column_count) {
		throw BinderException("to_aggregate_state: argument count %llu exceeds the number of state columns (%llu)",
		                      (uint64_t)argument_count, (uint64_t)column_count);
	}

	auto reconstructed = FunctionBinder::BindSortedAggregateState(context, inner->aggr, std::move(inner->bind_data),
	                                                              buffer_struct, orders, argument_count);
	BoundAggregateFunction wrapper(reconstructed.first);
	const auto state_size = wrapper.GetStateSize(reconstructed.second.get());
	return make_uniq<ExportAggregateBindData>(std::move(wrapper), std::move(reconstructed.second), state_size);
}

unique_ptr<FunctionData> BindAggregateState(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto bind_data =
	    BindAggregateStateInternal(input.GetClientContext(), input.GetBoundFunction(), input.GetArguments());

	// combine - both arguments must be aggregate states of the same function with the same signature
	if (arguments.size() == 2 && arguments[0]->GetReturnType() != arguments[1]->GetReturnType()) {
		auto &left_type = arguments[0]->GetReturnType();
		auto &right_type = arguments[1]->GetReturnType();
		if (left_type.IsAggregateState() && right_type.IsAggregateState()) {
			// both are aggregate states but they are not equal - if the function and signature match, the states
			// were bound with different constant parameters (e.g. string_agg states with different separators)
			auto &left_props = left_type.GetExtensionInfo()->properties;
			auto &right_props = right_type.GetExtensionInfo()->properties;
			auto left_name = left_props.find("function_name");
			auto right_name = right_props.find("function_name");
			if (left_name != left_props.end() && right_name != right_props.end() &&
			    left_name->second == right_name->second) {
				auto left_params = left_props.find("parameters");
				auto right_params = right_props.find("parameters");
				throw BinderException(
				    "Cannot COMBINE aggregate states of \"%s\" that were created with different parameters: %s <> %s",
				    StringValue::Get(left_name->second),
				    left_params == left_props.end() ? "?" : left_params->second.ToString(),
				    right_params == right_props.end() ? "?" : right_params->second.ToString());
			}
		}
		throw BinderException("Cannot COMBINE aggregate states from different functions, %s <> %s",
		                      left_type.ToString(), right_type.ToString());
	}

	if (bound_function.GetName() == "finalize") {
		bound_function.SetReturnType(bind_data->aggr.GetReturnType());
	} else {
		D_ASSERT(bound_function.GetName() == "combine");
		bound_function.SetReturnType(arguments[0]->GetReturnType());
	}
	// these run the finalize/combine of the underlying aggregate, which can throw, e.g. skewness of a large sum
	bound_function.SetFallible();

	return std::move(bind_data);
}

void ExportAggregateFinalize(Vector &state, AggregateFinalizeInputData &aggr_input_data, Vector &result, idx_t count,
                             idx_t offset) {
	const data_ptr_t *addresses_ptrs;
	if (state.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		if (count != 1) {
			throw InternalException("Finalize with a constant vector only supported with count of 1");
		}
		addresses_ptrs = ConstantVector::GetData<data_ptr_t>(state);
	} else {
		addresses_ptrs = FlatVector::GetData<data_ptr_t>(state);
	}

	auto layout = GetLayout(aggr_input_data.function, aggr_input_data.bind_data);

	// only flatten the result the first time it is written - ordered aggregates finalize one group at a time at
	// increasing offsets, appending into the (already flat) result
	if (offset == 0) {
		result.Flatten();
	}
	AggregateStateSerialization::SerializeStates(layout, result, count, addresses_ptrs, offset);
}

// the executor invokes this callback with combine_aggr's own bind data (ExportAggregateBindData) - the underlying
// aggregate's combine expects its own bind data, so we forward it here
void CombineAggrStateCombine(Vector &source, Vector &target, AggregateInputData &aggr_input_data, idx_t count) {
	auto &bind_data = aggr_input_data.bind_data->Cast<ExportAggregateBindData>();
	AggregateInputData combine_input(bind_data.aggr, bind_data.bind_data.get(), aggr_input_data.allocator,
	                                 aggr_input_data.combine_type);
	combine_input.combine_multiplicities = aggr_input_data.combine_multiplicities;
	bind_data.aggr.GetStateCombineCallback()(source, target, combine_input, count);
}

// destroys combine_aggr's accumulator states by forwarding to the underlying aggregate's destructor (with its own bind
// data) - otherwise states that own heap (e.g. approx_top_k's hash map, approx_quantile's t-digest) would leak
void CombineAggrStateDestroy(Vector &state, AggregateInputData &aggr_input_data, idx_t count) {
	auto &bind_data = aggr_input_data.bind_data->Cast<ExportAggregateBindData>();
	AggregateInputData destroy_input(bind_data.aggr, bind_data.bind_data.get(), aggr_input_data.allocator);
	bind_data.aggr.GetStateDestructorCallback()(state, destroy_input, count);
}

unique_ptr<FunctionData> CombineAggrBind(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();

	D_ASSERT(arguments.size() == 1 || arguments.size() == 2);
	auto bind_data = BindAggregateStateInternal(input.GetClientContext(), function, arguments);

	// Copy underlying aggregate's callbacks into this function (same pattern as `ExportAggregateFunction::Bind`)
	function.SetStateSizeCallback(bind_data->aggr.GetStateSizeCallback());
	function.SetStateInitCallback(bind_data->aggr.GetStateInitCallback());
	function.SetStateCombineCallback(CombineAggrStateCombine);
	if (bind_data->aggr.HasStateDestructorCallback()) {
		function.SetStateDestructorCallback(CombineAggrStateDestroy);
	}

	function.SetReturnType(arguments[0]->GetReturnType());

	return std::move(bind_data);
}

void CombineAggrUpdate(Vector inputs[], AggregateInputData &aggr_input_data, idx_t input_count, Vector &states,
                       idx_t count) {
	D_ASSERT(input_count == 1 || input_count == 2);

	auto &bind_data = aggr_input_data.bind_data->Cast<ExportAggregateBindData>();
	auto &underlying_aggr = bind_data.aggr;

	auto layout = GetLayout(underlying_aggr, bind_data.bind_data.get());

	auto aligned_size = layout.total_state_size;
	unsafe_unique_array<data_t> temp_state_buf = make_unsafe_uniq_array<data_t>(count * aligned_size);

	auto state_values = states.Values<data_ptr_t>();

	// source_vec holds pointers to the binary states buffer (temp_state_buf) deserialized from the input states
	Vector source_vec(LogicalType::POINTER);
	auto source_data = FlatVector::Writer<data_ptr_t>(source_vec, count);

	// target_vec will hold pointers to the binary state buffer where the combined states should be stored, built by the
	// underlying aggregate function's combine callback
	Vector target_vec(LogicalType::POINTER);
	auto target_data = FlatVector::Writer<data_ptr_t>(target_vec, count);

	AggregateStateInput state_input(underlying_aggr, bind_data.bind_data.get());
	for (idx_t i = 0; i < count; i++) {
		data_ptr_t temp_ptr = temp_state_buf.get() + i * aligned_size;
		underlying_aggr.GetStateInitCallback()(state_input, &temp_ptr, 1);
		source_data.WriteValue(temp_ptr);
		target_data.WriteValue(state_values[i].GetValue());
	}

	try {
		AggregateStateSerialization::DeserializeStates(underlying_aggr, layout, inputs[0], count, temp_state_buf.get(),
		                                               aggr_input_data.allocator, StateMemoryOwnership::BORROWED);
		AggregateInputData combine_input(bind_data.aggr, bind_data.bind_data.get(), aggr_input_data.allocator,
		                                 AggregateCombineType::ALLOW_DESTRUCTIVE);
		if (input_count == 2) {
			combine_input.combine_multiplicities = inputs[1];
		}
		underlying_aggr.GetStateCombineCallback()(source_vec, target_vec, combine_input, count);
	} catch (...) {
		DestroyExportStates(underlying_aggr, bind_data.bind_data.get(), source_vec, count, aggr_input_data.allocator);
		throw;
	}
	DestroyExportStates(underlying_aggr, bind_data.bind_data.get(), source_vec, count, aggr_input_data.allocator);
}

void CombineAggrFinalize(Vector &state, AggregateFinalizeInputData &aggr_input_data, Vector &result, idx_t count,
                         idx_t offset) {
	auto &bind_data = aggr_input_data.bind_data->Cast<ExportAggregateBindData>();
	auto &underlying_aggr = bind_data.aggr;

	auto layout = GetLayout(underlying_aggr, bind_data.bind_data.get());

	// only flatten the result the first time it is written - ordered aggregates finalize one group at a time at
	// increasing offsets, appending into the (already flat) result
	if (offset == 0) {
		result.Flatten();
	}
	AggregateStateSerialization::SerializeStates(underlying_aggr, bind_data.bind_data.get(), layout, state, count,
	                                             result, aggr_input_data.allocator, offset);
}

// stores the function name and signature (with any constant parameters) into the extension type info, so that the
// aggregate can be re-bound later (e.g. by FINALIZE/COMBINE)
void EncodeStateParameters(ExtensionTypeInfo &ext_info, const BoundAggregateFunction &bound_function,
                           const AggregateStateLayout &layout) {
	ext_info.properties.emplace("function_name", bound_function.GetName());
	auto &call_arguments = bound_function.GetArguments();
	vector<Value> arguments;
	if (layout.constant_parameters.empty()) {
		// all parameters are plain types - store the parameters as a list of types
		for (auto &arg : call_arguments) {
			arguments.push_back(Value::TYPE(arg));
		}
		ext_info.properties.emplace("parameters", Value::LIST(LogicalType::TYPE(), std::move(arguments)));
	} else {
		// some parameters were bound to a constant (e.g. string_agg's separator) - store the parameters as a list of
		// (type, value) pairs, where the value holds the constant the parameter must be re-bound with
		for (idx_t arg_idx = 0; arg_idx < call_arguments.size(); arg_idx++) {
			child_list_t<Value> children;
			children.emplace_back("type", Value::TYPE(call_arguments[arg_idx]));
			auto constant_entry = layout.constant_parameters.find(arg_idx);
			if (constant_entry == layout.constant_parameters.end()) {
				children.emplace_back("value", Value(LogicalType::VARIANT()));
			} else {
				children.emplace_back("value", constant_entry->second.DefaultCastAs(LogicalType::VARIANT()));
			}
			arguments.push_back(Value::STRUCT(std::move(children)));
		}
		auto entry_type = LogicalType::STRUCT({{"type", LogicalType::TYPE()}, {"value", LogicalType::VARIANT()}});
		ext_info.properties.emplace("parameters", Value::LIST(entry_type, std::move(arguments)));
	}
}

// the STRUCT type of a single ORDER BY entry encoded into an ordered aggregate state's extension info. It matches the
// shape of to_aggregate_state's ORDER BY argument: the buffered column the key sorts on, and the modifier string.
LogicalType OrderByEntryType() {
	return LogicalType::STRUCT({{"column", LogicalType::UINTEGER}, {"order", LogicalType::VARCHAR}});
}

// renders an order modifier (e.g. ASC NULLS LAST) into the string form parsed by OrderModifiers::Parse
string OrderModifierToString(OrderType order_type, OrderByNullType null_order) {
	return StringUtil::Format("%s %s", EnumUtil::ToChars(order_type), EnumUtil::ToChars(null_order));
}

// constructs the AGGREGATE_STATE type for the given bound aggregate function
// the state layout (a struct) is aliased to AGGREGATE_STATE, with the function name and signature stored in the
// extension type info so that the aggregate can be re-bound later (e.g. by FINALIZE/COMBINE)
LogicalType CreateAggregateStateType(const BoundAggregateFunction &bound_function,
                                     optional_ptr<FunctionData> bind_data) {
	auto layout = bound_function.GetStateType(bind_data);
	auto ext_info = make_uniq<ExtensionTypeInfo>();
	EncodeStateParameters(*ext_info, bound_function, layout);
	return layout.type.WithAlias("AGGREGATE_STATE").WithExtensionInfo(std::move(ext_info));
}

// constructs the AGGREGATE_STATE type for an ordered aggregate - the state is the buffer of values
// (LIST<buffered_struct>), with the inner signature and the ORDER BY spec stored in the extension info
LogicalType CreateSortedAggregateStateType(const BoundAggregateFunction &inner_function,
                                           optional_ptr<FunctionData> inner_bind_data, const LogicalType &buffer_struct,
                                           const vector<SortedAggregateStateOrder> &orders) {
	LogicalType state_layout = LogicalType::LIST(buffer_struct);
	auto ext_info = make_uniq<ExtensionTypeInfo>();
	EncodeStateParameters(*ext_info, inner_function, inner_function.GetStateType(inner_bind_data));
	// per key: the buffered column it sorts on and the modifier string (the argument count is re-derived on re-bind)
	vector<Value> order_values;
	for (auto &order : orders) {
		child_list_t<Value> children;
		children.emplace_back("column", Value::UINTEGER(UnsafeNumericCast<uint32_t>(order.column)));
		children.emplace_back("order", Value(OrderModifierToString(order.order_type, order.null_order)));
		order_values.push_back(Value::STRUCT(std::move(children)));
	}
	ext_info->properties.emplace("order_bys", Value::LIST(OrderByEntryType(), std::move(order_values)));
	return state_layout.WithAlias("AGGREGATE_STATE").WithExtensionInfo(std::move(ext_info));
}

// parses a single entry of the to_aggregate_state signature list - either a TYPE value (e.g. make_type('VARCHAR'))
// or a string naming a type (e.g. 'VARCHAR')
LogicalType ParseSignatureType(ClientContext &context, const Value &arg) {
	if (arg.IsNull()) {
		throw BinderException("to_aggregate_state: the signature cannot contain NULL values");
	}
	if (arg.type().id() == LogicalTypeId::TYPE) {
		return TypeValue::GetType(arg);
	}
	if (arg.type().id() == LogicalTypeId::VARCHAR) {
		auto &name = StringValue::Get(arg);
		// ANY describes a canonical aggregate argument, not an ordinary SQL value type.
		if (StringUtil::CIEquals(name, "ANY")) {
			return LogicalType::ANY;
		}
		return TransformStringToLogicalType(name, context);
	}
	throw BinderException("to_aggregate_state: the signature must be a list of types");
}

// parses the optional fourth argument of to_aggregate_state: the constant values for arguments that must be bound to
// a specific constant rather than a NULL value of the argument type (e.g. string_agg's separator). It is supplied as
// a LIST with one entry per argument (i.e. the same length as the signature): NULL for arguments that are not bound to
// a constant, and the constant value for arguments that are, e.g. [NULL, '|'] to bind argument 1 to the constant '|'
void ParseConstantParameters(const Value &constants, idx_t argument_count, map<idx_t, Value> &constant_parameters) {
	if (constants.IsNull()) {
		return;
	}
	if (constants.type().id() != LogicalTypeId::LIST) {
		throw BinderException("to_aggregate_state: the constant parameters must be a list with one entry per argument "
		                      "(use NULL for arguments that are not bound to a constant), e.g. [NULL, '|']");
	}
	auto &children = ListValue::GetChildren(constants);
	if (children.size() != argument_count) {
		throw BinderException("to_aggregate_state: the constant parameters list has %llu entries but the aggregate has "
		                      "%llu arguments - it must have exactly one entry per argument (use NULL for arguments "
		                      "that are not bound to a constant)",
		                      children.size(), argument_count);
	}
	for (idx_t i = 0; i < children.size(); i++) {
		if (children[i].IsNull()) {
			continue;
		}
		constant_parameters[i] = children[i];
	}
}

// parses the optional fifth argument of to_aggregate_state: the ORDER BY specification of an ordered aggregate, as a
// list of {column, order} structs, e.g. [{'column': 1, 'order': 'DESC NULLS LAST'}]. 'column' is the buffered struct
// column the key sorts on, 'order' is the modifier string (parsed with the same rules as create_sort_key).
void ParseOrderBys(const Value &order_value, idx_t column_count, vector<SortedAggregateStateOrder> &orders) {
	if (order_value.IsNull()) {
		return;
	}
	if (order_value.type().id() != LogicalTypeId::LIST) {
		throw BinderException(
		    "to_aggregate_state: the ORDER BY argument must be a list of {column, order} structs, e.g. "
		    "[{'column': 1, 'order': 'DESC NULLS LAST'}]");
	}
	for (auto &entry : ListValue::GetChildren(order_value)) {
		if (entry.IsNull() || entry.type().id() != LogicalTypeId::STRUCT) {
			throw BinderException("to_aggregate_state: each ORDER BY entry must be a {column, order} struct");
		}
		auto &field_types = StructType::GetChildTypes(entry.type());
		auto &field_values = StructValue::GetChildren(entry);
		Value column, order;
		for (idx_t f = 0; f < field_types.size(); f++) {
			if (field_types[f].first == "column") {
				column = field_values[f];
			} else if (field_types[f].first == "order") {
				order = field_values[f];
			}
		}
		if (column.IsNull() || order.IsNull()) {
			throw BinderException("to_aggregate_state: each ORDER BY entry must have a non-NULL 'column' and 'order'");
		}
		if (order.type().id() != LogicalTypeId::VARCHAR) {
			throw BinderException("to_aggregate_state: the 'order' of an ORDER BY entry must be a string, e.g. "
			                      "'DESC NULLS LAST'");
		}
		SortedAggregateStateOrder state_order;
		state_order.column = column.GetValue<uint32_t>();
		if (state_order.column >= column_count) {
			throw BinderException(
			    "to_aggregate_state: ORDER BY column %llu is out of range (the state has %llu columns)",
			    (uint64_t)state_order.column, (uint64_t)column_count);
		}
		auto modifiers = OrderModifiers::Parse(StringValue::Get(order));
		state_order.order_type = modifiers.order_type;
		state_order.null_order = modifiers.null_type;
		orders.push_back(state_order);
	}
}

//! Binds the exported aggregate from the constant arguments, and resolves the state / return types from it
template <class INPUT>
unique_ptr<FunctionData> BindToAggregateState(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto &context = input.GetClientContext();
	for (idx_t i = 1; i < arguments.size(); i++) {
		if (arguments[i]->HasParameter()) {
			throw ParameterNotResolvedException();
		}
		if (!arguments[i]->IsFoldable()) {
			throw BinderException("to_aggregate_state: the aggregate name, signature and constant parameters must be "
			                      "constant");
		}
	}
	// the arguments are not yet cast to the parameter types when binding
	auto function_name_val = input.GetNonNullConstant(1).DefaultCastAs(LogicalType::VARCHAR);
	auto function_name = StringValue::Get(function_name_val);

	auto signature_val = input.GetNonNullConstant(2);
	if (signature_val.type().id() != LogicalTypeId::LIST) {
		throw BinderException("to_aggregate_state: the signature must be a list of types");
	}
	// the signature lists all of the argument types in order
	vector<LogicalType> argument_types;
	for (auto &arg : ListValue::GetChildren(signature_val)) {
		argument_types.push_back(ParseSignatureType(context, arg));
	}

	// constant_parameters holds the values of arguments that must be re-bound with a specific constant rather than a
	// NULL value of the argument type (e.g. string_agg's separator), keyed by the argument's index
	map<idx_t, Value> constant_parameters;
	if (arguments.size() > 3) {
		auto constants_val = input.GetConstant(3);
		ParseConstantParameters(constants_val, argument_types.size(), constant_parameters);
	}
	for (auto &entry : constant_parameters) {
		// cast each constant to the argument type declared in the signature
		entry.second = entry.second.DefaultCastAs(argument_types[entry.first]);
	}

	auto bind_data = BindExportedAggregate(context, function_name, argument_types, constant_parameters);
	auto &aggr = bind_data->aggr;
	if (!aggr.HasGetStateTypeCallback()) {
		throw BinderException(
		    "Aggregate function \"%s\" does not have a state type callback defined - cannot convert to its state",
		    function_name);
	}

	if (arguments.size() > 4) {
		// an ordered aggregate state: the value is the buffer (LIST<buffered_struct>), the fifth argument the ORDER BY
		auto &state_type = arguments[0]->GetReturnType();
		if (state_type.id() != LogicalTypeId::LIST ||
		    ListType::GetChildType(state_type).id() != LogicalTypeId::STRUCT) {
			throw BinderException("to_aggregate_state: an ordered aggregate state value must be a LIST of STRUCTs (the "
			                      "buffer of values), e.g. [{'v0': ...}, ...]");
		}
		auto buffer_columns = StructType::GetChildTypes(ListType::GetChildType(state_type));
		const idx_t column_count = buffer_columns.size();
		// the leading buffered columns are the aggregate's arguments - cast them to the argument types
		auto &aggr_arguments = aggr.GetArguments();
		if (aggr_arguments.empty()) {
			throw BinderException("to_aggregate_state: aggregate \"%s\" has no arguments and cannot have an ordered "
			                      "aggregate state",
			                      function_name);
		}
		if (aggr_arguments.size() > column_count) {
			throw BinderException("to_aggregate_state: argument count %llu exceeds the number of state columns (%llu)",
			                      (uint64_t)aggr_arguments.size(), (uint64_t)column_count);
		}
		for (idx_t i = 0; i < aggr_arguments.size(); i++) {
			if (aggr_arguments[i].IsComplete()) {
				buffer_columns[i].second = aggr_arguments[i];
			}
		}
		const auto buffer_struct = LogicalType::STRUCT(std::move(buffer_columns));
		if (!buffer_struct.IsComplete()) {
			throw BinderException("to_aggregate_state: the ordered state buffer must have a complete type");
		}
		vector<SortedAggregateStateOrder> orders;
		auto order_value = input.GetConstant(4);
		ParseOrderBys(order_value, column_count, orders);
		if (orders.empty()) {
			throw BinderException("to_aggregate_state: an ordered aggregate state must have at least one ORDER BY key");
		}
		bound_function.GetArguments()[0] = LogicalType::LIST(buffer_struct);
		bound_function.SetReturnType(
		    CreateSortedAggregateStateType(aggr, bind_data->bind_data.get(), buffer_struct, orders));
		return std::move(bind_data);
	}

	auto state_layout = aggr.GetStateType(bind_data->bind_data.get()).type;
	bound_function.GetArguments()[0] = state_layout;
	bound_function.SetReturnType(CreateAggregateStateType(aggr, bind_data->bind_data.get()));
	return std::move(bind_data);
}

void ToAggregateStateResolveTypes(ResolveScalarFunctionTypesInput &input) {
	BindToAggregateState(input);
}

unique_ptr<FunctionData> ToAggregateStateBind(BindScalarFunctionInput &input) {
	return BindToAggregateState(input);
}

void ToAggregateStateFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	// the input type is verified to match the state layout at bind time - we only need to reinterpret the vector
	result.Reinterpret(input.data[0]);
}

} // namespace

unique_ptr<ParsedExpression> ExportAggregateFunction::StateToSQL(const LogicalType &type,
                                                                 unique_ptr<ParsedExpression> value) {
	auto info = type.GetExtensionInfo();
	if (!type.IsAggregateState() || !info) {
		return nullptr;
	}
	auto name = info->properties.find("function_name");
	auto parameters = info->properties.find("parameters");
	const bool has_function_name =
	    name != info->properties.end() && !name->second.IsNull() && name->second.type().id() == LogicalTypeId::VARCHAR;
	const bool has_parameters = parameters != info->properties.end() && !parameters->second.IsNull() &&
	                            parameters->second.type().id() == LogicalTypeId::LIST;
	if (!has_function_name || !has_parameters) {
		return nullptr;
	}
	vector<LogicalType> types;
	map<idx_t, Value> constants;
	ParseStateParameters(parameters->second, types, constants);
	// Concrete TYPE values retain collations that the SQL type name alone cannot express.
	// Canonical ANY arguments are signature markers, not SQL value types.
	const bool has_any = std::find(types.begin(), types.end(), LogicalType::ANY) != types.end();
	vector<Value> signature;
	vector<unique_ptr<ParsedExpression>> constant_arguments;
	for (idx_t i = 0; i < types.size(); i++) {
		if (has_any && ((types[i] != LogicalType::ANY && !TypeExpression::CanRepresent(types[i])) ||
		                TypeVisitor::Contains(types[i], [](const LogicalType &child) {
			                return child.id() == LogicalTypeId::VARCHAR && !StringType::GetCollation(child).empty();
		                }))) {
			return nullptr;
		}
		signature.push_back(has_any ? Value(types[i].ToString()) : Value::TYPE(types[i]));
		auto entry = constants.find(i);
		unique_ptr<ParsedExpression> constant;
		try {
			constant = ConstantExpression::FromValue(entry == constants.end() ? Value() : entry->second);
		} catch (const NotImplementedException &) {
			return nullptr;
		}
		constant_arguments.push_back(make_uniq<CastExpression>(LogicalType::VARIANT(), std::move(constant)));
	}
	vector<unique_ptr<ParsedExpression>> arguments;
	arguments.push_back(std::move(value));
	arguments.push_back(ConstantExpression::FromValue(name->second));
	try {
		arguments.push_back(ConstantExpression::FromValue(
		    Value::LIST(has_any ? LogicalType::VARCHAR : LogicalType::TYPE(), std::move(signature))));
	} catch (const NotImplementedException &) {
		return nullptr;
	}
	arguments.push_back(
	    make_uniq<FunctionExpression>(QualifiedName("system", "main", "list_value"), std::move(constant_arguments)));
	auto orders = info->properties.find("order_bys");
	if (orders != info->properties.end()) {
		arguments.push_back(ConstantExpression::FromValue(orders->second));
	}
	return make_uniq<FunctionExpression>(QualifiedName("system", "main", "to_aggregate_state"), std::move(arguments));
}

void ExportAggregateFunction::GetSortedStateOrders(const LogicalType &state_type,
                                                   vector<SortedAggregateStateOrder> &orders) {
	auto ext_info = state_type.GetExtensionInfo();
	if (!ext_info || state_type.id() != LogicalTypeId::LIST) {
		throw InternalException("GetSortedStateOrders called on a state type that is not an ordered aggregate state");
	}
	auto order_entry = ext_info->properties.find("order_bys");
	if (order_entry == ext_info->properties.end()) {
		throw InternalException("GetSortedStateOrders called on a state type that is not an ordered aggregate state");
	}
	auto &buffer_struct = ListType::GetChildType(state_type);
	ParseOrderBys(order_entry->second, StructType::GetChildTypes(buffer_struct).size(), orders);
}

void ExportAggregateFunction::SetStateExport(BoundAggregateExpression &aggregate, LogicalType state_layout) {
	auto &bound_function = aggregate.FunctionMutable();
	// functions with an explicit export callback use it as the finalize; others use the field-based serialization
	bound_function.SetStateFinalizeCallback(bound_function.HasExportAggregateStateCallback()
	                                            ? bound_function.GetExportAggregateStateCallback()
	                                            : ExportAggregateFinalize);
	// statistics propagation is no longer correct post
	bound_function.SetStatisticsCallback(nullptr);
	bound_function.SetReturnType(state_layout);
	// exported state always produces a valid (non-NULL) struct even for empty inputs
	bound_function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	aggregate.StateExportModeMutable() = AggregateStateExportMode::STATE_EXPORT;
	aggregate.SetReturnType(std::move(state_layout));
}

unique_ptr<BoundAggregateExpression>
ExportAggregateFunction::Bind(unique_ptr<BoundAggregateExpression> child_aggregate) {
	auto &bound_function = child_aggregate->Function();
	if (!bound_function.HasStateCombineCallback()) {
		throw BinderException("Cannot use EXPORT_STATE for non-combinable function %s", bound_function.GetName());
	}
	if (bound_function.HasExportAggregateStateCallback() != bound_function.HasImportAggregateStateCallback()) {
		throw InternalException("Aggregate function %s must define either both or neither of the "
		                        "export_aggregate_state/import_aggregate_state callbacks",
		                        bound_function.GetName());
	}
	if (!bound_function.HasGetStateTypeCallback()) {
		throw NotImplementedException(
		    "Aggregate function %s does not have a state type callback defined - cannot export state",
		    bound_function.GetName());
	}
	D_ASSERT(bound_function.HasStateSizeCallback());
	D_ASSERT(bound_function.HasStateFinalizeCallback());
	D_ASSERT(child_aggregate->Function().GetReturnType().id() != LogicalTypeId::INVALID);
	if (child_aggregate->GetOrderBys() && !child_aggregate->GetOrderBys()->orders.empty() &&
	    !child_aggregate->GetChildren().empty()) {
		// ordered aggregate (the ORDER BY of an aggregate without arguments is ignored): export the buffer of values.
		// The sorted wrapper is built later (physical planning); here we only fix the AGGREGATE_STATE type so
		// downstream binding (finalize/combine) sees it
		LogicalType buffer_struct;
		vector<SortedAggregateStateOrder> orders;
		idx_t argument_count; // re-derived from the inner aggregate on re-bind
		FunctionBinder::GetSortedAggregateStateLayout(*child_aggregate, buffer_struct, orders, argument_count);
		SetStateExport(*child_aggregate, CreateSortedAggregateStateType(
		                                     bound_function, child_aggregate->BindInfo().get(), buffer_struct, orders));
		return child_aggregate;
	}
	SetStateExport(*child_aggregate, CreateAggregateStateType(bound_function, child_aggregate->BindInfo().get()));
	return child_aggregate;
}

ExportAggregateFunctionBindData::ExportAggregateFunctionBindData(unique_ptr<Expression> aggregate_p) {
	D_ASSERT(aggregate_p->GetExpressionType() == ExpressionType::BOUND_AGGREGATE);
	aggregate = unique_ptr_cast<Expression, BoundAggregateExpression>(std::move(aggregate_p));
}

unique_ptr<FunctionData> ExportAggregateFunctionBindData::Copy() const {
	return make_uniq<ExportAggregateFunctionBindData>(aggregate->Copy());
}

bool ExportAggregateFunctionBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ExportAggregateFunctionBindData>();
	return aggregate->Equals(*other.aggregate);
}

ScalarFunction FinalizeFun::GetFunction() {
	auto function = ScalarFunction("finalize", {}, LogicalTypeId::INVALID, AggregateStateFinalize, BindAggregateState,
	                               nullptr, InitFinalizeState);
	function.GetSignature().AddParameter("state", LogicalTypeId::ANY);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);

	return function;
}

ScalarFunction CombineFun::GetFunction() {
	auto function = ScalarFunction("combine", {}, LogicalTypeId::ANY, AggregateStateCombine, BindAggregateState,
	                               nullptr, InitCombineState);
	function.GetSignature().AddParameter("state1", LogicalTypeId::ANY).AddParameter("state2", LogicalTypeId::ANY);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return function;
}

ScalarFunctionSet ToAggregateStateFun::GetFunctions() {
	ScalarFunctionSet set("to_aggregate_state");
	vector<LogicalType> arguments {LogicalTypeId::ANY, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::ANY)};
	for (idx_t optional_args = 0; optional_args < 3; optional_args++) {
		if (optional_args == 1) {
			// optional fourth argument: constant parameter values as a list with one entry per argument (e.g.
			// [NULL, ','])
			arguments.emplace_back(LogicalTypeId::ANY);
		} else if (optional_args == 2) {
			// optional fifth argument: the ORDER BY of an ordered aggregate, as a list of {column, order} structs
			// (e.g. [{'column': 1, 'order': 'DESC NULLS LAST'}])
			arguments.emplace_back(LogicalTypeId::ANY);
		}
		ScalarFunction function("to_aggregate_state", arguments, LogicalTypeId::ANY, ToAggregateStateFunction,
		                        ToAggregateStateBind);
		function.SetResolveTypesCallback(ToAggregateStateResolveTypes);
		auto &sig = function.GetSignature();
		sig.GetParameter(0).SetName("data");
		sig.GetParameter(1).SetName("name");
		sig.GetParameter(2).SetName("signature");
		if (sig.GetParameterCount() > 3) {
			sig.GetParameter(3).SetName("constant_parameters");
		}
		if (sig.GetParameterCount() > 4) {
			sig.GetParameter(4).SetName("order_by");
		}
		function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
		set.AddFunction(std::move(function));
	}
	return set;
}

AggregateFunction CombineAggrFun::GetFunction() {
	auto function = AggregateFunction("combine_aggr", {}, LogicalTypeId::ANY, nullptr, nullptr, CombineAggrUpdate,
	                                  nullptr, CombineAggrFinalize, FunctionNullHandling::SPECIAL_HANDLING, nullptr,
	                                  CombineAggrBind, nullptr, nullptr, nullptr);
	function.GetSignature().AddParameter("arg", LogicalTypeId::ANY);
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// importing states validates them, and the underlying combine can throw (e.g. mismatched approx_top_k k values)
	function.SetFallible();
	return function;
}

AggregateFunctionSet CombineAggrFun::GetFunctions() {
	AggregateFunctionSet set("combine_aggr");
	set.AddFunction(GetFunction());
	auto repeated = AggregateFunction("combine_aggr", {}, LogicalTypeId::ANY, nullptr, nullptr, CombineAggrUpdate,
	                                  nullptr, CombineAggrFinalize, FunctionNullHandling::SPECIAL_HANDLING, nullptr,
	                                  CombineAggrBind, nullptr, nullptr, nullptr);
	repeated.GetSignature().AddParameter("arg", LogicalTypeId::ANY).AddParameter("multiplicities", LogicalType::BIGINT);
	repeated.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// importing states validates them, and the repeated combine throws on multiplicity overflow
	repeated.SetFallible();
	set.AddFunction(std::move(repeated));
	return set;
}

} // namespace duckdb
