#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/operator/add.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

static void RepeatFunction(DataChunk &args, ExpressionState &, Vector &result) {
	const auto &str_vector = args.data[0];
	const auto &cnt_vector = args.data[1];

	BinaryExecutor::Execute<string_t, int64_t, string_t>(
	    str_vector, cnt_vector, result, [&](string_t str, int64_t cnt) {
		    auto input_str = str.GetData();
		    auto size_str = str.GetSize();
		    idx_t copy_count = cnt <= 0 || size_str == 0 ? 0 : UnsafeNumericCast<idx_t>(cnt);

		    idx_t copy_size;
		    if (TryMultiplyOperator::Operation(size_str, copy_count, copy_size)) {
			    auto result_str = StringVector::EmptyString(result, copy_size);
			    auto result_data = result_str.GetDataWriteable();
			    for (idx_t i = 0; i < copy_count; i++) {
				    memcpy(result_data + i * size_str, input_str, size_str);
			    }
			    result_str.Finalize();
			    return result_str;
		    } else {
			    throw OutOfRangeException(
			        "Cannot create a string of size: '%d' * '%d', the maximum supported string size is: '%d'", size_str,
			        copy_count, string_t::MAX_STRING_SIZE);
		    }
	    });
}

static void RepeatListFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto &list_vector = args.data[0];
	auto &cnt_vector = args.data[1];
	auto &source_child = ListVector::GetChildMutable(list_vector);
	auto count = args.size();

	auto list_entries = list_vector.Values<list_entry_t>();
	auto cnt_entries = cnt_vector.Values<int64_t>();

	auto result_writer = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto list_entry = list_entries[i];
		auto cnt_entry = cnt_entries[i];
		if (!list_entry.IsValid() || !cnt_entry.IsValid()) {
			result_writer.WriteNull();
			continue;
		}
		const auto &list_input = list_entry.GetValue();
		const auto cnt = cnt_entry.GetValue();
		const idx_t copy_count = cnt <= 0 || list_input.length == 0 ? 0 : UnsafeNumericCast<idx_t>(cnt);
		idx_t result_length;
		if (!TryMultiplyOperator::Operation(list_input.length, copy_count, result_length)) {
			throw OutOfRangeException("Cannot create a list of size: '%d' * '%d', the result is too large",
			                          list_input.length, copy_count);
		}
		// reserve the worst-case child capacity up front so an absurd target
		// size fails before we enter a 10^N-iteration Append loop
		ListVector::Reserve(result, ListVector::GetListSize(result) + result_length);
		auto list = result_writer.WriteDynamicList();
		for (idx_t j = 0; j < copy_count; j++) {
			list.Append(source_child, *FlatVector::IncrementalSelectionVector(), list_input.offset + list_input.length,
			            list_input.offset, list_input.length);
		}
	}
	result.Verify();
}

static unique_ptr<BaseStatistics> RepeatStringStats(ClientContext &, FunctionStatisticsInput &input) {
	auto &children = input.expr.GetChildren();
	if (children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT ||
	    !StringStats::HasMaxStringLength(input.child_stats[0])) {
		return nullptr;
	}
	auto &count_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (count_value.IsNull()) {
		return nullptr;
	}
	auto count = count_value.GetValue<int64_t>();
	auto input_size = StringStats::MaxStringLength(input.child_stats[0]);
	uint64_t result_size = 0;
	if (count > 0 && !TryMultiplyOperator::Operation<uint64_t, uint64_t, uint64_t>(
	                     input_size, NumericCast<uint64_t>(count), result_size)) {
		return nullptr;
	}
	if (result_size > string_t::MAX_STRING_SIZE || result_size > NumericLimits<uint32_t>::Maximum()) {
		return nullptr;
	}
	auto result = StringStats::CreateUnknown(input.expr.GetReturnType());
	StringStats::SetMaxStringLength(result, NumericCast<uint32_t>(result_size));
	result.CopyValidity(input.child_stats[0]);
	if (input.child_stats[1].CanHaveNull()) {
		result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	}
	if (!input.child_stats[1].CanHaveNoNull()) {
		result.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}
	input.expr.FunctionMutable().SetErrorMode(FunctionErrors::CANNOT_ERROR);
	return result.ToUnique();
}

ScalarFunctionSet RepeatFun::GetFunctions() {
	ScalarFunctionSet repeat;

	ScalarFunction string_fun({}, LogicalType::VARCHAR, RepeatFunction, nullptr, RepeatStringStats);
	string_fun.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("count", LogicalType::BIGINT);
	repeat.AddFunction(string_fun);

	ScalarFunction blob_fun({}, LogicalType::BLOB, RepeatFunction, nullptr, RepeatStringStats);
	blob_fun.GetSignature().AddParameter("blob", LogicalType::BLOB).AddParameter("count", LogicalType::BIGINT);
	repeat.AddFunction(blob_fun);

	ScalarFunction list_fun({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), RepeatListFunction);
	list_fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::TEMPLATE("T")))
	    .AddParameter("count", LogicalType::BIGINT);
	repeat.AddFunction(list_fun);

	repeat.SetFallible();
	return repeat;
}

} // namespace duckdb
