#include "core_functions/aggregate/histogram_helpers.hpp"
#include "core_functions/aggregate/nested_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/owning_string_map.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/types/sql_value_map.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"

namespace duckdb {

namespace {
static unique_ptr<BoundAggregateExpression> BindAggregate(ClientContext &context, const char *name,
                                                          vector<unique_ptr<Expression>> children,
                                                          unique_ptr<Expression> filter = nullptr) {
	auto &catalog = Catalog::GetSystemCatalog(context);
	auto &entry = catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(catalog.GetName(), Identifier::DefaultSchema(), name));
	vector<LogicalType> child_types;
	for (auto &child : children) {
		child_types.push_back(child->GetReturnType());
	}
	const auto &function = entry.functions.GetFunctionByArguments(context, child_types);
	FunctionBinder function_binder(context);
	return function_binder.BindAggregateFunction(function, std::move(children), std::move(filter));
}

static unique_ptr<Expression> BindScalar(ClientContext &context, const char *name,
                                         vector<unique_ptr<Expression>> children) {
	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), name, std::move(children));
}

template <class MAP_TYPE>
struct HistogramFunction {
	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &) {
		if (state.hist) {
			delete state.hist;
		}
	}

	static bool IgnoreNull() {
		return true;
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &input_data) {
		if (!source.hist) {
			return;
		}
		if (!target.hist) {
			target.hist = MAP_TYPE::CreateEmpty(input_data.allocator);
		}
		for (auto &entry : *source.hist) {
			(*target.hist)[entry.first] += entry.second;
		}
	}
};

template <class TYPE>
struct DefaultMapType {
	using MAP_TYPE = TYPE;

	static TYPE *CreateEmpty(ArenaAllocator &) {
		return new TYPE();
	}
};

template <class TYPE>
struct StringMapType {
	using MAP_TYPE = TYPE;

	static TYPE *CreateEmpty(ArenaAllocator &allocator) {
		return new TYPE(allocator);
	}
};

template <class OP, class T, class MAP_TYPE>
void HistogramUpdateFunction(Vector inputs[], AggregateInputData &aggr_input, idx_t input_count, Vector &state_vector,
                             idx_t count) {
	D_ASSERT(input_count == 1);

	auto &input = inputs[0];

	auto extra_state = OP::CreateExtraState();
	UnifiedVectorFormat input_data;
	OP::PrepareData(input, extra_state, input_data);

	auto states = state_vector.Values<HistogramAggState<T, typename MAP_TYPE::MAP_TYPE> *>();
	auto input_values = UnifiedVectorFormat::GetData<T>(input_data);
	for (idx_t i = 0; i < count; i++) {
		auto idx = input_data.sel->get_index(i);
		if (!input_data.validity.RowIsValid(idx)) {
			continue;
		}
		auto &state = *states[i].GetValue();
		if (!state.hist) {
			state.hist = MAP_TYPE::CreateEmpty(aggr_input.allocator);
		}
		auto &input_value = input_values[idx];
		++(*state.hist)[input_value];
	}
}

template <class OP, class T, class MAP_TYPE>
void HistogramFinalizeFunction(Vector &state_vector, AggregateFinalizeInputData &, Vector &result, idx_t count,
                               idx_t offset) {
	using HIST_STATE = HistogramAggState<T, typename MAP_TYPE::MAP_TYPE>;

	auto states = state_vector.Values<HIST_STATE *>();

	auto &mask = FlatVector::ValidityMutable(result);
	auto old_len = ListVector::GetListSize(result);
	idx_t new_entries = 0;
	// figure out how much space we need
	for (idx_t i = 0; i < count; i++) {
		auto &state = *states[i].GetValue();
		if (!state.hist) {
			continue;
		}
		new_entries += state.hist->size();
	}
	// reserve space in the list vector
	ListVector::Reserve(result, old_len + new_entries);
	auto &keys = MapVector::GetKeys(result);
	auto &values = MapVector::GetValues(result);
	auto list_entries = FlatVector::GetDataMutable<list_entry_t>(result);
	auto count_entries = FlatVector::GetDataMutable<uint64_t>(values);

	idx_t current_offset = old_len;
	for (idx_t i = 0; i < count; i++) {
		const auto rid = i + offset;
		auto &state = *states[i].GetValue();
		if (!state.hist) {
			mask.SetInvalid(rid);
			continue;
		}

		auto &list_entry = list_entries[rid];
		list_entry.offset = current_offset;
		for (auto &entry : *state.hist) {
			OP::template HistogramFinalize<T>(entry.first, keys, current_offset);
			count_entries[current_offset] = entry.second;
			current_offset++;
		}
		list_entry.length = current_offset - list_entry.offset;
	}
	D_ASSERT(current_offset == old_len + new_entries);
	ListVector::SetListSize(result, current_offset);
	result.Verify();
}

