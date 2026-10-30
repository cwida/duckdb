#include "core_functions/aggregate/distributive_functions.hpp"
#include "core_functions/aggregate/sum_helpers.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/bignum.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"

namespace duckdb {

namespace {

struct SumSetOperation {
	template <class STATE>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		if (!source.is_set) {
			return;
		}
		if (!target.is_set) {
			target.value = source.value;
			target.is_set = true;
		} else {
			CombineSumStateValue(target.value, source.value);
		}
	}
	template <class STATE>
	static void AddValues(STATE &state, idx_t count) {
		state.is_set = true;
	}
};

struct KahanSumSetOperation {
	template <class STATE>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &) {
		target.Combine(source);
	}
	template <class STATE>
	static void AddValues(STATE &state, idx_t count) {
		state.is_set = true;
	}
};

template <class BASE>
struct ClusteredSumStateCopy : public BASE, public ClusteredStateCopy {
	template <class STATE>
	static void FlushClusteredLocal(STATE &state, STATE &local, bool saw_value) {
		if (saw_value) {
			local.is_set = true;
		}
		state = local;
	}
};

template <class ADD_OP>
struct ClusteredAddOp {
	template <class STATE, class INPUT_TYPE>
	static void Execute(STATE &local, const INPUT_TYPE &input) {
		ADD_OP::template AddNumber<STATE, INPUT_TYPE>(local, input);
	}

	template <class STATE, class INPUT_TYPE>
	static void Execute(STATE &local, const INPUT_TYPE &input, idx_t count) {
		ADD_OP::template AddConstant<STATE, INPUT_TYPE>(local, input, count);
	}
};

template <class BASE, class LOCAL_OP>
struct ClusteredSumOperation : public ClusteredSumStateCopy<BASE> {
	static constexpr bool ClusteredI64HugeintSum = std::is_same<LOCAL_OP, ClusteredAddOp<AddToHugeint>>::value;

	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input) {
		LOCAL_OP::template Execute<STATE>(local, input);
	}

	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input, idx_t count) {
		LOCAL_OP::template Execute<STATE>(local, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = state.value;
		}
	}

	template <class STATE_TYPE, class INPUT_TYPE>
	static void ExecuteFlatI64HugeintSum(const INPUT_TYPE *vals, const ClusteredAggr &clustered,
	                                     const SelectionVector *isel, const sel_t *cluster_iter) {
		idx_t pos = 0;
		for (auto &run : clustered.runs()) {
			auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
			const auto *run_sel = run.sel;
			const idx_t run_count = run.count;
			if (run_count == 0) {
				continue;
			}
			state.is_set = true;
			int64_t local64 = 0;
			auto add_row = [&](idx_t idx) {
				const int64_t v = static_cast<int64_t>(vals[idx]);
				if (DUCKDB_UNLIKELY(!I64VectorSumSafe(v))) {
					state.value = Hugeint::Add(state.value, v);
				} else {
					local64 += v;
				}
			};
			if (cluster_iter) {
				for (idx_t k = 0; k < run_count; k++) {
					add_row(cluster_iter[pos + k]);
				}
			} else if (isel && run_sel) {
				for (idx_t k = 0; k < run_count; k++) {
					add_row(isel->get_index(run_sel[k]));
				}
			} else if (isel) {
				for (idx_t k = 0; k < run_count; k++) {
					add_row(isel->get_index(k));
				}
			} else if (run_sel) {
				for (idx_t k = 0; k < run_count; k++) {
					add_row(run_sel[k]);
				}
			} else {
				for (idx_t k = 0; k < run_count; k++) {
					add_row(k);
				}
			}
			pos += run_count;
			state.value = Hugeint::Add(state.value, local64);
		}
	}

	template <class STATE_TYPE>
	static void ExecuteDictI64HugeintSum(Vector &input, const ClusteredAggr &clustered, const int64_t *safe_vals) {
		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);
		const auto *dict_sel = idata.sel->data();
		auto &validity = idata.validity;
		for (auto &run : clustered.runs()) {
			auto &state = *reinterpret_cast<STATE_TYPE *>(run.state);
			const auto *run_sel = run.sel;
			const idx_t run_count = run.count;
			int64_t local64 = 0;
			if (run_sel) {
				for (idx_t k = 0; k < run_count; k++) {
					local64 += safe_vals[dict_sel[run_sel[k]]];
				}
			} else {
				for (idx_t k = 0; k < run_count; k++) {
					local64 += safe_vals[dict_sel[k]];
				}
			}
			if (local64 != 0) {
				state.is_set = true;
				state.value = Hugeint::Add(state.value, local64);
			} else if (!state.is_set) { // rare: we added 0 -- were all values NULL?
				if (!validity.CanHaveNull()) {
					state.is_set = true;
				} else {
					for (idx_t k = 0; k < run_count; k++) {
						const idx_t i = dict_sel[run_sel ? run_sel[k] : k];
						if (validity.RowIsValidUnsafe(i)) { // we added non-NULL
							state.is_set = true;
							break;
						}
					}
				}
			}
		}
	}
};

