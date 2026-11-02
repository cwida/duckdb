#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/types/blob.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "utf8proc.hpp"

namespace duckdb {

static const int64_t SUPPORTED_UPPER_BOUND = NumericLimits<uint32_t>::Maximum();
static const int64_t SUPPORTED_LOWER_BOUND = -SUPPORTED_UPPER_BOUND - 1;

static inline void AssertInSupportedRange(idx_t input_size, int64_t offset, int64_t length) {
	if (input_size > (uint64_t)SUPPORTED_UPPER_BOUND) {
		throw OutOfRangeException("Substring input size is too large (> %d)", SUPPORTED_UPPER_BOUND);
	}
	if (offset < SUPPORTED_LOWER_BOUND) {
		throw OutOfRangeException("Substring offset outside of supported range (< %d)", SUPPORTED_LOWER_BOUND);
	}
	if (offset > SUPPORTED_UPPER_BOUND) {
		throw OutOfRangeException("Substring offset outside of supported range (> %d)", SUPPORTED_UPPER_BOUND);
	}
	if (length < SUPPORTED_LOWER_BOUND) {
		throw OutOfRangeException("Substring length outside of supported range (< %d)", SUPPORTED_LOWER_BOUND);
	}
	if (length > SUPPORTED_UPPER_BOUND) {
		throw OutOfRangeException("Substring length outside of supported range (> %d)", SUPPORTED_UPPER_BOUND);
	}
}

bool SubstringInSupportedRange(int64_t offset, int64_t length) {
	return offset >= SUPPORTED_LOWER_BOUND && offset <= SUPPORTED_UPPER_BOUND && length >= SUPPORTED_LOWER_BOUND &&
	       length <= SUPPORTED_UPPER_BOUND;
}

static string_t SubstringEmptyString(Vector &result) {
	auto result_string = StringVector::EmptyString(result, 0);
	result_string.Finalize();
	return result_string;
}

static string_t SubstringSlice(Vector &result, const char *input_data, int64_t offset, int64_t length) {
	auto result_string = StringVector::EmptyString(result, UnsafeNumericCast<idx_t>(length));
	auto result_data = result_string.GetDataWriteable();
	memcpy(result_data, input_data + offset, UnsafeNumericCast<size_t>(length));
	result_string.Finalize();
	return result_string;
}

// compute start and end characters from the given input size and offset/length
static bool SubstringStartEnd(int64_t offset, int64_t length, int64_t &start, int64_t &end) {
	if (length == 0) {
		return false;
	}
	if (offset > 0) {
		// positive offset: scan from start
		start = offset - 1;
	} else if (offset < 0) {
		// negative offset: scan from end (i.e. start = end + offset)
		start = offset;
	} else {
		// offset = 0: special case, we start 1 character BEHIND the first character
		start = 0;
		length--;
		if (length <= 0) {
			return false;
		}
	}
	if (length > 0) {
		// positive length: go forward (i.e. end = start + offset)
		end = start + length;
	} else {
		// negative length: go backwards (i.e. end = start, start = start + length)
		end = start;
		start += length;
	}
	return true;
}

static bool SubstringASCIIBounds(int64_t input_size, int64_t offset, int64_t length, int64_t &start, int64_t &end) {
	if (!SubstringStartEnd(offset, length, start, end)) {
		return false;
	}
	// Clamp start and end to string bounds
	auto cast_input_size = UnsafeNumericCast<int64_t>(input_size);
	if (offset < 0) {
		start += cast_input_size;
		end += cast_input_size;
	}
	start = MaxValue<int64_t>(0, MinValue<int64_t>(cast_input_size, start));
	end = MaxValue<int64_t>(0, MinValue<int64_t>(cast_input_size, end));
	return start < end;
}

string_t SubstringASCII(Vector &result, string_t input, int64_t offset, int64_t length) {
	auto input_data = input.GetData();
	auto input_size = input.GetSize();

	AssertInSupportedRange(input_size, offset, length);

	int64_t start, end;
	if (!SubstringASCIIBounds(UnsafeNumericCast<int64_t>(input_size), offset, length, start, end)) {
		return SubstringEmptyString(result);
	}
	return SubstringSlice(result, input_data, start, UnsafeNumericCast<int64_t>(end - start));
}

string_t SubstringUnicode(Vector &result, string_t input, int64_t offset, int64_t length) {
	auto input_data = input.GetData();
	auto input_size = input.GetSize();

	AssertInSupportedRange(input_size, offset, length);

	if (offset > 0) {
		// the result only depends on the first offset-1+max(length,0) code points; if those bytes are
		// ASCII, code points and bytes coincide and we can slice directly
		const auto needed =
		    MinValue<int64_t>(UnsafeNumericCast<int64_t>(input_size), offset - 1 + MaxValue<int64_t>(length, 0));
		if (IsAscii(input_data, UnsafeNumericCast<idx_t>(needed))) {
			return SubstringASCII(result, input, offset, length);
		}
	}

	if (length == 0) {
		return SubstringEmptyString(result);
	}
	// first figure out which direction we need to scan
	idx_t start_pos;
	idx_t end_pos;
	// negative offset: scan backwards
	int64_t start, end;
	if (!SubstringStartEnd(offset, length, start, end)) {
		return SubstringEmptyString(result);
	}
	if (offset < 0) {
		start_pos = 0;
		end_pos = DConstants::INVALID_INDEX;

		// we express start and end as unicode codepoints from the back
		start = -start + 1;
		end = -end + 1;
		if (end <= 0) {
			end_pos = input_size;
		}
		int64_t current_character = 0;
		for (idx_t i = input_size; i > 0; i--) {
			if (IsCharacter(input_data[i - 1])) {
				current_character++;
				if (current_character == start) {
					start_pos = i;
					break;
				} else if (current_character == end) {
					end_pos = i;
				}
			}
		}
		while (start_pos < input_size && !IsCharacter(input_data[start_pos])) {
			start_pos++;
		}
		while (end_pos < input_size && !IsCharacter(input_data[end_pos])) {
			end_pos++;
		}

		if (end_pos == DConstants::INVALID_INDEX) {
			return SubstringEmptyString(result);
		}
	} else {
		start_pos = DConstants::INVALID_INDEX;
		end_pos = input_size;

		// we express start and end as unicode codepoints from the front
		start = MaxValue<int64_t>(0, start);

		int64_t current_character = 0;
		for (idx_t i = 0; i < input_size; i++) {
			if (IsCharacter(input_data[i])) {
				if (current_character == start) {
					start_pos = i;
				} else if (current_character == end) {
					end_pos = i;
					break;
				}
				current_character++;
			}
		}
		if (start_pos == DConstants::INVALID_INDEX || end == 0 || end <= start) {
			return SubstringEmptyString(result);
		}
	}
	D_ASSERT(end_pos >= start_pos);
	// after we have found these, we can slice the substring
	return SubstringSlice(result, input_data, UnsafeNumericCast<int64_t>(start_pos),
	                      UnsafeNumericCast<int64_t>(end_pos - start_pos));
}

string_t SubstringGrapheme(Vector &result, string_t input, int64_t offset, int64_t length) {
	auto input_data = input.GetData();
	auto input_size = input.GetSize();

	AssertInSupportedRange(input_size, offset, length);

	// we don't know yet if the substring is ascii, but we assume it is (for now)
	// first get the start and end as if this was an ascii string
	int64_t start, end;
	if (!SubstringASCIIBounds(UnsafeNumericCast<int64_t>(input_size), offset, length, start, end)) {
		return SubstringEmptyString(result);
	}

	// now check if all the characters between 0 and end are ascii characters
	// note that we scan one further to check for a potential combining diacritics (e.g. i + diacritic is ï)
	bool is_ascii = true;
	idx_t ascii_end = MinValue<idx_t>(UnsafeNumericCast<idx_t>(end + 1), input_size);
	for (idx_t i = 0; i < ascii_end; i++) {
		if (input_data[i] & 0x80) {
			// found a non-ascii character: eek
			is_ascii = false;
			break;
		}
	}
	if (is_ascii) {
		// all characters are ascii, we can just slice the substring
		return SubstringSlice(result, input_data, start, end - start);
	}
	// if the characters are not ascii, we need to scan grapheme clusters
	// first figure out which direction we need to scan
	// offset = 0 case is taken care of in SubstringStartEnd
	if (offset < 0) {
		// negative offset, this case is more difficult
		// we first need to count the number of characters in the string
		idx_t num_characters = Utf8Proc::GraphemeCount(input_data, input_size);
		// now call substring start and end again, but with the number of unicode characters this time
		if (!SubstringASCIIBounds(UnsafeNumericCast<int64_t>(num_characters), offset, length, start, end)) {
			return SubstringEmptyString(result);
		}
	}

	// now scan the graphemes of the string to find the positions of the start and end characters
	int64_t current_character = 0;
	idx_t start_pos = DConstants::INVALID_INDEX, end_pos = input_size;
	for (auto cluster : Utf8Proc::GraphemeClusters(input_data, input_size)) {
		if (current_character == start) {
			start_pos = cluster.start;
		} else if (current_character == end) {
			end_pos = cluster.start;
			break;
		}
		current_character++;
	}
	if (start_pos == DConstants::INVALID_INDEX) {
		return SubstringEmptyString(result);
	}
	// after we have found these, we can slice the substring
	return SubstringSlice(result, input_data, UnsafeNumericCast<int64_t>(start_pos),
	                      UnsafeNumericCast<int64_t>(end_pos - start_pos));
}

namespace {

struct SubstringUnicodeOp {
	static string_t Substring(Vector &result, string_t input, int64_t offset, int64_t length) {
		return SubstringUnicode(result, input, offset, length);
	}
};

struct SubstringGraphemeOp {
	static string_t Substring(Vector &result, string_t input, int64_t offset, int64_t length) {
		return SubstringGrapheme(result, input, offset, length);
	}
};

template <class OP>
void SubstringFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input_vector = args.data[0];
	const auto &offset_vector = args.data[1];
	if (args.ColumnCount() == 3) {
		const auto &length_vector = args.data[2];

		TernaryExecutor::Execute<string_t, int64_t, int64_t, string_t>(
		    input_vector, offset_vector, length_vector, result,
		    [&](string_t input_string, int64_t offset, int64_t length) {
			    return OP::Substring(result, input_string, offset, length);
		    });
	} else {
		BinaryExecutor::Execute<string_t, int64_t, string_t>(
		    input_vector, offset_vector, result, [&](string_t input_string, int64_t offset) {
			    return OP::Substring(result, input_string, offset, NumericLimits<uint32_t>::Maximum());
		    });
	}
}

void SubstringFunctionASCII(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &input_vector = args.data[0];
	const auto &offset_vector = args.data[1];
	if (args.ColumnCount() == 3) {
		const auto &length_vector = args.data[2];

		TernaryExecutor::Execute<string_t, int64_t, int64_t, string_t>(
		    input_vector, offset_vector, length_vector, result,
		    [&](string_t input_string, int64_t offset, int64_t length) {
			    return SubstringASCII(result, input_string, offset, length);
		    });
	} else {
		BinaryExecutor::Execute<string_t, int64_t, string_t>(
		    input_vector, offset_vector, result, [&](string_t input_string, int64_t offset) {
			    return SubstringASCII(result, input_string, offset, NumericLimits<uint32_t>::Maximum());
		    });
	}
}

struct SubstringStatsParameters {
	// 0-based index of the first character of the substring.
	idx_t start_character_index = 0;
	optional_idx character_count;
};

void PropagateStrictFunctionValidity(BaseStatistics &result, const vector<BaseStatistics> &child_stats) {
	D_ASSERT(!child_stats.empty());
	result.CopyValidity(child_stats[0]);
	for (idx_t child_idx = 1; child_idx < child_stats.size(); child_idx++) {
		if (child_stats[child_idx].CanHaveNull()) {
			result.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
		}
		if (!child_stats[child_idx].CanHaveNoNull()) {
			result.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
		}
	}
}

unique_ptr<FunctionData> SubstringBind(BindScalarFunctionInput &input) {
	auto offset = input.TryGetConstant(1);
	if (!offset || offset->IsNull()) {
		return nullptr;
	}
	auto offset_value = offset->DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
	int64_t length_value = NumericLimits<uint32_t>::Maximum();
	if (input.GetArguments().size() == 3) {
		auto length = input.TryGetConstant(2);
		if (!length || length->IsNull()) {
			return nullptr;
		}
		length_value = length->DefaultCastAs(LogicalType::BIGINT).GetValue<int64_t>();
	}
	if (SubstringInSupportedRange(offset_value, length_value)) {
		input.GetBoundFunction().SetErrorMode(FunctionErrors::CANNOT_ERROR);
	}
	return nullptr;
}

bool TryGetConstantInt64(const Expression &expression, int64_t &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expression.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return false;
	}
	result = value.GetValue<int64_t>();
	return true;
}

