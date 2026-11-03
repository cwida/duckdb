#include "core_functions/scalar/date_functions.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/types/timestamp.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/variadic_executor.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"

#include <cmath>

namespace duckdb {

namespace {

void MakeDateFromEpoch(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);
	result.Reinterpret(input.data[0]);
}

struct MakeDateOperator {
	template <typename YYYY, typename MM, typename DD, typename RESULT_TYPE>
	static RESULT_TYPE Operation(YYYY yyyy, MM mm, DD dd) {
		return Date::FromDate(Cast::Operation<YYYY, int32_t>(yyyy), Cast::Operation<MM, int32_t>(mm),
		                      Cast::Operation<DD, int32_t>(dd));
	}
};

template <typename T>
void ExecuteMakeDate(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 3);
	const auto &yyyy = input.data[0];
	const auto &mm = input.data[1];
	const auto &dd = input.data[2];

	TernaryExecutor::Execute<T, T, T, date_t>(yyyy, mm, dd, result, MakeDateOperator::Operation<T, T, T, date_t>);
}

template <typename T>
date_t FromDateCast(T year, T month, T day) {
	date_t result;
	if (!Date::TryFromDate(Cast::Operation<T, int32_t>(year), Cast::Operation<T, int32_t>(month),
	                       Cast::Operation<T, int32_t>(day), result)) {
		throw ConversionException("Date out of range: %d-%d-%d", year, month, day);
	}
	return result;
}

template <typename T>
void ExecuteStructMakeDate(DataChunk &input, ExpressionState &state, Vector &result) {
	// this should be guaranteed by the binder
	D_ASSERT(input.ColumnCount() == 1);
	const auto &vec = input.data[0];
	const auto count = input.size();

	auto iter = vec.Values<VectorStructType<T, T, T>>();
	auto writer = FlatVector::Writer<date_t>(result, count);
	for (const auto entry : iter) {
		const auto y = entry.template GetChildValue<0>();
		const auto m = entry.template GetChildValue<1>();
		const auto d = entry.template GetChildValue<2>();
		if (!entry.IsValid() || !y.IsValid() || !m.IsValid() || !d.IsValid()) {
			writer.WriteNull();
			continue;
		}
		writer.WriteValue(FromDateCast<T>(y.GetValueUnsafe(), m.GetValueUnsafe(), d.GetValueUnsafe()));
	}
}

struct MakeTimeOperator {
	template <typename HH, typename MM, typename SS, typename RESULT_TYPE>
	static RESULT_TYPE Operation(HH hh, MM mm, SS ss) {
		auto hh_32 = Cast::Operation<HH, int32_t>(hh);
		auto mm_32 = Cast::Operation<MM, int32_t>(mm);
		if (!std::isfinite(static_cast<double>(ss))) {
			throw ConversionException("Time out of range: seconds must be a finite number");
		}
		// Truncate the seconds: safe casting of DOUBLE => INT32 rounds.
		auto ss_32 = Cast::Operation<SS, int32_t>(std::floor(ss));
		auto micros = LossyNumericCast<int32_t>(std::round((ss - ss_32) * Interval::MICROS_PER_SEC));

		if (!Time::IsValidTime(hh_32, mm_32, ss_32, micros)) {
			throw ConversionException("Time out of range: %d:%d:%d.%06d", hh_32, mm_32, ss_32, micros);
		}
		return Time::FromTime(hh_32, mm_32, ss_32, micros);
	}
};

template <typename T>
void ExecuteMakeTime(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 3);
	const auto &yyyy = input.data[0];
	const auto &mm = input.data[1];
	const auto &dd = input.data[2];

	TernaryExecutor::Execute<T, T, double, dtime_t>(yyyy, mm, dd, result,
	                                                MakeTimeOperator::Operation<T, T, double, dtime_t>);
}

struct MakeTimestampOperator {
	template <typename YYYY, typename MM, typename DD, typename HR, typename MN, typename SS, typename RESULT_TYPE>
	static RESULT_TYPE Operation(YYYY yyyy, MM mm, DD dd, HR hr, MN mn, SS ss) {
		const auto d = MakeDateOperator::Operation<YYYY, MM, DD, date_t>(yyyy, mm, dd);
		const auto t = MakeTimeOperator::Operation<HR, MN, SS, dtime_t>(hr, mn, ss);
		return Timestamp::FromDatetime(d, t);
	}

	template <typename T, typename RESULT_TYPE>
	static RESULT_TYPE Operation(T value) {
		const auto result = RESULT_TYPE(value);
		if (!result.IsFinite()) {
			throw ConversionException("Timestamp microseconds out of range: %ld", value);
		}
		return RESULT_TYPE(value);
	}
};

