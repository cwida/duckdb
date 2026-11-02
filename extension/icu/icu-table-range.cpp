#include "duckdb/common/atomic.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/subtract.hpp"
#include "duckdb/common/types/interval.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/table_function.hpp"
#include "include/icu-datefunc.hpp"
#include "tz_calendar.hpp"

namespace duckdb {

struct ICUTableRange {
	using CalendarPtr = unique_ptr<Calendar>;

	struct ICURangeBindData : public TableFunctionData {
		ICURangeBindData(const ICURangeBindData &other)
		    : TableFunctionData(other), tz_setting(other.tz_setting), cal_setting(other.cal_setting),
		      calendar(other.calendar->Copy()), cardinality(other.cardinality) {
		}

		explicit ICURangeBindData(ClientContext &context, const vector<Value> &inputs) : cal_setting("gregorian") {
			context.TryGetCurrentSetting("TimeZone", tz_setting);
			context.TryGetCurrentSetting("Calendar", cal_setting);
			auto tz = TimeZone::TryCreate(tz_setting);
			calendar = Calendar::TryCreate(cal_setting, tz ? std::move(tz) : TimeZone::TryCreate("UTC"));
			if (!calendar) {
				throw InternalException("Unable to create calendar.");
			}

			timestamp_tz_t bounds[2];
			interval_t step;
			for (idx_t i = 0; i < inputs.size(); i++) {
				if (inputs[i].IsNull()) {
					return;
				}
				if (i >= 2) {
					step = inputs[i].GetValue<interval_t>();
				} else {
					bounds[i] = inputs[i].GetValue<timestamp_tz_t>();
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

			cardinality = idx_t(delta / increment);
		}

		string tz_setting;
		string cal_setting;
		CalendarPtr calendar;
		idx_t cardinality;
	};

	//! Progress of a range used as a source (i.e. with a single input row)
	struct ICURangeGlobalState : public GlobalTableFunctionState {
		//! The bounds of the range, and the timestamp that is generated next
		atomic<int64_t> start {0};
		atomic<int64_t> end {0};
		atomic<int64_t> current {0};

		void Initialize(timestamp_tz_t start_p, timestamp_tz_t end_p) {
			start.store(start_p.value, std::memory_order_relaxed);
			end.store(end_p.value, std::memory_order_relaxed);
			current.store(start_p.value, std::memory_order_relaxed);
		}

		//! Input rows are independent, so an in-out call can use all threads
		idx_t MaxThreads() const override {
			return GlobalTableFunctionState::MAX_THREADS;
		}
	};

	struct ICURangeLocalState : public LocalTableFunctionState {
		explicit ICURangeLocalState(optional_ptr<GlobalTableFunctionState> global_state)
		    : global_state(global_state ? &global_state->Cast<ICURangeGlobalState>() : nullptr) {
		}

		optional_ptr<ICURangeGlobalState> global_state;

		bool initialized_row = false;
		idx_t current_input_row = 0;
		timestamp_tz_t current_state;

		timestamp_tz_t start;
		timestamp_tz_t end;
		interval_t increment;
		bool inclusive_bound;
		bool greater_than_check;

		bool empty_range = false;

		bool Finished(timestamp_tz_t current_value) const {
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
	static void GenerateRangeDateTimeParameters(DataChunk &input, idx_t row_id, ICURangeLocalState &result) {
		input.Flatten();
		// the local state is reused for every input row
		result.empty_range = false;
		for (idx_t c = 0; c < input.ColumnCount(); c++) {
			if (FlatVector::IsNull(input.data[c], row_id)) {
				result.start = timestamp_tz_t::epoch();
				result.end = timestamp_tz_t::epoch();
				result.increment = interval_t();
				result.greater_than_check = true;
				result.inclusive_bound = false;
				return;
			}
		}

		result.start = FlatVector::GetValue<timestamp_tz_t>(input.data[0], row_id);
		result.end = FlatVector::GetValue<timestamp_tz_t>(input.data[1], row_id);
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

	template <bool GENERATE_SERIES>
	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
		auto result = make_uniq<ICURangeBindData>(context, input.inputs);

		return_types.push_back(LogicalType::TIMESTAMP_TZ);
		if (GENERATE_SERIES) {
			names.emplace_back("generate_series");
		} else {
			names.emplace_back("range");
		}
		return std::move(result);
	}

	static unique_ptr<LocalTableFunctionState> RangeDateTimeLocalInit(ExecutionContext &context,
	                                                                  TableFunctionInitInput &input,
	                                                                  GlobalTableFunctionState *global_state) {
		return make_uniq<ICURangeLocalState>(global_state);
	}

	static unique_ptr<GlobalTableFunctionState> RangeDateTimeGlobalInit(ClientContext &context,
	                                                                    TableFunctionInitInput &input) {
		return make_uniq<ICURangeGlobalState>();
	}

	static double Progress(ClientContext &context, const FunctionData *bind_data,
	                       const GlobalTableFunctionState *global_state) {
		auto &state = global_state->Cast<ICURangeGlobalState>();
		auto start = static_cast<double>(state.start.load(std::memory_order_relaxed));
		auto range = static_cast<double>(state.end.load(std::memory_order_relaxed)) - start;
		if (range == 0) {
			return 0;
		}
		auto fraction = (static_cast<double>(state.current.load(std::memory_order_relaxed)) - start) / range;
		return 100.0 * MaxValue<double>(MinValue<double>(fraction, 1.0), 0.0);
	}

	static unique_ptr<NodeStatistics> Cardinality(ClientContext &context, const FunctionData *bind_data_p) {
		if (!bind_data_p) {
			return nullptr;
		}
		auto &bind_data = bind_data_p->Cast<ICURangeBindData>();
		return make_uniq<NodeStatistics>(bind_data.cardinality, bind_data.cardinality);
	}

	template <bool GENERATE_SERIES>
	static OperatorResultType ICUTableRangeFunction(ExecutionContext &context, TableFunctionInput &data_p,
	                                                DataChunk &input, DataChunk &output) {
		auto &bind_data = data_p.bind_data->Cast<ICURangeBindData>();
		auto &state = data_p.local_state->Cast<ICURangeLocalState>();
		TZCalendar calendar(*bind_data.calendar, bind_data.cal_setting);
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
			auto data = FlatVector::ScatterWriter<timestamp_tz_t>(output.data[0]);
			while (true) {
				if (state.Finished(state.current_state)) {
					break;
				}
				data[size] = state.current_state;
				size++;
				state.current_state = ICUDateFunc::Add(calendar, state.current_state, state.increment);
				if (size >= STANDARD_VECTOR_SIZE) {
					break;
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

	static void AddICUTableRangeFunction(ExtensionLoader &loader) {
		const auto timestamps = FunctionSignature()
		                            .AddPositionalOnly("start", LogicalType::TIMESTAMP_TZ)
		                            .AddPositionalOnly("stop", LogicalType::TIMESTAMP_TZ)
		                            .AddPositionalOnly("step", LogicalType::INTERVAL);
		TableFunctionSet range("range");
		TableFunction range_function(timestamps, nullptr, Bind<false>, RangeDateTimeGlobalInit, RangeDateTimeLocalInit);
		range_function.in_out_function = ICUTableRangeFunction<false>;
		range_function.table_scan_progress = Progress;
		range_function.cardinality = Cardinality;
		range_function.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
		range.AddFunction(range_function);

		loader.RegisterFunction(range);

		// generate_series: similar to range, but inclusive instead of exclusive bounds on the RHS
		TableFunctionSet generate_series("generate_series");
		TableFunction generate_series_function(timestamps, nullptr, Bind<true>, RangeDateTimeGlobalInit,
		                                       RangeDateTimeLocalInit);
		generate_series_function.in_out_function = ICUTableRangeFunction<true>;
		generate_series_function.table_scan_progress = Progress;
		generate_series_function.cardinality = Cardinality;
		generate_series_function.return_type = TableFunctionReturnType::SET_RETURNING_FUNCTION;
		generate_series.AddFunction(generate_series_function);

		loader.RegisterFunction(generate_series);
	}
};

void RegisterICUTableRangeFunctions(ExtensionLoader &loader) {
	ICUTableRange::AddICUTableRangeFunction(loader);
}

} // namespace duckdb
