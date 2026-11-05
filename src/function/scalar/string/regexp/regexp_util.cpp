#include "duckdb/function/scalar/regexp.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "re2/re2.h"
#include "re2/stringpiece.h"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

namespace regexp_util {

bool TryParseConstantPattern(optional<Value> pattern_value, string &constant_string) {
	if (!pattern_value || pattern_value->IsNull() || pattern_value->type().id() != LogicalTypeId::VARCHAR) {
		return false;
	}
	constant_string = StringValue::Get(*pattern_value);
	return true;
}

void ParseRegexOptions(const string &options, duckdb_re2::RE2::Options &result, bool *global_replace,
                       bool *no_match_returns_input) {
	for (idx_t i = 0; i < options.size(); i++) {
		switch (options[i]) {
		case 'c':
			// case-sensitive matching
			result.set_case_sensitive(true);
			break;
		case 'i':
			// case-insensitive matching
			result.set_case_sensitive(false);
			break;
		case 'l':
			// literal matching
			result.set_literal(true);
			break;
		case 'm':
		case 'n':
		case 'p':
			// newline-sensitive matching
			result.set_dot_nl(false);
			break;
		case 's':
			// non-newline-sensitive matching
			result.set_dot_nl(true);
			break;
		case 'g':
			// global replace, only available for regexp_replace
			if (global_replace) {
				*global_replace = true;
			} else {
				throw InvalidInputException("Option 'g' (global replace) is only valid for regexp_replace");
			}
			break;
		case 'k':
			// keep (return) the input on no match instead of an empty string (regexp_extract only)
			if (no_match_returns_input) {
				*no_match_returns_input = true;
			} else {
				throw InvalidInputException("Option 'k' (keep input on no match) is only valid for regexp_extract");
			}
			break;
		case ' ':
		case '\t':
		case '\n':
			// ignore whitespace
			break;
		default:
			throw InvalidInputException("Unrecognized Regex option %c", options[i]);
		}
	}
}

void ParseRegexOptions(const Value &options_str, RE2::Options &target, bool *global_replace,
                       bool *no_match_returns_input) {
	if (options_str.IsNull()) {
		throw InvalidInputException("Regex options field must not be NULL");
	}
	if (options_str.type().id() != LogicalTypeId::VARCHAR) {
		throw InvalidInputException("Regex options field must be a string");
	}
	ParseRegexOptions(StringValue::Get(options_str), target, global_replace, no_match_returns_input);
}

void ParseGroupNameList(const string &function_name, const Value &list_val, const string &pattern_string,
                        RE2::Options &options, bool require_constant_pattern, vector<string> &out_names,
                        child_list_t<LogicalType> &out_struct_children) {
	if (list_val.IsNull() || list_val.type().id() != LogicalTypeId::LIST) {
		throw BinderException("Group specification must be a non-NULL LIST");
	}
	auto &children = ListValue::GetChildren(list_val);
	if (children.empty()) {
		throw BinderException("Group name list must be non-empty");
	}
	identifier_set_t name_set;
	for (auto &child : children) {
		if (child.IsNull()) {
			throw BinderException("NULL group name in %s", function_name);
		}
		auto name = child.ToString();
		if (name.empty()) {
			throw BinderException("Empty group name in %s", function_name);
		}
		if (name_set.find(Identifier(name)) != name_set.end()) {
			throw BinderException("Duplicate group name '%s' in %s", name, function_name);
		}
		name_set.insert(Identifier(name));
		out_names.push_back(name);
		out_struct_children.emplace_back(make_pair(name, LogicalType::VARCHAR));
	}
	if (require_constant_pattern) {
		duckdb_re2::StringPiece const_piece(pattern_string.c_str(), pattern_string.size());
		RE2 constant_re(const_piece, options);
		auto group_cnt = constant_re.NumberOfCapturingGroups();
		if (group_cnt == -1) {
			throw BinderException("Pattern failed to parse: %s", constant_re.error());
		}
		if ((idx_t)group_cnt < out_names.size()) {
			throw BinderException("Not enough capturing groups (%d) for provided names (%llu)", group_cnt,
			                      NumericCast<uint64_t>(out_names.size()));
		}
	}
}

// Advance exactly one UTF-8 codepoint starting at 'base'. Falls back to single byte on invalid lead.
// Does not do a full validation of UTF-8 sequence, assumes input is mostly valid UTF-8.
idx_t AdvanceOneUTF8Basic(const duckdb_re2::StringPiece &input, idx_t base) {
	if (base >= input.length()) {
		return 1; // Out of bounds, just advance one byte
	}
	unsigned char first = static_cast<unsigned char>(input[base]);
	idx_t char_len = 1;
	if ((first & 0x80) == 0) {
		char_len = 1; // ASCII
	} else if ((first & 0xE0) == 0xC0) {
		char_len = 2;
	} else if ((first & 0xF0) == 0xE0) {
		char_len = 3;
	} else if ((first & 0xF8) == 0xF0) {
		char_len = 4;
	} else {
		// \C can leave us in the middle of a character
		return 1;
	}
	if (base + char_len > input.length()) {
		return 1;
	}
	return char_len;
}

void VerifyUTF8Result(const char *data, idx_t size) {
	if (!Utf8Proc::IsValid(data, size)) {
		throw InvalidInputException("Regular expression result is not valid UTF-8 - \\C matched part of a character");
	}
}

} // namespace regexp_util

} // namespace duckdb
