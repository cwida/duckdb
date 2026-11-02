#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"

namespace duckdb {

namespace {

string IntegralCompressFunctionName(const LogicalType &result_type) {
	return StringUtil::Format("__internal_compress_integral_%s",
	                          StringUtil::Lower(LogicalTypeIdToString(result_type.id())));
}

template <class INPUT_TYPE, class RESULT_TYPE>
struct TemplatedIntegralCompress {
	static inline RESULT_TYPE Operation(const INPUT_TYPE &input, const INPUT_TYPE &min_val) {
		D_ASSERT(min_val <= input);
		return UnsafeNumericCast<RESULT_TYPE>(input - min_val);
	}
};

template <class RESULT_TYPE>
struct TemplatedIntegralCompress<hugeint_t, RESULT_TYPE> {
	static inline RESULT_TYPE Operation(const hugeint_t &input, const hugeint_t &min_val) {
		D_ASSERT(min_val <= input);
		return UnsafeNumericCast<RESULT_TYPE>((input - min_val).lower);
	}
};

template <class RESULT_TYPE>
struct TemplatedIntegralCompress<uhugeint_t, RESULT_TYPE> {
	static inline RESULT_TYPE Operation(const uhugeint_t &input, const uhugeint_t &min_val) {
		D_ASSERT(min_val <= input);
		return UnsafeNumericCast<RESULT_TYPE>((input - min_val).lower);
	}
};

template <class INPUT_TYPE, class RESULT_TYPE>
void IntegralCompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	D_ASSERT(args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR);
	const auto min_val = ConstantVector::GetData<INPUT_TYPE>(args.data[1])[0];
	UnaryExecutor::Execute<INPUT_TYPE, RESULT_TYPE>(
	    args.data[0], result,
	    [&](const INPUT_TYPE &input) {
		    return TemplatedIntegralCompress<INPUT_TYPE, RESULT_TYPE>::Operation(input, min_val);
	    },
#if defined(D_ASSERT_IS_ENABLED)
	    FunctionErrors::CAN_THROW_RUNTIME_ERROR); // Can only throw a runtime error when assertions are enabled
#else
	    FunctionErrors::CANNOT_ERROR);
#endif
}

template <class INPUT_TYPE, class RESULT_TYPE>
scalar_function_t GetIntegralCompressFunction(const LogicalType &input_type, const LogicalType &result_type) {
	return IntegralCompressFunction<INPUT_TYPE, RESULT_TYPE>;
}

template <class INPUT_TYPE>
scalar_function_t GetIntegralCompressFunctionResultSwitch(const LogicalType &input_type,
                                                          const LogicalType &result_type) {
	switch (result_type.id()) {
	case LogicalTypeId::UTINYINT:
		return GetIntegralCompressFunction<INPUT_TYPE, uint8_t>(input_type, result_type);
	case LogicalTypeId::USMALLINT:
		return GetIntegralCompressFunction<INPUT_TYPE, uint16_t>(input_type, result_type);
	case LogicalTypeId::UINTEGER:
		return GetIntegralCompressFunction<INPUT_TYPE, uint32_t>(input_type, result_type);
	case LogicalTypeId::UBIGINT:
		return GetIntegralCompressFunction<INPUT_TYPE, uint64_t>(input_type, result_type);
	default:
		throw InternalException("Unexpected result type in GetIntegralCompressFunctionResultSwitch");
	}
}

scalar_function_t GetIntegralCompressFunctionInputSwitch(const LogicalType &input_type,
                                                         const LogicalType &result_type) {
	switch (input_type.id()) {
	case LogicalTypeId::SMALLINT:
		return GetIntegralCompressFunctionResultSwitch<int16_t>(input_type, result_type);
	case LogicalTypeId::INTEGER:
		return GetIntegralCompressFunctionResultSwitch<int32_t>(input_type, result_type);
	case LogicalTypeId::BIGINT:
		return GetIntegralCompressFunctionResultSwitch<int64_t>(input_type, result_type);
	case LogicalTypeId::HUGEINT:
		return GetIntegralCompressFunctionResultSwitch<hugeint_t>(input_type, result_type);
	case LogicalTypeId::USMALLINT:
		return GetIntegralCompressFunctionResultSwitch<uint16_t>(input_type, result_type);
	case LogicalTypeId::UINTEGER:
		return GetIntegralCompressFunctionResultSwitch<uint32_t>(input_type, result_type);
	case LogicalTypeId::UBIGINT:
		return GetIntegralCompressFunctionResultSwitch<uint64_t>(input_type, result_type);
	case LogicalTypeId::UHUGEINT:
		return GetIntegralCompressFunctionResultSwitch<uhugeint_t>(input_type, result_type);
	default:
		throw InternalException("Unexpected input type in GetIntegralCompressFunctionInputSwitch");
	}
}

