#include "core_functions/scalar/operators_functions.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/types/cast_helpers.hpp"
#include "duckdb/common/types/bit.hpp"

namespace duckdb {
template <class OP>
static scalar_function_t GetScalarIntegerUnaryFunction(const LogicalType &type) {
	scalar_function_t function;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		function = &ScalarFunction::UnaryFunction<int8_t, int8_t, OP>;
		break;
	case LogicalTypeId::SMALLINT:
		function = &ScalarFunction::UnaryFunction<int16_t, int16_t, OP>;
		break;
	case LogicalTypeId::INTEGER:
		function = &ScalarFunction::UnaryFunction<int32_t, int32_t, OP>;
		break;
	case LogicalTypeId::BIGINT:
		function = &ScalarFunction::UnaryFunction<int64_t, int64_t, OP>;
		break;
	case LogicalTypeId::UTINYINT:
		function = &ScalarFunction::UnaryFunction<uint8_t, uint8_t, OP>;
		break;
	case LogicalTypeId::USMALLINT:
		function = &ScalarFunction::UnaryFunction<uint16_t, uint16_t, OP>;
		break;
	case LogicalTypeId::UINTEGER:
		function = &ScalarFunction::UnaryFunction<uint32_t, uint32_t, OP>;
		break;
	case LogicalTypeId::UBIGINT:
		function = &ScalarFunction::UnaryFunction<uint64_t, uint64_t, OP>;
		break;
	case LogicalTypeId::HUGEINT:
		function = &ScalarFunction::UnaryFunction<hugeint_t, hugeint_t, OP>;
		break;
	case LogicalTypeId::UHUGEINT:
		function = &ScalarFunction::UnaryFunction<uhugeint_t, uhugeint_t, OP>;
		break;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarIntegerUnaryFunction");
	}
	return function;
}

template <class OP>
static scalar_function_t GetScalarIntegerBinaryFunction(const LogicalType &type) {
	scalar_function_t function;
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
		function = &ScalarFunction::BinaryFunction<int8_t, int8_t, int8_t, OP>;
		break;
	case LogicalTypeId::SMALLINT:
		function = &ScalarFunction::BinaryFunction<int16_t, int16_t, int16_t, OP>;
		break;
	case LogicalTypeId::INTEGER:
		function = &ScalarFunction::BinaryFunction<int32_t, int32_t, int32_t, OP>;
		break;
	case LogicalTypeId::BIGINT:
		function = &ScalarFunction::BinaryFunction<int64_t, int64_t, int64_t, OP>;
		break;
	case LogicalTypeId::UTINYINT:
		function = &ScalarFunction::BinaryFunction<uint8_t, uint8_t, uint8_t, OP>;
		break;
	case LogicalTypeId::USMALLINT:
		function = &ScalarFunction::BinaryFunction<uint16_t, uint16_t, uint16_t, OP>;
		break;
	case LogicalTypeId::UINTEGER:
		function = &ScalarFunction::BinaryFunction<uint32_t, uint32_t, uint32_t, OP>;
		break;
	case LogicalTypeId::UBIGINT:
		function = &ScalarFunction::BinaryFunction<uint64_t, uint64_t, uint64_t, OP>;
		break;
	case LogicalTypeId::HUGEINT:
		function = &ScalarFunction::BinaryFunction<hugeint_t, hugeint_t, hugeint_t, OP>;
		break;
	case LogicalTypeId::UHUGEINT:
		function = &ScalarFunction::BinaryFunction<uhugeint_t, uhugeint_t, uhugeint_t, OP>;
		break;
	default:
		throw NotImplementedException("Unimplemented type for GetScalarIntegerBinaryFunction");
	}
	return function;
}

//===--------------------------------------------------------------------===//
// & [bitwise_and]
//===--------------------------------------------------------------------===//
namespace {

struct BitwiseANDOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return left & right;
	}
};

void BitwiseANDOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result,
	                                                      [&](string_t rhs, string_t lhs) {
		                                                      string_t target = heap.EmptyString(rhs.GetSize());

		                                                      Bit::BitwiseAnd(rhs, lhs, target);
		                                                      return target;
	                                                      });
}

} // namespace

ScalarFunctionSet BitwiseAndFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerBinaryFunction<BitwiseANDOperator>(type));
		fun.GetSignature().AddParameter("left", type).AddParameter("right", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseANDOperation);
	bit_fun.GetSignature().AddParameter("left", LogicalType::BIT).AddParameter("right", LogicalType::BIT);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

//===--------------------------------------------------------------------===//
// | [bitwise_or]
//===--------------------------------------------------------------------===//
namespace {

struct BitwiseOROperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return left | right;
	}
};

void BitwiseOROperation(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result,
	                                                      [&](string_t rhs, string_t lhs) {
		                                                      string_t target = heap.EmptyString(rhs.GetSize());

		                                                      Bit::BitwiseOr(rhs, lhs, target);
		                                                      return target;
	                                                      });
}

} // namespace

ScalarFunctionSet BitwiseOrFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerBinaryFunction<BitwiseOROperator>(type));
		fun.GetSignature().AddParameter("left", type).AddParameter("right", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseOROperation);
	bit_fun.GetSignature().AddParameter("left", LogicalType::BIT).AddParameter("right", LogicalType::BIT);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

//===--------------------------------------------------------------------===//
// # [bitwise_xor]
//===--------------------------------------------------------------------===//
namespace {

struct BitwiseXOROperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA left, TB right) {
		return left ^ right;
	}
};

