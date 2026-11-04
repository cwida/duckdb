#include "duckdb/common/vector/flat_vector.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {

namespace {

struct NumericRangeInfo {
	using TYPE = int64_t;
	using INCREMENT_TYPE = int64_t;

	static int64_t DefaultStart() {
		return 0;
	}
	static int64_t DefaultIncrement() {
		return 1;
	}

	static uint64_t ListLength(int64_t start_value, int64_t end_value, int64_t increment_value, bool inclusive_bound) {
		if (increment_value == 0) {
			return 0;
		}
		if (start_value > end_value && increment_value > 0) {
			return 0;
		}
		if (start_value < end_value && increment_value < 0) {
			return 0;
		}
		hugeint_t total_diff = AbsValue(hugeint_t(end_value) - hugeint_t(start_value));
		hugeint_t increment = AbsValue(hugeint_t(increment_value));
		hugeint_t total_values = total_diff / increment;
		if (total_diff % increment == 0) {
			if (inclusive_bound) {
				total_values += 1;
			}
		} else {
			total_values += 1;
		}
		if (total_values > NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("Lists larger than 2^32 elements are not supported");
		}
		return Hugeint::Cast<uint64_t>(total_values);
	}

	static void Increment(int64_t &input, int64_t increment) {
		input += increment;
	}
};
struct TimestampRangeInfo {
	using TYPE = timestamp_t;
	using INCREMENT_TYPE = interval_t;

	static timestamp_t DefaultStart() {
		throw InternalException("Default start not implemented for timestamp range");
	}
	static interval_t DefaultIncrement() {
		throw InternalException("Default increment not implemented for timestamp range");
	}
	static uint64_t ListLength(timestamp_t start_value, timestamp_t end_value, interval_t increment_value,
	                           bool inclusive_bound) {
		bool is_positive = increment_value.months > 0 || increment_value.days > 0 || increment_value.micros > 0;
		bool is_negative = increment_value.months < 0 || increment_value.days < 0 || increment_value.micros < 0;
		if (!is_negative && !is_positive) {
			// interval is 0: no result
			return 0;
		}
		// We don't allow infinite bounds because they generate errors or infinite loops
		if (!start_value.IsFinite() || !end_value.IsFinite()) {
			throw InvalidInputException("Interval infinite bounds not supported");
		}

		if (is_negative && is_positive) {
			// we don't allow a mix of
			throw InvalidInputException("Interval with mix of negative/positive entries not supported");
		}
		if (start_value > end_value && is_positive) {
			return 0;
		}
		if (start_value < end_value && is_negative) {
			return 0;
		}
		uint64_t total_values = 0;
		if (is_negative) {
			// negative interval, start_value is going down
			while (inclusive_bound ? start_value >= end_value : start_value > end_value) {
				start_value = Interval::Add(start_value, increment_value);
				total_values++;
				if (total_values > NumericLimits<uint32_t>::Maximum()) {
					throw InvalidInputException("Lists larger than 2^32 elements are not supported");
				}
			}
		} else {
			// positive interval, start_value is going up
			while (inclusive_bound ? start_value <= end_value : start_value < end_value) {
				start_value = Interval::Add(start_value, increment_value);
				total_values++;
				if (total_values > NumericLimits<uint32_t>::Maximum()) {
					throw InvalidInputException("Lists larger than 2^32 elements are not supported");
				}
			}
		}
		return total_values;
	}

	static void Increment(timestamp_t &input, interval_t increment) {
		input = Interval::Add(input, increment);
	}
};

template <class OP, bool INCLUSIVE_BOUND>
class RangeInfoStruct {
public:
	explicit RangeInfoStruct(DataChunk &args_p) : args(args_p) {
		switch (args.ColumnCount()) {
		case 1:
			args.data[0].ToUnifiedFormat(vdata[0]);
			break;
		case 2:
			args.data[0].ToUnifiedFormat(vdata[0]);
			args.data[1].ToUnifiedFormat(vdata[1]);
			break;
		case 3:
			args.data[0].ToUnifiedFormat(vdata[0]);
			args.data[1].ToUnifiedFormat(vdata[1]);
			args.data[2].ToUnifiedFormat(vdata[2]);
			break;
		default:
			throw InternalException("Unsupported number of parameters for range");
		}
	}

	bool RowIsValid(idx_t row_idx) {
		for (idx_t i = 0; i < args.ColumnCount(); i++) {
			auto idx = vdata[i].sel->get_index(row_idx);
			if (!vdata[i].validity.RowIsValid(idx)) {
				return false;
			}
		}
		return true;
	}

	typename OP::TYPE StartListValue(idx_t row_idx) {
		if (args.ColumnCount() == 1) {
			return OP::DefaultStart();
		} else {
			auto data = (typename OP::TYPE *)vdata[0].data;
			auto idx = vdata[0].sel->get_index(row_idx);
			return data[idx];
		}
	}