string IntegralDecompressFunctionName(const LogicalType &result_type) {
	return StringUtil::Format("__internal_decompress_integral_%s",
	                          StringUtil::Lower(LogicalTypeIdToString(result_type.id())));
}

template <class INPUT_TYPE, class RESULT_TYPE>
struct TemplatedIntegralDecompress {
	static inline RESULT_TYPE Operation(const INPUT_TYPE &input, const RESULT_TYPE &min_val) {
		return min_val + UnsafeNumericCast<RESULT_TYPE, INPUT_TYPE>(input);
	}
};

template <class INPUT_TYPE>
struct TemplatedIntegralDecompress<INPUT_TYPE, hugeint_t> {
	static inline hugeint_t Operation(const INPUT_TYPE &input, const hugeint_t &min_val) {
		return min_val + hugeint_t(0, input);
	}
};

template <class INPUT_TYPE>
struct TemplatedIntegralDecompress<INPUT_TYPE, uhugeint_t> {
	static inline uhugeint_t Operation(const INPUT_TYPE &input, const uhugeint_t &min_val) {
		return min_val + uhugeint_t(0, input);
	}
};

template <class INPUT_TYPE, class RESULT_TYPE>
void IntegralDecompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 2);
	D_ASSERT(args.data[1].GetVectorType() == VectorType::CONSTANT_VECTOR);
	D_ASSERT(args.data[1].GetType() == result.GetType());
	const auto min_val = ConstantVector::GetData<RESULT_TYPE>(args.data[1])[0];
	UnaryExecutor::Execute<INPUT_TYPE, RESULT_TYPE>(
	    args.data[0], result,
	    [&](const INPUT_TYPE &input) {
		    return TemplatedIntegralDecompress<INPUT_TYPE, RESULT_TYPE>::Operation(input, min_val);
	    },
	    FunctionErrors::CANNOT_ERROR);
}

template <class INPUT_TYPE, class RESULT_TYPE>
scalar_function_t GetIntegralDecompressFunction(const LogicalType &input_type, const LogicalType &result_type) {
	return IntegralDecompressFunction<INPUT_TYPE, RESULT_TYPE>;
}

template <class INPUT_TYPE>
scalar_function_t GetIntegralDecompressFunctionResultSwitch(const LogicalType &input_type,
                                                            const LogicalType &result_type) {
	switch (result_type.id()) {
	case LogicalTypeId::SMALLINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, int16_t>(input_type, result_type);
	case LogicalTypeId::INTEGER:
		return GetIntegralDecompressFunction<INPUT_TYPE, int32_t>(input_type, result_type);
	case LogicalTypeId::BIGINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, int64_t>(input_type, result_type);
	case LogicalTypeId::HUGEINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, hugeint_t>(input_type, result_type);
	case LogicalTypeId::USMALLINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, uint16_t>(input_type, result_type);
	case LogicalTypeId::UINTEGER:
		return GetIntegralDecompressFunction<INPUT_TYPE, uint32_t>(input_type, result_type);
	case LogicalTypeId::UBIGINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, uint64_t>(input_type, result_type);
	case LogicalTypeId::UHUGEINT:
		return GetIntegralDecompressFunction<INPUT_TYPE, uhugeint_t>(input_type, result_type);
	default:
		throw InternalException("Unexpected input type in GetIntegralDecompressFunctionSetSwitch");
	}
}

scalar_function_t GetIntegralDecompressFunctionInputSwitch(const LogicalType &input_type,
                                                           const LogicalType &result_type) {
	switch (input_type.id()) {
	case LogicalTypeId::UTINYINT:
		return GetIntegralDecompressFunctionResultSwitch<uint8_t>(input_type, result_type);
	case LogicalTypeId::USMALLINT:
		return GetIntegralDecompressFunctionResultSwitch<uint16_t>(input_type, result_type);
	case LogicalTypeId::UINTEGER:
		return GetIntegralDecompressFunctionResultSwitch<uint32_t>(input_type, result_type);
	case LogicalTypeId::UBIGINT:
		return GetIntegralDecompressFunctionResultSwitch<uint64_t>(input_type, result_type);
	default:
		throw InternalException("Unexpected result type in GetIntegralDecompressFunctionInputSwitch");
	}
}

void CMIntegralSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                         const BoundScalarFunction &function) {
	serializer.WriteProperty(100, "arguments", function.GetArguments());
	serializer.WriteProperty(101, "return_type", function.GetReturnType());
}

template <scalar_function_t (*GET_FUNCTION)(const LogicalType &, const LogicalType &)>
unique_ptr<FunctionData> CMIntegralDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	function.GetArguments() = deserializer.ReadProperty<vector<LogicalType>>(100, "arguments");
	auto return_type = deserializer.ReadProperty<LogicalType>(101, "return_type");
	function.SetFunctionCallback(GET_FUNCTION(function.GetArguments()[0], return_type));
	return nullptr;
}

