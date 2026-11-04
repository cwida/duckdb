#include "duckdb/common/bswap.hpp"
#include "duckdb/function/scalar/compressed_materialization_functions.hpp"
#include "duckdb/function/scalar/compressed_materialization_utils.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

namespace {

string StringCompressFunctionName(const LogicalType &result_type) {
	return StringUtil::Format("__internal_compress_string_%s",
	                          StringUtil::Lower(LogicalTypeIdToString(result_type.id())));
}

template <idx_t LENGTH>
inline void TemplatedReverseMemCpy(const data_ptr_t &__restrict dest, const const_data_ptr_t &__restrict src) {
	idx_t offset = 0;
	if (LENGTH > sizeof(uint64_t)) {
		for (; offset + sizeof(uint32_t) <= LENGTH; offset += sizeof(uint32_t)) {
			Store<uint32_t>(BSwap(Load<uint32_t>(src + LENGTH - offset - sizeof(uint32_t))), dest + offset);
		}
	}
	for (; offset < LENGTH; offset++) {
		dest[offset] = src[LENGTH - offset - 1];
	}
}

inline void ReverseMemCpy(const data_ptr_t &__restrict dest, const const_data_ptr_t &__restrict src,
                          const idx_t &length) {
	for (idx_t i = 0; i < length; i++) {
		dest[i] = src[length - 1 - i];
	}
}

template <class RESULT_TYPE>
inline RESULT_TYPE StringCompressInternal(const string_t &input) {
	RESULT_TYPE result;
	const auto result_ptr = data_ptr_cast(&result);
	if (sizeof(RESULT_TYPE) <= string_t::INLINE_LENGTH) {
		TemplatedReverseMemCpy<sizeof(RESULT_TYPE)>(result_ptr, const_data_ptr_cast(input.GetPrefix()));
	} else if (input.IsInlined()) {
		static constexpr auto REMAINDER = sizeof(RESULT_TYPE) - string_t::INLINE_LENGTH;
		TemplatedReverseMemCpy<string_t::INLINE_LENGTH>(result_ptr + REMAINDER, const_data_ptr_cast(input.GetPrefix()));
		memset(result_ptr, '\0', REMAINDER);
	} else {
		const auto size = MinValue<idx_t>(sizeof(RESULT_TYPE), input.GetSize());
		const auto remainder = sizeof(RESULT_TYPE) - size;
		ReverseMemCpy(result_ptr + remainder, data_ptr_cast(input.GetPointer()), size);
		memset(result_ptr, '\0', remainder);
	}
	result_ptr[0] = UnsafeNumericCast<data_t>(input.GetSize());
	return BSwapIfBE(result);
}

template <class RESULT_TYPE>
inline RESULT_TYPE StringCompress(const string_t &input) {
	D_ASSERT(input.GetSize() < sizeof(RESULT_TYPE));
	return StringCompressInternal<RESULT_TYPE>(input);
}

template <class RESULT_TYPE>
inline RESULT_TYPE MiniStringCompress(const string_t &input) {
	RESULT_TYPE result;
	if (sizeof(RESULT_TYPE) <= string_t::INLINE_LENGTH) {
		result = UnsafeNumericCast<RESULT_TYPE>(input.GetSize() + *const_data_ptr_cast(input.GetPrefix()));
	} else if (input.GetSize() == 0) {
		result = 0;
	} else {
		result = UnsafeNumericCast<RESULT_TYPE>(input.GetSize() + *const_data_ptr_cast(input.GetPointer()));
	}
	return BSwapIfBE(result);
}

template <>
inline uint8_t StringCompress(const string_t &input) {
	D_ASSERT(input.GetSize() <= sizeof(uint8_t));
	return MiniStringCompress<uint8_t>(input);
}

template <class RESULT_TYPE>
void StringCompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	UnaryExecutor::Execute<string_t, RESULT_TYPE>(
	    args.data[0], result, [](const string_t &input) { return StringCompress<RESULT_TYPE>(input); },
#if defined(D_ASSERT_IS_ENABLED)
	    FunctionErrors::CAN_THROW_RUNTIME_ERROR); // Can only throw a runtime error when assertions are enabled
#else
	    FunctionErrors::CANNOT_ERROR);
#endif
}

template <class RESULT_TYPE>
scalar_function_t GetStringCompressFunction(const LogicalType &result_type) {
	return StringCompressFunction<RESULT_TYPE>;
}

