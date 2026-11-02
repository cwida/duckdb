#include "core_functions/aggregate/distributive_functions.hpp"
#include "core_functions/aggregate/holistic_functions.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/owning_string_map.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/column/column_data_collection.hpp"
#include "duckdb/common/types/sql_value_map.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/function/aggregate/sort_key_helpers.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/optimizer/aggregate_rewrite.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"

// MODE( <expr1> )
// Returns the most frequent value for the values within expr1.
// NULL values are ignored. If all the values are NULL, or there are 0 rows, then the function returns NULL.

namespace std {} // namespace std

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

static unique_ptr<Expression> BindScalar(ClientContext &context, const char *name, unique_ptr<Expression> child) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(child));
	return BindScalar(context, name, std::move(children));
}

static unique_ptr<Expression> BindScalar(ClientContext &context, const char *name, unique_ptr<Expression> left,
                                         unique_ptr<Expression> right) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return BindScalar(context, name, std::move(children));
}

static FrequencyAggregateFinalizeResult FinalizeModeRewrite(FrequencyAggregateFinalizeInput &input) {
	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(input.value));
	const char *aggregate_name;
	if (input.order_key) {
		vector<unique_ptr<Expression>> sort_children;
		sort_children.push_back(std::move(input.frequency));
		sort_children.push_back(make_uniq<BoundConstantExpression>(Value("DESC NULLS LAST")));
		sort_children.push_back(std::move(input.order_key));
		sort_children.push_back(make_uniq<BoundConstantExpression>(Value("ASC NULLS LAST")));
		auto sort_key = BindScalar(input.rewrite_input.context, "create_sort_key", std::move(sort_children));
		children.push_back(std::move(sort_key));
		aggregate_name = "arg_min";
	} else {
		children.push_back(std::move(input.frequency));
		aggregate_name = "arg_max";
	}

	FrequencyAggregateFinalizeResult result;
	auto aggregate =
	    BindAggregate(input.rewrite_input.context, aggregate_name, std::move(children), std::move(input.filter));
	D_ASSERT(aggregate->GetReturnType() == input.rewrite_input.aggregate.GetReturnType());
	result.aggregates.push_back(std::move(aggregate));
	result.result = make_uniq<BoundColumnRefExpression>(input.rewrite_input.aggregate.GetReturnType(),
	                                                    ColumnBinding(input.aggregate_index, ProjectionIndex(0)));
	return result;
}

static unique_ptr<AggregateRewritePlan> RewriteMode(AggregateRewriteInput &input) {
	return FrequencyAggregateRewrite::Create(input, true, true, FinalizeModeRewrite);
}

struct ModeAttr {
	ModeAttr() : count(0), first_row(std::numeric_limits<idx_t>::max()) {
	}
	size_t count;
	idx_t first_row;
};

template <class T>
struct ModeStandard {
	using MAP_TYPE = sql_value_map_t<T, ModeAttr>;

	static MAP_TYPE *CreateEmpty(ArenaAllocator &) {
		return new MAP_TYPE();
	}
	static MAP_TYPE *CreateEmpty(Allocator &) {
		return new MAP_TYPE();
	}

	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Assign(Vector &result, INPUT_TYPE input) {
		return RESULT_TYPE(input);
	}

	static bool IsEqual(const T &left, const T &right) {
		return Equals::Operation(left, right);
	}

	static T *Update(T *mode, idx_t &, const T &key, AggregateInputData &) {
		if (!mode) {
			mode = new T(key);
		}
		*mode = key;

		return mode;
	}
};

struct ModeString {
	using MAP_TYPE = OwningStringMap<ModeAttr>;

	static MAP_TYPE *CreateEmpty(ArenaAllocator &allocator) {
		return new MAP_TYPE(allocator);
	}
	static MAP_TYPE *CreateEmpty(Allocator &allocator) {
		return new MAP_TYPE(allocator);
	}

	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Assign(Vector &result, INPUT_TYPE input) {
		return StringVector::AddStringOrBlob(result, input);
	}

