#include "duckdb/function/table/range.hpp"
#include "duckdb/function/table/summary.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// Range (integers)
//===--------------------------------------------------------------------===//
static void GetParameters(int64_t values[], idx_t value_count, hugeint_t &start, hugeint_t &end, hugeint_t &increment) {
	if (value_count < 2) {
		// single argument: only the end is specified
		start = 0;
		end = values[0];
	} else {
		// two arguments: first two arguments are start and end
		start = values[0];
		end = values[1];
	}
	if (value_count < 3) {
		increment = 1;
	} else {
		increment = values[2];
	}
}

struct RangeFunctionBindData : public TableFunctionData {
	explicit RangeFunctionBindData(const vector<Value> &inputs, bool generate_series) : cardinality(0) {
		int64_t values[3];
		for (idx_t i = 0; i < inputs.size(); i++) {
			if (inputs[i].IsNull()) {
				return;
			}
			values[i] = inputs[i].GetValue<int64_t>();
		}
		hugeint_t start;
		hugeint_t end;
		hugeint_t increment;
		GetParameters(values, inputs.size(), start, end, increment);
		if (generate_series) {
			// generate_series has inclusive bounds on the RHS
			end += increment < 0 ? -1 : 1;
		}

		cardinality = Hugeint::Cast<idx_t>((end - start) / increment);
		if ((end - start) % increment != 0) {
			cardinality += 1;
		}
	}

	idx_t cardinality;
};

template <bool GENERATE_SERIES>
static unique_ptr<FunctionData> RangeFunctionBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.emplace_back(LogicalType::BIGINT);
	if (GENERATE_SERIES) {
		names.emplace_back("generate_series");
	} else {
		names.emplace_back("range");
	}
	if (input.inputs.empty() || input.inputs.size() > 3) {
		return nullptr;
	}
	return make_uniq<RangeFunctionBindData>(input.inputs, GENERATE_SERIES);
}

//! Progress of a range used as a source (i.e. with a single input row)
struct RangeFunctionGlobalState : public GlobalTableFunctionState {
	//! The number of values of the input rows, and the number of values that were generated
	atomic<idx_t> total_values {0};
	atomic<idx_t> generated_values {0};

	//! Input rows are independent, so an in-out call can use all threads
	idx_t MaxThreads() const override {
		return GlobalTableFunctionState::MAX_THREADS;
	}
};

static unique_ptr<GlobalTableFunctionState> RangeFunctionGlobalInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<RangeFunctionGlobalState>();
}

static double RangeFunctionProgress(ClientContext &context, const FunctionData *bind_data,
                                    const GlobalTableFunctionState *global_state) {
	auto &state = global_state->Cast<RangeFunctionGlobalState>();
	auto total = state.total_values.load(std::memory_order_relaxed);
	if (total == 0) {
		return 0;
	}
	auto generated = MinValue<idx_t>(state.generated_values.load(std::memory_order_relaxed), total);
	return 100.0 * static_cast<double>(generated) / static_cast<double>(total);
}

struct RangeFunctionLocalState : public LocalTableFunctionState {
	explicit RangeFunctionLocalState(optional_ptr<GlobalTableFunctionState> global_state)
	    : global_state(global_state ? &global_state->Cast<RangeFunctionGlobalState>() : nullptr) {
	}

	optional_ptr<RangeFunctionGlobalState> global_state;

	bool initialized_row = false;
	idx_t current_input_row = 0;
	idx_t current_idx = 0;

	hugeint_t start;
	hugeint_t end;
	hugeint_t increment;
	//! The number of values generated for the current input row
	idx_t row_count = 0;

	bool empty_range = false;
};

static unique_ptr<LocalTableFunctionState> RangeFunctionLocalInit(ExecutionContext &context,
                                                                  TableFunctionInitInput &input,
                                                                  GlobalTableFunctionState *global_state) {
	return make_uniq<RangeFunctionLocalState>(global_state);
}