template <typename T>
void ExecuteMakeTimestamp(DataChunk &input, ExpressionState &state, Vector &result) {
	if (input.ColumnCount() == 1) {
		auto func = MakeTimestampOperator::Operation<T, timestamp_t>;
		UnaryExecutor::Execute<T, timestamp_t>(input.data[0], result, func);
		return;
	}

	D_ASSERT(input.ColumnCount() == 6);

	auto func = MakeTimestampOperator::Operation<T, T, T, T, T, double, timestamp_t>;
	VariadicExecutor::Execute<timestamp_t, T, T, T, T, T, double>(input, result, func);
}

template <typename T>
void ExecuteMakeTimestampNs(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);

	auto func = MakeTimestampOperator::Operation<T, timestamp_ns_t>;
	UnaryExecutor::Execute<T, timestamp_ns_t>(input.data[0], result, func);
	return;
}

} // namespace

ScalarFunctionSet MakeDateFun::GetFunctions() {
	ScalarFunctionSet make_date("make_date");
	ScalarFunction epoch_fun({}, LogicalType::DATE, MakeDateFromEpoch);
	epoch_fun.GetSignature().AddParameter("days", LogicalType::INTEGER);
	make_date.AddFunction(epoch_fun);
	ScalarFunction ymd_fun({}, LogicalType::DATE, ExecuteMakeDate<int64_t>);
	ymd_fun.GetSignature()
	    .AddParameter("year", LogicalType::BIGINT)
	    .AddParameter("month", LogicalType::BIGINT)
	    .AddParameter("day", LogicalType::BIGINT);
	make_date.AddFunction(ymd_fun);

	child_list_t<LogicalType> make_date_children {
	    {"year", LogicalType::BIGINT}, {"month", LogicalType::BIGINT}, {"day", LogicalType::BIGINT}};
	ScalarFunction struct_fun({}, LogicalType::DATE, ExecuteStructMakeDate<int64_t>);
	struct_fun.GetSignature().AddParameter("date-struct", LogicalType::STRUCT(make_date_children));
	make_date.AddFunction(struct_fun);
	make_date.ApplyToFunctions([](ScalarFunction &func) {
		func.SetFallible();
		func.SetUnaryArgProperties(ArgProperties().StrictlyIncreasing());
	});
	return make_date;
}

ScalarFunction MakeTimeFun::GetFunction() {
	ScalarFunction function({}, LogicalType::TIME, ExecuteMakeTime<int64_t>);
	function.GetSignature()
	    .AddParameter("hour", LogicalType::BIGINT)
	    .AddParameter("minute", LogicalType::BIGINT)
	    .AddParameter("seconds", LogicalType::DOUBLE);
	function.SetFallible();
	return function;
}

ScalarFunctionSet MakeTimestampFun::GetFunctions() {
	ScalarFunctionSet operator_set("make_timestamp");
	ScalarFunction full_fun({}, LogicalType::TIMESTAMP, ExecuteMakeTimestamp<int64_t>);
	full_fun.GetSignature()
	    .AddParameter("year", LogicalType::BIGINT)
	    .AddParameter("month", LogicalType::BIGINT)
	    .AddParameter("day", LogicalType::BIGINT)
	    .AddParameter("hour", LogicalType::BIGINT)
	    .AddParameter("minute", LogicalType::BIGINT)
	    .AddParameter("seconds", LogicalType::DOUBLE);
	operator_set.AddFunction(full_fun);
	ScalarFunction micros_fun({}, LogicalType::TIMESTAMP, ExecuteMakeTimestamp<int64_t>);
	micros_fun.GetSignature().AddParameter("micros", LogicalType::BIGINT);
	operator_set.AddFunction(micros_fun);

	operator_set.ApplyToFunctions([](ScalarFunction &func) {
		func.SetFallible();
		func.SetUnaryArgProperties(ArgProperties().StrictlyIncreasing());
	});
	return operator_set;
}

ScalarFunctionSet MakeTimestampNsFun::GetFunctions() {
	ScalarFunctionSet operator_set("make_timestamp_ns");
	ScalarFunction nanos_fun({}, LogicalType::TIMESTAMP_NS, ExecuteMakeTimestampNs<int64_t>);
	nanos_fun.GetSignature().AddParameter("nanos", LogicalType::BIGINT);
	operator_set.AddFunction(nanos_fun);
	// throws if the microseconds are out of range for a timestamp
	operator_set.SetFallible();
	operator_set.SetUnaryArgProperties(ArgProperties().StrictlyIncreasing());
	return operator_set;
}

} // namespace duckdb