	static bool IsEqual(const string_t &left, const string_t &right) {
		return Equals::Operation(left, right);
	}

	static string_t *Update(string_t *mode, idx_t &alloc_size, const string_t &key,
	                        AggregateInputData &aggr_input_data) {
		if (key.IsInlined()) {
			if (!mode) {
				mode = new string_t(nullptr, 0);
			}
			*mode = key;
			alloc_size = 0;
			return mode;
		}

		// non-inlined string, need to allocate space for it somehow
		const auto len = key.GetSize();
		char *ptr;
		if (mode && alloc_size >= len) {
			// this fits into the current arena allocation - reuse it
			ptr = mode->GetDataWriteable();
		} else {
			// round up so repeatedly growing keys don't churn through a new arena allocation every time
			alloc_size = NextPowerOfTwo(len);
			ptr = char_ptr_cast(aggr_input_data.allocator.Allocate(alloc_size));
			if (!mode) {
				mode = new string_t(nullptr, 0);
			}
		}
		memcpy(ptr, key.GetData(), len);
		*mode = string_t(ptr, len);
		return mode;
	}
};

template <class KEY_TYPE, class TYPE_OP>
struct ModeState {
	using Counts = typename TYPE_OP::MAP_TYPE;

	ModeState() {
	}

	SubFrames prevs;
	Counts *frequency_map = nullptr;
	KEY_TYPE *mode = nullptr;
	//! Must be wide enough for NextPowerOfTwo(MAX_STRING_SIZE), which does not fit in a uint32_t
	idx_t mode_alloc_size = 0;
	size_t nonzero = 0;
	bool valid = false;
	size_t count = 0;

	//! The collection being read
	const ColumnDataCollection *inputs;
	//! The state used for reading the collection on this thread
	ColumnDataScanState *scan = nullptr;
	//! The data chunk paged into into
	DataChunk page;
	//! Cell access into the page, rebuilt per page load - scans can return constant and dictionary vectors
	unique_ptr<VectorIterator<KEY_TYPE>> values;

	~ModeState() {
		if (frequency_map) {
			delete frequency_map;
		}
		delete mode;
		if (scan) {
			delete scan;
		}
	}

	void InitializePage(const WindowPartitionInput &partition) {
		if (!scan) {
			scan = new ColumnDataScanState();
		}
		if (page.ColumnCount() == 0) {
			D_ASSERT(partition.inputs);
			inputs = partition.inputs;
			D_ASSERT(partition.column_ids.size() == 1);
			inputs->InitializeScan(*scan, partition.column_ids);
			inputs->InitializeScanChunk(*scan, page);
		}
	}

	inline sel_t RowOffset(idx_t row_idx) const {
		D_ASSERT(RowIsVisible(row_idx));
		return UnsafeNumericCast<sel_t>(row_idx - scan->current_row_index);
	}

	inline bool RowIsVisible(idx_t row_idx) const {
		return (row_idx < scan->next_row_index && scan->current_row_index <= row_idx);
	}

	inline idx_t Seek(idx_t row_idx) {
		if (!RowIsVisible(row_idx)) {
			D_ASSERT(inputs);
			inputs->Seek(row_idx, *scan, page);
			values = make_uniq<VectorIterator<KEY_TYPE>>(page.data[0]);
		}
		return RowOffset(row_idx);
	}

	inline const KEY_TYPE &GetCell(idx_t row_idx) {
		const auto offset = Seek(row_idx);
		return (*values)[offset].GetValueUnsafe();
	}

	inline bool RowIsValid(idx_t row_idx) {
		const auto offset = Seek(row_idx);
		return (*values)[offset].IsValid();
	}

	void Reset() {
		if (frequency_map) {
			frequency_map->clear();
		}
		nonzero = 0;
		count = 0;
		valid = false;
	}

