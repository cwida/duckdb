#include "core_functions/scalar/bit_functions.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/types/cast_helpers.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// BitStringFunction
//===--------------------------------------------------------------------===//
template <bool FROM_STRING>
static void BitStringFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t input, int32_t n) {
		    if (n < 0) {
			    throw InvalidInputException("The bitstring length cannot be negative");
		    }
		    idx_t input_length;
		    if (FROM_STRING) {
			    input_length = input.GetSize();
		    } else {
			    input_length = Bit::BitLength(input);
		    }
		    if (idx_t(n) < input_length) {
			    throw InvalidInputException("Length must be equal or larger than input string");
		    }

		    idx_t len = Bit::ComputeBitstringLen(UnsafeNumericCast<idx_t>(n));
		    string_t target = StringVector::EmptyString(result, len);
		    if (FROM_STRING) {
			    Bit::BitString(input, UnsafeNumericCast<idx_t>(n), target);
		    } else {
			    Bit::ExtendBitString(input, UnsafeNumericCast<idx_t>(n), target);
		    }
		    target.Finalize();
		    return target;
	    });
}

ScalarFunctionSet BitStringFun::GetFunctions() {
	ScalarFunctionSet set("bitstring");

	ScalarFunction fun_varchar({}, LogicalType::BIT, BitStringFunction<true>);
	fun_varchar.GetSignature()
	    .AddParameter("bitstring", LogicalType::VARCHAR)
	    .AddParameter("length", LogicalType::INTEGER);
	set.AddFunction(std::move(fun_varchar));

	ScalarFunction fun_bit({}, LogicalType::BIT, BitStringFunction<false>);
	fun_bit.GetSignature().AddParameter("bitstring", LogicalType::BIT).AddParameter("length", LogicalType::INTEGER);
	set.AddFunction(std::move(fun_bit));

	set.SetFallible();
	return set;
}

namespace {

// Keep the key bytes above the BLOB escape range while preserving 0 < 1.
constexpr data_t BIT_SORT_KEY_ZERO = 2;
constexpr data_t BIT_SORT_KEY_ONE = 3;

string_t CreateBitStringSortKey(string_t input, Vector &result) {
	const auto bit_length = Bit::BitLength(input);
	auto target = StringVector::EmptyString(result, bit_length);
	auto data = data_ptr_cast(target.GetDataWriteable());
	for (idx_t bit_idx = 0; bit_idx < bit_length; bit_idx++) {
		data[bit_idx] = Bit::GetBit(input, bit_idx) ? BIT_SORT_KEY_ONE : BIT_SORT_KEY_ZERO;
	}
	target.Finalize();
	return target;
}

} // namespace

static void BitStringSortKeyFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result,
	                                           [&](string_t input) { return CreateBitStringSortKey(input, result); });
}

ScalarFunction BitStringSortKeyFun::GetFunction() {
	ScalarFunction func({}, LogicalType::BLOB, BitStringSortKeyFunction);
	func.GetSignature().AddParameter("bitstring", LogicalType::BIT);
	return func;
}

//===--------------------------------------------------------------------===//
// get_bit
//===--------------------------------------------------------------------===//
namespace {

struct GetBitOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB n) {
		if (n < 0 || UnsafeNumericCast<idx_t>(n) >= Bit::BitLength(input)) {
			throw OutOfRangeException("bit index %s out of valid range (0..%s)", NumericHelper::ToString(n),
			                          NumericHelper::ToString(Bit::BitLength(input) - 1));
		}
		return UnsafeNumericCast<TR>(Bit::GetBit(input, UnsafeNumericCast<idx_t>(n)));
	}
};

} // namespace
ScalarFunction GetBitFun::GetFunction() {
	ScalarFunction func({}, LogicalType::INTEGER,
	                    ScalarFunction::BinaryFunction<string_t, int32_t, int32_t, GetBitOperator>);
	func.GetSignature().AddParameter("bitstring", LogicalType::BIT).AddParameter("index", LogicalType::INTEGER);
	func.SetFallible();
	return func;
}

//===--------------------------------------------------------------------===//
// set_bit
//===--------------------------------------------------------------------===//
static void SetBitOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	TernaryExecutor::Execute<string_t, int32_t, int32_t, string_t>(
	    args.data[0], args.data[1], args.data[2], result, [&](string_t input, int32_t n, int32_t new_value) {
		    if (new_value != 0 && new_value != 1) {
			    throw InvalidInputException("The new bit must be 1 or 0");
		    }
		    if (n < 0 || UnsafeNumericCast<idx_t>(n) >= Bit::BitLength(input)) {
			    throw OutOfRangeException("bit index %s out of valid range (0..%s)", NumericHelper::ToString(n),
			                              NumericHelper::ToString(Bit::BitLength(input) - 1));
		    }
		    string_t target = StringVector::EmptyString(result, input.GetSize());
		    memcpy(target.GetDataWriteable(), input.GetData(), input.GetSize());
		    Bit::SetBit(target, UnsafeNumericCast<idx_t>(n), UnsafeNumericCast<idx_t>(new_value));
		    return target;
	    });
}

ScalarFunction SetBitFun::GetFunction() {
	ScalarFunction function({}, LogicalType::BIT, SetBitOperation);
	function.GetSignature()
	    .AddParameter("bitstring", LogicalType::BIT)
	    .AddParameter("index", LogicalType::INTEGER)
	    .AddParameter("new_value", LogicalType::INTEGER);
	function.SetFallible();
	return function;
}

//===--------------------------------------------------------------------===//
// bit_position
//===--------------------------------------------------------------------===//
namespace {

struct BitPositionOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA substring, TB input) {
		if (substring.GetSize() > input.GetSize()) {
			return 0;
		}
		return UnsafeNumericCast<TR>(Bit::BitPosition(substring, input));
	}
};

} // namespace

ScalarFunction BitPositionFun::GetFunction() {
	ScalarFunction func({}, LogicalType::INTEGER,
	                    ScalarFunction::BinaryFunction<string_t, string_t, int32_t, BitPositionOperator>);
	func.GetSignature().AddParameter("substring", LogicalType::BIT).AddParameter("bitstring", LogicalType::BIT);
	return func;
}

} // namespace duckdb
