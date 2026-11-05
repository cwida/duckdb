#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/common/types/string_type.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

static bool SuffixFunction(const string_t &str, const string_t &suffix) {
	auto suffix_size = suffix.GetSize();
	auto str_size = str.GetSize();
	if (suffix_size > str_size) {
		return false;
	}

	return memcmp(str.GetData() + str_size - suffix_size, suffix.GetData(), suffix_size) == 0;
}

struct SuffixOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return SuffixFunction(left, right);
	}
};

FilterPropagateResult SuffixFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto &children = input.function.GetChildren();
	if (children.size() != 2 || children[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	auto string_stats = input.ChildStats(0);
	if (!string_stats || string_stats->GetStatsType() != StatisticsType::STRING_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!string_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	auto &suffix_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (suffix_value.IsNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto &suffix = StringValue::Get(suffix_value);
	if (StringStats::HasMaxStringLength(*string_stats) && StringStats::MaxStringLength(*string_stats) < suffix.size()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	if (StringStats::GetMinType(*string_stats) != StringStatsType::EXACT_STATS ||
	    StringStats::GetMaxType(*string_stats) != StringStatsType::EXACT_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	auto min = StringStats::Min(*string_stats);
	if (min != StringStats::Max(*string_stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!SuffixFunction(string_t(min), string_t(suffix))) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	return string_stats->CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL
	                                   : FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

} // namespace

ScalarFunction SuffixFun::GetFunction() {
	ScalarFunction function("suffix", {}, LogicalType::BOOLEAN,
	                        ScalarFunction::BinaryFunction<string_t, string_t, bool, SuffixOperator>);
	function.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	function.SetFilterPruneCallback(SuffixFilterPrune);
	return function;
}

} // namespace duckdb
