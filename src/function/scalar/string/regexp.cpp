#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/scalar/regexp.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "re2/stringpiece.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

using regexp_util::CreateStringPiece;
using regexp_util::ParseRegexOptions;
using regexp_util::TryParseConstantPattern;
using regexp_util::VerifyUTF8Result;

// Zero-copy slice of capture group `group_index` from `input`'s buffer; caller must call
// StringVector::AddHeapReference(result, source) first.
static inline string_t ExtractCaptureGroup(const string_t &input, const RE2 &re, int8_t group_index,
                                           bool no_match_returns_input) {
	if (group_index < 0 || group_index > re.NumberOfCapturingGroups()) {
		return no_match_returns_input ? input : string_t(nullptr, 0);
	}
	duckdb_re2::StringPiece in_piece(input.GetData(), input.GetSize());
	duckdb_re2::StringPiece submatch[10];
	const int nsubmatch = group_index + 1;
	if (re.Match(in_piece, 0, in_piece.size(), duckdb_re2::RE2::UNANCHORED, submatch, nsubmatch)) {
		auto &group = submatch[group_index];
		VerifyUTF8Result(group.data(), group.size());
		return string_t(group.data(), UnsafeNumericCast<uint32_t>(group.size()));
	}
	return no_match_returns_input ? input : string_t(nullptr, 0);
}

static bool RegexOptionsEquals(const duckdb_re2::RE2::Options &opt_a, const duckdb_re2::RE2::Options &opt_b) {
	return opt_a.case_sensitive() == opt_b.case_sensitive();
}

RegexpBaseBindData::RegexpBaseBindData() : constant_pattern(false) {
}
RegexpBaseBindData::RegexpBaseBindData(duckdb_re2::RE2::Options options, string constant_string_p,
                                       bool constant_pattern)
    : options(options), constant_string(std::move(constant_string_p)), constant_pattern(constant_pattern) {
}

RegexpBaseBindData::~RegexpBaseBindData() {
}

bool RegexpBaseBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<RegexpBaseBindData>();
	return constant_pattern == other.constant_pattern && constant_string == other.constant_string &&
	       RegexOptionsEquals(options, other.options);
}

unique_ptr<FunctionLocalState> RegexInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                   FunctionData *bind_data) {
	auto &info = bind_data->Cast<RegexpBaseBindData>();
	if (info.constant_pattern) {
		return make_uniq<RegexLocalState>(info);
	}
	return nullptr;
}

//===--------------------------------------------------------------------===//
// Regexp Matches
//===--------------------------------------------------------------------===//
RegexpMatchesBindData::RegexpMatchesBindData(duckdb_re2::RE2::Options options, string constant_string_p,
                                             bool constant_pattern)
    : RegexpBaseBindData(options, std::move(constant_string_p), constant_pattern) {
	if (constant_pattern) {
		// an invalid pattern is reported when the function state is initialized
		auto pattern = make_uniq<RE2>(constant_string, options);
		range_success = pattern->ok() && pattern->PossibleMatchRange(&range_min, &range_max, 1000);
	} else {
		range_success = false;
	}
}

RegexpMatchesBindData::RegexpMatchesBindData(duckdb_re2::RE2::Options options, string constant_string_p,
                                             bool constant_pattern, string range_min_p, string range_max_p,
                                             bool range_success)
    : RegexpBaseBindData(options, std::move(constant_string_p), constant_pattern), range_min(std::move(range_min_p)),
      range_max(std::move(range_max_p)), range_success(range_success) {
}

unique_ptr<FunctionData> RegexpMatchesBindData::Copy() const {
	return make_uniq<RegexpMatchesBindData>(options, constant_string, constant_pattern, range_min, range_max,
	                                        range_success);
}

unique_ptr<FunctionData> RegexpMatchesBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	// pattern is the second argument. If its constant, we can already prepare the pattern and store it for later.
	D_ASSERT(arguments.size() == 2 || arguments.size() == 3);
	RE2::Options options;
	options.set_log_errors(false);
	if (arguments.size() == 3) {
		ParseRegexOptions(input.GetConstant(2), options);
	}

	string constant_string;
	bool constant_pattern;
	constant_pattern = TryParseConstantPattern(input.TryGetConstant(1), constant_string);
	return make_uniq<RegexpMatchesBindData>(options, std::move(constant_string), constant_pattern);
}