	void ModeAdd(idx_t row, AggregateInputData &aggr_input_data) {
		const auto &key = GetCell(row);
		auto &attr = (*frequency_map)[key];
		auto new_count = (attr.count += 1);
		nonzero += size_t(new_count == 1);
		if (new_count > count) {
			valid = true;
			count = new_count;
			Update(key, aggr_input_data);
		} else if (new_count == count) {
			// The key now ties the mode, so the tie must be broken by frame position
			valid = false;
		}
	}

	void ModeRm(idx_t frame) {
		const auto &key = GetCell(frame);
		auto &attr = (*frequency_map)[key];
		auto old_count = attr.count;
		nonzero -= size_t(old_count == 1);

		attr.count -= 1;
		if (count == old_count && TYPE_OP::IsEqual(key, *mode)) {
			valid = false;
		}
	}

	void Update(const KEY_TYPE &key, AggregateInputData &aggr_input_data) {
		mode = TYPE_OP::Update(mode, mode_alloc_size, key, aggr_input_data);
	}

	//! Rescan the frequency map, breaking ties by the first tied value in the frames.
	//! The per-value first_row is not tracked by ModeRm, so it cannot be used here.
	template <class INCLUDED>
	void ScanFrames(const SubFrames &frames, const INCLUDED &included, AggregateInputData &aggr_input_data) {
		auto highest_frequency = frequency_map->begin();
		size_t n_ties = 0;
		for (auto i = frequency_map->begin(); i != frequency_map->end(); ++i) {
			if (i == highest_frequency || i->second.count > highest_frequency->second.count) {
				highest_frequency = i;
				n_ties = 1;
			} else if (i->second.count == highest_frequency->second.count) {
				++n_ties;
			}
		}
		valid = highest_frequency != frequency_map->end() && highest_frequency->second.count > 0;
		if (!valid) {
			return;
		}
		count = highest_frequency->second.count;
		if (n_ties == 1) {
			Update(highest_frequency->first, aggr_input_data);
			return;
		}
		for (const auto &frame : frames) {
			for (auto i = frame.start; i < frame.end; ++i) {
				if (!included(i)) {
					continue;
				}
				const auto &key = GetCell(i);
				const auto attr = frequency_map->find(key);
				if (attr != frequency_map->end() && attr->second.count == count) {
					Update(key, aggr_input_data);
					return;
				}
			}
		}
	}

	typename Counts::const_iterator Scan() const {
		//! Initialize control variables to first variable of the frequency map
		auto highest_frequency = frequency_map->begin();
		for (auto i = highest_frequency; i != frequency_map->end(); ++i) {
			// Tie break with the lowest insert position
			if (i->second.count > highest_frequency->second.count ||
			    (i->second.count == highest_frequency->second.count &&
			     i->second.first_row < highest_frequency->second.first_row)) {
				highest_frequency = i;
			}
		}
		return highest_frequency;
	}
};

template <typename STATE>
struct ModeIncluded {
	inline explicit ModeIncluded(const ValidityMask &fmask_p, STATE &state) : fmask(fmask_p), state(state) {
	}

	inline bool operator()(const idx_t &idx) const {
		return fmask.RowIsValid(idx) && state.RowIsValid(idx);
	}
	const ValidityMask &fmask;
	STATE &state;
};