template <bool GENERATE_SERIES>
static void GenerateRangeParameters(DataChunk &input, idx_t row_id, RangeFunctionLocalState &result) {
	input.Flatten();
	for (idx_t c = 0; c < input.ColumnCount(); c++) {
		if (FlatVector::IsNull(input.data[c], row_id)) {
			result.start = GENERATE_SERIES ? 1 : 0;
			result.end = 0;
			result.increment = 1;
			result.row_count = 0;
			return;
		}
	}
	int64_t values[3];
	for (idx_t c = 0; c < input.ColumnCount(); c++) {
		if (c >= 3) {
			throw InternalException("Unsupported parameter count for range function");
		}
		values[c] = FlatVector::GetValue<int64_t>(input.data[c], row_id);
	}
	GetParameters(values, input.ColumnCount(), result.start, result.end, result.increment);
	if (result.increment == 0) {
		throw BinderException("interval cannot be 0!");
	}
	result.empty_range = false;
	if (result.start > result.end && result.increment > 0) {
		result.empty_range = true;
	}
	if (result.start < result.end && result.increment < 0) {
		result.empty_range = true;
	}
	if (GENERATE_SERIES) {
		// generate_series has inclusive bounds on the RHS
		if (result.increment < 0) {
			result.end = result.end - 1;
		} else {
			result.end = result.end + 1;
		}
	}
	if (result.empty_range) {
		result.row_count = 0;
	} else {
		auto difference = result.end - result.start;
		auto row_count = difference / result.increment;
		if (difference % result.increment != 0) {
			row_count += 1;
		}
		if (!Hugeint::TryCast<idx_t>(row_count, result.row_count)) {
			result.row_count = NumericLimits<idx_t>::Maximum();
		}
	}
}

template <bool GENERATE_SERIES>
static OperatorResultType RangeFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                        DataChunk &output) {
	auto &state = data_p.local_state->Cast<RangeFunctionLocalState>();
	while (true) {
		if (!state.initialized_row) {
			// initialize for the current input row
			if (state.current_input_row >= input.size()) {
				// ran out of rows
				state.current_input_row = 0;
				state.initialized_row = false;
				return OperatorResultType::NEED_MORE_INPUT;
			}
			GenerateRangeParameters<GENERATE_SERIES>(input, state.current_input_row, state);
			state.initialized_row = true;
			state.current_idx = 0;
			if (state.global_state) {
				state.global_state->total_values.fetch_add(state.row_count, std::memory_order_relaxed);
			}
		}
		if (state.empty_range) {
			// empty range
			state.current_input_row++;
			state.initialized_row = false;
			return OperatorResultType::HAVE_MORE_OUTPUT;
		}
		auto increment = state.increment;
		auto end = state.end;
		hugeint_t current_value = state.start + increment * UnsafeNumericCast<int64_t>(state.current_idx);
		int64_t current_value_i64;
		if (!Hugeint::TryCast<int64_t>(current_value, current_value_i64)) {
			// move to next row
			state.current_input_row++;
			state.initialized_row = false;
			continue;
		}
		int64_t offset = increment < 0 ? 1 : -1;
		idx_t remaining = MinValue<idx_t>(
		    Hugeint::Cast<idx_t>((end - current_value + (increment + offset)) / increment), STANDARD_VECTOR_SIZE);
		// set the result vector as a sequence vector
		output.data[0].Sequence(current_value_i64, Hugeint::Cast<int64_t>(increment), remaining);
		// increment the index pointer by the remaining count
		state.current_idx += remaining;
		if (remaining == 0) {
			// move to next row
			state.current_input_row++;
			state.initialized_row = false;
			continue;
		}
		if (state.global_state) {
			state.global_state->generated_values.fetch_add(remaining, std::memory_order_relaxed);
		}
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
}

unique_ptr<NodeStatistics> RangeCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	if (!bind_data_p) {
		return nullptr;
	}
	auto &bind_data = bind_data_p->Cast<RangeFunctionBindData>();
	return make_uniq<NodeStatistics>(bind_data.cardinality, bind_data.cardinality);
}

//===--------------------------------------------------------------------===//
// Range (timestamp)
//===--------------------------------------------------------------------===//
struct RangeDateTimeBindData : public TableFunctionData {
	explicit RangeDateTimeBindData(const vector<Value> &inputs) : cardinality(0) {
		timestamp_t bounds[2];
		interval_t step;
		for (idx_t i = 0; i < inputs.size(); i++) {
			if (inputs[i].IsNull()) {
				return;
			}
			if (i >= 2) {
				step = inputs[i].GetValue<interval_t>();
			} else {
				bounds[i] = inputs[i].GetValue<timestamp_t>();
			}
		}
		// Estimate cardinality using micros.
		int64_t increment = 0;
		if (!Interval::TryGetMicro(step, increment) || !increment) {
			return;
		}
		int64_t delta = 0;
		if (!TrySubtractOperator::Operation(bounds[1].value, bounds[0].value, delta)) {
			return;
		}

		// a step pointing away from the end produces an empty range
		cardinality = delta / increment < 0 ? 0 : idx_t(delta / increment);
	}

