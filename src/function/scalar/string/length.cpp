#include "duckdb/common/exception.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/types/bit.hpp"
#include "duckdb/common/vector/vector_iterator.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "utf8proc.hpp"

namespace duckdb {

namespace {

// length returns the number of unicode codepoints
struct StringLengthOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return Length<TA, TR>(input);
	}
};

struct GraphemeCountOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return GraphemeCount<TA, TR>(input);
	}
};

// strlen returns the size in bytes
struct StrLenOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return UnsafeNumericCast<TR>(input.GetSize());
	}
};

struct OctetLenOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return UnsafeNumericCast<TR>(Bit::OctetLength(input));
	}
};

// bitlen returns the size in bits
struct BitLenOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return UnsafeNumericCast<TR>(8 * input.GetSize());
	}
};

// bitstringlen returns the amount of bits in a bitstring
struct BitStringLenOperator {
	template <class TA, class TR>
	static inline TR Operation(TA input) {
		return UnsafeNumericCast<TR>(Bit::BitLength(input));
	}
};

template <bool COUNTS_CODEPOINTS>
unique_ptr<BaseStatistics> LengthPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 1);
	const bool can_contain_unicode = StringStats::CanContainUnicode(child_stats[0]);
	if (!can_contain_unicode) {
		expr.FunctionMutable().SetFunctionCallback(ScalarFunction::UnaryFunction<string_t, int64_t, StrLenOperator>);
	}
	if (!StringStats::HasMaxStringLength(child_stats[0])) {
		return nullptr;
	}

	// String stats are stored as byte length, so we need to convert to character length
	const auto max_length = NumericCast<int64_t>(StringStats::MaxStringLength(child_stats[0]));
	int64_t min_length = 0;
	auto min_string_length = StringStats::MinStringLength(child_stats[0]);
	if (min_string_length.IsValid()) {
		if (!can_contain_unicode) {
			// ASCII-only: the character count is exactly the byte count
			min_length = NumericCast<int64_t>(min_string_length.GetIndex());
		} else if (min_string_length.GetIndex() > 0) {
			if (COUNTS_CODEPOINTS) {
				// every codepoint takes at most four bytes
				min_length = NumericCast<int64_t>((min_string_length.GetIndex() + 3) / 4);
			} else {
				// a grapheme cluster can span arbitrarily many codepoints - no better bound exists
				min_length = 1;
			}
		}
	}
	auto result = NumericStats::CreateEmpty(expr.GetReturnType());
	NumericStats::SetMin(result, Value::BIGINT(min_length));
	NumericStats::SetMax(result, Value::BIGINT(max_length));
	result.CopyValidity(child_stats[0]);
	return result.ToUnique();
}

unique_ptr<BaseStatistics> StringSizePropagateStats(FunctionStatisticsInput &input, int64_t multiplier) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	D_ASSERT(child_stats.size() == 1);
	if (!StringStats::HasMaxStringLength(child_stats[0])) {
		return nullptr;
	}

	int64_t min_length = 0;
	const auto min_string_length = StringStats::MinStringLength(child_stats[0]);
	if (min_string_length.IsValid()) {
		min_length = NumericCast<int64_t>(min_string_length.GetIndex()) * multiplier;
	}
	const auto max_length = NumericCast<int64_t>(StringStats::MaxStringLength(child_stats[0])) * multiplier;
	auto result = NumericStats::CreateEmpty(expr.GetReturnType());
	NumericStats::SetMin(result, Value::BIGINT(min_length));
	NumericStats::SetMax(result, Value::BIGINT(max_length));
	result.CopyValidity(child_stats[0]);
	return result.ToUnique();
}

unique_ptr<BaseStatistics> ByteLengthPropagateStats(ClientContext &, FunctionStatisticsInput &input) {
	return StringSizePropagateStats(input, /*multiplier=*/1);
}

unique_ptr<BaseStatistics> BitLengthPropagateStats(ClientContext &, FunctionStatisticsInput &input) {
	return StringSizePropagateStats(input, /*multiplier=*/8);
}

//------------------------------------------------------------------
// ARRAY / LIST LENGTH
//------------------------------------------------------------------
void ListLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input = args.data[0];
	D_ASSERT(input.GetType().id() == LogicalTypeId::LIST);
	UnaryExecutor::Execute<list_entry_t, int64_t>(
	    input, result, [](list_entry_t input) { return UnsafeNumericCast<int64_t>(input.length); });
}

void ArrayLengthFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input = args.data[0];
	auto array_size = static_cast<int64_t>(ArrayType::GetSize(input.GetType()));
	auto validity_entries = input.Validity();

	if (!validity_entries.CanHaveNull()) {
		// for arrays the length is constant
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::GetData<int64_t>(result)[0] = array_size;
		FlatVector::SetSize(result, args.size());
		return;
	}
	// we need to inherit the null values of the parent
	auto result_data = FlatVector::Writer<int64_t>(result, args.size());
	for (idx_t r = 0; r < args.size(); r++) {
		if (!validity_entries.IsValid(r)) {
			result_data.WriteNull();
		} else {
			result_data.WriteValue(array_size);
		}
	}
}

void ArrayOrListLengthResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &argument = input.GetArgument(0);
	if (argument.HasParameter() || argument.GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}

	const auto &arg_type = argument.GetReturnType().id();
	if (arg_type == LogicalTypeId::ARRAY) {
		bound_function.SetFunctionCallback(ArrayLengthFunction);
	} else if (arg_type == LogicalTypeId::LIST) {
		bound_function.SetFunctionCallback(ListLengthFunction);
	} else {
		// Unreachable
		throw BinderException("length can only be used on arrays or lists");
	}
	bound_function.GetArguments()[0] = argument.GetReturnType();
}

//------------------------------------------------------------------
// ARRAY / LIST WITH DIMENSION
//------------------------------------------------------------------
void ListLengthBinaryFunction(DataChunk &args, ExpressionState &, Vector &result) {
	auto type = args.data[0].GetType();
	const auto &input = args.data[0];
	const auto &dimension = args.data[1];
	BinaryExecutor::Execute<list_entry_t, int64_t, int64_t>(
	    input, dimension, result, [](list_entry_t input, int64_t dimension) {
		    if (dimension != 1) {
			    throw NotImplementedException("array_length for lists with dimensions other than 1 not implemented");
		    }
		    return UnsafeNumericCast<int64_t>(input.length);
	    });
}

struct ArrayLengthBinaryFunctionData : public FunctionData {
	vector<int64_t> dimensions;

	unique_ptr<FunctionData> Copy() const override {
		auto copy = make_uniq<ArrayLengthBinaryFunctionData>();
		copy->dimensions = dimensions;
		return std::move(copy);
	}

	bool Equals(const FunctionData &other) const override {
		auto &other_data = other.Cast<const ArrayLengthBinaryFunctionData>();
		return dimensions == other_data.dimensions;
	}
};

void ArrayLengthBinaryFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto type = args.data[0].GetType();
	const auto &dimension = args.data[1];

	auto &expr = state.expr.Cast<BoundFunctionExpression>();
	auto &data = expr.BindInfo()->Cast<ArrayLengthBinaryFunctionData>();
	auto &dimensions = data.dimensions;
	auto max_dimension = static_cast<int64_t>(dimensions.size());

	// the result is NULL for NULL arrays
	const auto count = args.size();
	auto array_validity = args.data[0].Validity();
	auto dimension_values = dimension.Values<int64_t>();
	auto writer = FlatVector::Writer<int64_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto entry = dimension_values[i];
		if (!array_validity.IsValid(i) || !entry.IsValid()) {
			writer.WriteNull();
			continue;
		}
		auto dim = entry.GetValue();
		if (dim < 1 || dim > max_dimension) {
			throw OutOfRangeException(StringUtil::Format(
			    "array_length dimension '%lld' out of range (min: '1', max: '%lld')", dim, max_dimension));
		}
		writer.WriteValue(dimensions[UnsafeNumericCast<idx_t>(dim - 1)]);
	}
}

void ArrayOrListLengthBinaryResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &argument = input.GetArgument(0);
	if (argument.HasParameter() || argument.GetReturnType().id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}
	auto &type = argument.GetReturnType();
	if (type.id() == LogicalTypeId::ARRAY) {
		bound_function.SetFunctionCallback(ArrayLengthBinaryFunction);
	} else if (type.id() == LogicalTypeId::LIST) {
		bound_function.SetFunctionCallback(ListLengthBinaryFunction);
	} else {
		// Unreachable
		throw BinderException("array_length can only be used on arrays or lists");
	}
	bound_function.GetArguments()[0] = type;
}