struct IntegerSumOperation
    : public ClusteredSumOperation<BaseSumOperation<SumSetOperation, RegularAdd>, ClusteredAddOp<RegularAdd>> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedSumState::Combine(source, target, input, count);
	}

	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = Hugeint::Convert(state.value);
		}
	}
};

struct SumToHugeintOperation
    : public ClusteredSumOperation<BaseSumOperation<SumSetOperation, AddToHugeint>, ClusteredAddOp<AddToHugeint>> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedSumState::Combine(source, target, input, count);
	}
};

struct NumericSumOperation
    : public ClusteredSumOperation<BaseSumOperation<SumSetOperation, RegularAdd>, ClusteredAddOp<RegularAdd>> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedSumState::Combine(source, target, input, count);
	}
};

struct KahanSumOperation : public BaseSumOperation<KahanSumSetOperation, KahanAdd> {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = state.value;
		}
	}
};

struct HugeintSumOperation
    : public ClusteredSumOperation<BaseSumOperation<SumSetOperation, HugeintAdd>, ClusteredAddOp<HugeintAdd>> {
	template <class STATE, class OP>
	static void RepeatedCombine(const STATE &source, STATE &target, AggregateInputData &input, idx_t count) {
		RepeatedSumState::Combine(source, target, input, count);
	}
};

//! The sum of a DECIMAL is a DECIMAL(38) - finalizing throws if the sum does not fit in it
template <class OP>
struct DecimalSumOperation : public OP {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		OP::template Finalize<T, STATE>(state, target, finalize_data);
		if (!state.is_set) {
			return;
		}
		const auto &max_value = Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL];
		if (target >= max_value || target <= -max_value) {
			throw OutOfRangeException("Overflow in SUM of DECIMAL: the result does not fit in DECIMAL(%d)",
			                          Decimal::MAX_WIDTH_DECIMAL);
		}
	}
};

//! Binds the regular sum that sum_no_overflow is a specialized implementation of
pair<BoundAggregateFunction, unique_ptr<FunctionData>> BindLogicalSum(ClientContext &context,
                                                                      vector<unique_ptr<Expression>> &arguments) {
	auto &entry = Catalog::GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), Identifier("sum")));
	vector<pair<Identifier, unique_ptr<Expression>>> keyword_args;
	auto selection = FunctionBinder::SelectFunction(context, entry.functions, {arguments, keyword_args});
	if (!selection.IsSuccess()) {
		selection.GetError().Throw();
	}
	FunctionBinder binder(context);
	return binder.ResolveAggregateFunction(selection.GetBinding(), arguments, keyword_args);
}

void SumNoOverflowSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                            const BoundAggregateFunction &function) {
	// record whether the implementation was installed without a statistics proof (bound by name)
	serializer.WritePropertyWithDefault<bool>(100, "fallible",
	                                          function.GetErrorMode() == FunctionErrors::CAN_THROW_RUNTIME_ERROR);
}

unique_ptr<FunctionData> SumNoOverflowDeserialize(Deserializer &deserializer, BoundAggregateFunction &function) {
	bool fallible = false;
	deserializer.ReadPropertyWithExplicitDefault<bool>(100, "fallible", fallible, false);
	auto &context = deserializer.Get<ClientContext &>();
	auto &return_type = deserializer.Get<const LogicalType &>();
	auto &children = deserializer.Get<const const_expression_list_t &>();
	vector<unique_ptr<Expression>> arguments;
	for (auto &child : children) {
		arguments.push_back(child.get().Copy());
	}
	auto logical = BindLogicalSum(context, arguments);
	if (logical.second || (!return_type.IsAggregateState() && logical.first.GetReturnType() != return_type)) {
		throw SerializationException("Cannot reconstruct the logical sum signature");
	}
	logical.first.ReplaceImplementation(function);
	function = std::move(logical.first);
	function.SetReturnType(return_type);
	if (fallible) {
		function.SetFallible();
	}
	return nullptr;
}