template <typename TYPE_OP>
struct BaseModeFunction {
	template <class STATE>
	static void Initialize(STATE &state) {
		new (&state) STATE();
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Execute(STATE &state, const INPUT_TYPE &key, AggregateInputData &input_data) {
		if (!state.frequency_map) {
			state.frequency_map = TYPE_OP::CreateEmpty(input_data.allocator);
		}
		auto &i = (*state.frequency_map)[key];
		++i.count;
		i.first_row = MinValue<idx_t>(i.first_row, state.count);
		++state.count;
	}

	template <class INPUT_TYPE, class STATE, class OP>
	static void Operation(STATE &state, const INPUT_TYPE &key, AggregateUnaryInput &aggr_input) {
		Execute<INPUT_TYPE, STATE, OP>(state, key, aggr_input.input);
	}

	template <class STATE, class OP>
	static void Combine(const STATE &source, STATE &target, AggregateInputData &aggr_input_data) {
		if (!source.frequency_map) {
			return;
		}
		if (!target.frequency_map) {
			target.frequency_map = TYPE_OP::CreateEmpty(aggr_input_data.allocator);
		}
		for (auto &val : *source.frequency_map) {
			auto &i = (*target.frequency_map)[val.first];
			i.count += val.second.count;
			i.first_row = MinValue(i.first_row, val.second.first_row);
		}
		target.count += source.count;
	}

	static bool IgnoreNull() {
		return true;
	}

	template <class STATE>
	static void Destroy(STATE &state, AggregateInputData &aggr_input_data) {
		state.~STATE();
	}
};

template <typename TYPE_OP>
struct TypedModeFunction : BaseModeFunction<TYPE_OP> {
	template <class INPUT_TYPE, class STATE, class OP>
	static void ConstantOperation(STATE &state, const INPUT_TYPE &key, AggregateUnaryInput &aggr_input, idx_t count) {
		if (!state.frequency_map) {
			state.frequency_map = TYPE_OP::CreateEmpty(aggr_input.input.allocator);
		}
		auto &i = (*state.frequency_map)[key];
		i.count += count;
		i.first_row = MinValue<idx_t>(i.first_row, state.count);
		state.count += count;
	}
};

template <typename TYPE_OP>
struct ModeFunction : TypedModeFunction<TYPE_OP> {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		if (!state.frequency_map) {
			finalize_data.ReturnNull();
			return;
		}
		auto highest_frequency = state.Scan();
		if (highest_frequency != state.frequency_map->end()) {
			target = TYPE_OP::template Assign<T, T>(finalize_data.result, highest_frequency->first);
		} else {
			finalize_data.ReturnNull();
		}
	}

	template <typename STATE, typename INPUT_TYPE>
	struct UpdateWindowState {
		STATE &state;
		ModeIncluded<STATE> &included;
		AggregateInputData &aggr_input_data;

		inline UpdateWindowState(STATE &state, ModeIncluded<STATE> &included, AggregateInputData &aggr_input_data)
		    : state(state), included(included), aggr_input_data(aggr_input_data) {
		}

		inline void Neither(idx_t begin, idx_t end) {
		}

		inline void Left(idx_t begin, idx_t end) {
			for (; begin < end; ++begin) {
				if (included(begin)) {
					state.ModeRm(begin);
				}
			}
		}

		inline void Right(idx_t begin, idx_t end) {
			for (; begin < end; ++begin) {
				if (included(begin)) {
					state.ModeAdd(begin, aggr_input_data);
				}
			}
		}

		inline void Both(idx_t begin, idx_t end) {
		}
	};

	template <class STATE, class INPUT_TYPE, class RESULT_TYPE>
	static void Window(AggregateInputData &aggr_input_data, const WindowPartitionInput &partition,
	                   const_data_ptr_t g_state, data_ptr_t l_state, const SubFrames *subframes_per_row, idx_t count,
	                   Vector &result, idx_t row_idx) {
		auto &state = *reinterpret_cast<STATE *>(l_state);

		state.InitializePage(partition);
		const auto &fmask = partition.filter_mask;

		auto rdata = FlatVector::GetDataMutable<RESULT_TYPE>(result);
		auto &rmask = FlatVector::ValidityMutable(result);
		auto &prevs = state.prevs;
		if (prevs.empty()) {
			prevs.resize(1);
		}

		ModeIncluded<STATE> included(fmask, state);

		using Updater = UpdateWindowState<STATE, INPUT_TYPE>;
		Updater updater(state, included, aggr_input_data);

		if (!state.frequency_map) {
			state.frequency_map = TYPE_OP::CreateEmpty(Allocator::DefaultAllocator());
		}
		const size_t tau_inverse = 4; // tau==0.25
		for (idx_t rid = 0; rid < count; ++rid) {
			const auto &frames = subframes_per_row[rid];

			if (state.nonzero <= (state.frequency_map->size() / tau_inverse) ||
			    prevs.back().end <= frames.front().start || frames.back().end <= prevs.front().start) {
				state.Reset();
				// for f ∈ F do
				for (const auto &frame : frames) {
					for (auto i = frame.start; i < frame.end; ++i) {
						if (included(i)) {
							state.ModeAdd(i, aggr_input_data);
						}
					}
				}
			} else {
				AggregateExecutor::IntersectFrames(prevs, frames, updater);
			}

			if (!state.valid) {
				state.ScanFrames(frames, included, aggr_input_data);
			}

			if (state.valid) {
				rdata[rid] = TYPE_OP::template Assign<INPUT_TYPE, RESULT_TYPE>(result, *state.mode);
			} else {
				rmask.Set(rid, false);
			}

			prevs = frames;
		}
	}
};