template <class OP, class T, class MAP_TYPE>
AggregateFunction GetHistogramFunction(const LogicalType &type) {
	using STATE_TYPE = HistogramAggState<T, typename MAP_TYPE::MAP_TYPE>;
	using HIST_FUNC = HistogramFunction<MAP_TYPE>;

	auto struct_type = LogicalType::MAP(type, LogicalType::UBIGINT);
	auto function = AggregateFunction(
	    "histogram", {}, struct_type, AggregateFunction::StateSize<STATE_TYPE>,
	    AggregateFunction::StateInitialize<STATE_TYPE, HIST_FUNC>, HistogramUpdateFunction<OP, T, MAP_TYPE>,
	    AggregateFunction::StateCombine<STATE_TYPE, HIST_FUNC>, HistogramFinalizeFunction<OP, T, MAP_TYPE>, nullptr,
	    nullptr, AggregateFunction::StateDestroy<STATE_TYPE, HIST_FUNC>);
	function.GetSignature().AddParameter("arg", type);
	function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return function;
}

template <class OP, class T, class MAP_TYPE>
AggregateFunction GetMapTypeInternal(const LogicalType &type) {
	return GetHistogramFunction<OP, T, MAP_TYPE>(type);
}

template <class OP, class T, bool IS_ORDERED>
AggregateFunction GetMapType(const LogicalType &type) {
	if (IS_ORDERED) {
		return GetMapTypeInternal<OP, T, DefaultMapType<sql_value_ordered_map_t<T, idx_t>>>(type);
	}
	return GetMapTypeInternal<OP, T, DefaultMapType<sql_value_map_t<T, idx_t>>>(type);
}

template <class OP, bool IS_ORDERED>
AggregateFunction GetStringMapType(const LogicalType &type) {
	if (IS_ORDERED) {
		return GetMapTypeInternal<OP, string_t, StringMapType<OrderedOwningStringMap<idx_t>>>(type);
	} else {
		return GetMapTypeInternal<OP, string_t, StringMapType<OwningStringMap<idx_t>>>(type);
	}
}

template <bool IS_ORDERED = true>
AggregateFunction GetHistogramFunction(const LogicalType &type) {
	switch (type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(histogram_types)
	case PhysicalType::BOOL:
		return GetMapType<HistogramFunctor, bool, IS_ORDERED>(type);
	case PhysicalType::UINT8:
		return GetMapType<HistogramFunctor, uint8_t, IS_ORDERED>(type);
	case PhysicalType::UINT16:
		return GetMapType<HistogramFunctor, uint16_t, IS_ORDERED>(type);
	case PhysicalType::UINT32:
		return GetMapType<HistogramFunctor, uint32_t, IS_ORDERED>(type);
	case PhysicalType::UINT64:
		return GetMapType<HistogramFunctor, uint64_t, IS_ORDERED>(type);
	case PhysicalType::INT8:
		return GetMapType<HistogramFunctor, int8_t, IS_ORDERED>(type);
	case PhysicalType::INT16:
		return GetMapType<HistogramFunctor, int16_t, IS_ORDERED>(type);
	case PhysicalType::INT32:
		return GetMapType<HistogramFunctor, int32_t, IS_ORDERED>(type);
	case PhysicalType::INT64:
		return GetMapType<HistogramFunctor, int64_t, IS_ORDERED>(type);
	case PhysicalType::FLOAT:
		return GetMapType<HistogramFunctor, float, IS_ORDERED>(type);
	case PhysicalType::DOUBLE:
		return GetMapType<HistogramFunctor, double, IS_ORDERED>(type);
	case PhysicalType::VARCHAR:
		return GetStringMapType<HistogramStringFunctor, IS_ORDERED>(type);
#endif
	default:
		return GetStringMapType<HistogramGenericFunctor, IS_ORDERED>(type);
	}
}