	idx_t cardinality;
};

template <bool GENERATE_SERIES>
static unique_ptr<FunctionData> RangeDateTimeBind(ClientContext &context, TableFunctionBindInput &input,
                                                  vector<LogicalType> &return_types, vector<Identifier> &names) {
	return_types.push_back(LogicalType::TIMESTAMP);
	if (GENERATE_SERIES) {
		names.emplace_back("generate_series");
	} else {
		names.emplace_back("range");
	}
	if (input.inputs.size() != 3) {
		return nullptr;
	}
	return make_uniq<RangeDateTimeBindData>(input.inputs);
}

unique_ptr<NodeStatistics> RangeDateTimeCardinality(ClientContext &context, const FunctionData *bind_data_p) {
	if (!bind_data_p) {
		return nullptr;
	}
	auto &bind_data = bind_data_p->Cast<RangeDateTimeBindData>();
	return make_uniq<NodeStatistics>(bind_data.cardinality, bind_data.cardinality);
}

//! Progress of a timestamp range used as a source (i.e. with a single input row)
struct RangeDateTimeGlobalState : public GlobalTableFunctionState {
	//! The bounds of the range, and the timestamp that is generated next
	atomic<int64_t> start {0};
	atomic<int64_t> end {0};
	atomic<int64_t> current {0};

	void Initialize(timestamp_t start_p, timestamp_t end_p) {
		start.store(start_p.value, std::memory_order_relaxed);
		end.store(end_p.value, std::memory_order_relaxed);
		current.store(start_p.value, std::memory_order_relaxed);
	}

	//! Input rows are independent, so an in-out call can use all threads
	idx_t MaxThreads() const override {
		return GlobalTableFunctionState::MAX_THREADS;
	}
};

static unique_ptr<GlobalTableFunctionState> RangeDateTimeGlobalInit(ClientContext &context,
                                                                    TableFunctionInitInput &input) {
	return make_uniq<RangeDateTimeGlobalState>();
}

static double RangeDateTimeProgress(ClientContext &context, const FunctionData *bind_data,
                                    const GlobalTableFunctionState *global_state) {
	auto &state = global_state->Cast<RangeDateTimeGlobalState>();
	auto start = static_cast<double>(state.start.load(std::memory_order_relaxed));
	auto range = static_cast<double>(state.end.load(std::memory_order_relaxed)) - start;
	if (range == 0) {
		return 0;
	}
	auto fraction = (static_cast<double>(state.current.load(std::memory_order_relaxed)) - start) / range;
	return 100.0 * MaxValue<double>(MinValue<double>(fraction, 1.0), 0.0);
}

struct RangeDateTimeLocalState : public LocalTableFunctionState {
	explicit RangeDateTimeLocalState(optional_ptr<GlobalTableFunctionState> global_state)
	    : global_state(global_state ? &global_state->Cast<RangeDateTimeGlobalState>() : nullptr) {
	}

	optional_ptr<RangeDateTimeGlobalState> global_state;

	bool initialized_row = false;
	idx_t current_input_row = 0;
	timestamp_t current_state;

	timestamp_t start;
	timestamp_t end;
	interval_t increment;
	bool inclusive_bound;
	bool greater_than_check;

	bool empty_range = false;
	//! Whether the next step went past the timestamp range, which ends the series also when the end lies below
	//! -infinity, as INT64_MIN does
	bool past_range = false;

	bool Finished(timestamp_t current_value) const {
		if (greater_than_check) {
			if (inclusive_bound) {
				return current_value > end;
			} else {
				return current_value >= end;
			}
		} else {
			if (inclusive_bound) {
				return current_value < end;
			} else {
				return current_value <= end;
			}
		}
	}
};