unique_ptr<FunctionData> ArrayOrListLengthBinaryBind(BindScalarFunctionInput &input) {
	auto type = input.GetBoundFunction().GetArguments()[0];
	if (type.id() == LogicalTypeId::ARRAY) {
		// If the input is an array, the dimensions are constant, so we can calculate them at bind time
		vector<int64_t> dimensions;
		while (true) {
			if (type.id() == LogicalTypeId::ARRAY) {
				dimensions.push_back(UnsafeNumericCast<int64_t>(ArrayType::GetSize(type)));
				type = ArrayType::GetChildType(type);
			} else {
				break;
			}
		}
		auto data = make_uniq<ArrayLengthBinaryFunctionData>();
		data->dimensions = dimensions;
		return std::move(data);
	}
	return nullptr;
}

} // namespace

ScalarFunctionSet LengthFun::GetFunctions() {
	ScalarFunctionSet length("length");

	ScalarFunction string_fun({}, LogicalType::BIGINT,
	                          ScalarFunction::UnaryFunction<string_t, int64_t, StringLengthOperator>, nullptr,
	                          LengthPropagateStats<true>);
	string_fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	length.AddFunction(string_fun);

	ScalarFunction bit_fun({}, LogicalType::BIGINT,
	                       ScalarFunction::UnaryFunction<string_t, int64_t, BitStringLenOperator>);
	bit_fun.GetSignature().AddParameter("bit", LogicalType::BIT);
	length.AddFunction(bit_fun);

	ScalarFunction list_fun({}, LogicalType::BIGINT, nullptr);
	list_fun.SetResolveTypesCallback(ArrayOrListLengthResolveTypes);
	list_fun.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));
	length.AddFunction(list_fun);

	return (length);
}

ScalarFunctionSet LengthGraphemeFun::GetFunctions() {
	ScalarFunctionSet length_grapheme("length_grapheme");
	ScalarFunction fun({}, LogicalType::BIGINT, ScalarFunction::UnaryFunction<string_t, int64_t, GraphemeCountOperator>,
	                   nullptr, LengthPropagateStats<false>);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	length_grapheme.AddFunction(fun);
	return (length_grapheme);
}

ScalarFunctionSet ArrayLengthFun::GetFunctions() {
	ScalarFunctionSet array_length("array_length");

	ScalarFunction unary({}, LogicalType::BIGINT, nullptr);
	unary.SetResolveTypesCallback(ArrayOrListLengthResolveTypes);
	unary.GetSignature().AddParameter("list", LogicalType::LIST(LogicalType::ANY));
	array_length.AddFunction(unary);

	ScalarFunction binary({}, LogicalType::BIGINT, nullptr, ArrayOrListLengthBinaryBind);
	binary.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("dimension", LogicalType::BIGINT);
	binary.SetResolveTypesCallback(ArrayOrListLengthBinaryResolveTypes);
	array_length.AddFunction(binary);

	array_length.SetFallible();
	return (array_length);
}

ScalarFunction StrlenFun::GetFunction() {
	ScalarFunction fun("strlen", {}, LogicalType::BIGINT,
	                   ScalarFunction::UnaryFunction<string_t, int64_t, StrLenOperator>, nullptr,
	                   ByteLengthPropagateStats);
	fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	return fun;
}

ScalarFunctionSet BitLengthFun::GetFunctions() {
	ScalarFunctionSet bit_length("bit_length");

	ScalarFunction string_fun({}, LogicalType::BIGINT, ScalarFunction::UnaryFunction<string_t, int64_t, BitLenOperator>,
	                          nullptr, BitLengthPropagateStats);
	string_fun.GetSignature().AddParameter("string", LogicalType::VARCHAR);
	bit_length.AddFunction(string_fun);

	ScalarFunction bit_fun({}, LogicalType::BIGINT,
	                       ScalarFunction::UnaryFunction<string_t, int64_t, BitStringLenOperator>);
	bit_fun.GetSignature().AddParameter("bit", LogicalType::BIT);
	bit_length.AddFunction(bit_fun);

	return (bit_length);
}

ScalarFunctionSet OctetLengthFun::GetFunctions() {
	// length for BLOB type
	ScalarFunctionSet octet_length("octet_length");

	ScalarFunction blob_fun({}, LogicalType::BIGINT, ScalarFunction::UnaryFunction<string_t, int64_t, StrLenOperator>,
	                        nullptr, ByteLengthPropagateStats);
	blob_fun.GetSignature().AddParameter("blob", LogicalType::BLOB);
	octet_length.AddFunction(blob_fun);

	ScalarFunction bitstring_fun({}, LogicalType::BIGINT,
	                             ScalarFunction::UnaryFunction<string_t, int64_t, OctetLenOperator>);
	bitstring_fun.GetSignature().AddParameter("bitstring", LogicalType::BIT);
	octet_length.AddFunction(bitstring_fun);

	return (octet_length);
}

} // namespace duckdb