struct RegexPartialMatch {
	static inline bool Operation(const duckdb_re2::StringPiece &input, duckdb_re2::RE2 &re) {
		return duckdb_re2::RE2::PartialMatch(input, re);
	}
};

struct RegexFullMatch {
	static inline bool Operation(const duckdb_re2::StringPiece &input, duckdb_re2::RE2 &re) {
		return duckdb_re2::RE2::FullMatch(input, re);
	}
};

template <class OP>
static void RegexpMatchesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto &strings = args.data[0];
	const auto &patterns = args.data[1];

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<RegexpMatchesBindData>();

	if (info.constant_pattern) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RegexLocalState>();
		UnaryExecutor::Execute<string_t, bool>(strings, result, [&](string_t input) {
			return OP::Operation(CreateStringPiece(input), lstate.constant_pattern);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, bool>(strings, patterns, result,
		                                                  [&](string_t input, string_t pattern) {
			                                                  RE2 re(CreateStringPiece(pattern), info.options);
			                                                  if (!re.ok()) {
				                                                  throw InvalidInputException(re.error());
			                                                  }
			                                                  return OP::Operation(CreateStringPiece(input), re);
		                                                  });
	}
}

//===--------------------------------------------------------------------===//
// Regexp Replace
//===--------------------------------------------------------------------===//
RegexpReplaceBindData::RegexpReplaceBindData() : global_replace(false) {
}

RegexpReplaceBindData::RegexpReplaceBindData(duckdb_re2::RE2::Options options, string constant_string_p,
                                             bool constant_pattern, bool global_replace)
    : RegexpBaseBindData(options, std::move(constant_string_p), constant_pattern), global_replace(global_replace) {
}

unique_ptr<FunctionData> RegexpReplaceBindData::Copy() const {
	auto copy = make_uniq<RegexpReplaceBindData>(options, constant_string, constant_pattern, global_replace);
	return std::move(copy);
}

bool RegexpReplaceBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<RegexpReplaceBindData>();
	return RegexpBaseBindData::Equals(other) && global_replace == other.global_replace;
}

static unique_ptr<FunctionData> RegexReplaceBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	auto data = make_uniq<RegexpReplaceBindData>();

	data->constant_pattern = TryParseConstantPattern(input.TryGetConstant(1), data->constant_string);
	if (arguments.size() == 4) {
		ParseRegexOptions(input.GetConstant(3), data->options, &data->global_replace);
	}
	data->options.set_log_errors(false);

	auto replacement = input.TryGetConstant(2);
	if (data->constant_pattern && replacement && !replacement->IsNull() &&
	    data->constant_string.find("\\C") == string::npos) {
		RE2 validation_re(data->constant_string, data->options);
		std::string rewrite_error;
		if (validation_re.ok() &&
		    validation_re.CheckRewriteString(CreateStringPiece(StringValue::Get(*replacement)), &rewrite_error)) {
			// A valid pattern without byte escapes and a valid replacement cannot throw.
			input.GetBoundFunction().SetErrorMode(FunctionErrors::CANNOT_ERROR);
		}
	}
	return std::move(data);
}

