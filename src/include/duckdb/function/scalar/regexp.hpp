//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/regexp.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"
#include "re2/re2.h"
#include "duckdb/function/built_in_functions.hpp"
#include "re2/stringpiece.h"

namespace duckdb {

namespace regexp_util {

bool TryParseConstantPattern(optional<Value> pattern_value, string &constant_string);
void ParseRegexOptions(const string &options, duckdb_re2::RE2::Options &result, bool *global_replace = nullptr,
                       bool *no_match_returns_input = nullptr);
void ParseRegexOptions(const Value &options_str, RE2::Options &target, bool *global_replace = nullptr,
                       bool *no_match_returns_input = nullptr);
void ParseGroupNameList(const string &function_name, const Value &list_val, const string &pattern_string,
                        RE2::Options &options, bool require_constant_pattern, vector<string> &out_names,
                        child_list_t<LogicalType> &out_struct_children);

idx_t AdvanceOneUTF8Basic(const duckdb_re2::StringPiece &input, idx_t base);
//! Throws if a regex result is not valid UTF-8
void VerifyUTF8Result(const char *data, idx_t size);

inline duckdb_re2::StringPiece CreateStringPiece(const string_t &input) {
	return duckdb_re2::StringPiece(input.GetData(), input.GetSize());
}

} // namespace regexp_util

struct RegexpExtractAll {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result);
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);
	static unique_ptr<FunctionLocalState> InitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
	                                                     FunctionData *bind_data);
};

struct RegexpBaseBindData : public FunctionData {
	RegexpBaseBindData();
	RegexpBaseBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern = true);
	~RegexpBaseBindData() override;

	duckdb_re2::RE2::Options options;
	string constant_string;
	bool constant_pattern;

	bool Equals(const FunctionData &other_p) const override;
};

struct RegexpExtractAllStructBindData : public RegexpBaseBindData {
	RegexpExtractAllStructBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern,
	                               vector<string> group_names)
	    : RegexpBaseBindData(options, std::move(constant_string), constant_pattern),
	      group_names(std::move(group_names)) {
	}

	vector<string> group_names; // order preserved

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<RegexpExtractAllStructBindData>(options, constant_string, constant_pattern, group_names);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<RegexpExtractAllStructBindData>();
		return RegexpBaseBindData::Equals(other) && group_names == other.group_names;
	}
};

struct RegexpExtractAllStruct {
	static void Execute(DataChunk &args, ExpressionState &state, Vector &result);
	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);
	static unique_ptr<FunctionLocalState> InitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
	                                                     FunctionData *bind_data);
};

struct RegexpMatchesBindData : public RegexpBaseBindData {
	RegexpMatchesBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern);
	RegexpMatchesBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern,
	                      string range_min, string range_max, bool range_success);

	string range_min;
	string range_max;
	bool range_success;

	unique_ptr<FunctionData> Copy() const override;
};

struct RegexpReplaceBindData : public RegexpBaseBindData {
	RegexpReplaceBindData();
	RegexpReplaceBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern,
	                      bool global_replace);

	bool global_replace;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
};

struct RegexpExtractBindData : public RegexpBaseBindData {
	RegexpExtractBindData();
	RegexpExtractBindData(duckdb_re2::RE2::Options options, string constant_string, bool constant_pattern,
	                      int8_t group_index, bool no_match_returns_input = false);

	// `regexp_extract` always extracts a single capture group. -1 represents "no group requested"
	// (e.g. NULL group argument), which the runtime treats as a no-match (returns empty/input).
	int8_t group_index = 0;
	// On no match, return the input instead of an empty string (set via the `k` option, also used by
	// the regexp_replace -> regexp_extract optimizer rewrite).
	bool no_match_returns_input = false;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other_p) const override;
};

struct RegexStringPieceArgs {
	RegexStringPieceArgs() : size(0), capacity(0), group_buffer(nullptr) {
	}
	void Init(idx_t size) {
		this->size = size;
		// Allocate for one extra, for the all-encompassing match group
		this->capacity = size + 1;
		group_buffer = AllocateArray<duckdb_re2::StringPiece>(capacity);
	}
	void SetSize(idx_t size) {
		this->size = size;
		if (size + 1 > capacity) {
			Clear();
			Init(size);
		}
	}

	RegexStringPieceArgs &operator=(RegexStringPieceArgs &&other) noexcept {
		this->size = other.size;
		this->capacity = other.capacity;
		this->group_buffer = other.group_buffer;
		other.size = 0;
		other.capacity = 0;
		other.group_buffer = nullptr;
		return *this;
	}

	~RegexStringPieceArgs() {
		Clear();
	}

private:
	void Clear() {
		DeleteArray<duckdb_re2::StringPiece>(group_buffer, capacity);
		group_buffer = nullptr;

		size = 0;
		capacity = 0;
	}

public:
	idx_t size;
	//! The currently allocated capacity for the groups
	idx_t capacity;
	//! Used by ExtractAll to pre-allocate the storage for the groups
	duckdb_re2::StringPiece *group_buffer;
};

struct RegexLocalState : public FunctionLocalState {
	explicit RegexLocalState(RegexpBaseBindData &info, bool extract_all = false)
	    : constant_pattern(duckdb_re2::StringPiece(info.constant_string.c_str(), info.constant_string.size()),
	                       info.options) {
		if (!constant_pattern.ok()) {
			throw InvalidInputException(constant_pattern.error());
		}
		if (extract_all) {
			auto group_count_p = constant_pattern.NumberOfCapturingGroups();
			if (group_count_p != -1) {
				group_buffer.Init(NumericCast<idx_t>(group_count_p));
			}
		}
		D_ASSERT(info.constant_pattern);
	}

	RE2 constant_pattern;
	//! Used by regexp_extract_all to pre-allocate the args
	RegexStringPieceArgs group_buffer;
};

unique_ptr<FunctionLocalState> RegexInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                   FunctionData *bind_data);
unique_ptr<FunctionData> RegexpMatchesBind(BindScalarFunctionInput &input);

} // namespace duckdb