unique_ptr<FunctionData> SumNoOverflowBind(BindAggregateFunctionInput &input);

AggregateFunction GetSumAggregateNoOverflow(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT32: {
		auto function = AggregateFunction::UnaryAggregate<SumState<int64_t>, int32_t, hugeint_t, IntegerSumOperation>(
		    LogicalType::INTEGER, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetName("sum_no_overflow");
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		function.SetBindCallback(SumNoOverflowBind);
		function.SetSerializeCallback(SumNoOverflowSerialize);
		function.SetDeserializeCallback(SumNoOverflowDeserialize);
		return function;
	}
	case PhysicalType::INT64: {
		auto function = AggregateFunction::UnaryAggregate<SumState<int64_t>, int64_t, hugeint_t, IntegerSumOperation>(
		    LogicalType::BIGINT, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetName("sum_no_overflow");
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		function.SetBindCallback(SumNoOverflowBind);
		function.SetSerializeCallback(SumNoOverflowSerialize);
		function.SetDeserializeCallback(SumNoOverflowDeserialize);
		return function;
	}
	default:
		throw BinderException("Unsupported internal type for sum_no_overflow");
	}
}

AggregateFunction GetSumAggregateNoOverflowDecimal() {
	AggregateFunction aggr({}, LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr,
	                       FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NoClusterUpdate(),
	                       SumNoOverflowBind);
	aggr.GetSignature().AddParameter("arg", LogicalTypeId::DECIMAL);
	aggr.SetSerializeCallback(SumNoOverflowSerialize);
	aggr.SetDeserializeCallback(SumNoOverflowDeserialize);
	return aggr;
}

//! Binding sum_no_overflow by name (e.g. when re-binding an exported aggregate state) delegates to the regular sum,
//! with the no-overflow implementation swapped in as statistics propagation does
unique_ptr<FunctionData> SumNoOverflowBind(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	const auto parameter_type = function.GetArguments()[0];
	auto logical = BindLogicalSum(input.GetClientContext(), arguments);
	if (parameter_type.id() != LogicalTypeId::DECIMAL) {
		logical.first.ReplaceImplementation(GetSumAggregateNoOverflow(parameter_type.InternalType()));
		// bound by name there is no statistics proof that the int64 accumulator cannot overflow
		logical.first.SetFallible();
	}
	function = std::move(logical.first);
	return std::move(logical.second);
}

unique_ptr<BaseStatistics> SumPropagateStats(ClientContext &context, BoundAggregateExpression &expr,
                                             AggregateStatisticsInput &input) {
	if (!input.node_stats || !input.node_stats->has_max_cardinality) {
		return nullptr;
	}
	auto &numeric_stats = input.child_stats[0];
	if (!NumericStats::HasMinMax(numeric_stats)) {
		return nullptr;
	}
	auto internal_type = numeric_stats.GetType().InternalType();
	hugeint_t max_negative = 0;
	hugeint_t max_positive = 0;
	switch (internal_type) {
	case PhysicalType::BOOL:
		max_negative = NumericStats::Min(numeric_stats).GetValueUnsafe<bool>() ? 1 : 0;
		max_positive = NumericStats::Max(numeric_stats).GetValueUnsafe<bool>() ? 1 : 0;
		break;
	case PhysicalType::INT16:
		max_negative = NumericStats::Min(numeric_stats).GetValueUnsafe<int16_t>();
		max_positive = NumericStats::Max(numeric_stats).GetValueUnsafe<int16_t>();
		break;
	case PhysicalType::INT32:
		max_negative = NumericStats::Min(numeric_stats).GetValueUnsafe<int32_t>();
		max_positive = NumericStats::Max(numeric_stats).GetValueUnsafe<int32_t>();
		break;
	case PhysicalType::INT64:
		max_negative = NumericStats::Min(numeric_stats).GetValueUnsafe<int64_t>();
		max_positive = NumericStats::Max(numeric_stats).GetValueUnsafe<int64_t>();
		break;
	case PhysicalType::INT128:
		max_negative = NumericStats::Min(numeric_stats).GetValueUnsafe<hugeint_t>();
		max_positive = NumericStats::Max(numeric_stats).GetValueUnsafe<hugeint_t>();
		break;
	default:
		throw InternalException("Unsupported type for propagate sum stats");
	}

	const auto max_card = Hugeint::Convert(input.node_stats->max_cardinality);
	hugeint_t wide_negative = 0;
	hugeint_t wide_positive = 0;
	if (!Hugeint::TryMultiply(max_negative, max_card, wide_negative) ||
	    !Hugeint::TryMultiply(max_positive, max_card, wide_positive)) {
		return nullptr;
	}

	// Replace with fast implementation when possible: only INT32/INT64 have a narrower no-overflow implementation to
	// swap in.
	const bool has_no_overflow_variant = internal_type == PhysicalType::INT32 || internal_type == PhysicalType::INT64;
	const bool sum_fits_in_int64 =
	    wide_negative > NumericLimits<int64_t>::Minimum() && wide_positive < NumericLimits<int64_t>::Maximum();
	if (has_no_overflow_variant && sum_fits_in_int64) {
		expr.FunctionMutable().ReplaceImplementation(GetSumAggregateNoOverflow(internal_type));
	}

	// Propagate stats.
	auto sum_min = max_negative <= 0 ? wide_negative : max_negative;
	auto sum_max = max_positive >= 0 ? wide_positive : max_positive;
	auto result = NumericStats::CreateEmpty(expr.GetReturnType()).ToUnique();
	NumericStats::SetMin(*result, Value::HUGEINT(sum_min));
	NumericStats::SetMax(*result, Value::HUGEINT(sum_max));
	result->Set(StatsInfo::CAN_HAVE_NULL_AND_VALID_VALUES);
	return result;
}

AggregateFunction GetSumAggregate(PhysicalType type) {
	switch (type) {
	case PhysicalType::BOOL: {
		auto function = AggregateFunction::UnaryAggregate<SumState<int64_t>, bool, hugeint_t, IntegerSumOperation>(
		    LogicalType::BOOLEAN, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetStatisticsCallback(SumPropagateStats);
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		return function;
	}
	case PhysicalType::INT16: {
		auto function = AggregateFunction::UnaryAggregate<SumState<int64_t>, int16_t, hugeint_t, IntegerSumOperation>(
		    LogicalType::SMALLINT, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetStatisticsCallback(SumPropagateStats);
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		return function;
	}

	case PhysicalType::INT32: {
		auto function =
		    AggregateFunction::UnaryAggregate<SumState<hugeint_t>, int32_t, hugeint_t, SumToHugeintOperation>(
		        LogicalType::INTEGER, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetStatisticsCallback(SumPropagateStats);
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		return function;
	}
	case PhysicalType::INT64: {
		auto function =
		    AggregateFunction::UnaryAggregate<SumState<hugeint_t>, int64_t, hugeint_t, SumToHugeintOperation>(
		        LogicalType::BIGINT, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetStatisticsCallback(SumPropagateStats);
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		return function;
	}
	case PhysicalType::INT128: {
		auto function =
		    AggregateFunction::UnaryAggregate<SumState<hugeint_t>, hugeint_t, hugeint_t, HugeintSumOperation>(
		        LogicalType::HUGEINT, LogicalType::HUGEINT);
		function.GetSignature().GetParameter(0).SetName("arg");
		function.SetStatisticsCallback(SumPropagateStats);
		function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
		function.SetFallible(); // adding HUGEINT values to the HUGEINT accumulator throws on overflow
		return function;
	}
	default:
		throw InternalException("Unimplemented sum aggregate");
	}
}

template <class INPUT_TYPE, class OP>
AggregateFunction GetDecimalSumAggregate(const LogicalType &input_type) {
	auto function =
	    AggregateFunction::UnaryAggregate<SumState<hugeint_t>, INPUT_TYPE, hugeint_t, DecimalSumOperation<OP>>(
	        input_type, LogicalType::HUGEINT);
	function.GetSignature().GetParameter(0).SetName("arg");
	function.SetStatisticsCallback(SumPropagateStats);
	function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return function;
}

AggregateFunction GetDecimalSumAggregate(PhysicalType type) {
	switch (type) {
	case PhysicalType::INT32:
		return GetDecimalSumAggregate<int32_t, SumToHugeintOperation>(LogicalType::INTEGER);
	case PhysicalType::INT64:
		return GetDecimalSumAggregate<int64_t, SumToHugeintOperation>(LogicalType::BIGINT);
	case PhysicalType::INT128: {
		auto function = GetDecimalSumAggregate<hugeint_t, HugeintSumOperation>(LogicalType::HUGEINT);
		// wide decimal sums overflow the HUGEINT accumulator and the DECIMAL(38) bound in finalize - narrower
		// widths accumulate at most 2^64 values of less than 10^18, which stays below both limits
		function.SetFallible();
		return function;
	}
	default:
		// the sum of smaller decimals is accumulated in an int64_t, which always fits in a DECIMAL(38)
		return GetSumAggregate(type);
	}
}

unique_ptr<FunctionData> BindDecimalSum(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	auto decimal_type = BindDecimalArgument(input.GetClientContext(), arguments[0], "sum");
	function.ReplaceImplementation(GetDecimalSumAggregate(decimal_type.InternalType()));
	function.SetName("sum");
	function.GetArguments()[0] = decimal_type;
	function.SetReturnType(LogicalType::DECIMAL(Decimal::MAX_WIDTH_DECIMAL, DecimalType::GetScale(decimal_type)));
	function.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return nullptr;
}

struct BignumState {
	bool is_set;
	BignumIntermediate value;
};

struct BignumOperation {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input,
	                              idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			Operation<INPUT_TYPE, STATE, OP>(state, input, unary_input);
		}
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &input, AggregateUnaryInput &unary_input) {
		if (!state.is_set) {
			state.is_set = true;
			state.value.Initialize(unary_input.input.allocator);
		}
		BignumIntermediate rhs(input);
		state.value.AddInPlace(unary_input.input.allocator, rhs);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &input) {
		if (!source.is_set) {
			return;
		}
		if (!target.is_set) {
			target.value.Initialize(input.allocator);
			target.is_set = true;
		}
		target.value.AddInPlace(input.allocator, source.value);
	}

	template <class TARGET_TYPE, class STATE>
	static void Finalize(STATE &state, TARGET_TYPE &target, AggregateFinalizeData &finalize_data) {
		if (!state.is_set) {
			finalize_data.ReturnNull();
		} else {
			target = state.value.ToBignum(finalize_data.input.allocator);
		}
	}

	static bool IgnoreNull() {
		return true;
	}
};

} // namespace

AggregateFunctionSet SumFun::GetFunctions() {
	AggregateFunctionSet sum;
	// decimal
	AggregateFunction decimal_sum({}, LogicalTypeId::DECIMAL, nullptr, nullptr, nullptr, nullptr, nullptr,
	                              FunctionNullHandling::DEFAULT_NULL_HANDLING, nullptr, BindDecimalSum);
	decimal_sum.GetSignature().AddParameter("arg", LogicalTypeId::DECIMAL);
	sum.AddFunction(decimal_sum);
	sum.AddFunction(GetSumAggregate(PhysicalType::BOOL));
	sum.AddFunction(GetSumAggregate(PhysicalType::INT16));
	sum.AddFunction(GetSumAggregate(PhysicalType::INT32));
	sum.AddFunction(GetSumAggregate(PhysicalType::INT64));
	sum.AddFunction(GetSumAggregate(PhysicalType::INT128));
	auto sum_double = AggregateFunction::UnaryAggregate<SumState<double>, double, double, NumericSumOperation>(
	    LogicalType::DOUBLE, LogicalType::DOUBLE);
	sum_double.GetSignature().GetParameter(0).SetName("arg");
	sum.AddFunction(sum_double);
	auto sum_bignum = AggregateFunction::UnaryAggregate<BignumState, bignum_t, bignum_t, BignumOperation>(
	    LogicalType::BIGNUM, LogicalType::BIGNUM);
	sum_bignum.GetSignature().GetParameter(0).SetName("arg");
	sum.AddFunction(sum_bignum);
	return sum;
}

AggregateFunction CountIfFun::GetFunction() {
	return GetSumAggregate(PhysicalType::BOOL);
}

AggregateFunctionSet SumNoOverflowFun::GetFunctions() {
	AggregateFunctionSet sum_no_overflow;
	sum_no_overflow.AddFunction(GetSumAggregateNoOverflow(PhysicalType::INT32));
	sum_no_overflow.AddFunction(GetSumAggregateNoOverflow(PhysicalType::INT64));
	sum_no_overflow.AddFunction(GetSumAggregateNoOverflowDecimal());
	return sum_no_overflow;
}

AggregateFunction KahanSumFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<KahanSumState, double, double, KahanSumOperation>(LogicalType::DOUBLE,
	                                                                                               LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("arg");
	return fun;
}

} // namespace duckdb
