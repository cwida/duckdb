//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/match_recognize.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/planner/expression.hpp"

namespace duckdb {

//! Prefix that keeps a pattern symbol's plan column apart from an input column of the same name
constexpr const char *MATCH_RECOGNIZE_DEFINE_PREFIX = "__mr_define_";

//! The column MATCH_NUMBER() reads, which the matcher rewrites for every match it attempts
constexpr const char *MATCH_RECOGNIZE_MATCH_NUMBER_COLUMN = "__mr_match_number";

//! RUNNING and FINAL reach the binder as these markers, wrapping the measure they applied to
constexpr const char *MATCH_RECOGNIZE_RUNNING_MARKER = "__mr_running";
constexpr const char *MATCH_RECOGNIZE_FINAL_MARKER = "__mr_final";
//! What CLASSIFIER(X) reads off a row: written as a column of X while a navigation or an aggregate
//! is resolved, so that the variable it names scopes it the way it scopes a column (5.9)
constexpr const char *MATCH_RECOGNIZE_CLASSIFIER_FIELD = "__mr_classifier";

//! The plan column a pattern variable is qualified with
inline string MatchRecognizeDefineColumn(const string &symbol) {
	return MATCH_RECOGNIZE_DEFINE_PREFIX + symbol;
}

//! The user facing pattern variable for a prefixed plan column
inline string MatchRecognizeSymbolName(const string &column_name) {
	const auto prefix_size = strlen(MATCH_RECOGNIZE_DEFINE_PREFIX);
	if (StringUtil::StartsWith(column_name, MATCH_RECOGNIZE_DEFINE_PREFIX)) {
		return column_name.substr(prefix_size);
	}
	return column_name;
}

//! The pattern algebra: a variable, an anchor that matches a place rather than a row, and the ways of
//! putting parts together
enum class MatchRecognizePatternType : uint8_t { SYMBOL, ANCHOR, ALTERNATION, CONCATENATION, QUANTIFIER };

//! The pattern the matcher walks, owned by the bind data below and compiled into the matcher's
//! program. Nothing about it is an SQL expression.
struct MatchRecognizePattern {
	explicit MatchRecognizePattern(MatchRecognizePatternType type_p) : type(type_p) {
	}

	MatchRecognizePatternType type;
	//! SYMBOL: the pattern variable, as an index into the bind data's symbols
	idx_t symbol = 0;
	//! ANCHOR: whether it holds past the partition's last row rather than at its first
	bool at_end = false;
	//! QUANTIFIER: how many repetitions it allows, unset for unbounded in that direction
	optional_idx min_count;
	optional_idx max_count;
	//! QUANTIFIER: {- -}, whose rows take part in the match but are left out of the output
	bool excluded = false;
	//! QUANTIFIER: a trailing ?, preferring the fewest repetitions rather than the most
	bool reluctant = false;
	//! ALTERNATION has two, CONCATENATION as many as it concatenates, QUANTIFIER one, the rest none
	vector<unique_ptr<MatchRecognizePattern>> children;

	unique_ptr<MatchRecognizePattern> Copy() const {
		auto result = make_uniq<MatchRecognizePattern>(type);
		result->symbol = symbol;
		result->at_end = at_end;
		result->min_count = min_count;
		result->max_count = max_count;
		result->excluded = excluded;
		result->reluctant = reluctant;
		for (auto &child : children) {
			result->children.push_back(child->Copy());
		}
		return result;
	}

	bool Equals(const MatchRecognizePattern &other) const {
		if (type != other.type || symbol != other.symbol || at_end != other.at_end || min_count != other.min_count ||
		    max_count != other.max_count || excluded != other.excluded || reluctant != other.reluctant ||
		    children.size() != other.children.size()) {
			return false;
		}
		for (idx_t i = 0; i < children.size(); i++) {
			if (!children[i]->Equals(*other.children[i])) {
				return false;
			}
		}
		return true;
	}

