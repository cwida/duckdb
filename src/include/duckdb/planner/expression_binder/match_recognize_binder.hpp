//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder/match_recognize_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/function/match_recognize.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/planner/expression_binder/select_binder.hpp"

namespace duckdb {

//! Names for the columns a MATCH_RECOGNIZE clause generates. The input is bound first, so a generated
//! name is one the input demonstrably does not have, and no two of them are the same.
struct GeneratedNames {
	explicit GeneratedNames(case_insensitive_set_t taken_p) : taken(std::move(taken_p)) {
	}

	string Reserve(const string &base) {
		auto name = base;
		for (idx_t suffix = 0; taken.count(name); suffix++) {
			name = base + "_" + to_string(suffix);
		}
		taken.insert(name);
		return name;
	}

	case_insensitive_set_t taken;
};

//! A FIRST()/LAST() call in a DEFINE condition, which the matcher resolves per row
struct MatchRecognizeNavigation {
	bool last;
	//! The pattern variable navigated, empty for the match as a whole
	string symbol;
	//! Where the navigated expression sits in the projection below the matcher
	idx_t column;
	idx_t offset;
	//! Whether it reads the classifier of the row reached, which the matcher supplies in a field of
	//! its own: `slot` says which, counted with the other fields the matcher supplies
	bool classifier = false;
	idx_t slot = 0;
	//! Rows stepped through the partition from the row reached, backwards when negative
	int64_t step = 0;
};

//! An aggregate in a DEFINE condition, over the rows a variable has matched so far
struct MatchRecognizeAggregate {
	//! The pattern variable whose rows it reads, empty for the match as a whole
	string symbol;
	//! Where the operand sits in the projection below the matcher, unset for COUNT(*)
	optional_idx column;
	//! The aggregate itself, bound over one reference of the operand's type
	unique_ptr<Expression> expression;
	//! Which of the fields the matcher supplies holds the value
	idx_t slot;
};

//! What a DEFINE condition needs from the plan below the matcher: one column per value the matcher
//! cannot compute itself, plus the descriptors telling it what to do with them
struct MatchRecognizeConditionInputs {
	//! The projection the matcher reads from, built as the conditions are bound
	BoundSelectNode &projection;
	//! The table a matcher-supplied field is a column of, which no operator produces
	TableIndex match_number_index;
	//! Names the output does not report, because they only exist for the matcher
	vector<string> &hidden;
	GeneratedNames &generated;
	vector<MatchRecognizeNavigation> &navigations;
	vector<MatchRecognizeAggregate> &aggregates;
	//! How many fields the matcher supplies so far: the match number is the first, and every
	//! aggregate or classifier read takes the next
	idx_t supplied = 1;

	//! Compute this below the matcher and read it back as a column of its own
	unique_ptr<Expression> Project(unique_ptr<Expression> value, const string &base);
	unique_ptr<Expression> ProjectAs(unique_ptr<Expression> value, const string &name);
};

//! Where the value being bound is evaluated, which is what decides whether it may read the state the
//! matcher holds while it assembles a match.
enum class MatchRecognizeScope : uint8_t {
	//! The row the matcher is testing
	CANDIDATE_ROW,
	//! A window over the ordered partition
	WINDOW,
	//! The expression a navigation reads off a row of the match
	NAVIGATED,
	//! The partitioning the matcher walks
	PARTITION_BY,
	//! The ordering the matcher walks
	ORDER_BY
};

//! Binds a DEFINE condition. The matcher settles a condition one candidate row at a time, so
//! anything a condition reads that is not the row being tested - a navigation over the match, a
//! neighbour in the ordered partition - becomes a column of the projection below it.
class MatchRecognizeDefineBinder : public SelectBinder {
public:
	MatchRecognizeDefineBinder(Binder &binder, ClientContext &context, BoundSelectNode &node,
	                           MatchRecognizeConditionInputs &inputs, const WindowExpression &window_template,
	                           const case_insensitive_set_t &symbols, const case_insensitive_map_t<string> &universal,
	                           const unique_ptr<Expression> &match_number);

	//! Bind the condition of this variable, which is decided on the row the matcher is testing
	void BeginDefine(const string &name) {
		define_name = name;
		scope = MatchRecognizeScope::CANDIDATE_ROW;
	}
	//! Bind the partitioning or the ordering, which the matcher walks rather than produces
	void BeginFrame(MatchRecognizeScope frame) {
		define_name.clear();
		scope = frame;
	}

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) override;
	BindResult BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function, idx_t depth) override;
	string UnsupportedAggregateMessage() override;

public:
private:
	//! PREV()/NEXT() walk the ordered partition rather than the match, so they are ordinary windows
	BindResult BindNeighbour(FunctionExpression &function, const string &function_name,
	                         unique_ptr<ParsedExpression> &expr_ptr, idx_t depth);
	//! FIRST()/LAST() read a row of the match being assembled, so the plan supplies the expression and
	//! the matcher reads it off the row it navigated to
	BindResult BindNavigation(FunctionExpression &function, const string &function_name, idx_t depth);
	BindResult BindNavigated(unique_ptr<ParsedExpression> inner, string symbol, bool last, idx_t offset, idx_t depth,
	                         int64_t step = 0);
	//! Reject something that only means anything while the matcher is assembling a match
	void OutsideMatch(const string &what) const;

	MatchRecognizeConditionInputs &inputs;
	const WindowExpression &window_template;
	//! Every pattern variable the clause declares
	const case_insensitive_set_t &symbols;
	//! The hoisted references that read the whole match, by the name each was written with
	const case_insensitive_map_t<string> &universal;
	//! What the matcher's own match number reads as until the plan is built
	const unique_ptr<Expression> &match_number;
	//! The variable whose condition is being bound
	string define_name;
	MatchRecognizeScope scope = MatchRecognizeScope::CANDIDATE_ROW;
};