static void RegexReplaceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<RegexpReplaceBindData>();

	const auto &strings = args.data[0];
	const auto &patterns = args.data[1];
	const auto &replaces = args.data[2];

	auto &heap = StringVector::GetStringHeap(result);
	if (info.constant_pattern) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RegexLocalState>();
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    strings, replaces, result, [&](string_t input, string_t replace) {
			    auto replace_piece = CreateStringPiece(replace);
			    std::string rewrite_error;
			    if (!lstate.constant_pattern.CheckRewriteString(replace_piece, &rewrite_error)) {
				    throw InvalidInputException("Invalid replacement string for regexp_replace: %s", rewrite_error);
			    }
			    std::string sstring = input.GetString();
			    if (info.global_replace) {
				    RE2::GlobalReplace(&sstring, lstate.constant_pattern, replace_piece);
			    } else {
				    RE2::Replace(&sstring, lstate.constant_pattern, replace_piece);
			    }
			    VerifyUTF8Result(sstring.c_str(), sstring.size());
			    return heap.AddString(sstring);
		    });
	} else {
		TernaryExecutor::Execute<string_t, string_t, string_t, string_t>(
		    strings, patterns, replaces, result, [&](string_t input, string_t pattern, string_t replace) {
			    RE2 re(CreateStringPiece(pattern), info.options);
			    if (!re.ok()) {
				    throw InvalidInputException(re.error());
			    }
			    auto replace_piece = CreateStringPiece(replace);
			    std::string rewrite_error;
			    if (!re.CheckRewriteString(replace_piece, &rewrite_error)) {
				    throw InvalidInputException("Invalid replacement string for regexp_replace: %s", rewrite_error);
			    }
			    std::string sstring = input.GetString();
			    if (info.global_replace) {
				    RE2::GlobalReplace(&sstring, re, replace_piece);
			    } else {
				    RE2::Replace(&sstring, re, replace_piece);
			    }
			    VerifyUTF8Result(sstring.c_str(), sstring.size());
			    return heap.AddString(sstring);
		    });
	}
}

//===--------------------------------------------------------------------===//
// Regexp Extract
//===--------------------------------------------------------------------===//
RegexpExtractBindData::RegexpExtractBindData() {
}

RegexpExtractBindData::RegexpExtractBindData(duckdb_re2::RE2::Options options, string constant_string_p,
                                             bool constant_pattern, int8_t group_index, bool no_match_returns_input)
    : RegexpBaseBindData(options, std::move(constant_string_p), constant_pattern), group_index(group_index),
      no_match_returns_input(no_match_returns_input) {
}

unique_ptr<FunctionData> RegexpExtractBindData::Copy() const {
	return make_uniq<RegexpExtractBindData>(options, constant_string, constant_pattern, group_index,
	                                        no_match_returns_input);
}

bool RegexpExtractBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<RegexpExtractBindData>();
	return RegexpBaseBindData::Equals(other) && group_index == other.group_index &&
	       no_match_returns_input == other.no_match_returns_input;
}

static void RegexExtractFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<RegexpExtractBindData>();

	const auto &strings = args.data[0];
	const auto &patterns = args.data[1];
	// Result strings are zero-copy slices of the input vector (or the input itself when
	// no_match_returns_input is set), so register the heap reference once per chunk regardless of
	// per-row outcomes.
	StringVector::AddHeapReference(result, strings);

	if (info.constant_pattern) {
		auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RegexLocalState>();
		const auto &re = lstate.constant_pattern;
		UnaryExecutor::Execute<string_t, string_t>(strings, result, [&](string_t input) {
			return ExtractCaptureGroup(input, re, info.group_index, info.no_match_returns_input);
		});
	} else {
		BinaryExecutor::Execute<string_t, string_t, string_t>(
		    strings, patterns, result, [&](string_t input, string_t pattern) {
			    RE2 re(CreateStringPiece(pattern), info.options);
			    if (!re.ok()) {
				    throw InvalidInputException(re.error());
			    }
			    return ExtractCaptureGroup(input, re, info.group_index, info.no_match_returns_input);
		    });
	}
}