template <typename TYPE_OP>
struct ModeFallbackFunction : BaseModeFunction<TYPE_OP> {
	template <class STATE>
	static void Finalize(STATE &state, AggregateFinalizeData &finalize_data) {
		if (!state.frequency_map) {
			finalize_data.ReturnNull();
			return;
		}
		auto highest_frequency = state.Scan();
		if (highest_frequency != state.frequency_map->end()) {
			CreateSortKeyHelpers::DecodeSortKey(highest_frequency->first, finalize_data.result,
			                                    finalize_data.result_idx,
			                                    OrderModifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST));
		} else {
			finalize_data.ReturnNull();
		}
	}
};

AggregateFunction GetFallbackModeFunction(const LogicalType &type) {
	using STATE = ModeState<string_t, ModeString>;
	using OP = ModeFallbackFunction<ModeString>;
	AggregateFunction aggr({type}, type, AggregateFunction::StateSize<STATE>,
	                       AggregateFunction::StateInitialize<STATE, OP, AggregateDestructorType::LEGACY>,
	                       AggregateSortKeyHelpers::UnaryUpdate<STATE, OP>, AggregateFunction::StateCombine<STATE, OP>,
	                       AggregateFunction::StateVoidFinalize<STATE, OP>, FunctionNullHandling::DEFAULT_NULL_HANDLING,
	                       AggregateFunction::NoClusterUpdate());
	aggr.SetStateDestructorCallback(AggregateFunction::StateDestroy<STATE, OP>);
	aggr.SetIsHolistic(true);
	return aggr;
}

template <typename INPUT_TYPE, typename TYPE_OP = ModeStandard<INPUT_TYPE>>
AggregateFunction GetTypedModeFunction(const LogicalType &type) {
	using STATE = ModeState<INPUT_TYPE, TYPE_OP>;
	using OP = ModeFunction<TYPE_OP>;
	auto func = AggregateFunction::UnaryAggregate<STATE, INPUT_TYPE, INPUT_TYPE, OP, AggregateDestructorType::LEGACY>(
	    type, type);
	func.SetWindowBatchCallback(OP::template Window<STATE, INPUT_TYPE, INPUT_TYPE>);
	func.SetIsHolistic(true);
	return func;
}

AggregateFunction GetModeAggregate(const LogicalType &type) {
	switch (type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(mode_types)
	case PhysicalType::INT8:
		return GetTypedModeFunction<int8_t>(type);
	case PhysicalType::UINT8:
		return GetTypedModeFunction<uint8_t>(type);
	case PhysicalType::INT16:
		return GetTypedModeFunction<int16_t>(type);
	case PhysicalType::UINT16:
		return GetTypedModeFunction<uint16_t>(type);
	case PhysicalType::INT32:
		return GetTypedModeFunction<int32_t>(type);
	case PhysicalType::UINT32:
		return GetTypedModeFunction<uint32_t>(type);
	case PhysicalType::INT64:
		return GetTypedModeFunction<int64_t>(type);
	case PhysicalType::UINT64:
		return GetTypedModeFunction<uint64_t>(type);
	case PhysicalType::INT128:
		return GetTypedModeFunction<hugeint_t>(type);
	case PhysicalType::UINT128:
		return GetTypedModeFunction<uhugeint_t>(type);
	case PhysicalType::FLOAT:
		return GetTypedModeFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetTypedModeFunction<double>(type);
	case PhysicalType::INTERVAL:
		return GetTypedModeFunction<interval_t>(type);
	case PhysicalType::VARCHAR:
		return GetTypedModeFunction<string_t, ModeString>(type);
#endif
	default:
		return GetFallbackModeFunction(type);
	}
}