	typename OP::TYPE EndListValue(idx_t row_idx) {
		idx_t vdata_idx = args.ColumnCount() == 1 ? 0 : 1;
		auto data = (typename OP::TYPE *)vdata[vdata_idx].data;
		auto idx = vdata[vdata_idx].sel->get_index(row_idx);
		return data[idx];
	}

	typename OP::INCREMENT_TYPE ListIncrementValue(idx_t row_idx) {
		if (args.ColumnCount() < 3) {
			return OP::DefaultIncrement();
		} else {
			auto data = (typename OP::INCREMENT_TYPE *)vdata[2].data;
			auto idx = vdata[2].sel->get_index(row_idx);
			return data[idx];
		}
	}

	void GetListValues(idx_t row_idx, typename OP::TYPE &start_value, typename OP::TYPE &end_value,
	                   typename OP::INCREMENT_TYPE &increment_value) {
		start_value = StartListValue(row_idx);
		end_value = EndListValue(row_idx);
		increment_value = ListIncrementValue(row_idx);
	}

	uint64_t ListLength(idx_t row_idx) {
		typename OP::TYPE start_value;
		typename OP::TYPE end_value;
		typename OP::INCREMENT_TYPE increment_value;
		GetListValues(row_idx, start_value, end_value, increment_value);
		return OP::ListLength(start_value, end_value, increment_value, INCLUSIVE_BOUND);
	}

private:
	DataChunk &args;
	UnifiedVectorFormat vdata[3];
};

template <class OP, bool INCLUSIVE_BOUND>
void ListRangeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	RangeInfoStruct<OP, INCLUSIVE_BOUND> info(args);
	auto args_size = args.size();
	auto list_writer = FlatVector::Writer<VectorListType<typename OP::TYPE>>(result, args_size);
	for (idx_t i = 0; i < args_size; i++) {
		if (!info.RowIsValid(i)) {
			list_writer.WriteNull();
			continue;
		}
		typename OP::TYPE start_value = info.StartListValue(i);
		typename OP::INCREMENT_TYPE increment = info.ListIncrementValue(i);
		const auto length = info.ListLength(i);

		typename OP::TYPE range_value = start_value;
		bool seen_value = false;
		for (auto &child_writer : list_writer.WriteList(length)) {
			if (seen_value) {
				OP::Increment(range_value, increment);
			}
			child_writer.WriteValue(range_value);
			seen_value = true;
		}
	}
}

} // namespace

template <bool GENERATE_SERIES>
static void AddRangeFunctions(ScalarFunctionSet &set) {
	ScalarFunction stop_only({}, LogicalType::LIST(LogicalType::BIGINT),
	                         ListRangeFunction<NumericRangeInfo, GENERATE_SERIES>);
	stop_only.GetSignature().AddParameter("stop", LogicalType::BIGINT);
	set.AddFunction(stop_only);

	ScalarFunction start_stop({}, LogicalType::LIST(LogicalType::BIGINT),
	                          ListRangeFunction<NumericRangeInfo, GENERATE_SERIES>);
	start_stop.GetSignature().AddParameter("start", LogicalType::BIGINT).AddParameter("stop", LogicalType::BIGINT);
	set.AddFunction(start_stop);

	ScalarFunction start_stop_step({}, LogicalType::LIST(LogicalType::BIGINT),
	                               ListRangeFunction<NumericRangeInfo, GENERATE_SERIES>);
	start_stop_step.GetSignature()
	    .AddParameter("start", LogicalType::BIGINT)
	    .AddParameter("stop", LogicalType::BIGINT)
	    .AddParameter("step", LogicalType::BIGINT);
	set.AddFunction(start_stop_step);

	ScalarFunction timestamp_range({}, LogicalType::LIST(LogicalType::TIMESTAMP),
	                               ListRangeFunction<TimestampRangeInfo, GENERATE_SERIES>);
	timestamp_range.GetSignature()
	    .AddParameter("start", LogicalType::TIMESTAMP)
	    .AddParameter("stop", LogicalType::TIMESTAMP)
	    .AddParameter("step", LogicalType::INTERVAL);
	set.AddFunction(timestamp_range);

	set.SetFallible();
}

ScalarFunctionSet ListRangeFun::GetFunctions() {
	// the arguments and return types are actually set in the binder function
	ScalarFunctionSet range_set;
	AddRangeFunctions<false>(range_set);
	return range_set;
}

ScalarFunctionSet GenerateSeriesFun::GetFunctions() {
	ScalarFunctionSet generate_series;
	AddRangeFunctions<true>(generate_series);
	return generate_series;
}

} // namespace duckdb