ScalarFunctionSet GetIntegralCompressFunctionSet(const LogicalType &result_type) {
	ScalarFunctionSet set {Identifier(IntegralCompressFunctionName(result_type))};
	for (const auto &input_type : LogicalType::Integral()) {
		if (GetTypeIdSize(result_type.InternalType()) < GetTypeIdSize(input_type.InternalType())) {
			set.AddFunction(CMIntegralCompressFun::GetFunction(input_type, result_type));
		}
	}
	return set;
}

ScalarFunctionSet GetIntegralDecompressFunctionSet(const LogicalType &result_type) {
	ScalarFunctionSet set {Identifier(IntegralDecompressFunctionName(result_type))};
	for (const auto &input_type : CMUtils::IntegralTypes()) {
		if (GetTypeIdSize(result_type.InternalType()) > GetTypeIdSize(input_type.InternalType())) {
			set.AddFunction(CMIntegralDecompressFun::GetFunction(input_type, result_type));
		}
	}
	return set;
}

} // namespace

ScalarFunction CMIntegralCompressFun::GetFunction(const LogicalType &input_type, const LogicalType &result_type) {
	ScalarFunction result(Identifier(IntegralCompressFunctionName(result_type)), {input_type, input_type}, result_type,
	                      GetIntegralCompressFunctionInputSwitch(input_type, result_type), CMUtils::Bind);
	result.GetSignature().GetParameter(0).SetName("value");
	result.GetSignature().GetParameter(1).SetName("min");
	result.SetSerializeCallback(CMIntegralSerialize);
	result.SetDeserializeCallback(CMIntegralDeserialize<GetIntegralCompressFunctionInputSwitch>);
#if defined(D_ASSERT_IS_ENABLED)
	result.SetFallible(); // Can only throw runtime error when assertions are enabled
#else
	result.SetErrorMode(FunctionErrors::CANNOT_ERROR);
#endif
	return result;
}

ScalarFunction CMIntegralDecompressFun::GetFunction(const LogicalType &input_type, const LogicalType &result_type) {
	ScalarFunction result(Identifier(IntegralDecompressFunctionName(result_type)), {input_type, result_type},
	                      result_type, GetIntegralDecompressFunctionInputSwitch(input_type, result_type),
	                      CMUtils::Bind);
	result.GetSignature().GetParameter(0).SetName("value");
	result.GetSignature().GetParameter(1).SetName("min");
	result.SetSerializeCallback(CMIntegralSerialize);
	result.SetDeserializeCallback(CMIntegralDeserialize<GetIntegralDecompressFunctionInputSwitch>);
	return result;
}

ScalarFunctionSet InternalCompressIntegralUtinyintFun::GetFunctions() {
	return GetIntegralCompressFunctionSet(LogicalType(LogicalTypeId::UTINYINT));
}

ScalarFunctionSet InternalCompressIntegralUsmallintFun::GetFunctions() {
	return GetIntegralCompressFunctionSet(LogicalType(LogicalTypeId::USMALLINT));
}

ScalarFunctionSet InternalCompressIntegralUintegerFun::GetFunctions() {
	return GetIntegralCompressFunctionSet(LogicalType(LogicalTypeId::UINTEGER));
}

ScalarFunctionSet InternalCompressIntegralUbigintFun::GetFunctions() {
	return GetIntegralCompressFunctionSet(LogicalType(LogicalTypeId::UBIGINT));
}

ScalarFunctionSet InternalDecompressIntegralSmallintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::SMALLINT));
}

ScalarFunctionSet InternalDecompressIntegralIntegerFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::INTEGER));
}

ScalarFunctionSet InternalDecompressIntegralBigintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::BIGINT));
}

ScalarFunctionSet InternalDecompressIntegralHugeintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::HUGEINT));
}

ScalarFunctionSet InternalDecompressIntegralUsmallintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::USMALLINT));
}

ScalarFunctionSet InternalDecompressIntegralUintegerFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::UINTEGER));
}

ScalarFunctionSet InternalDecompressIntegralUbigintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::UBIGINT));
}

ScalarFunctionSet InternalDecompressIntegralUhugeintFun::GetFunctions() {
	return GetIntegralDecompressFunctionSet(LogicalType(LogicalTypeId::UHUGEINT));
}

CMExpressionType CMUtils::GetIntegralType(const BoundScalarFunction &function) {
	if (function.GetDeserializeCallback() == CMIntegralDeserialize<GetIntegralCompressFunctionInputSwitch>) {
		return CMExpressionType::COMPRESS;
	}
	if (function.GetDeserializeCallback() == CMIntegralDeserialize<GetIntegralDecompressFunctionInputSwitch>) {
		return CMExpressionType::DECOMPRESS;
	}
	return CMExpressionType::NONE;
}

} // namespace duckdb