unique_ptr<FunctionData> BindModeAggregate(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	function.ReplaceImplementation(GetModeAggregate(arguments[0]->GetReturnType()));
	function.SetName("mode");
	function.SetRewriteCallback(RewriteMode, AggregateRewritePolicy::MANDATORY);
	function.SetStatisticsCallback(AggregateFunction::PropagateInputValueStats);
	return nullptr;
}

} // namespace

AggregateFunctionSet ModeFun::GetFunctions() {
	AggregateFunctionSet mode("mode");
	AggregateFunction fun({}, LogicalTypeId::ANY, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NoClusterUpdate(),
	                      BindModeAggregate);
	fun.GetSignature().AddParameter("x", LogicalTypeId::ANY);
	mode.AddFunction(fun);
	return mode;
}

//===--------------------------------------------------------------------===//
// Entropy
//===--------------------------------------------------------------------===//
namespace {

static FrequencyAggregateFinalizeResult FinalizeEntropyRewrite(FrequencyAggregateFinalizeInput &input) {
	auto &context = input.rewrite_input.context;
	vector<unique_ptr<Expression>> total_children;
	total_children.push_back(input.frequency->Copy());
	auto total =
	    BindAggregate(context, "sum", std::move(total_children), input.filter ? input.filter->Copy() : nullptr);

	auto log_frequency = BindScalar(context, "log2", input.frequency->Copy());
	auto weighted_frequency = BindScalar(context, "*", std::move(input.frequency), std::move(log_frequency));
	vector<unique_ptr<Expression>> weighted_children;
	weighted_children.push_back(std::move(weighted_frequency));
	auto weighted = BindAggregate(context, "sum", std::move(weighted_children), std::move(input.filter));

	FrequencyAggregateFinalizeResult result;
	auto total_type = total->GetReturnType();
	auto weighted_type = weighted->GetReturnType();
	result.aggregates.push_back(std::move(total));
	result.aggregates.push_back(std::move(weighted));

	auto total_ref =
	    make_uniq<BoundColumnRefExpression>(total_type, ColumnBinding(input.aggregate_index, ProjectionIndex(0)));
	auto weighted_ref =
	    make_uniq<BoundColumnRefExpression>(weighted_type, ColumnBinding(input.aggregate_index, ProjectionIndex(1)));
	auto log_total = BindScalar(context, "log2", total_ref->Copy());
	auto normalized = BindScalar(context, "/", std::move(weighted_ref), std::move(total_ref));
	auto entropy = BindScalar(context, "-", std::move(log_total), std::move(normalized));
	auto coalesce = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_COALESCE, LogicalType::DOUBLE);
	coalesce->GetChildrenMutable().push_back(std::move(entropy));
	coalesce->GetChildrenMutable().push_back(make_uniq<BoundConstantExpression>(Value::DOUBLE(0)));
	result.result = std::move(coalesce);
	return result;
}

static unique_ptr<AggregateRewritePlan> RewriteEntropy(AggregateRewriteInput &input) {
	return FrequencyAggregateRewrite::Create(input, true, false, FinalizeEntropyRewrite);
}

template <class STATE>
double FinalizeEntropy(STATE &state) {
	if (!state.frequency_map) {
		return 0;
	}
	double count = static_cast<double>(state.count);
	double entropy = 0;
	for (auto &val : *state.frequency_map) {
		double val_sec = static_cast<double>(val.second.count);
		entropy += (val_sec / count) * log2(count / val_sec);
	}
	return entropy;
}

