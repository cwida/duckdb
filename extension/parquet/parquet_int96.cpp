//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_int96.cpp
//
//
//===----------------------------------------------------------------------===//

#include "parquet_int96.hpp"

#include <string.h>

#include "duckdb/common/types/date.hpp"
#include "duckdb/common/types/time.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "parquet_timestamp.hpp"

namespace duckdb {

static LogicalType Int96StructType() {
	return LogicalType::STRUCT({{"date", LogicalType::DATE}, {"time", LogicalType::TIME_NS}});
}

static void Int96AsStructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto count = args.size();
	auto blob_values = args.data[0].Values<string_t>();
	auto writer = FlatVector::Writer<VectorStructType<date_t, dtime_ns_t>>(result, count);
	for (auto entry : blob_values) {
		if (!entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto blob = entry.GetValue();
		Int96 raw_ts;
		memcpy(raw_ts.value, blob.GetData(), sizeof(Int96));
		writer.WriteValue([&](auto &date_writer, auto &time_writer) {
			date_writer.WriteValue(ImpalaTimestampToDate(raw_ts));
			time_writer.WriteValue(ImpalaTimestampToTimeNs(raw_ts));
		});
	}
}

unique_ptr<Expression> CreateInt96AsStructExpression(ClientContext &context) {
	auto args = vector<unique_ptr<Expression>>();
	auto ref = make_uniq_base<Expression, BoundReferenceExpression>(LogicalTypeId::BLOB, 0);
	args.push_back(std::move(ref));
	ScalarFunction func("__internal_int96_as_struct", vector<LogicalType> {LogicalType::BLOB}, Int96StructType(),
	                    Int96AsStructFunction);
	return func.Bind(context, std::move(args));
}

unique_ptr<Expression> CreateInt96AsStructChildExpression(ClientContext &context, idx_t child_index) {
	auto args = vector<unique_ptr<Expression>>();
	args.push_back(CreateInt96AsStructExpression(context));
	args.push_back(
	    make_uniq_base<Expression, BoundConstantExpression>(Value::BIGINT(NumericCast<int64_t>(child_index + 1))));
	return StructExtractAtFun::GetFunction().Bind(context, std::move(args));
}

} // namespace duckdb
