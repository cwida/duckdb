#include "core_functions/scalar/blob_functions.hpp"
#include "utf8proc_wrapper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"

namespace duckdb {

namespace {

void EncodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// encode is essentially a nop cast from varchar to blob
	// we only need to reinterpret the data using the blob type
	result.Reinterpret(args.data[0]);
}

enum class DecodeErrorBehavior : uint8_t {
	STRICT = 1,  // raise an error
	REPLACE = 2, // replace invalid characters with '?'
	IGNORE = 3   // ignore invalid characters (remove from string)
};

DecodeErrorBehavior GetDecodeErrorBehavior(const string_t &specifier_p) {
	auto size = specifier_p.GetSize();
	auto data = specifier_p.GetData();
	if (StringUtil::CIEquals(data, size, "strict", 6)) {
		return DecodeErrorBehavior::STRICT;
	} else if (StringUtil::CIEquals(data, size, "replace", 7)) {
		return DecodeErrorBehavior::REPLACE;
	} else if (StringUtil::CIEquals(data, size, "ignore", 6)) {
		return DecodeErrorBehavior::IGNORE;
	} else {
		throw ConversionException("decode error behavior specifier \"%s\" not recognized", specifier_p.GetString());
	}
}

struct UnaryBlobDecodeOperator {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		auto input_data = input.GetData();
		auto input_length = input.GetSize();
		if (Utf8Proc::Analyze(input_data, input_length) == UnicodeType::INVALID) {
			throw ConversionException(
			    "Failure in decode: could not convert blob to UTF8 string, the blob "
			    "contained invalid UTF8 characters. \n"
			    "Use try(decode(BLOB)) to return NULL and continue instead of returning an error. "
			    "Specify decode(BLOB, 'replace') to replace invalid characters with '?'. "
			    "Specify decode(BLOB, 'ignore') to remove invalid characters when encountered.");
		}
		return input;
	}
};

void UnaryDecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	UnaryExecutor::Execute<string_t, string_t, UnaryBlobDecodeOperator>(args.data[0], result);
	StringVector::AddHeapReference(result, args.data[0]);
}

void BinaryDecodeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// decode is also a nop cast, but requires verification if the provided string is actually
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t input, string_t error_option) {
		    auto input_data = input.GetData();
		    auto input_length = input.GetSize();

		    if (Utf8Proc::Analyze(input_data, input_length) != UnicodeType::INVALID) {
			    return input;
		    }
		    auto const error_behavior = GetDecodeErrorBehavior(error_option);

		    switch (error_behavior) {
		    case DecodeErrorBehavior::REPLACE: {
			    auto target = StringVector::EmptyString(result, input_length);
			    auto output = target.GetDataWriteable();
			    memcpy(output, input_data, input_length);
			    Utf8Proc::MakeValid(output, input_length);
			    target.Finalize();
			    return target;
		    }

		    case DecodeErrorBehavior::IGNORE: {
			    auto new_str = Utf8Proc::RemoveInvalid(input_data, input_length);
			    auto target = StringVector::EmptyString(result, new_str.size());
			    auto output = target.GetDataWriteable();
			    memcpy(output, new_str.data(), new_str.size()); // NOLINT(bugprone-not-null-terminated-result)
			    target.Finalize();
			    return target;
		    }

		    case DecodeErrorBehavior::STRICT:
			    throw ConversionException(
			        "Failure in decode: could not convert blob to UTF8 string, the blob "
			        "contained invalid UTF8 characters. \n"
			        "Use try(decode(BLOB)) to return NULL and continue instead of returning an error. "
			        "Specify decode(BLOB, 'replace') to replace invalid characters with '?'. "
			        "Specify decode(BLOB, 'ignore') to remove invalid characters when encountered.");

		    default:
			    throw InternalException("Unimplemented decode error behavior");
		    }
	    });
	StringVector::AddHeapReference(result, args.data[0]);
}

} // namespace

ScalarFunction EncodeFun::GetFunction() {
	ScalarFunction func({}, LogicalType::BLOB, EncodeFunction);
	func.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return func;
}

ScalarFunctionSet DecodeFun::GetFunctions() {
	ScalarFunctionSet decode("decode");

	ScalarFunction unary_function({}, LogicalType::VARCHAR, UnaryDecodeFunction);
	unary_function.GetSignature().AddParameter("blob", LogicalType::BLOB);

	ScalarFunction binary_function({}, LogicalType::VARCHAR, BinaryDecodeFunction);
	binary_function.GetSignature()
	    .AddParameter("blob", LogicalType::BLOB)
	    .AddParameter("error_option", LogicalType::VARCHAR);

	decode.AddFunction(unary_function);
	decode.AddFunction(binary_function);
	decode.SetFallible();

	return decode;
}

} // namespace duckdb
