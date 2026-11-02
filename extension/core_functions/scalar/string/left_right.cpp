#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

#include <ctype.h>
#include <algorithm>

namespace duckdb {

struct LeftRightUnicode {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Length<TA, TR>(input);
	}

	static string_t Substring(Vector &result, string_t input, int64_t offset, int64_t length) {
		return SubstringUnicode(result, input, offset, length);
	}
};

struct LeftRightGrapheme {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return GraphemeCount<TA, TR>(input);
	}

	static string_t Substring(Vector &result, string_t input, int64_t offset, int64_t length) {
		return SubstringGrapheme(result, input, offset, length);
	}
};

template <class OP>
static string_t LeftScalarFunction(Vector &result, const string_t str, int64_t pos) {
	if (pos >= 0) {
		return OP::Substring(result, str, 1, pos);
	}

	int64_t num_characters = OP::template Operation<string_t, int64_t>(str);
	pos = MaxValue<int64_t>(0, num_characters + pos);
	return OP::Substring(result, str, 1, pos);
}

template <class OP>
static void LeftFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &pos_vec = args.data[1];

	BinaryExecutor::Execute<string_t, int64_t, string_t>(
	    str_vec, pos_vec, result, [&](string_t str, int64_t pos) { return LeftScalarFunction<OP>(result, str, pos); });
}

static unique_ptr<BaseStatistics> LeftPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &children = input.expr.GetChildren();
	D_ASSERT(children.size() == 2);
	if (children[1]->GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return nullptr;
	}
	auto &length_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (length_value.IsNull()) {
		return nullptr;
	}
	auto length = length_value.GetValue<int64_t>();
	// a negative length counts from the back of each string, so no bounds can be derived
	if (length < 0 || length > NumericLimits<uint32_t>::Maximum()) {
		return nullptr;
	}
	return PropagateStringSliceStats(input, /*start_character_index=*/0,
	                                 /*character_count=*/NumericCast<idx_t>(length));
}

ScalarFunction LeftFun::GetFunction() {
	ScalarFunction function({}, LogicalType::VARCHAR, LeftFunction<LeftRightUnicode>, /*bind=*/nullptr,
	                        LeftPropagateStats);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("count", LogicalType::BIGINT);
	// throws if the resulting substring is out of the supported range
	function.SetFallible();
	return function;
}

ScalarFunction LeftGraphemeFun::GetFunction() {
	ScalarFunction function({}, LogicalType::VARCHAR, LeftFunction<LeftRightGrapheme>);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("count", LogicalType::BIGINT);
	// throws if the resulting substring is out of the supported range
	function.SetFallible();
	return function;
}

template <class OP>
static string_t RightScalarFunction(Vector &result, const string_t str, int64_t pos) {
	int64_t num_characters = OP::template Operation<string_t, int64_t>(str);
	if (pos >= 0) {
		int64_t len = MinValue<int64_t>(num_characters, pos);
		int64_t start = num_characters - len + 1;
		return OP::Substring(result, str, start, len);
	}

	int64_t len = 0;
	if (pos != std::numeric_limits<int64_t>::min()) {
		len = num_characters - MinValue<int64_t>(num_characters, -pos);
	}
	int64_t start = num_characters - len + 1;
	return OP::Substring(result, str, start, len);
}

template <class OP>
static void RightFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &str_vec = args.data[0];
	const auto &pos_vec = args.data[1];
	BinaryExecutor::Execute<string_t, int64_t, string_t>(
	    str_vec, pos_vec, result, [&](string_t str, int64_t pos) { return RightScalarFunction<OP>(result, str, pos); });
}

ScalarFunction RightFun::GetFunction() {
	ScalarFunction function({}, LogicalType::VARCHAR, RightFunction<LeftRightUnicode>);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("count", LogicalType::BIGINT);
	// throws if the resulting substring is out of the supported range
	function.SetFallible();
	return function;
}

ScalarFunction RightGraphemeFun::GetFunction() {
	ScalarFunction function({}, LogicalType::VARCHAR, RightFunction<LeftRightGrapheme>);
	function.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("count", LogicalType::BIGINT);
	// throws if the resulting substring is out of the supported range
	function.SetFallible();
	return function;
}

} // namespace duckdb
