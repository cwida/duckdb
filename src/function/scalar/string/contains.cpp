#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar/map_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

namespace {

struct ContainsAligned {
	template <class UNSIGNED>
	static bool MatchRemainder(const unsigned char *haystack, const unsigned char *needle, idx_t needle_size) {
		return true;
	}
};

struct ContainsUnaligned {
	template <class UNSIGNED>
	static bool MatchRemainder(const unsigned char *haystack, const unsigned char *needle, idx_t needle_size) {
		idx_t matches = 0;
		for (idx_t i = sizeof(UNSIGNED); i < needle_size; i++) {
			matches += haystack[i] == needle[i];
		}
		return matches == needle_size - sizeof(UNSIGNED);
	}
};

template <class UNSIGNED, class OP>
static idx_t Contains(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                      idx_t needle_size, idx_t base_offset) {
	if (needle_size > haystack_size) {
		// needle is bigger than haystack: haystack cannot contain needle
		return DConstants::INVALID_INDEX;
	}
	haystack_size -= needle_size - 1;
	auto needle_entry = Load<UNSIGNED>(needle);
	for (idx_t offset = 0; offset < haystack_size; offset++) {
		// FindStrInStr already found the first candidate.
		auto location =
		    offset == 0
		        ? haystack
		        : static_cast<const unsigned char *>(memchr(haystack + offset, needle[0], haystack_size - offset));
		if (!location) {
			return DConstants::INVALID_INDEX;
		}
		// for this position we first compare the haystack with the needle
		offset = UnsafeNumericCast<idx_t>(location - haystack);
		auto haystack_entry = Load<UNSIGNED>(location);
		if (needle_entry == haystack_entry) {
			if (OP::template MatchRemainder<UNSIGNED>(location, needle, needle_size)) {
				return base_offset + offset;
			}
		}
	}
	return DConstants::INVALID_INDEX;
}

//! Linear-time (Knuth-Morris-Pratt) search, used when the needle keeps almost-matching the haystack
static idx_t ContainsLinear(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                            idx_t needle_size, idx_t base_offset) {
	// prefix[i] is the length of the longest proper prefix of needle[0..i] that is also a suffix of it
	vector<idx_t> prefix(needle_size, 0);
	for (idx_t i = 1, k = 0; i < needle_size; i++) {
		while (k > 0 && needle[i] != needle[k]) {
			k = prefix[k - 1];
		}
		if (needle[i] == needle[k]) {
			k++;
		}
		prefix[i] = k;
	}
	for (idx_t i = 0, k = 0; i < haystack_size; i++) {
		while (k > 0 && haystack[i] != needle[k]) {
			k = prefix[k - 1];
		}
		if (haystack[i] == needle[k]) {
			k++;
		}
		if (k == needle_size) {
			return base_offset + i + 1 - needle_size;
		}
	}
	return DConstants::INVALID_INDEX;
}

//! Like Contains<uint64_t, ContainsGeneric>, but every candidate position compares up to needle_size bytes, which is
//! quadratic when the needle keeps almost-matching - switch to a linear-time search once that happens
static idx_t ContainsLong(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                          idx_t needle_size, idx_t base_offset) {
	if (needle_size > haystack_size) {
		return DConstants::INVALID_INDEX;
	}
	const idx_t candidate_end = haystack_size - (needle_size - 1);
	auto needle_entry = Load<uint64_t>(needle);
	// the number of candidates whose remainder we compare before switching, roughly linear in the haystack
	const idx_t max_full_compares = 16 + (haystack_size * 8) / needle_size;
	idx_t full_compares = 0;
	for (idx_t offset = 0; offset < candidate_end; offset++) {
		auto location =
		    static_cast<const unsigned char *>(memchr(haystack + offset, needle[0], candidate_end - offset));
		if (!location) {
			return DConstants::INVALID_INDEX;
		}
		offset = UnsafeNumericCast<idx_t>(location - haystack);
		if (Load<uint64_t>(location) != needle_entry) {
			continue;
		}
		if (memcmp(location + sizeof(uint64_t), needle + sizeof(uint64_t), needle_size - sizeof(uint64_t)) == 0) {
			return base_offset + offset;
		}
		if (++full_compares > max_full_compares) {
			return ContainsLinear(location, haystack_size - offset, needle, needle_size, base_offset + offset);
		}
	}
	return DConstants::INVALID_INDEX;
}

struct ContainsOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return FindStrInStr(left, right) != DConstants::INVALID_INDEX;
	}
};

} // namespace