template <typename TYPE_OP>
struct EntropyFunction : TypedModeFunction<TYPE_OP> {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		target = FinalizeEntropy(state);
	}
};

template <typename TYPE_OP>
struct EntropyFallbackFunction : BaseModeFunction<TYPE_OP> {
	template <class T, class STATE>
	static void Finalize(STATE &state, T &target, AggregateFinalizeData &finalize_data) {
		target = FinalizeEntropy(state);
	}
};

template <typename INPUT_TYPE, typename TYPE_OP = ModeStandard<INPUT_TYPE>>
AggregateFunction GetTypedEntropyFunction(const LogicalType &type) {
	using STATE = ModeState<INPUT_TYPE, TYPE_OP>;
	using OP = EntropyFunction<TYPE_OP>;
	auto func = AggregateFunction::UnaryAggregate<STATE, INPUT_TYPE, double, OP, AggregateDestructorType::LEGACY>(
	    type, LogicalType::DOUBLE);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return func;
}

AggregateFunction GetFallbackEntropyFunction(const LogicalType &type) {
	using STATE = ModeState<string_t, ModeString>;
	using OP = EntropyFallbackFunction<ModeString>;
	AggregateFunction func({type}, LogicalType::DOUBLE, AggregateFunction::StateSize<STATE>,
	                       AggregateFunction::StateInitialize<STATE, OP, AggregateDestructorType::LEGACY>,
	                       AggregateSortKeyHelpers::UnaryUpdate<STATE, OP>, AggregateFunction::StateCombine<STATE, OP>,
	                       AggregateFunction::StateFinalize<STATE, double, OP>,
	                       FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NoClusterUpdate());
	func.SetStateDestructorCallback(AggregateFunction::StateDestroy<STATE, OP>);
	func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	func.SetOrderDependent(AggregateOrderDependent::NOT_ORDER_DEPENDENT);
	return func;
}

AggregateFunction GetEntropyFunction(const LogicalType &type) {
	switch (type.InternalType()) {
#if !DUCKDB_SMALLER_BINARY(entropy_types)
	case PhysicalType::UINT16:
		return GetTypedEntropyFunction<uint16_t>(type);
	case PhysicalType::UINT32:
		return GetTypedEntropyFunction<uint32_t>(type);
	case PhysicalType::UINT64:
		return GetTypedEntropyFunction<uint64_t>(type);
	case PhysicalType::INT16:
		return GetTypedEntropyFunction<int16_t>(type);
	case PhysicalType::INT32:
		return GetTypedEntropyFunction<int32_t>(type);
	case PhysicalType::INT64:
		return GetTypedEntropyFunction<int64_t>(type);
	case PhysicalType::FLOAT:
		return GetTypedEntropyFunction<float>(type);
	case PhysicalType::DOUBLE:
		return GetTypedEntropyFunction<double>(type);
	case PhysicalType::VARCHAR:
		return GetTypedEntropyFunction<string_t, ModeString>(type);
#endif
	default:
		return GetFallbackEntropyFunction(type);
	}
}

unique_ptr<FunctionData> BindEntropyAggregate(BindAggregateFunctionInput &input) {
	auto &function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	function.ReplaceImplementation(GetEntropyFunction(arguments[0]->GetReturnType()));
	function.SetName("entropy");
	function.SetRewriteCallback(RewriteEntropy, AggregateRewritePolicy::MANDATORY);
	return nullptr;
}

} // namespace

AggregateFunctionSet EntropyFun::GetFunctions() {
	AggregateFunctionSet entropy("entropy");
	AggregateFunction fun({}, LogicalType::DOUBLE, nullptr, nullptr, nullptr, nullptr, nullptr,
	                      FunctionNullHandling::DEFAULT_NULL_HANDLING, AggregateFunction::NoClusterUpdate(),
	                      BindEntropyAggregate);
	fun.GetSignature().AddParameter("x", LogicalTypeId::ANY);
	entropy.AddFunction(fun);
	return entropy;
}

} // namespace duckdb