scalar_function_t GetStringCompressFunctionSwitch(const LogicalType &result_type) {
	switch (result_type.id()) {
	case LogicalTypeId::UTINYINT:
		return GetStringCompressFunction<uint8_t>(result_type);
	case LogicalTypeId::USMALLINT:
		return GetStringCompressFunction<uint16_t>(result_type);
	case LogicalTypeId::UINTEGER:
		return GetStringCompressFunction<uint32_t>(result_type);
	case LogicalTypeId::UBIGINT:
		return GetStringCompressFunction<uint64_t>(result_type);
	case LogicalTypeId::UHUGEINT:
		return GetStringCompressFunction<uhugeint_t>(result_type);
	case LogicalTypeId::HUGEINT:
		// Never generated, only for backwards compatibility
		return GetStringCompressFunction<hugeint_t>(result_type);
	default:
		throw InternalException("Unexpected type in GetStringCompressFunctionSwitch");
	}
}

string StringDecompressFunctionName() {
	return "__internal_decompress_string";
}

struct StringDecompressLocalState : public FunctionLocalState {
public:
	explicit StringDecompressLocalState(ClientContext &context) : allocator(Allocator::Get(context)) {
	}

	static unique_ptr<FunctionLocalState> Init(ExpressionState &state, const BoundFunctionExpression &expr,
	                                           FunctionData *bind_data) {
		return make_uniq<StringDecompressLocalState>(state.GetContext());
	}

public:
	ArenaAllocator allocator;
};

template <class INPUT_TYPE>
inline string_t StringDecompress(const INPUT_TYPE &input, ArenaAllocator &allocator) {
	const auto le_input = BSwapIfBE(input);
	const auto le_input_str = const_data_ptr_cast(&le_input);
	string_t result(le_input_str[0]);
	if (sizeof(INPUT_TYPE) <= string_t::INLINE_LENGTH) {
		const auto result_ptr = data_ptr_cast(result.GetPrefixWriteable());
		TemplatedReverseMemCpy<sizeof(INPUT_TYPE)>(result_ptr, le_input_str);
		memset(result_ptr + sizeof(INPUT_TYPE) - 1, '\0', string_t::INLINE_LENGTH - sizeof(INPUT_TYPE) + 1);
	} else if (result.GetSize() <= string_t::INLINE_LENGTH) {
		static constexpr auto REMAINDER = sizeof(INPUT_TYPE) - string_t::INLINE_LENGTH;
		const auto result_ptr = data_ptr_cast(result.GetPrefixWriteable());
		TemplatedReverseMemCpy<string_t::INLINE_LENGTH>(result_ptr, le_input_str + REMAINDER);
	} else {
		result.SetPointer(char_ptr_cast(allocator.Allocate(sizeof(INPUT_TYPE))));
		TemplatedReverseMemCpy<sizeof(INPUT_TYPE)>(data_ptr_cast(result.GetPointer()), le_input_str);
		memcpy(result.GetPrefixWriteable(), result.GetPointer(), string_t::PREFIX_LENGTH);
	}
	return result;
}

template <class INPUT_TYPE>
inline string_t MiniStringDecompress(const INPUT_TYPE &input, ArenaAllocator &allocator) {
	const auto le_input = BSwapIfBE(input);
	if (le_input == 0) {
		string_t result(uint32_t(0));
		memset(result.GetPrefixWriteable(), '\0', string_t::INLINE_BYTES);
		return result;
	}

	string_t result(1);
	if (sizeof(INPUT_TYPE) <= string_t::INLINE_LENGTH) {
		memset(result.GetPrefixWriteable(), '\0', string_t::INLINE_BYTES);
		*data_ptr_cast(result.GetPrefixWriteable()) = le_input - 1;
	} else {
		result.SetPointer(char_ptr_cast(allocator.Allocate(1)));
		*data_ptr_cast(result.GetPointer()) = le_input - 1;
		memset(result.GetPrefixWriteable(), '\0', string_t::PREFIX_LENGTH);
		*result.GetPrefixWriteable() = *result.GetPointer();
	}
	return result;
}

template <>
inline string_t StringDecompress(const uint8_t &input, ArenaAllocator &allocator) {
	return MiniStringDecompress<uint8_t>(input, allocator);
}

template <class INPUT_TYPE>
void StringDecompressFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &allocator = ExecuteFunctionState::GetFunctionState(state)->Cast<StringDecompressLocalState>().allocator;
	allocator.Reset();
	UnaryExecutor::Execute<INPUT_TYPE, string_t>(
	    args.data[0], result, [&](const INPUT_TYPE &input) { return StringDecompress<INPUT_TYPE>(input, allocator); },
	    FunctionErrors::CANNOT_ERROR);
}

template <class INPUT_TYPE>
scalar_function_t GetStringDecompressFunction(const LogicalType &input_type) {
	return StringDecompressFunction<INPUT_TYPE>;
}