	void Serialize(Serializer &serializer) const;
	static unique_ptr<MatchRecognizePattern> Deserialize(Deserializer &deserializer);
};

struct MatchRecognizeFunctionData : FunctionData {
	unique_ptr<MatchRecognizePattern> pattern;
	//! One condition per pattern symbol, reading the window's arguments by position
	vector<unique_ptr<Expression>> conditions;
	//! The symbol each condition defines, in the same order
	vector<string> symbols;
	//! Whether any condition reads MATCH_NUMBER(), which is what forces re-evaluation per match
	bool depends_on_match_number = false;
	//! Where the match number sits among the values a condition reads, after the plan's own columns
	idx_t match_number_field = 0;
	//! FIRST()/LAST() calls, resolved against the match being assembled
	struct Navigation {
		bool last;
		string symbol;
		idx_t field;
		idx_t offset;
		//! Whether what is read off the row reached is its classifier rather than a column, which
		//! the matcher supplies itself
		bool classifier = false;
		//! Rows to step through the partition from the row navigated to, backwards when negative,
		//! which is PREV or NEXT around the navigation (5.6.4, 5.9)
		int64_t step = 0;

		bool Equals(const Navigation &other) const {
			return last == other.last && symbol == other.symbol && field == other.field && offset == other.offset &&
			       classifier == other.classifier && step == other.step;
		}
	};
	vector<Navigation> navigations;
	//! An aggregate in a DEFINE condition, over the rows a variable has matched so far. Running
	//! semantics are the only ones a condition has (ISO/IEC 19075-5 5.5), so the set it reads is the
	//! rows mapped up to and including the one being tested.
	struct Aggregate {
		//! The variable whose rows it reads, empty for the match as a whole
		string symbol;
		//! The collected column holding the operand, unset for COUNT(*)
		optional_idx operand;
		//! The field the matcher writes the result into, past the collected columns
		idx_t field;
		//! The aggregate itself; its function and bind data are what the matcher folds rows with
		unique_ptr<Expression> expression;

		bool Equals(const Aggregate &other) const {
			return symbol == other.symbol && operand == other.operand && field == other.field &&
			       Expression::Equals(expression, other.expression);
		}
		Aggregate Copy() const {
			return Aggregate {symbol, operand, field, expression->Copy()};
		}
	};
	vector<Aggregate> aggregates;
	//! Conditions that read a navigation or an aggregate field, and so are evaluated row by row
	vector<bool> row_scoped;
	//! How to resume scanning after a match has been found
	MatchRecognizeAfterMatch after_match = MatchRecognizeAfterMatch::MATCH_RECOGNIZE_AFTER_MATCH_DEFAULT;
	//! The target pattern variable for the SKIP TO FIRST/LAST forms
	string after_match_variable;
	//! A union row pattern variable, which stands for the rows of any of its members (ISO/IEC
	//! 19075-5 4.15). A navigation, an aggregate or a SKIP TO naming one reads all of them.
	struct Subset {
		string name;
		vector<string> members;

		bool Equals(const Subset &other) const {
			return name == other.name && members == other.members;
		}
	};
	vector<Subset> subsets;

	unique_ptr<FunctionData> Copy() const override {
		auto res = make_uniq<MatchRecognizeFunctionData>();

		res->pattern = pattern->Copy();
		for (auto &condition : conditions) {
			res->conditions.push_back(condition->Copy());
		}
		res->symbols = symbols;
		res->depends_on_match_number = depends_on_match_number;
		res->match_number_field = match_number_field;
		res->navigations = navigations;
		for (auto &aggregate : aggregates) {
			res->aggregates.push_back(aggregate.Copy());
		}
		res->row_scoped = row_scoped;
		res->after_match = after_match;
		res->after_match_variable = after_match_variable;
		res->subsets = subsets;
		return std::move(res);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<MatchRecognizeFunctionData>();
		if (navigations.size() != other.navigations.size()) {
			return false;
		}
		for (idx_t i = 0; i < navigations.size(); i++) {
			if (!navigations[i].Equals(other.navigations[i])) {
				return false;
			}
		}
		if (aggregates.size() != other.aggregates.size()) {
			return false;
		}
		for (idx_t i = 0; i < aggregates.size(); i++) {
			if (!aggregates[i].Equals(other.aggregates[i])) {
				return false;
			}
		}
		if (subsets.size() != other.subsets.size()) {
			return false;
		}
		for (idx_t i = 0; i < subsets.size(); i++) {
			if (!subsets[i].Equals(other.subsets[i])) {
				return false;
			}
		}
		return other.pattern->Equals(*pattern) && Expression::ListEquals(conditions, other.conditions) &&
		       other.symbols == symbols && other.depends_on_match_number == depends_on_match_number &&
		       other.match_number_field == match_number_field && other.row_scoped == row_scoped &&
		       other.after_match == after_match && other.after_match_variable == after_match_variable;
	}
};

} // namespace duckdb