idx_t FindStrInStr(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle, idx_t needle_size) {
	D_ASSERT(needle_size > 0);
	// start off by performing a memchr to find the first character of the
	auto location = memchr(haystack, needle[0], haystack_size);
	if (location == nullptr) {
		return DConstants::INVALID_INDEX;
	}
	idx_t base_offset = UnsafeNumericCast<idx_t>(const_uchar_ptr_cast(location) - haystack);
	haystack_size -= base_offset;
	haystack = const_uchar_ptr_cast(location);
	// switch algorithm depending on needle size
	switch (needle_size) {
	case 1:
		return base_offset;
	case 2:
		return Contains<uint16_t, ContainsAligned>(haystack, haystack_size, needle, 2, base_offset);
	case 3:
		return Contains<uint16_t, ContainsUnaligned>(haystack, haystack_size, needle, 3, base_offset);
	case 4:
		return Contains<uint32_t, ContainsAligned>(haystack, haystack_size, needle, 4, base_offset);
	case 5:
		return Contains<uint32_t, ContainsUnaligned>(haystack, haystack_size, needle, 5, base_offset);
	case 6:
		return Contains<uint32_t, ContainsUnaligned>(haystack, haystack_size, needle, 6, base_offset);
	case 7:
		return Contains<uint32_t, ContainsUnaligned>(haystack, haystack_size, needle, 7, base_offset);
	case 8:
		return Contains<uint64_t, ContainsAligned>(haystack, haystack_size, needle, 8, base_offset);
	default:
		return ContainsLong(haystack, haystack_size, needle, needle_size, base_offset);
	}
}

idx_t FindStrInStr(const string_t &haystack_s, const string_t &needle_s) {
	auto haystack = const_uchar_ptr_cast(haystack_s.GetData());
	auto haystack_size = haystack_s.GetSize();
	auto needle = const_uchar_ptr_cast(needle_s.GetData());
	auto needle_size = needle_s.GetSize();
	if (needle_size == 0) {
		// empty needle: always true
		return 0;
	}
	return FindStrInStr(haystack, haystack_size, needle, needle_size);
}

static FilterPropagateResult ContainsFilterPrune(const FunctionStatisticsPruneInput &input) {
	auto &children = input.function.GetChildren();
	if (children.size() != 2 || children[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	auto haystack_stats = input.ChildStats(0);
	if (!haystack_stats || haystack_stats->GetStatsType() != StatisticsType::STRING_STATS) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	if (!haystack_stats->CanHaveNoNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	auto &needle_value = children[1]->Cast<BoundConstantExpression>().GetValue();
	if (needle_value.IsNull()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	auto &needle = StringValue::Get(needle_value);

	// if the max string length is less than the needle size, we can prune the filter.
	if (StringStats::HasMaxStringLength(*haystack_stats) &&
	    StringStats::MaxStringLength(*haystack_stats) < needle.size()) {
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}

	if (!StringStats::HasMinMax(*haystack_stats)) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}

	const auto min = StringStats::Min(*haystack_stats);
	const auto max = StringStats::Max(*haystack_stats);
	if (min != max) {
		return FilterPropagateResult::NO_PRUNING_POSSIBLE;
	}
	// All strings share the stored prefix, so a needle within it always matches.
	if (FindStrInStr(string_t(min), string_t(needle)) == DConstants::INVALID_INDEX) {
		// A missing needle could still occur beyond the truncated prefix.
		if (StringStats::GetMinType(*haystack_stats) != StringStatsType::EXACT_STATS ||
		    StringStats::GetMaxType(*haystack_stats) != StringStatsType::EXACT_STATS) {
			return FilterPropagateResult::NO_PRUNING_POSSIBLE;
		}
		return FilterPropagateResult::FILTER_ALWAYS_FALSE;
	}
	return haystack_stats->CanHaveNull() ? FilterPropagateResult::FILTER_TRUE_OR_NULL
	                                     : FilterPropagateResult::FILTER_ALWAYS_TRUE;
}

ScalarFunction GetStringContains() {
	ScalarFunction string_fun("contains", {}, LogicalType::BOOLEAN,
	                          ScalarFunction::BinaryFunction<string_t, string_t, bool, ContainsOperator>);
	string_fun.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	string_fun.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	string_fun.SetFilterPruneCallback(ContainsFilterPrune);
	return string_fun;
}

ScalarFunctionSet ContainsFun::GetFunctions() {
	auto string_fun = GetStringContains();
	auto list_fun = ListContainsFun::GetFunction();
	auto map_fun = MapContainsFun::GetFunction();
	auto struct_fun = StructContainsFun::GetFunction();
	ScalarFunctionSet set("contains");
	set.AddFunction(string_fun);
	set.AddFunction(list_fun);
	set.AddFunction(map_fun);
	set.AddFunction(struct_fun);
	return set;
}

} // namespace duckdb