bool TryGetSubstringStatsParameters(BoundFunctionExpression &expr, SubstringStatsParameters &result) {
	auto &children = expr.GetChildren();
	if (children.size() != 2 && children.size() != 3) {
		return false;
	}

	int64_t offset = 0;
	if (!TryGetConstantInt64(*children[1], offset)) {
		return false;
	}
	if (children.size() == 3) {
		int64_t length = 0;
		if (!TryGetConstantInt64(*children[2], length) || length <= 0 || length > NumericLimits<uint32_t>::Maximum()) {
			return false;
		}
		result.character_count = NumericCast<idx_t>(length);
	}
	// Normalize start offset to 1-based index.
	if (offset == 0) {
		if (!result.character_count.IsValid() || result.character_count.GetIndex() <= 1) {
			return false;
		}
		offset = 1;
		result.character_count = result.character_count.GetIndex() - 1;
	}
	// Reject negative offset.
	if (offset < 1) {
		return false;
	}
	result.start_character_index = NumericCast<idx_t>(offset - 1);
	return true;
}

unique_ptr<BaseStatistics> SubstringStatsFromSharedPrefix(FunctionStatisticsInput &input) {
	auto &expr = input.expr;
	SubstringStatsParameters parameters;
	auto has_parameters = TryGetSubstringStatsParameters(expr, parameters);
	unique_ptr<BaseStatistics> result;
	if (has_parameters) {
		result = PropagateStringSliceStats(input, parameters.start_character_index, parameters.character_count);
	}

	optional_idx max_output_length;
	if (has_parameters && parameters.character_count.IsValid()) {
		idx_t bytes_per_character = StringStats::CanContainUnicode(input.child_stats[0]) ? 4 : 1;
		auto character_count = parameters.character_count.GetIndex();
		if (character_count <= NumericLimits<uint32_t>::Maximum() / bytes_per_character) {
			max_output_length = character_count * bytes_per_character;
		}
	}
	if (StringStats::HasMaxStringLength(input.child_stats[0])) {
		auto input_max = StringStats::MaxStringLength(input.child_stats[0]);
		max_output_length = max_output_length.IsValid() ? MinValue<idx_t>(max_output_length.GetIndex(), input_max)
		                                                : optional_idx(input_max);
	}
	if (!result && max_output_length.IsValid()) {
		result = StringStats::CreateUnknown(input.expr.GetReturnType()).ToUnique();
	}
	if (max_output_length.IsValid()) {
		StringStats::SetMaxStringLength(*result, NumericCast<uint32_t>(max_output_length.GetIndex()));
	}
	if (result) {
		PropagateStrictFunctionValidity(*result, input.child_stats);
	}
	return result;
}