scalar_function_t GetStringDecompressFunctionSwitch(const LogicalType &input_type) {
	switch (input_type.id()) {
	case LogicalTypeId::UTINYINT:
		return GetStringDecompressFunction<uint8_t>(input_type);
	case LogicalTypeId::USMALLINT:
		return GetStringDecompressFunction<uint16_t>(input_type);
	case LogicalTypeId::UINTEGER:
		return GetStringDecompressFunction<uint32_t>(input_type);
	case LogicalTypeId::UBIGINT:
		return GetStringDecompressFunction<uint64_t>(input_type);
	case LogicalTypeId::UHUGEINT:
		return GetStringDecompressFunction<uhugeint_t>(input_type);
	case LogicalTypeId::HUGEINT:
		return GetStringDecompressFunction<hugeint_t>(input_type);
	default:
		throw InternalException("Unexpected type in GetStringDecompressFunctionSwitch");
	}
}

void CMStringCompressSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                               const BoundScalarFunction &function) {
	serializer.WriteProperty(100, "arguments", function.GetArguments());
	serializer.WriteProperty(101, "return_type", function.GetReturnType());
}

unique_ptr<FunctionData> CMStringCompressDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	function.GetArguments() = deserializer.ReadProperty<vector<LogicalType>>(100, "arguments");
	auto return_type = deserializer.ReadProperty<LogicalType>(101, "return_type");
	function.SetFunctionCallback(GetStringCompressFunctionSwitch(return_type));
	return nullptr;
}

void CMStringDecompressSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                 const BoundScalarFunction &function) {
	serializer.WriteProperty(100, "arguments", function.GetArguments());
}

unique_ptr<FunctionData> CMStringDecompressDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	function.GetArguments() = deserializer.ReadProperty<vector<LogicalType>>(100, "arguments");
	function.SetFunctionCallback(GetStringDecompressFunctionSwitch(function.GetArguments()[0]));
	function.SetReturnType(deserializer.Get<const LogicalType &>());
	return nullptr;
}

ScalarFunctionSet GetStringDecompressFunctionSet() {
	ScalarFunctionSet set {Identifier(StringDecompressFunctionName())};
	auto string_types = CMUtils::StringTypes();
	// For backwards compatibility, see internal issue 5306
	string_types.push_back(LogicalType::HUGEINT);
	for (const auto &input_type : string_types) {
		set.AddFunction(CMStringDecompressFun::GetFunction(input_type));
	}
	return set;
}

} // namespace

ScalarFunction CMStringCompressFun::GetFunction(const LogicalType &result_type) {
	ScalarFunction result(Identifier(StringCompressFunctionName(result_type)), {LogicalType::VARCHAR}, result_type,
	                      GetStringCompressFunctionSwitch(result_type), CMUtils::Bind);
	result.GetSignature().GetParameter(0).SetName("value");
	result.SetSerializeCallback(CMStringCompressSerialize);
	result.SetDeserializeCallback(CMStringCompressDeserialize);
#if defined(D_ASSERT_IS_ENABLED)
	result.SetFallible(); // Can only throw runtime error when assertions are enabled
#else
	result.SetErrorMode(FunctionErrors::CANNOT_ERROR);
#endif
	return result;
}

ScalarFunction CMStringDecompressFun::GetFunction(const LogicalType &input_type) {
	ScalarFunction result(Identifier(StringDecompressFunctionName()), {input_type}, LogicalType::VARCHAR,
	                      GetStringDecompressFunctionSwitch(input_type), CMUtils::Bind, nullptr,
	                      StringDecompressLocalState::Init);
	result.GetSignature().GetParameter(0).SetName("value");
	result.SetSerializeCallback(CMStringDecompressSerialize);
	result.SetDeserializeCallback(CMStringDecompressDeserialize);
	return result;
}

ScalarFunction InternalCompressStringUtinyintFun::GetFunction() {
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::UTINYINT));
}

ScalarFunction InternalCompressStringUsmallintFun::GetFunction() {
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::USMALLINT));
}

ScalarFunction InternalCompressStringUintegerFun::GetFunction() {
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::UINTEGER));
}

ScalarFunction InternalCompressStringUbigintFun::GetFunction() {
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::UBIGINT));
}

ScalarFunction InternalCompressStringHugeintFun::GetFunction() {
	// We never generate this, but it's needed for backwards compatibility
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::HUGEINT));
}

ScalarFunction InternalCompressStringUhugeintFun::GetFunction() {
	return CMStringCompressFun::GetFunction(LogicalType(LogicalTypeId::UHUGEINT));
}

ScalarFunctionSet InternalDecompressStringFun::GetFunctions() {
	return GetStringDecompressFunctionSet();
}

CMExpressionType CMUtils::GetStringType(const BoundScalarFunction &function) {
	if (function.GetDeserializeCallback() == CMStringCompressDeserialize) {
		return CMExpressionType::COMPRESS;
	}
	if (function.GetDeserializeCallback() == CMStringDecompressDeserialize) {
		return CMExpressionType::DECOMPRESS;
	}
	return CMExpressionType::NONE;
}

} // namespace duckdb