//===--------------------------------------------------------------------===//
// Regexp Extract Struct
//===--------------------------------------------------------------------===//
static void RegexExtractStructFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// This function assumes a constant pre-compiled pattern stored in the local state.
	// If a non-constant pattern reaches here it indicates a binder bug. Return a clean error instead of crashing.
	if (!ExecuteFunctionState::GetFunctionState(state)) {
		throw InternalException("REGEXP_EXTRACT struct variant executed without constant pattern state");
	}
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RegexLocalState>();

	const auto &input = args.data[0];

	auto &child_entries = StructVector::GetEntries(result);
	const auto groupSize = child_entries.size();
	// Reference the 'input' StringBuffer, because we won't need to allocate new data
	// for the result, all returned strings are substrings of the originals
	for (auto &child_entry : child_entries) {
		StringVector::AddHeapReference(child_entry, input);
	}

	vector<RE2::Arg> argv(groupSize);
	vector<RE2::Arg *> groups(groupSize);
	vector<duckdb_re2::StringPiece> ws(groupSize);
	for (size_t i = 0; i < groupSize; ++i) {
		groups[i] = &argv[i];
		argv[i] = &ws[i];
	}

	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);

		if (ConstantVector::IsNull(input)) {
			ConstantVector::SetNull(result, count_t(args.size()));
		} else {
			ConstantVector::SetNull(result, false);
			auto idata = ConstantVector::GetData<string_t>(input);
			auto str = CreateStringPiece(idata[0]);
			auto match = duckdb_re2::RE2::PartialMatchN(str, lstate.constant_pattern, groups.data(),
			                                            UnsafeNumericCast<int>(groups.size()));
			for (size_t col = 0; col < child_entries.size(); ++col) {
				auto &child_entry = child_entries[col];
				ConstantVector::SetNull(child_entry, false);
				auto &extracted = ws[col];
				if (match) {
					VerifyUTF8Result(extracted.data(), extracted.size());
				}
				auto cdata = ConstantVector::GetData<string_t>(child_entry);
				cdata[0] = string_t(extracted.data(), UnsafeNumericCast<uint32_t>(match ? extracted.size() : 0));
			}
		}
	} else {
		// Start with a valid flat vector
		result.SetVectorType(VectorType::FLAT_VECTOR);

		// Start with valid children
		for (size_t col = 0; col < child_entries.size(); ++col) {
			auto &child_entry = child_entries[col];
			child_entry.SetVectorType(VectorType::FLAT_VECTOR);
		}

		for (auto entry : input.Values<string_t>()) {
			if (!entry.IsValid()) {
				FlatVector::SetNull(result, entry.GetIndex(), true);
				continue;
			}
			auto str = CreateStringPiece(entry.GetValue());
			auto match = duckdb_re2::RE2::PartialMatchN(str, lstate.constant_pattern, groups.data(),
			                                            UnsafeNumericCast<int>(groups.size()));
			for (size_t col = 0; col < child_entries.size(); ++col) {
				auto &child_entry = child_entries[col];
				auto cdata = FlatVector::GetDataMutable<string_t>(child_entry);
				auto &extracted = ws[col];
				if (match) {
					VerifyUTF8Result(extracted.data(), extracted.size());
				}
				cdata[entry.GetIndex()] =
				    string_t(extracted.data(), UnsafeNumericCast<uint32_t>(match ? extracted.size() : 0));
			}
		}
	}
}

static unique_ptr<FunctionData> RegexExtractBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(arguments.size() >= 2);

	duckdb_re2::RE2::Options options;

	string constant_string;
	bool constant_pattern = TryParseConstantPattern(input.TryGetConstant(1), constant_string);

	bool no_match_returns_input = false;

	if (arguments.size() >= 4) {
		ParseRegexOptions(input.GetConstant(3), options, nullptr, &no_match_returns_input);
	}

	int8_t group_index = 0;
	if (arguments.size() >= 3) {
		Value group_or_options = input.GetConstant(2);
		if (group_or_options.type().id() == LogicalTypeId::LIST) {
			if (!constant_pattern) {
				throw BinderException("%s with LIST of group names requires a constant pattern",
				                      bound_function.GetName());
			}
			vector<string> dummy_names; // not reused after bind
			child_list_t<LogicalType> struct_children;
			regexp_util::ParseGroupNameList(bound_function.GetName().GetIdentifierName(), group_or_options,
			                                constant_string, options, constant_pattern, dummy_names, struct_children);
			bound_function.SetReturnType(LogicalType::STRUCT(struct_children));
		} else if (group_or_options.type() == LogicalType::VARCHAR) {
			ParseRegexOptions(group_or_options, options, nullptr, &no_match_returns_input);
		} else if (group_or_options.IsNull()) {
			// NULL group → never returns a capture; runtime treats out-of-range index as no match.
			group_index = -1;
		} else {
			int32_t group_idx = group_or_options.GetValue<int32_t>();
			if (group_idx < 0 || group_idx > 9) {
				throw InvalidInputException("Group index must be between 0 and 9!");
			}
			group_index = static_cast<int8_t>(group_idx);
		}
	}

	if (constant_pattern && constant_string.find("\\C") == string::npos) {
		auto validation_options = options;
		validation_options.set_log_errors(false);
		if (RE2(constant_string, validation_options).ok()) {
			// A valid UTF-8 pattern without byte escapes cannot return a partial character.
			bound_function.SetErrorMode(FunctionErrors::CANNOT_ERROR);
		}
	}

	return make_uniq<RegexpExtractBindData>(options, std::move(constant_string), constant_pattern, group_index,
	                                        no_match_returns_input);
}

