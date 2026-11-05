#include "core_functions/scalar/string_functions.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";

static void ToBaseFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input = args.data[0];
	const auto &radix = args.data[1];
	const auto &min_length = args.data[2];

	auto &heap = StringVector::GetStringHeap(result);
	TernaryExecutor::Execute<int64_t, int32_t, int32_t, string_t>(
	    input, radix, min_length, result, [&](int64_t input, int32_t radix, int32_t min_length) {
		    if (input < 0) {
			    throw InvalidInputException("'to_base' number must be greater than or equal to 0");
		    }
		    if (radix < 2 || radix > 36) {
			    throw InvalidInputException("'to_base' radix must be between 2 and 36");
		    }
		    if (min_length > 64 || min_length < 0) {
			    throw InvalidInputException("'to_base' min_length must be between 0 and 64");
		    }

		    char buf[64];
		    char *end = buf + sizeof(buf);
		    char *ptr = end;
		    do {
			    *--ptr = alphabet[input % radix];
			    input /= radix;
		    } while (input > 0);

		    auto length = end - ptr;
		    while (length < min_length) {
			    *--ptr = '0';
			    length++;
		    }

		    return heap.AddString(ptr, UnsafeNumericCast<idx_t>(end - ptr));
	    });
}

ScalarFunctionSet ToBaseFun::GetFunctions() {
	ScalarFunctionSet set("to_base");

	auto function = ScalarFunction({}, LogicalType::VARCHAR, ToBaseFunction);
	function.GetSignature()
	    .AddParameter("number", LogicalType::BIGINT)
	    .AddParameter("radix", LogicalType::INTEGER)
	    .AddParameter("min_length", LogicalType::INTEGER);
	function.GetSignature().GetParameter(2).SetDefaultValue(Value::INTEGER(0));
	set.AddFunction(std::move(function));

	// throws if the number, radix or min_length are out of range
	set.SetFallible();
	return set;
}

} // namespace duckdb