template <bool GENERATE_SERIES>
static void GenerateRangeDateTimeParameters(DataChunk &input, idx_t row_id, RangeDateTimeLocalState &result) {
	input.Flatten();

	// The local state is reused for every input row, so empty_range must be reset
	// here. Without this it is only ever set to true and never back to false, so
	// once ONE row produces an empty range every subsequent row is treated as
	// empty and silently emits nothing. The integer overload
	// (GenerateRangeParameters) already resets it, which is why only the
	// timestamp overload was affected.
	result.empty_range = false;

	for (idx_t c = 0; c < input.ColumnCount(); c++) {
		if (FlatVector::IsNull(input.data[c], row_id)) {
			result.start = timestamp_t(0);
			result.end = timestamp_t(0);
			result.increment = interval_t();
			result.greater_than_check = true;
			result.inclusive_bound = false;
			return;
		}
	}

	result.start = FlatVector::GetValue<timestamp_t>(input.data[0], row_id);
	result.end = FlatVector::GetValue<timestamp_t>(input.data[1], row_id);
	result.increment = FlatVector::GetValue<interval_t>(input.data[2], row_id);

	// Infinities either cause errors or infinite loops, so just ban them
	if (!result.start.IsFinite() || !result.end.IsFinite()) {
		throw BinderException("RANGE with infinite bounds is not supported");
	}

	if (result.increment.months == 0 && result.increment.days == 0 && result.increment.micros == 0) {
		throw BinderException("interval cannot be 0!");
	}
	// all elements should point in the same direction
	if (result.increment.months > 0 || result.increment.days > 0 || result.increment.micros > 0) {
		if (result.increment.months < 0 || result.increment.days < 0 || result.increment.micros < 0) {
			throw BinderException("RANGE with composite interval that has mixed signs is not supported");
		}
		result.greater_than_check = true;
		if (result.start > result.end) {
			result.empty_range = true;
		}
	} else {
		result.greater_than_check = false;
		if (result.start < result.end) {
			result.empty_range = true;
		}
	}
	result.inclusive_bound = GENERATE_SERIES;
}

static unique_ptr<LocalTableFunctionState> RangeDateTimeLocalInit(ExecutionContext &context,
                                                                  TableFunctionInitInput &input,
                                                                  GlobalTableFunctionState *global_state) {
	return make_uniq<RangeDateTimeLocalState>(global_state);
}

template <bool GENERATE_SERIES>
static OperatorResultType RangeDateTimeFunction(ExecutionContext &context, TableFunctionInput &data_p, DataChunk &input,
                                                DataChunk &output) {
	auto &state = data_p.local_state->Cast<RangeDateTimeLocalState>();
	while (true) {
		if (!state.initialized_row) {
			// initialize for the current input row
			if (state.current_input_row >= input.size()) {
				// ran out of rows
				state.current_input_row = 0;
				state.initialized_row = false;
				return OperatorResultType::NEED_MORE_INPUT;
			}
			GenerateRangeDateTimeParameters<GENERATE_SERIES>(input, state.current_input_row, state);
			state.initialized_row = true;
			state.current_state = state.start;
			state.past_range = false;
			if (state.global_state) {
				state.global_state->Initialize(state.start, state.end);
			}
		}
		if (state.empty_range) {
			// empty range
			state.current_input_row++;
			state.initialized_row = false;
			return OperatorResultType::HAVE_MORE_OUTPUT;
		}
		idx_t size = 0;
		auto result_data = FlatVector::ScatterWriter<timestamp_t>(output.data[0]);
		while (true) {
			if (state.past_range || state.Finished(state.current_state)) {
				break;
			}
			if (size >= STANDARD_VECTOR_SIZE) {
				break;
			}
			result_data[size] = state.current_state;
			size++;
			if (!TryAddOperator::Operation<timestamp_t, interval_t, timestamp_t>(state.current_state, state.increment,
			                                                                     state.current_state)) {
				// the next value is outside of the timestamp range: the series ends here
				state.current_state = state.greater_than_check ? timestamp_t::infinity() : timestamp_t::ninfinity();
				state.past_range = true;
			}
		}
		if (size == 0) {
			// move to next row
			state.current_input_row++;
			state.initialized_row = false;
			continue;
		}
		output.SetChildCardinality(size);
		if (state.global_state) {
			state.global_state->current.store(state.current_state.value, std::memory_order_relaxed);
		}
		return OperatorResultType::HAVE_MORE_OUTPUT;
	}
}