// Constructs a regex-family ScalarFunction and names its (up to 4) parameters via the builder pattern, since these
// functions share the same string/regex/group/options/name_list/replacement parameter names across many overloads.
static ScalarFunction MakeRegexFunction(const vector<pair<Identifier, LogicalType>> &params, LogicalType return_type,
                                        scalar_function_t function, bind_scalar_function_t bind,
                                        init_local_state_t init_state = nullptr) {
	ScalarFunction fun({}, std::move(return_type), std::move(function), bind, nullptr, init_state, LogicalType::INVALID,
	                   FunctionStability::CONSISTENT, FunctionNullHandling::SPECIAL_HANDLING);
	for (auto &param : params) {
		fun.GetSignature().AddParameter(param.first, param.second);
	}
	return fun;
}

ScalarFunctionSet RegexpFun::GetFunctions() {
	ScalarFunctionSet regexp_full_match("regexp_full_match");
	regexp_full_match.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}},
	                                                LogicalType::BOOLEAN, RegexpMatchesFunction<RegexFullMatch>,
	                                                RegexpMatchesBind, RegexInitLocalState));
	regexp_full_match.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"options", LogicalType::VARCHAR}},
	    LogicalType::BOOLEAN, RegexpMatchesFunction<RegexFullMatch>, RegexpMatchesBind, RegexInitLocalState));
	regexp_full_match.SetFallible();
	return (regexp_full_match);
}

ScalarFunctionSet RegexpMatchesFun::GetFunctions() {
	ScalarFunctionSet regexp_partial_match("regexp_matches");
	regexp_partial_match.AddFunction(
	    MakeRegexFunction({{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}}, LogicalType::BOOLEAN,
	                      RegexpMatchesFunction<RegexPartialMatch>, RegexpMatchesBind, RegexInitLocalState));
	regexp_partial_match.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"options", LogicalType::VARCHAR}},
	    LogicalType::BOOLEAN, RegexpMatchesFunction<RegexPartialMatch>, RegexpMatchesBind, RegexInitLocalState));
	regexp_partial_match.SetFallible();
	return (regexp_partial_match);
}

ScalarFunctionSet RegexpReplaceFun::GetFunctions() {
	ScalarFunctionSet regexp_replace("regexp_replace");
	regexp_replace.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"replacement", LogicalType::VARCHAR}},
	    LogicalType::VARCHAR, RegexReplaceFunction, RegexReplaceBind, RegexInitLocalState));
	regexp_replace.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                              {"regex", LogicalType::VARCHAR},
	                                              {"replacement", LogicalType::VARCHAR},
	                                              {"options", LogicalType::VARCHAR}},
	                                             LogicalType::VARCHAR, RegexReplaceFunction, RegexReplaceBind,
	                                             RegexInitLocalState));
	// throws for invalid replacement strings
	regexp_replace.SetFallible();
	return (regexp_replace);
}

