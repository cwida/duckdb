#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

static bool StartsWith(const unsigned char *haystack, idx_t haystack_size, const unsigned char *needle,
                       idx_t needle_size) {
	D_ASSERT(needle_size > 0);
	if (needle_size > haystack_size) {
		// needle is bigger than haystack: haystack cannot start with needle
		return false;
	}
	return memcmp(haystack, needle, needle_size) == 0;
}

static bool StartsWith(const string_t &haystack_s, const string_t &needle_s) {
	auto haystack = const_uchar_ptr_cast(haystack_s.GetData());
	auto haystack_size = haystack_s.GetSize();
	auto needle = const_uchar_ptr_cast(needle_s.GetData());
	auto needle_size = needle_s.GetSize();
	if (needle_size == 0) {
		// empty needle: always true
		return true;
	}
	return StartsWith(haystack, haystack_size, needle, needle_size);
}

struct StartsWithOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return StartsWith(left, right);
	}
};

ScalarFunction StartsWithOperatorFun::GetFunction() {
	ScalarFunction starts_with({}, LogicalType::BOOLEAN,
	                           ScalarFunction::BinaryFunction<string_t, string_t, bool, StartsWithOperator>);
	starts_with.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	starts_with.SetCollationHandling(FunctionCollationHandling::PUSH_COMBINABLE_COLLATIONS);
	starts_with.SetFilterPruneCallback(PrefixFilterPrune);
	return starts_with;
}

} // namespace duckdb
