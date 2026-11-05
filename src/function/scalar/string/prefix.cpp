#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/function/scalar/string_common.hpp"

namespace duckdb {

namespace {

bool PrefixFunction(const string_t &str, const string_t &pattern) {
	auto str_length = str.GetSize();
	auto patt_length = pattern.GetSize();
	if (patt_length > str_length) {
		return false;
	}
	if (patt_length <= string_t::PREFIX_LENGTH) {
		// short prefix
		if (patt_length == 0) {
			// length = 0, return true
			return true;
		}

		// prefix early out
		const char *str_pref = str.GetPrefix();
		const char *patt_pref = pattern.GetPrefix();
		for (idx_t i = 0; i < patt_length; ++i) {
			if (str_pref[i] != patt_pref[i]) {
				return false;
			}
		}
		return true;
	} else {
		// prefix early out
		const char *str_pref = str.GetPrefix();
		const char *patt_pref = pattern.GetPrefix();
		for (idx_t i = 0; i < string_t::PREFIX_LENGTH; ++i) {
			if (str_pref[i] != patt_pref[i]) {
				// early out
				return false;
			}
		}
		// compare the rest of the prefix
		const char *str_data = str.GetData();
		const char *patt_data = pattern.GetData();
		D_ASSERT(patt_length <= str_length);
		for (idx_t i = string_t::PREFIX_LENGTH; i < patt_length; ++i) {
			if (str_data[i] != patt_data[i]) {
				return false;
			}
		}
		return true;
	}
}

struct PrefixOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return PrefixFunction(left, right);
	}
};

} // namespace

ScalarFunction PrefixFun::GetFunction() {
	ScalarFunction function("prefix", {}, LogicalType::BOOLEAN,
	                        ScalarFunction::BinaryFunction<string_t, string_t, bool, PrefixOperator>);
	function.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("search_string", LogicalType::VARCHAR);
	function.SetFilterPruneCallback(PrefixFilterPrune);
	return function;
}

} // namespace duckdb
