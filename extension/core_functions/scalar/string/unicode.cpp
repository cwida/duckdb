#include "core_functions/scalar/string_functions.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "utf8proc.hpp"

#include <string.h>

namespace duckdb {

struct UnicodeOperator {
	template <class TA, class TR>
	static inline TR Operation(const TA &input) {
		auto str = reinterpret_cast<const utf8proc_uint8_t *>(input.GetData());
		auto len = input.GetSize();
		utf8proc_int32_t codepoint;
		(void)utf8proc_iterate(str, UnsafeNumericCast<utf8proc_ssize_t>(len), &codepoint);
		return codepoint;
	}
};

ScalarFunction UnicodeFun::GetFunction() {
	ScalarFunction fun({}, LogicalType::INTEGER, ScalarFunction::UnaryFunction<string_t, int32_t, UnicodeOperator>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

} // namespace duckdb