ScalarFunctionSet RegexpExtractFun::GetFunctions() {
	ScalarFunctionSet regexp_extract("regexp_extract");
	regexp_extract.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}},
	                                             LogicalType::VARCHAR, RegexExtractFunction, RegexExtractBind,
	                                             RegexInitLocalState));
	regexp_extract.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"group", LogicalType::INTEGER}},
	    LogicalType::VARCHAR, RegexExtractFunction, RegexExtractBind, RegexInitLocalState));
	regexp_extract.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"options", LogicalType::VARCHAR}},
	    LogicalType::VARCHAR, RegexExtractFunction, RegexExtractBind, RegexInitLocalState));
	regexp_extract.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                              {"regex", LogicalType::VARCHAR},
	                                              {"group", LogicalType::INTEGER},
	                                              {"options", LogicalType::VARCHAR}},
	                                             LogicalType::VARCHAR, RegexExtractFunction, RegexExtractBind,
	                                             RegexInitLocalState));
	// REGEXP_EXTRACT(<string>, <pattern>, [<group 1 name>[, <group n name>]...])
	regexp_extract.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                              {"regex", LogicalType::VARCHAR},
	                                              {"name_list", LogicalType::LIST(LogicalType::VARCHAR)}},
	                                             LogicalType::VARCHAR, RegexExtractStructFunction, RegexExtractBind,
	                                             RegexInitLocalState));
	// REGEXP_EXTRACT(<string>, <pattern>, [<group 1 name>[, <group n name>]...], <options>)
	regexp_extract.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                              {"regex", LogicalType::VARCHAR},
	                                              {"name_list", LogicalType::LIST(LogicalType::VARCHAR)},
	                                              {"options", LogicalType::VARCHAR}},
	                                             LogicalType::VARCHAR, RegexExtractStructFunction, RegexExtractBind,
	                                             RegexInitLocalState));
	// throws when \C splits a character
	regexp_extract.SetFallible();
	return (regexp_extract);
}

ScalarFunctionSet RegexpExtractAllFun::GetFunctions() {
	ScalarFunctionSet regexp_extract_all("regexp_extract_all");
	regexp_extract_all.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}}, LogicalType::LIST(LogicalType::VARCHAR),
	    RegexpExtractAll::Execute, RegexpExtractAll::Bind, RegexpExtractAll::InitLocalState));
	regexp_extract_all.AddFunction(MakeRegexFunction(
	    {{"string", LogicalType::VARCHAR}, {"regex", LogicalType::VARCHAR}, {"group", LogicalType::INTEGER}},
	    LogicalType::LIST(LogicalType::VARCHAR), RegexpExtractAll::Execute, RegexpExtractAll::Bind,
	    RegexpExtractAll::InitLocalState));
	regexp_extract_all.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                                  {"regex", LogicalType::VARCHAR},
	                                                  {"group", LogicalType::INTEGER},
	                                                  {"options", LogicalType::VARCHAR}},
	                                                 LogicalType::LIST(LogicalType::VARCHAR), RegexpExtractAll::Execute,
	                                                 RegexpExtractAll::Bind, RegexpExtractAll::InitLocalState));
	// Struct multi-match variant(s): pattern must be constant due to bind-time struct shape inference
	// return type is temporary, replaced in bind
	regexp_extract_all.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                                  {"regex", LogicalType::VARCHAR},
	                                                  {"name_list", LogicalType::LIST(LogicalType::VARCHAR)}},
	                                                 LogicalType::LIST(LogicalType::VARCHAR),
	                                                 RegexpExtractAllStruct::Execute, RegexpExtractAllStruct::Bind,
	                                                 RegexpExtractAllStruct::InitLocalState));
	regexp_extract_all.AddFunction(MakeRegexFunction({{"string", LogicalType::VARCHAR},
	                                                  {"regex", LogicalType::VARCHAR},
	                                                  {"name_list", LogicalType::LIST(LogicalType::VARCHAR)},
	                                                  {"options", LogicalType::VARCHAR}},
	                                                 LogicalType::LIST(LogicalType::VARCHAR),
	                                                 RegexpExtractAllStruct::Execute, RegexpExtractAllStruct::Bind,
	                                                 RegexpExtractAllStruct::InitLocalState));
	// throws when accessing a group that the pattern does not have
	regexp_extract_all.SetFallible();
	return (regexp_extract_all);
}

} // namespace duckdb