static bool RangeIsRepeatable(optional_ptr<const FunctionData>) {
	return true;
}

void RangeTableFunction::RegisterFunction(BuiltinFunctions &set) {
	TableFunctionSet range("range");

	const auto stop_only = FunctionSignature().AddPositionalOnly("stop", LogicalType::BIGINT);
	const auto start_stop = FunctionSignature()
	                            .AddPositionalOnly("start", LogicalType::BIGINT)
	                            .AddPositionalOnly("stop", LogicalType::BIGINT);
	auto start_stop_step = start_stop;
	start_stop_step.AddPositionalOnly("step", LogicalType::BIGINT);
	const auto timestamps = FunctionSignature()
	                            .AddPositionalOnly("start", LogicalType::TIMESTAMP)
	                            .AddPositionalOnly("stop", LogicalType::TIMESTAMP)
	                            .AddPositionalOnly("step", LogicalType::INTERVAL);

	TableFunction range_function(stop_only, nullptr, RangeFunctionBind<false>, RangeFunctionGlobalInit,
	                             RangeFunctionLocalInit);
	range_function.table_scan_progress = RangeFunctionProgress;
	range_function.in_out_function = RangeFunction<false>;
	range_function.cardinality = RangeCardinality;
	range_function.is_repeatable = RangeIsRepeatable;
	range_function.parallelism = TableFunctionParallelism::FORCE_SINGLE_THREADED;
	range_function.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;

	// single argument range: (end) - implicit start = 0 and increment = 1
	range.AddFunction(range_function);
	// two arguments range: (start, end) - implicit increment = 1
	range_function.GetSignature() = start_stop;
	range.AddFunction(range_function);
	// three arguments range: (start, end, increment)
	range_function.GetSignature() = start_stop_step;
	range.AddFunction(range_function);
	TableFunction range_in_out(timestamps, nullptr, RangeDateTimeBind<false>, RangeDateTimeGlobalInit,
	                           RangeDateTimeLocalInit);
	range_in_out.table_scan_progress = RangeDateTimeProgress;
	range_in_out.in_out_function = RangeDateTimeFunction<false>;
	range_in_out.cardinality = RangeDateTimeCardinality;
	range_in_out.is_repeatable = RangeIsRepeatable;
	range_in_out.parallelism = TableFunctionParallelism::FORCE_SINGLE_THREADED;
	range_in_out.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
	range.AddFunction(range_in_out);
	set.AddFunction(range);
	// generate_series: similar to range, but inclusive instead of exclusive bounds on the RHS
	TableFunctionSet generate_series("generate_series");
	range_function.bind = RangeFunctionBind<true>;
	range_function.in_out_function = RangeFunction<true>;
	range_function.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
	range_function.GetSignature() = stop_only;
	generate_series.AddFunction(range_function);
	range_function.GetSignature() = start_stop;
	generate_series.AddFunction(range_function);
	range_function.GetSignature() = start_stop_step;
	generate_series.AddFunction(range_function);
	TableFunction generate_series_in_out(timestamps, nullptr, RangeDateTimeBind<true>, RangeDateTimeGlobalInit,
	                                     RangeDateTimeLocalInit);
	generate_series_in_out.table_scan_progress = RangeDateTimeProgress;
	generate_series_in_out.in_out_function = RangeDateTimeFunction<true>;
	generate_series_in_out.is_repeatable = RangeIsRepeatable;
	generate_series_in_out.parallelism = TableFunctionParallelism::FORCE_SINGLE_THREADED;
	generate_series_in_out.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
	generate_series.AddFunction(generate_series_in_out);
	set.AddFunction(generate_series);
}

void BuiltinFunctions::RegisterTableFunctions() {
	CheckpointFunction::RegisterFunction(*this);
	GlobTableFunction::RegisterFunction(*this);
	RangeTableFunction::RegisterFunction(*this);
	RepeatTableFunction::RegisterFunction(*this);
	SummaryTableFunction::RegisterFunction(*this);
	UnnestTableFunction::RegisterFunction(*this);
	RepeatRowTableFunction::RegisterFunction(*this);
	CSVSnifferFunction::RegisterFunction(*this);
	ReadBlobFunction::RegisterFunction(*this);
	ReadTextFunction::RegisterFunction(*this);
	QueryTableFunction::RegisterFunction(*this);
}

} // namespace duckdb