//! Binds the MEASURES clause: which rows of the match a value is read from, what a pattern variable in
//! front of a column means, and how much of the match RUNNING and FINAL let it see. Everything else is
//! ordinary binding, so an aggregate reaches the hook below only once the macros are expanded and the
//! overload chosen.
class MatchRecognizeMeasureBinder : public SelectBinder {
public:
	MatchRecognizeMeasureBinder(Binder &binder, ClientContext &context, BoundSelectNode &node, string state,
	                            const MatchRecognizeConfig &config,
	                            const case_insensitive_map_t<vector<string>> &symbols,
	                            const case_insensitive_map_t<string> &universal, bool all_rows);

protected:
	BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression) override;
	BindResult BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function, idx_t depth) override;

private:
	//! An aggregate in MEASURES aggregates the rows of the match, which is a window over them
	BindResult BindOverMatch(FunctionExpression &expr, idx_t depth);
	//! One field of the matcher's state, as it stands where the measures are projected
	unique_ptr<ParsedExpression> StateField(const string &field);
	//! Bind an expression this binder built, whose own parts are not the clause's to interpret again
	BindResult BindGenerated(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);
	BindResult BindNavigation(FunctionExpression &function, const string &function_name,
	                          unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);
	//! PREV/NEXT around CLASSIFIER(): the classifier of a row a fixed distance from a row of the match,
	//! read off the whole match once it is known
	BindResult BindClassifierStep(FunctionExpression &function, const string &function_name,
	                              unique_ptr<ParsedExpression> &expr_ptr, idx_t depth, bool root_expression);

	//! The column the matcher's state travels in
	string state;
	const MatchRecognizeConfig &config;
	//! Every pattern variable and SUBSET, mapped to the symbols it stands for
	const case_insensitive_map_t<vector<string>> &symbols;
	//! The hoisted references that read the whole match, by the name each was written with
	const case_insensitive_map_t<string> &universal;
	//! ONE ROW PER MATCH reports a finished match, so RUNNING and FINAL are the same thing there
	bool one_row;
	//! Whether the expression being bound sees the match up to the current row, or all of it
	bool running;
	//! Whether a value here is already read through something that scopes it to the match
	bool scoped = false;
};

//! Everything a reference names behind its first name is navigation into the column, so dropping a
//! qualifier drops that one name and keeps the rest: X.c.f is field f of column c, not column f.
unique_ptr<ParsedExpression> MatchRecognizeWithoutQualifier(const ColumnRefExpression &colref);

//! What a step reads off the row it starts from: FIRST or LAST within it says which of a variable's
//! rows that row is, and RUNNING or FINAL in front of that how much of the match is visible. The step
//! itself is computed below the matcher, so this is peeled off the step's argument and put back around
//! the column the step lands in.
struct MatchRecognizeSteppedNavigation {
	//! Whether the step wrote a navigation of its own rather than only naming a variable
	bool navigated = false;
	//! An ordinary reference is RUNNING LAST already, so that is what a step without one reads with
	bool last = true;
	//! Which of the variable's rows, as written and as the number it is
	unique_ptr<ParsedExpression> offset;
	idx_t offset_value = 0;
	//! The RUNNING or FINAL written in front of the navigation
	unique_ptr<ParsedExpression> marker;
	//! Whether that keyword was FINAL, which reads rows a DEFINE condition has not mapped yet
	bool final_semantics = false;

	//! Read the column the step landed in the way the step asked for it
	unique_ptr<ParsedExpression> Rebuild(const string &variable, const string &column);
};

//! Peel what the step reads with off its argument, leaving the expression the row it reaches supplies
MatchRecognizeSteppedNavigation MatchRecognizePeelStep(unique_ptr<ParsedExpression> &inner);

//! A step starts from a row the match names, and a step inside one names no row, so refuse the nesting
void MatchRecognizeRejectNestedStep(const ParsedExpression &inner, const string &function_name);
void MatchRecognizeRejectNavigationInAggregate(const ParsedExpression &argument, const string &function_name);
//! CLASSIFIER(), with or without a variable in front of it, anywhere in the expression
bool MatchRecognizeContainsClassifier(const ParsedExpression &expr);

//! Whether a name is a pattern variable. The two clauses hold their symbols differently, so which
//! names are theirs is the caller's to say.
using MatchRecognizeIsSymbol = std::function<bool(const string &)>;

//! The pattern variable a navigation reads its row from. What it reports is an expression of the
//! clause's own, so a variable may appear anywhere within it rather than only in front of it - but it
//! navigates to one row, so the expression cannot name two variables to read it from, nor one variable
//! and the whole match. The qualifiers are dropped and the variable reported back, empty for the match
//! as a whole.
string MatchRecognizeNavigationVariable(unique_ptr<ParsedExpression> &inner, const MatchRecognizeIsSymbol &symbols,
                                        const case_insensitive_map_t<string> &universal, const string &function_name);

//! FIRST() and LAST() count from the end of the match they read from, by a constant the two clauses
//! spell the same way.
idx_t MatchRecognizeNavigationOffset(const string &function_name, const ParsedExpression &offset_expr);

//! One field of the matcher's state, read from the column it travels in
unique_ptr<ParsedExpression> MatchRecognizeStateField(const string &state, const string &field);

//! The names a lambda binds for its body, or false when the expression is only spelled like one: the
//! arrow form is also the JSON operator, and which it is only settles when the function is bound.
bool MatchRecognizeLambdaParameters(const ParsedExpression &expr, identifier_set_t &parameters);

} // namespace duckdb