unique_ptr<BaseStatistics> SubstringPropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	// can only propagate stats if the children have stats
	// we only care about the stats of the first child (i.e. the string)
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(SubstringFunctionASCII);
	}
	return SubstringStatsFromSharedPrefix(input);
}

unique_ptr<BaseStatistics> SubstringGraphemePropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	if (!StringStats::CanContainUnicode(child_stats[0])) {
		expr.FunctionMutable().SetFunctionCallback(SubstringFunctionASCII);
	}
	if (!StringStats::HasMaxStringLength(input.child_stats[0])) {
		return nullptr;
	}
	auto result = StringStats::CreateUnknown(expr.GetReturnType());
	StringStats::SetMaxStringLength(result, StringStats::MaxStringLength(input.child_stats[0]));
	PropagateStrictFunctionValidity(result, input.child_stats);
	return result.ToUnique();
}

} // namespace

ScalarFunctionSet SubstringFun::GetFunctions() {
	ScalarFunctionSet substr("substring");

	ScalarFunction three_arg({}, LogicalType::VARCHAR, SubstringFunction<SubstringUnicodeOp>, SubstringBind,
	                         SubstringPropagateStats);
	three_arg.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("start", LogicalType::BIGINT)
	    .AddParameter("length", LogicalType::BIGINT);
	substr.AddFunction(three_arg);

	ScalarFunction two_arg({}, LogicalType::VARCHAR, SubstringFunction<SubstringUnicodeOp>, SubstringBind,
	                       SubstringPropagateStats);
	two_arg.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("start", LogicalType::BIGINT);
	substr.AddFunction(two_arg);

	// throws if the offset or length are out of the supported range
	substr.SetFallible();
	return (substr);
}

ScalarFunctionSet SubstringGraphemeFun::GetFunctions() {
	ScalarFunctionSet substr_grapheme("substring_grapheme");

	ScalarFunction three_arg({}, LogicalType::VARCHAR, SubstringFunction<SubstringGraphemeOp>, SubstringBind,
	                         SubstringGraphemePropagateStats);
	three_arg.GetSignature()
	    .AddParameter("string", LogicalType::VARCHAR)
	    .AddParameter("start", LogicalType::BIGINT)
	    .AddParameter("length", LogicalType::BIGINT);
	substr_grapheme.AddFunction(three_arg);

	ScalarFunction two_arg({}, LogicalType::VARCHAR, SubstringFunction<SubstringGraphemeOp>, SubstringBind,
	                       SubstringGraphemePropagateStats);
	two_arg.GetSignature().AddParameter("string", LogicalType::VARCHAR).AddParameter("start", LogicalType::BIGINT);
	substr_grapheme.AddFunction(two_arg);

	// throws if the offset or length are out of the supported range
	substr_grapheme.SetFallible();
	return (substr_grapheme);
}

} // namespace duckdb