static FrequencyAggregateFinalizeResult FinalizeHistogramRewrite(FrequencyAggregateFinalizeInput &input) {
	auto &context = input.rewrite_input.context;
	auto frequency = BoundCastExpression::AddCastToType(context, std::move(input.frequency), LogicalType::UBIGINT);
	vector<unique_ptr<Expression>> entry_children;
	entry_children.push_back(std::move(input.value));
	entry_children.push_back(std::move(frequency));
	auto entry = BindScalar(context, "row", std::move(entry_children));

	vector<unique_ptr<Expression>> list_children;
	list_children.push_back(std::move(entry));
	auto list = BindAggregate(context, "list", std::move(list_children), std::move(input.filter));
	auto list_type = list->GetReturnType();

	FrequencyAggregateFinalizeResult result;
	result.aggregates.push_back(std::move(list));
	auto list_ref = make_uniq<BoundColumnRefExpression>(
	    list_type, ColumnBinding(input.aggregate_index, ProjectionIndex(result.aggregates.size() - 1)));
	vector<unique_ptr<Expression>> sort_children;
	sort_children.push_back(std::move(list_ref));
	auto sorted_entries = BindScalar(context, "list_sort", std::move(sort_children));
	vector<unique_ptr<Expression>> map_children;
	map_children.push_back(std::move(sorted_entries));
	result.result = BindScalar(context, "map_from_entries", std::move(map_children));
	D_ASSERT(result.result->GetReturnType() == input.rewrite_input.aggregate.GetReturnType());
	return result;
}

static unique_ptr<AggregateRewritePlan> RewriteHistogram(AggregateRewriteInput &input) {
	return FrequencyAggregateRewrite::Create(input, true, false, FinalizeHistogramRewrite);
}

template <bool IS_ORDERED = true>
unique_ptr<FunctionData> HistogramBindFunction(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(arguments.size() == 1);

	if (arguments[0]->GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	function.ReplaceImplementation(GetHistogramFunction<IS_ORDERED>(arguments[0]->GetReturnType()));
	function.SetRewriteCallback(RewriteHistogram, AggregateRewritePolicy::MANDATORY);
	return make_uniq<VariableReturnBindData>(function.GetReturnType());
}

} // namespace

AggregateFunctionSet HistogramFun::GetFunctions() {
	AggregateFunctionSet fun;
	AggregateFunction histogram_function("histogram", {}, LogicalTypeId::MAP, nullptr, nullptr, nullptr, nullptr,
	                                     nullptr, nullptr, HistogramBindFunction, nullptr);
	histogram_function.GetSignature().AddParameter("arg", LogicalType::ANY);
	fun.AddFunction(HistogramFun::BinnedHistogramFunction());
	fun.AddFunction(histogram_function);
	return fun;
}

AggregateFunction HistogramFun::GetHistogramUnorderedMap(LogicalType &type) {
	AggregateFunction function("histogram", {}, LogicalTypeId::MAP, nullptr, nullptr, nullptr, nullptr, nullptr,
	                           nullptr, HistogramBindFunction<false>, nullptr);
	function.GetSignature().AddParameter("arg", LogicalType::ANY);
	return function;
}

} // namespace duckdb