void BitwiseXOROperation(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	BinaryExecutor::Execute<string_t, string_t, string_t>(args.data[0], args.data[1], result,
	                                                      [&](string_t rhs, string_t lhs) {
		                                                      string_t target = heap.EmptyString(rhs.GetSize());

		                                                      Bit::BitwiseXor(rhs, lhs, target);
		                                                      return target;
	                                                      });
}

} // namespace

ScalarFunctionSet BitwiseXorFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerBinaryFunction<BitwiseXOROperator>(type));
		fun.GetSignature().AddParameter("left", type).AddParameter("right", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseXOROperation);
	bit_fun.GetSignature().AddParameter("left", LogicalType::BIT).AddParameter("right", LogicalType::BIT);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

//===--------------------------------------------------------------------===//
// ~ [bitwise_not]
//===--------------------------------------------------------------------===//
namespace {

struct BitwiseNotOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return ~input;
	}
};

void BitwiseNOTOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(args.data[0], result, [&](string_t input) {
		string_t target = heap.EmptyString(input.GetSize());

		Bit::BitwiseNot(input, target);
		return target;
	});
}

} // namespace

ScalarFunctionSet BitwiseNotFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerUnaryFunction<BitwiseNotOperator>(type));
		fun.GetSignature().AddParameter("input", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseNOTOperation);
	bit_fun.GetSignature().AddParameter("input", LogicalType::BIT);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

//===--------------------------------------------------------------------===//
// << [bitwise_left_shift]
//===--------------------------------------------------------------------===//
namespace {

struct BitwiseShiftLeftOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB shift) {
		TA max_shift = TA(sizeof(TA) * 8) + (NumericLimits<TA>::IsSigned() ? 0 : 1);
		if (input < 0) {
			throw OutOfRangeException("Cannot left-shift negative number %s", NumericHelper::ToString(input));
		}
		if (shift < 0) {
			throw OutOfRangeException("Cannot left-shift by negative number %s", NumericHelper::ToString(shift));
		}
		if (shift >= max_shift) {
			if (input == 0) {
				return 0;
			}
			throw OutOfRangeException("Left-shift value %s is out of range", NumericHelper::ToString(shift));
		}
		if (shift == 0) {
			return input;
		}
		if (input == 0) {
			return 0;
		}
		TA max_value = UnsafeNumericCast<TA>((TA(1) << (max_shift - shift - 1)));
		if (input >= max_value) {
			throw OutOfRangeException("Overflow in left shift (%s << %s)", NumericHelper::ToString(input),
			                          NumericHelper::ToString(shift));
		}
		return UnsafeNumericCast<TR>(input << shift);
	}
};

void BitwiseShiftLeftOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t input, int32_t shift) {
		    auto max_shift = UnsafeNumericCast<int32_t>(Bit::BitLength(input));
		    if (shift == 0) {
			    return input;
		    }
		    if (shift < 0) {
			    throw OutOfRangeException("Cannot left-shift by negative number %s", NumericHelper::ToString(shift));
		    }
		    string_t target = StringVector::EmptyString(result, input.GetSize());

		    if (shift >= max_shift) {
			    Bit::SetEmptyBitString(target, input);
			    return target;
		    }
		    Bit::LeftShift(input, UnsafeNumericCast<idx_t>(shift), target);
		    return target;
	    });
}
} // namespace

ScalarFunctionSet LeftShiftFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerBinaryFunction<BitwiseShiftLeftOperator>(type));
		fun.GetSignature().AddParameter("input", type).AddParameter("shift", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseShiftLeftOperation);
	bit_fun.GetSignature().AddParameter("input", LogicalType::BIT).AddParameter("shift", LogicalType::INTEGER);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

//===--------------------------------------------------------------------===//
// >> [bitwise_right_shift]
//===--------------------------------------------------------------------===//
namespace {

template <class T>
bool RightShiftInRange(T shift) {
	return shift >= 0 && shift < T(sizeof(T) * 8);
}

struct BitwiseShiftRightOperator {
	template <class TA, class TB, class TR>
	static inline TR Operation(TA input, TB shift) {
		return RightShiftInRange(shift) ? input >> shift : 0;
	}
};

void BitwiseShiftRightOperation(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, int32_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t input, int32_t shift) {
		    auto max_shift = UnsafeNumericCast<int32_t>(Bit::BitLength(input));
		    if (shift == 0) {
			    return input;
		    }
		    string_t target = StringVector::EmptyString(result, input.GetSize());
		    if (shift < 0 || shift >= max_shift) {
			    Bit::SetEmptyBitString(target, input);
			    return target;
		    }
		    Bit::RightShift(input, UnsafeNumericCast<idx_t>(shift), target);
		    return target;
	    });
}

} // namespace

ScalarFunctionSet RightShiftFun::GetFunctions() {
	ScalarFunctionSet functions;
	for (auto &type : LogicalType::Integral()) {
		ScalarFunction fun({}, type, GetScalarIntegerBinaryFunction<BitwiseShiftRightOperator>(type));
		fun.GetSignature().AddParameter("input", type).AddParameter("shift", type);
		functions.AddFunction(fun);
	}
	ScalarFunction bit_fun({}, LogicalType::BIT, BitwiseShiftRightOperation);
	bit_fun.GetSignature().AddParameter("input", LogicalType::BIT).AddParameter("shift", LogicalType::INTEGER);
	functions.AddFunction(bit_fun);
	functions.SetFallible();
	return functions;
}

} // namespace duckdb
