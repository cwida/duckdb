
#include "duckdb/function/match_recognize.hpp"

#include "duckdb/function/window/match_recognize_functions.hpp"
#include "duckdb/function/window/window_match_recognize.hpp"

#include "duckdb/main/config.hpp"

#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/pattern_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/bound_ref_wrapper.hpp"
#include "duckdb/parser/tableref/match_recognize_ref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"

#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/column_binding_map.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_window_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/match_recognize_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_window.hpp"

namespace duckdb {

//! main's SelectNode and SelectStatement only default construct, so the members are filled in here
static unique_ptr<SelectNode> MakeSelectNode(unique_ptr<TableRef> from_table) {
	auto node = make_uniq<SelectNode>();
	node->from_table = std::move(from_table);
	return node;
}

static unique_ptr<SelectStatement> MakeSelectStatement(unique_ptr<QueryNode> node) {
	auto statement = make_uniq<SelectStatement>();
	statement->node = std::move(node);
	return statement;
}

//! Build the tree the matcher walks. The pattern is never evaluated, only compiled, so it is built
//! directly rather than bound.
static unique_ptr<MatchRecognizePattern> BuildPattern(const ParsedExpression &expr,
                                                      const case_insensitive_map_t<idx_t> &symbol_index) {
	switch (expr.GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		// a leaf carries an index into the symbols rather than the name
		auto result = make_uniq<MatchRecognizePattern>(MatchRecognizePatternType::SYMBOL);
		auto entry = symbol_index.find(expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
		if (entry == symbol_index.end()) {
			throw InternalException("MATCH_RECOGNIZE pattern symbol %s has no condition", expr.ToString());
		}
		result->symbol = entry->second;
		return result;
	}
	case ExpressionType::ANCHOR: {
		auto result = make_uniq<MatchRecognizePattern>(MatchRecognizePatternType::ANCHOR);
		result->at_end = expr.Cast<AnchorExpression>().at_end;
		return result;
	}
	case ExpressionType::ALTERNATION: {
		auto &alternation = expr.Cast<AlternationExpression>();
		auto result = make_uniq<MatchRecognizePattern>(MatchRecognizePatternType::ALTERNATION);
		result->children.push_back(BuildPattern(*alternation.child_left, symbol_index));
		result->children.push_back(BuildPattern(*alternation.child_right, symbol_index));
		return result;
	}
	case ExpressionType::CONCATENATION: {
		auto result = make_uniq<MatchRecognizePattern>(MatchRecognizePatternType::CONCATENATION);
		for (auto &child : expr.Cast<ConcatenationExpression>().children) {
			result->children.push_back(BuildPattern(*child, symbol_index));
		}
		return result;
	}
	case ExpressionType::QUANTIFIER: {
		auto &quantifier = expr.Cast<QuantifiedExpression>();
		auto result = make_uniq<MatchRecognizePattern>(MatchRecognizePatternType::QUANTIFIER);
		result->min_count = quantifier.min_count;
		result->max_count = quantifier.max_count;
		result->excluded = quantifier.excluded;
		result->reluctant = quantifier.reluctant;
		result->children.push_back(BuildPattern(*quantifier.child, symbol_index));
		return result;
	}
	default:
		throw NotImplementedException("Unimplemented pattern expression %s",
		                              ExpressionTypeToString(expr.GetExpressionType()));
	}
}

//! A reference into the input only resolves where the input still is one, and every clause here is
//! evaluated above a subquery of it - so it is computed down there verbatim and read back under a name
//! of its own. Computing rather than rewriting is what keeps two tables' columns of the same name
//! apart, and a struct field behind a qualifier from reading as a column.
struct HoistedInputRefs {
	HoistedInputRefs(GeneratedNames &names_p, const case_insensitive_set_t &input_columns_p,
	                 const case_insensitive_set_t &input_tables_p)
	    : names(names_p), input_columns(input_columns_p), input_tables(input_tables_p) {
	}

	//! The subquery column standing for this reference, allocated on first use
	const string &Hoist(const ColumnRefExpression &colref) {
		// length prefixed, so a dot inside a quoted identifier cannot read as a component boundary
		string key;
		for (auto &name : colref.ColumnNames()) {
			const auto &identifier = name.GetIdentifierName();
			key += to_string(identifier.size());
			key += ":";
			key += identifier;
		}
		auto entry = columns.find(key);
		if (entry != columns.end()) {
			return entry->second;
		}
		auto hoisted = colref.Copy();
		auto column = names.Reserve("__mr_ref_" + to_string(columns.size()));
		// a reference the input itself answers reads the row pattern input, and so every row of the
		// match; one it does not is the surrounding query's, and the same value on all of them
		const auto &root = colref.ColumnNames()[0].GetIdentifierName();
		if (input_columns.count(root) || input_tables.count(root)) {
			universal.emplace(column, colref.ToString());
		}
		hoisted->SetAlias(Identifier(column));
		select_list.push_back(std::move(hoisted));
		return columns.emplace(std::move(key), std::move(column)).first->second;
	}

	GeneratedNames &names;
	const case_insensitive_set_t &input_columns;
	const case_insensitive_set_t &input_tables;
	//! Keyed by what the reference spells, so that one written twice is computed once
	case_insensitive_map_t<string> columns;
	//! The generated names standing for a reference into the input, by what it was written as
	case_insensitive_map_t<string> universal;
	//! The expressions to add to the subquery the input is reached through
	vector<unique_ptr<ParsedExpression>> select_list;
};

//! A reference whose first name is a pattern variable, or one a lambda binds, is not the input's.
//! Everything else is computed against the input, which is also what makes an ambiguous reference say
//! so rather than quietly taking the first of them.
static void HoistInputReferences(unique_ptr<ParsedExpression> &expr, const case_insensitive_set_t &symbols,
                                 const case_insensitive_set_t &aliases, HoistedInputRefs &refs,
                                 vector<identifier_set_t> &lambda_parameters) {
	if (expr->GetExpressionClass() == ExpressionClass::LAMBDA) {
		identifier_set_t parameters;
		if (MatchRecognizeLambdaParameters(*expr, parameters)) {
			auto &lambda = expr->Cast<LambdaExpression>();
			lambda_parameters.push_back(std::move(parameters));
			HoistInputReferences(lambda.RightMutable(), symbols, aliases, refs, lambda_parameters);
			lambda_parameters.pop_back();
			return;
		}
	}
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		auto &names = colref.ColumnNames();
		if (LambdaExpression::IsLambdaParameter(lambda_parameters, names[0])) {
			return;
		}
		// alias.<name> names one of this clause's own measures rather than a column of the input
		const auto is_alias = names.size() == 2 && StringUtil::CIEquals("alias", names[0].GetIdentifierName()) &&
		                      aliases.count(names[1].GetIdentifierName()) > 0;
		if (!is_alias && !symbols.count(names[0].GetIdentifierName())) {
			auto alias = expr->GetAlias();
			expr = make_uniq<ColumnRefExpression>(Identifier(refs.Hoist(colref)));
			expr->SetAlias(std::move(alias));
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		HoistInputReferences(child, symbols, aliases, refs, lambda_parameters);
	});
}

static void HoistInputReferences(unique_ptr<ParsedExpression> &expr, const case_insensitive_set_t &symbols,
                                 const case_insensitive_set_t &aliases, HoistedInputRefs &refs) {
	vector<identifier_set_t> lambda_parameters;
	HoistInputReferences(expr, symbols, aliases, refs, lambda_parameters);
}

//! Whether any part of the pattern sits inside a {- -}
static bool HasExclusion(const ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::QUANTIFIER && expr.Cast<QuantifiedExpression>().excluded) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || HasExclusion(child); });
	return found;
}

//! PREV and NEXT walk the ordered partition rather than the match, so a measure's are computed per
//! input row below the pattern window and read back from above it
static void HoistMeasureNavigation(unique_ptr<ParsedExpression> &expr, const WindowExpression &pattern_window,
                                   const case_insensitive_map_t<vector<string>> &symbols,
                                   const case_insensitive_map_t<string> &universal,
                                   vector<unique_ptr<ParsedExpression>> &hoisted, GeneratedNames &names) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &function = expr->Cast<FunctionExpression>();
		auto function_name = StringUtil::Upper(function.FunctionName().GetIdentifierName());
		// A step always reads the row it steps to, so how much of the match is visible is not its to
		// say: RUNNING and FINAL belong on the navigation it steps from, not on the step (5.6.2)
		if (function.FunctionName() == MATCH_RECOGNIZE_RUNNING_MARKER ||
		    function.FunctionName() == MATCH_RECOGNIZE_FINAL_MARKER) {
			auto &wrapped = function.GetArguments()[0].GetExpression();
			if (wrapped.GetExpressionType() == ExpressionType::FUNCTION) {
				auto wrapped_name =
				    StringUtil::Upper(wrapped.Cast<FunctionExpression>().FunctionName().GetIdentifierName());
				if (wrapped_name == "PREV" || wrapped_name == "NEXT") {
					throw BinderException("%s() always reads the row it steps to, so RUNNING and FINAL cannot be "
					                      "written in front of it",
					                      wrapped_name);
				}
			}
		}
		if (function_name == "PREV" || function_name == "NEXT") {
			auto &arguments = function.GetArgumentsMutable();
			if (arguments.empty() || arguments.size() > 2) {
				throw BinderException("%s() takes an expression and an optional offset", function_name);
			}
			MatchRecognizeRejectNestedStep(arguments[0].GetExpression(), function_name);
			if (MatchRecognizeContainsClassifier(arguments[0].GetExpression())) {
				// no row is classified below the matcher, so this one is read off the finished match
				return;
			}
			for (auto &argument : arguments) {
				HoistMeasureNavigation(argument.GetExpressionMutable(), pattern_window, symbols, universal, hoisted,
				                       names);
			}
			// a step is a fixed distance through the partition, so the offset is a non-negative
			// constant - a negative one would step the other way and a column a different way per row
			if (arguments.size() == 2) {
				MatchRecognizeNavigationOffset(function_name, arguments[1].GetExpression());
			}
			auto inner = std::move(arguments[0].GetExpressionMutable());
			auto variable = MatchRecognizeNavigationVariable(
			    inner, [&](const string &name) { return symbols.find(name) != symbols.end(); }, universal,
			    function_name);
			// A step names the row it starts from rather than the one it reaches: a pattern variable in
			// front of it is the row that variable denotes (5.6.2), and FIRST or LAST within it says
			// which of that variable's rows (5.6.4). Stepping and then reading the row reached is the
			// same as reading the already-stepped column off the row it started from, so the step
			// becomes a column of the projection below and a navigation reads it from there.
			auto stepped = MatchRecognizePeelStep(inner);
			auto navigation = pattern_window.Copy();
			auto &window = navigation->Cast<WindowExpression>();
			window.SetFunctionName(function_name == "PREV" ? "lag" : "lead");
			window.GetArgumentsMutable().push_back(std::move(inner));
			if (arguments.size() == 2) {
				window.GetArgumentsMutable().push_back(std::move(arguments[1].GetExpressionMutable()));
			}
			auto column = names.Reserve("__mr_win");
			window.SetAlias(Identifier(column));
			hoisted.push_back(std::move(navigation));
			// the navigation may be the whole measure, whose alias names the output column
			auto alias = expr->GetAlias();
			if (variable.empty() && !stepped.navigated) {
				// nothing said where to start from, so the step starts where the measure is read
				expr = make_uniq<ColumnRefExpression>(Identifier(column));
			} else {
				expr = stepped.Rebuild(variable, column);
			}
			expr->SetAlias(std::move(alias));
			return;
		}
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		HoistMeasureNavigation(child, pattern_window, symbols, universal, hoisted, names);
	});
}

//! The conditions and the frame are bound against the input but evaluated over the projection above
//! it, so a reference into the input becomes one into that projection
static void RemapToProjection(unique_ptr<Expression> &expr, MatchRecognizeConditionInputs &inputs,
                              column_binding_map_t<idx_t> &input_columns) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		// the matcher evaluates a condition per candidate row, which a subquery cannot be reduced to
		throw BinderException("A DEFINE condition may not contain a subquery");
	}
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &colref = expr->Cast<BoundColumnRefExpression>();
		if (colref.Binding().table_index == inputs.projection.projection_index ||
		    colref.Binding().table_index == inputs.match_number_index) {
			return;
		}
		auto entry = input_columns.find(colref.Binding());
		if (entry == input_columns.end()) {
			auto column = inputs.ProjectAs(expr->Copy(), inputs.generated.Reserve("__mr_read"));
			input_columns[colref.Binding()] = inputs.projection.select_list.size() - 1;
			expr = std::move(column);
			return;
		}
		expr = make_uniq<BoundColumnRefExpression>(
		    colref.GetAlias(), colref.GetReturnType(),
		    ColumnBinding(inputs.projection.projection_index, ProjectionIndex(entry->second)));
		return;
	}
	ExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<Expression> &child) { RemapToProjection(child, inputs, input_columns); });
}

//! The frame the matcher walks is the input's to compute. The binder rejects the spellings that would
//! read a matcher field; this is the boundary that catches one it does not know about.
static void RejectMatcherFields(const Expression &expr, TableIndex match_number_index, const char *clause) {
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Binding().table_index == match_number_index) {
			throw InternalException("MATCH_RECOGNIZE built a %s that reads a field only the matcher supplies", clause);
		}
	});
}

//! Record a column the matcher is handed per row, or report where it already is
static idx_t AddMatcherInput(const unique_ptr<Expression> &column, vector<unique_ptr<Expression>> &children,
                             expression_map_t<idx_t> &child_index) {
	auto entry = child_index.find(*column);
	if (entry != child_index.end()) {
		return entry->second;
	}
	const auto index = children.size();
	children.push_back(column->Copy());
	child_index[*children.back()] = index;
	return index;
}

//! Point a condition's column references at the columns the matcher is handed
static void RebindToMatcherInputs(unique_ptr<Expression> &expr, vector<unique_ptr<Expression>> &children,
                                  expression_map_t<idx_t> &child_index, TableIndex match_number_index) {
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_SUBQUERY) {
		// the matcher evaluates a condition per candidate row, which a subquery cannot be reduced to
		throw BinderException("A DEFINE condition may not contain a subquery");
	}
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		if (expr->Cast<BoundColumnRefExpression>().Binding().table_index == match_number_index) {
			// the matcher supplies this one itself, so the plan hands it no column for it
			return;
		}
		const auto index = AddMatcherInput(expr, children, child_index);
		expr = make_uniq<BoundReferenceExpression>(expr->GetReturnType(), index);
		return;
	}
	ExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<Expression> &child) {
		RebindToMatcherInputs(child, children, child_index, match_number_index);
	});
}

//! Every name that belongs to the clause rather than to the input: the pattern variables, whether
//! they were defined or only mentioned, and the SUBSET names that stand for a set of them.
struct MatchRecognizeSymbols {
	//! Variables a DEFINE names, plus the ones only the PATTERN mentions
	case_insensitive_set_t declared;
	//! Variables the PATTERN mentions, which is what a DEFINE may define
	case_insensitive_set_t in_pattern;
	//! What may qualify a reference: the declared variables and the SUBSET names
	case_insensitive_set_t qualifying;
};

//! A condition may name any pattern variable, including one the PATTERN only mentions, so the whole
//! namespace is collected before the first expression is touched.
static MatchRecognizeSymbols CollectSymbols(const MatchRecognizeConfig &config) {
	MatchRecognizeSymbols symbols;
	for (auto &expr : config.defines_expression_list) {
		symbols.declared.insert(expr->GetAlias().GetIdentifierName());
	}
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *config.pattern, [&](const ColumnRefExpression &colref) {
		    symbols.in_pattern.insert(colref.GetColumnName().GetIdentifierName());
		    symbols.declared.insert(colref.GetColumnName().GetIdentifierName());
	    });
	symbols.qualifying = symbols.declared;
	for (auto &subset : config.subsets) {
		symbols.qualifying.insert(subset.name);
	}
	return symbols;
}

//! What the clause asks for that this implementation does not do, reported before anything is bound
static void ValidateClauses(const MatchRecognizeConfig &config, const MatchRecognizeSymbols &symbols,
                            bool has_exclusion) {
	// {- -} decides which of a match's rows reach the output. ONE ROW PER MATCH reports the match
	// rather than its rows, so there it is permitted and simply has no effect. WITH UNMATCHED ROWS is
	// there to report every row and {- -} is there to drop some: a row it dropped is one a match did
	// cover, so reporting it as unmatched would say the opposite.
	if (has_exclusion && config.rows_per_match == MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_UNMATCHED) {
		throw BinderException("Pattern exclusion syntax {- -} is not allowed with ALL ROWS PER MATCH WITH "
		                      "UNMATCHED ROWS");
	}
	case_insensitive_set_t subset_names;
	for (auto &subset : config.subsets) {
		subset_names.insert(subset.name);
	}
	if (!config.after_match_variable.empty() && !symbols.declared.count(config.after_match_variable) &&
	    !subset_names.count(config.after_match_variable)) {
		// resuming at a variable the pattern never mentions has nowhere to resume from
		throw BinderException("AFTER MATCH SKIP TO \"%s\", which is not a pattern variable of this MATCH_RECOGNIZE",
		                      config.after_match_variable);
	}
	// a measure is a column of the output, and a table does not have two columns of one name
	case_insensitive_set_t measure_names;
	for (auto &expr : config.measures_expression_list) {
		if (!measure_names.insert(expr->GetAlias().GetIdentifierName()).second) {
			throw BinderException("MEASURES names \"%s\" twice", expr->GetAlias().GetIdentifierName());
		}
	}
}

//! The window PREV() and NEXT() become: the partitioning and ordering the matcher walks
static unique_ptr<WindowExpression> BuildWindowTemplate(const MatchRecognizeConfig &config) {
	auto window_template = make_uniq<WindowExpression>("", "", "");
	window_template->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	window_template->WindowEndMutable() = WindowBoundary::UNBOUNDED_FOLLOWING;
	for (auto &expr : config.partition_expressions) {
		window_template->PartitionsMutable().push_back(expr->Copy());
	}
	for (auto &order : config.order_by_expressions) {
		window_template->OrderByMutable().emplace_back(order.type, order.null_order, order.expression->Copy());
	}
	return window_template;
}

//! Rewrite every reference that means something to the input into one of the projection over it
static void HoistClauseReferences(MatchRecognizeConfig &config, const MatchRecognizeSymbols &symbols,
                                  HoistedInputRefs &input_refs) {
	const case_insensitive_set_t no_aliases;
	for (auto &expr : config.partition_expressions) {
		HoistInputReferences(expr, symbols.qualifying, no_aliases, input_refs);
	}
	for (auto &order : config.order_by_expressions) {
		HoistInputReferences(order.expression, symbols.qualifying, no_aliases, input_refs);
	}
	for (auto &expr : config.defines_expression_list) {
		HoistInputReferences(expr, symbols.qualifying, no_aliases, input_refs);
	}
	// a measure may name one written before it, which is not the input's to compute - though a column
	// of the input by the same name still wins, where the measures are bound
	case_insensitive_set_t measure_aliases;
	for (auto &expr : config.measures_expression_list) {
		measure_aliases.insert(expr->GetAlias().GetIdentifierName());
	}
	auto measure_names = symbols.qualifying;
	for (auto &alias : measure_aliases) {
		measure_names.insert(alias);
	}
	for (auto &expr : config.measures_expression_list) {
		HoistInputReferences(expr, measure_names, measure_aliases, input_refs);
	}
}

//! A measure may name a pattern variable or a SUBSET of them; both resolve to a set of symbols
static case_insensitive_map_t<vector<string>> BuildMeasureSymbols(const case_insensitive_set_t &pattern_symbols,
                                                                  const vector<MatchRecognizeSubset> &subsets) {
	case_insensitive_map_t<vector<string>> measure_symbols;
	for (auto &symbol : pattern_symbols) {
		measure_symbols[symbol] = {symbol};
	}
	// there are no unions of unions, so a name declared by an earlier SUBSET is not a member the next
	// one may take - which the size of its expansion does not say, a union of one standing for a
	// single symbol just as a pattern variable does
	case_insensitive_set_t union_names;
	for (auto &subset : subsets) {
		if (measure_symbols.find(subset.name) != measure_symbols.end()) {
			throw BinderException("SUBSET name \"%s\" is already a pattern variable", subset.name);
		}
		vector<string> members;
		for (auto &member : subset.members) {
			if (union_names.count(member)) {
				throw BinderException("SUBSET \"%s\" is built out of \"%s\", which is itself a SUBSET - a union "
				                      "row pattern variable is a union of pattern variables",
				                      subset.name, member);
			}
			auto entry = measure_symbols.find(member);
			if (entry == measure_symbols.end() || entry->second.size() != 1) {
				throw BinderException("SUBSET \"%s\" refers to unknown pattern variable \"%s\"", subset.name, member);
			}
			members.push_back(entry->second[0]);
		}
		union_names.insert(subset.name);
		measure_symbols[subset.name] = std::move(members);
	}
	return measure_symbols;
}

//! The columns the window hands the matcher per row, in the order the conditions address them, with
//! the conditions rewritten to read them by position. Every condition that depends on the match being
//! assembled is marked as such, so that the matcher settles it per candidate row rather than once per
//! partition - which after every match would be quadratic.
static vector<unique_ptr<Expression>>
BuildMatcherInputs(BoundSelectNode &define_node, const vector<MatchRecognizeNavigation> &navigations,
                   vector<MatchRecognizeAggregate> &aggregates, TableIndex match_number_index,
                   vector<unique_ptr<Expression>> &conditions, MatchRecognizeFunctionData &match_data) {
	vector<unique_ptr<Expression>> children;
	expression_map_t<idx_t> child_index;

	// a navigated column is one the matcher fills in per row, so where it sits among the ones it is
	// handed is what the navigation descriptor names
	unordered_set<idx_t> navigation_fields;
	for (auto &navigation : navigations) {
		if (navigation.classifier) {
			// the matcher supplies the classifier itself, in a field placed once its own are counted
			continue;
		}
		auto &projected = define_node.select_list[navigation.column];
		unique_ptr<Expression> column = make_uniq<BoundColumnRefExpression>(
		    projected->GetAlias(), projected->GetReturnType(),
		    ColumnBinding(define_node.projection_index, ProjectionIndex(navigation.column)));
		const auto field = AddMatcherInput(column, children, child_index);
		match_data.navigations.push_back(
		    MatchRecognizeFunctionData::Navigation {navigation.last, navigation.symbol, field, navigation.offset});
		navigation_fields.insert(field);
	}

	// an aggregate reads its operand off the rows the matcher has mapped, so the operand is one of the
	// columns it is handed, the same as a navigation's is
	vector<optional_idx> aggregate_operands;
	for (auto &aggregate : aggregates) {
		if (!aggregate.column.IsValid()) {
			// COUNT(*) reads no column: what it counts is the rows themselves
			aggregate_operands.emplace_back();
			continue;
		}
		auto &projected = define_node.select_list[aggregate.column.GetIndex()];
		unique_ptr<Expression> column = make_uniq<BoundColumnRefExpression>(
		    projected->GetAlias(), projected->GetReturnType(),
		    ColumnBinding(define_node.projection_index, ProjectionIndex(aggregate.column.GetIndex())));
		aggregate_operands.emplace_back(AddMatcherInput(column, children, child_index));
	}

	for (auto &condition : conditions) {
		RebindToMatcherInputs(condition, children, child_index, match_number_index);
	}
	// the matcher's own fields come after the ones the plan supplies: the match number first, then one
	// per aggregate for the value it folds to
	match_data.match_number_field = children.size();
	unordered_set<idx_t> aggregate_fields;
	for (idx_t i = 0; i < aggregates.size(); i++) {
		const auto field = match_data.match_number_field + aggregates[i].slot;
		match_data.aggregates.push_back(MatchRecognizeFunctionData::Aggregate {
		    aggregates[i].symbol, aggregate_operands[i], field, std::move(aggregates[i].expression)});
		aggregate_fields.insert(field);
	}
	for (auto &navigation : navigations) {
		if (!navigation.classifier) {
			continue;
		}
		MatchRecognizeFunctionData::Navigation supplied {
		    navigation.last, navigation.symbol, match_data.match_number_field + navigation.slot, navigation.offset};
		supplied.classifier = true;
		supplied.step = navigation.step;
		match_data.navigations.push_back(supplied);
		navigation_fields.insert(supplied.field);
	}
	for (auto &condition : conditions) {
		bool reads_match_number = false;
		bool reads_navigation = false;
		ExpressionIterator::VisitExpressionMutable<BoundColumnRefExpression>(
		    condition, [&](BoundColumnRefExpression &colref, unique_ptr<Expression> &child) {
			    if (colref.Binding().table_index != match_number_index) {
				    return;
			    }
			    // the match number is the first of them, an aggregate's value one of the rest
			    const auto slot = colref.Binding().column_index;
			    child = make_uniq<BoundReferenceExpression>(colref.GetAlias(), colref.GetReturnType(),
			                                                match_data.match_number_field + slot);
			    reads_match_number = reads_match_number || slot == 0;
		    });
		ExpressionIterator::VisitExpression<BoundReferenceExpression>(
		    *condition, [&](const BoundReferenceExpression &bound_ref) {
			    reads_navigation = reads_navigation || navigation_fields.count(bound_ref.Index()) > 0 ||
			                       aggregate_fields.count(bound_ref.Index()) > 0;
		    });
		match_data.row_scoped.push_back(reads_navigation || reads_match_number);
		match_data.depends_on_match_number = match_data.depends_on_match_number || reads_match_number;
		match_data.conditions.push_back(std::move(condition));
	}
	return children;
}

//! What the pattern window is built from, once the conditions have settled what the matcher reads
//! The table indexes a bound select node needs before anything is bound against it
static void ReserveNodeIndexes(Binder &binder, BoundSelectNode &node) {
	node.projection_index = binder.GenerateTableIndex();
	node.group_index = binder.GenerateTableIndex();
	node.group_projection_index = binder.GenerateTableIndex();
	node.aggregate_index = binder.GenerateTableIndex();
	node.groupings_index = binder.GenerateTableIndex();
	node.window_index = binder.GenerateTableIndex();
	node.prune_index = binder.GenerateTableIndex();
}

//! The names the clause reports of its own, which the output node below reads
struct MatchRecognizeReportedNames {
	//! What the clause partitioned by, under the name it was spelled with
	vector<Identifier> partitions;
	//! The columns ALL ROWS PER MATCH reports ahead of the rest of the input: what the clause
	//! partitioned by, then what it ordered by
	vector<Identifier> leading;
};

//! Taken before hoisting rewrites where the partitioning is computed, so the names are the ones the
//! query wrote. Only a plain reference to a column of the input names one - an expression is not a
//! column of the output to begin with - and a column named twice is still reported once.
static MatchRecognizeReportedNames CollectReportedNames(const MatchRecognizeConfig &config,
                                                        const case_insensitive_set_t &input_column_names) {
	MatchRecognizeReportedNames reported;
	for (auto &expr : config.partition_expressions) {
		reported.partitions.push_back(expr->GetName());
	}
	case_insensitive_set_t seen;
	const auto add_leading = [&](const ParsedExpression &expr) {
		if (expr.GetExpressionType() != ExpressionType::COLUMN_REF) {
			return;
		}
		auto &column = expr.Cast<ColumnRefExpression>().GetColumnName();
		const auto &name = column.GetIdentifierName();
		if (!input_column_names.count(name) || !seen.insert(name).second) {
			return;
		}
		reported.leading.push_back(column);
	};
	for (auto &expr : config.partition_expressions) {
		add_leading(*expr);
	}
	for (auto &order : config.order_by_expressions) {
		add_leading(*order.expression);
	}
	return reported;
}

struct MatchRecognizeWindowInputs {
	unique_ptr<MatchRecognizeFunctionData> match_data;
	vector<unique_ptr<Expression>> children;
	vector<unique_ptr<Expression>> partitions;
	vector<BoundOrderByNode> orders;
};

//! Everything the matcher needs that comes from the clause rather than from binding its expressions
static void FinishMatchData(const MatchRecognizeConfig &config, vector<string> define_symbols,
                            MatchRecognizeFunctionData &match_data) {
	match_data.symbols = std::move(define_symbols);
	// the matcher compares symbols on every candidate row, so a pattern leaf carries an index into the
	// symbols rather than the name
	case_insensitive_map_t<idx_t> symbol_index;
	for (idx_t i = 0; i < match_data.symbols.size(); i++) {
		symbol_index[match_data.symbols[i]] = i;
	}
	match_data.pattern = BuildPattern(*config.pattern, symbol_index);
	match_data.after_match = config.after_match;
	if (!config.after_match_variable.empty()) {
		match_data.after_match_variable = MatchRecognizeDefineColumn(config.after_match_variable);
	}
	for (auto &subset : config.subsets) {
		MatchRecognizeFunctionData::Subset union_variable;
		union_variable.name = MatchRecognizeDefineColumn(subset.name);
		for (auto &member : subset.members) {
			union_variable.members.push_back(MatchRecognizeDefineColumn(member));
		}
		match_data.subsets.push_back(std::move(union_variable));
	}
}

//! The pattern window, and the unnest above it. The window reports every match a row takes part in, so
//! unnesting gives overlapping matches a row each and drops the rows that matched nothing.
static unique_ptr<SelectNode> PlanPatternWindow(Binder &binder, ClientContext &context, BoundStatement bound_define,
                                                TableIndex rows_index, MatchRecognizeWindowInputs inputs,
                                                GeneratedNames &names, const string &spans_column,
                                                const string &state_column, bool unmatched) {
	auto bound_window = make_uniq<BoundWindowExpression>(
	    WindowMatchRecognizeExecutor::ResultType(), nullptr,
	    make_uniq<BoundWindowFunction>(MatchRecognizeFun::GetFunction()), std::move(inputs.match_data));
	bound_window->GetChildrenMutable() = std::move(inputs.children);
	bound_window->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
	bound_window->WindowEndMutable() = WindowBoundary::UNBOUNDED_FOLLOWING;
	bound_window->PartitionsMutable() = std::move(inputs.partitions);
	bound_window->OrderByMutable() = std::move(inputs.orders);

	const auto window_index = binder.GenerateTableIndex();
	auto logical_window = make_uniq<LogicalWindow>(window_index);
	logical_window->expressions.push_back(std::move(bound_window));
	logical_window->AddChild(std::move(bound_define.plan));

	BoundStatement bound_window_result;
	bound_window_result.types = bound_define.types;
	bound_window_result.types.push_back(WindowMatchRecognizeExecutor::ResultType());
	bound_window_result.names = bound_define.names;
	bound_window_result.names.push_back(Identifier(spans_column));
	bound_window_result.plan = std::move(logical_window);

	auto spans_binder = Binder::CreateBinder(context, &binder);
	spans_binder->bind_context.AddGenericBinding(rows_index, Identifier(names.Reserve("__mr_rows")), bound_define.names,
	                                             bound_define.types);
	spans_binder->bind_context.AddGenericBinding(window_index, Identifier(names.Reserve("__mr_spans")),
	                                             {Identifier(spans_column)},
	                                             {WindowMatchRecognizeExecutor::ResultType()});

	auto unnest_node =
	    MakeSelectNode(make_uniq<BoundRefWrapper>(std::move(bound_window_result), std::move(spans_binder)));
	auto spans_star = make_uniq<StarExpression>();
	spans_star->ExcludeListMutable().insert(QualifiedColumnName(Identifier(spans_column)));
	unnest_node->select_list.push_back(std::move(spans_star));

	unique_ptr<ParsedExpression> spans = make_uniq<ColumnRefExpression>(Identifier(spans_column));
	if (unmatched) {
		// unnesting drops a row whose list is empty, which is every row in no match. WITH UNMATCHED
		// ROWS keeps those, with nothing where the match would be.
		vector<unique_ptr<ParsedExpression>> length;
		length.push_back(spans->Copy());
		CaseCheck matched;
		matched.when_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN,
		                                                    make_uniq<FunctionExpression>("len", std::move(length)),
		                                                    ConstantExpression::Integer(0));
		matched.then_expr = spans->Copy();
		vector<unique_ptr<ParsedExpression>> nothing;
		nothing.push_back(make_uniq<CastExpression>(ListType::GetChildType(WindowMatchRecognizeExecutor::ResultType()),
		                                            ConstantExpression::Null()));
		auto keep_row = make_uniq<CaseExpression>();
		keep_row->CaseChecksMutable().push_back(std::move(matched));
		keep_row->ElseMutable() = make_uniq<FunctionExpression>("list_value", std::move(nothing));
		spans = std::move(keep_row);
	}
	vector<unique_ptr<ParsedExpression>> spans_argument;
	spans_argument.push_back(std::move(spans));
	auto unnest_spans = make_uniq<FunctionExpression>("unnest", std::move(spans_argument));
	unnest_spans->SetAlias(Identifier(state_column));
	unnest_node->select_list.push_back(std::move(unnest_spans));
	return unnest_node;
}

//! Report this only on a row that is part of a match: WITH UNMATCHED ROWS carries the rows that are
//! not, and those have no match for a measure to be about.
static unique_ptr<ParsedExpression> OnlyWhenMatchedRow(const string &state, unique_ptr<ParsedExpression> value) {
	auto matched = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL,
	                                             make_uniq<ColumnRefExpression>(Identifier(state)));
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(matched);
	check.then_expr = std::move(value);
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = ConstantExpression::Null();
	return std::move(result);
}

//! How the output names what the measures computed: the name the user gave it, which the output
//! reports, and the internal one the projections below refer to it by
struct MatchRecognizeMeasureNames {
	vector<Identifier> aliases;
	vector<string> columns;
};

//! The rows the clause reports, out of the rows its measures were computed over
static unique_ptr<SelectNode> BuildOutputNode(const MatchRecognizeConfig &config, unique_ptr<SelectNode> select_node,
                                              const MatchRecognizeMeasureNames &measures,
                                              const vector<Identifier> &partition_names,
                                              const vector<Identifier> &leading_columns,
                                              const HoistedInputRefs &input_refs, const string &state_column,
                                              bool has_exclusion) {
	auto measures_select = MakeSelectStatement(std::move(select_node));
	auto output_node = MakeSelectNode(make_uniq<SubqueryRef>(std::move(measures_select)));
	// a row in no match has no measures to report, only itself
	const auto unmatched = config.rows_per_match == MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_UNMATCHED;
	// both shapes report the measures after the columns that identify the match
	vector<unique_ptr<ParsedExpression>> measure_list;
	for (idx_t i = 0; i < measures.columns.size(); i++) {
		unique_ptr<ParsedExpression> measure = make_uniq<ColumnRefExpression>(Identifier(measures.columns[i]));
		if (unmatched) {
			measure = OnlyWhenMatchedRow(state_column, std::move(measure));
		}
		measure->SetAlias(measures.aliases[i]);
		measure_list.push_back(std::move(measure));
	}
	if (MatchRecognizeReportsRows(config.rows_per_match)) {
		// the matcher's state has served the measures and the filter, and stops here, as do the columns
		// the projections below read the clause's own values by
		auto output_star = make_uniq<StarExpression>();
		output_star->ExcludeListMutable().insert(QualifiedColumnName(Identifier(state_column)));
		for (auto &entry : input_refs.columns) {
			output_star->ExcludeListMutable().insert(QualifiedColumnName(Identifier(entry.second)));
		}
		for (auto &column : measures.columns) {
			output_star->ExcludeListMutable().insert(QualifiedColumnName(Identifier(column)));
		}
		// ALL ROWS PER MATCH reports the partitioning columns, then the ordering columns, then the
		// measures, then what is left of the input - the order that lines the columns up against the
		// ONE ROW PER MATCH shape when a query is toggled between the two
		for (auto &column : leading_columns) {
			output_star->ExcludeListMutable().insert(QualifiedColumnName(column));
			output_node->select_list.push_back(make_uniq<ColumnRefExpression>(column));
		}
		for (auto &measure : measure_list) {
			output_node->select_list.push_back(std::move(measure));
		}
		output_node->select_list.push_back(std::move(output_star));
	} else {
		// ONE ROW PER MATCH reports the match rather than its rows, so the only columns it can report
		// are the ones identifying the match: what it was partitioned by, and what was measured
		if (config.partition_expressions.empty() && measures.aliases.empty()) {
			throw BinderException("MATCH_RECOGNIZE with ONE ROW PER MATCH reports the match rather than its rows, so "
			                      "without MEASURES or PARTITION BY it has no columns to return");
		}
		for (idx_t i = 0; i < config.partition_expressions.size(); i++) {
			auto partition = config.partition_expressions[i]->Copy();
			partition->SetAlias(partition_names[i]);
			output_node->select_list.push_back(std::move(partition));
		}
		for (auto &measure : measure_list) {
			output_node->select_list.push_back(std::move(measure));
		}
	}
	if (MatchRecognizeReportsRows(config.rows_per_match)) {
		// an excluded row still belongs to the match, so it is dropped here rather than before the
		// measures: the aggregates over the match have to have seen it. A row in no match is excluded
		// by nothing, which is not the same as being excluded.
		unique_ptr<ParsedExpression> keep;
		if (has_exclusion) {
			keep = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
			                                     MatchRecognizeStateField(state_column, "is_excluded"));
			if (unmatched) {
				// a row in no match is excluded from nothing, and its state has no field to read
				auto no_match = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL,
				                                              make_uniq<ColumnRefExpression>(Identifier(state_column)));
				keep = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(no_match),
				                                        std::move(keep));
			}
		}
		if (config.rows_per_match == MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_OMIT_EMPTY) {
			auto not_empty = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT,
			                                               MatchRecognizeStateField(state_column, "is_empty"));
			keep = keep ? unique_ptr<ParsedExpression>(make_uniq<ConjunctionExpression>(
			                  ExpressionType::CONJUNCTION_AND, std::move(keep), std::move(not_empty)))
			            : unique_ptr<ParsedExpression>(std::move(not_empty));
		}
		output_node->where_clause = std::move(keep);
	} else {
		// the last row is the one reported, which is the FINAL semantics a bare column has here
		output_node->where_clause = MatchRecognizeStateField(state_column, "is_match_end");
	}
	return output_node;
}

//! One condition per pattern variable, in the order the matcher's symbols are numbered. A variable the
//! PATTERN mentions but no DEFINE defines matches every row - or, under DEFINE AUTO, the column of the
//! same name reading as true.
static vector<unique_ptr<Expression>> BindDefineConditions(MatchRecognizeDefineBinder &condition_binder,
                                                           MatchRecognizeConfig &config,
                                                           const MatchRecognizeSymbols &symbols,
                                                           case_insensitive_set_t &pattern_symbols,
                                                           vector<string> &define_symbols) {
	vector<unique_ptr<Expression>> conditions;
	for (auto &expr : config.defines_expression_list) {
		auto define_name = expr->GetAlias().GetIdentifierName();
		D_ASSERT(!define_name.empty());
		if (pattern_symbols.find(define_name) != pattern_symbols.end()) {
			// a symbol stands for one condition, so a second one for the same symbol has nowhere to go
			throw BinderException("MATCH_RECOGNIZE defines pattern variable \"%s\" more than once", define_name);
		}
		if (!symbols.in_pattern.count(define_name)) {
			// a condition for a variable the pattern never mentions decides nothing at all
			throw BinderException("MATCH_RECOGNIZE defines \"%s\", which its PATTERN does not use", define_name);
		}
		condition_binder.BeginDefine(define_name);
		// the matcher reads a condition as a boolean, so the plan has to produce one
		unique_ptr<ParsedExpression> condition = make_uniq<CastExpression>(LogicalType::BOOLEAN, std::move(expr));
		conditions.push_back(condition_binder.Bind(condition));
		pattern_symbols.insert(define_name);
		define_symbols.push_back(MatchRecognizeDefineColumn(define_name));
	}

	// from here on the pattern names the internal symbols the matcher reports
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    *config.pattern, [&](ColumnRefExpression &colref) {
		    D_ASSERT(colref.ColumnNames().size() == 1);
		    colref.ColumnNamesMutable() = {
		        Identifier(MatchRecognizeDefineColumn(colref.GetColumnName().GetIdentifierName()))};
	    });

	vector<string> undefined_symbols;
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    *config.pattern, [&](const ColumnRefExpression &colref) {
		    auto column_name = colref.ColumnNames()[0].GetIdentifierName();
		    for (auto &existing : define_symbols) {
			    if (StringUtil::CIEquals(existing, column_name)) {
				    return;
			    }
		    }
		    for (auto &existing : undefined_symbols) {
			    if (StringUtil::CIEquals(existing, column_name)) {
				    return;
			    }
		    }
		    undefined_symbols.push_back(column_name);
	    });
	for (auto &column_name : undefined_symbols) {
		define_symbols.push_back(column_name);
		const auto symbol = MatchRecognizeSymbolName(column_name);
		unique_ptr<ParsedExpression> condition;
		if (config.define_auto) {
			condition =
			    make_uniq<CastExpression>(LogicalType::BOOLEAN, make_uniq<ColumnRefExpression>(Identifier(symbol)));
		} else {
			condition = make_uniq<CastExpression>(LogicalType::BOOLEAN, ConstantExpression::Boolean(true));
		}
		condition_binder.BeginDefine(symbol);
		conditions.push_back(condition_binder.Bind(condition));
		pattern_symbols.insert(symbol);
	}
	return conditions;
}

BoundStatement Binder::Bind(MatchRecognizeRef &ref) {
	// MATCH_RECOGNIZE is planned as a stack of select nodes:
	//   1. the input, plus one boolean column per DEFINE
	//   2. the pattern window, which classifies every row of every match; rows outside a match are
	//      dropped here
	//   3. the MEASURES, computed across the match a row belongs to
	//   4. for ONE ROW PER MATCH, a filter down to the row each match starts on

	// the input is bound once, here, while its tables and qualified columns are still what the query
	// wrote, and before this clause generates any name of its own
	auto input_binder = Binder::CreateBinder(context, this);
	auto bound_input = input_binder->Bind(*ref.input);
	// what the columns are called is what the bind context says, not what the table ref bind reports
	case_insensitive_set_t input_names;
	case_insensitive_set_t input_tables;
	for (auto &binding : input_binder->bind_context.GetBindingsList()) {
		input_tables.insert(binding->GetAlias().GetIdentifierName());
		for (auto &name : binding->GetColumnNames()) {
			input_names.insert(name.GetIdentifierName());
		}
	}
	// a correlated reference is the surrounding query's to resolve, so it is carried out to it
	MoveCorrelatedExpressions(*input_binder);
	auto input_ref = make_uniq<BoundRefWrapper>(std::move(bound_input), std::move(input_binder));

	// The matcher's state travels between the select nodes below in a column of its own. Every generated
	// column is named apart from the input's and from each other, which is also what keeps two stacked
	// clauses apart.
	// the output shape below still needs to know which names the input brought, and the generated
	// names take ownership of the set
	const case_insensitive_set_t input_column_names = input_names;
	GeneratedNames names(std::move(input_names));
	const string state_column = names.Reserve("__pattern_window");
	const string spans_column = names.Reserve(state_column + "_spans");
	HoistedInputRefs input_refs(names, input_column_names, input_tables);

	const auto symbols = CollectSymbols(*ref.config);
	// {- -} only decides which of a match's rows reach the output, so it is also read again below
	const bool has_exclusion = HasExclusion(*ref.config->pattern);
	ValidateClauses(*ref.config, symbols, has_exclusion);

	const auto reported = CollectReportedNames(*ref.config, input_column_names);
	HoistClauseReferences(*ref.config, symbols, input_refs);

	// The hoisted references sit in a projection of their own, so the navigation windows a DEFINE turns
	// into can order by them. It is there even when nothing was hoisted, because it is also what makes
	// the input's columns addressable by position: a base table hands out a column binding only once
	// something has asked for that column.
	auto refs_node = MakeSelectNode(std::move(input_ref));
	refs_node->select_list.push_back(make_uniq<StarExpression>());
	for (auto &expr : input_refs.select_list) {
		refs_node->select_list.push_back(std::move(expr));
	}
	unique_ptr<TableRef> input_table = make_uniq<SubqueryRef>(MakeSelectStatement(std::move(refs_node)));

	auto window_template = BuildWindowTemplate(*ref.config);

	// what the matcher reads that is not the row being tested is computed in a projection of its own,
	// built as the conditions are bound
	auto define_binder = Binder::CreateBinder(context, this);

	BoundSelectNode define_node;
	define_node.from_table = define_binder->Bind(*input_table);
	ReserveNodeIndexes(*this, define_node);

	// the input's own columns pass through, because the output still reports them
	column_binding_map_t<idx_t> input_columns;
	for (auto &binding : define_binder->bind_context.GetBindingsList()) {
		auto &column_names = binding->GetColumnNames();
		auto &column_types = binding->GetColumnTypes();
		for (idx_t i = 0; i < column_names.size(); i++) {
			const ColumnBinding source(binding->GetIndex(), ProjectionIndex(i));
			input_columns[source] = define_node.select_list.size();
			define_node.select_list.push_back(
			    make_uniq<BoundColumnRefExpression>(column_names[i], column_types[i], source));
			define_node.names.push_back(column_names[i]);
			define_node.types.push_back(column_types[i]);
		}
	}

	vector<string> hidden_columns;
	vector<MatchRecognizeNavigation> navigations;
	vector<MatchRecognizeAggregate> condition_aggregates;
	// MATCH_NUMBER() is the one thing a condition reads that the plan does not supply. Until the field
	// it lands in is known it is a column of a table no operator produces - a column rather than a
	// reference, so that it is captured out of a lambda body the way any other value is.
	const auto match_number_index = GenerateTableIndex();
	unique_ptr<Expression> match_number_ref = make_uniq<BoundColumnRefExpression>(
	    Identifier("match_number"), LogicalType::UBIGINT, ColumnBinding(match_number_index, ProjectionIndex(0)));

	MatchRecognizeConditionInputs inputs {define_node, match_number_index, hidden_columns,
	                                      names,       navigations,        condition_aggregates};

	MatchRecognizeDefineBinder condition_binder(*define_binder, context, define_node, inputs, *window_template,
	                                            symbols.qualifying, input_refs.universal, match_number_ref);
	case_insensitive_set_t pattern_symbols;
	vector<string> define_symbols;
	auto define_conditions =
	    BindDefineConditions(condition_binder, *ref.config, symbols, pattern_symbols, define_symbols);

	const auto measure_symbols = BuildMeasureSymbols(pattern_symbols, ref.config->subsets);

	// PREV() and NEXT() in MEASURES walk the matcher's input, so they are computed down here too
	vector<unique_ptr<ParsedExpression>> measure_navigation;
	for (auto &expr : ref.config->measures_expression_list) {
		HoistMeasureNavigation(expr, *window_template, measure_symbols, input_refs.universal, measure_navigation,
		                       names);
	}
	if (!measure_navigation.empty()) {
		SelectBinder navigation_binder(*define_binder, context, define_node);
		for (auto &expr : measure_navigation) {
			auto column = expr->GetAlias().GetIdentifierName();
			inputs.ProjectAs(navigation_binder.Bind(expr), column);
		}
	}

	auto match_data = make_uniq<MatchRecognizeFunctionData>();

	// the frame the matcher walks is evaluated over the same projection the conditions are
	vector<unique_ptr<Expression>> bound_partitions;
	for (auto &expr : ref.config->partition_expressions) {
		auto partition = expr->Copy();
		condition_binder.BeginFrame(MatchRecognizeScope::PARTITION_BY);
		bound_partitions.push_back(condition_binder.Bind(partition));
		RejectMatcherFields(*bound_partitions.back(), match_number_index, "PARTITION BY");
	}
	auto &order_config = DBConfig::GetConfig(context);
	vector<BoundOrderByNode> bound_orders;
	for (auto &order : ref.config->order_by_expressions) {
		auto expr = order.expression->Copy();
		// an ordering the query left unsaid is resolved here rather than reaching the sorter unresolved
		const auto type = order_config.ResolveOrder(context, order.type);
		const auto null_order = order_config.ResolveNullOrder(context, type, order.null_order);
		condition_binder.BeginFrame(MatchRecognizeScope::ORDER_BY);
		bound_orders.emplace_back(type, null_order, condition_binder.Bind(expr));
		RejectMatcherFields(*bound_orders.back().expression, match_number_index, "ORDER BY");
	}

	// everything bound against the input now reads the projection that computes it
	for (auto &condition : define_conditions) {
		RemapToProjection(condition, inputs, input_columns);
	}
	for (auto &partition : bound_partitions) {
		RemapToProjection(partition, inputs, input_columns);
	}
	for (auto &order : bound_orders) {
		RemapToProjection(order.expression, inputs, input_columns);
	}

	auto children = BuildMatcherInputs(define_node, navigations, condition_aggregates, match_number_index,
	                                   define_conditions, *match_data);

	MoveCorrelatedExpressions(*define_binder);

	// the projection is complete once the matcher knows what it reads, so it can be planned
	define_node.column_count = define_node.select_list.size();
	BoundStatement bound_define;
	bound_define.types = define_node.types;
	bound_define.names = define_node.names;
	bound_define.plan = CreatePlan(define_node);

	FinishMatchData(*ref.config, std::move(define_symbols), *match_data);

	MatchRecognizeWindowInputs window_inputs {std::move(match_data), std::move(children), std::move(bound_partitions),
	                                          std::move(bound_orders)};
	auto spans_node =
	    PlanPatternWindow(*this, context, std::move(bound_define), define_node.GetRootIndex(), std::move(window_inputs),
	                      names, spans_column, state_column,
	                      ref.config->rows_per_match == MatchRecognizeRows::MATCH_RECOGNIZE_ROWS_ALL_UNMATCHED);

	// MEASURES are projected on top of the pattern window, where the match a row belongs to is known,
	// by a binder of their own. It stays here rather than in a stage of its own because binding a
	// table ref and planning a node are the Binder's own to do.
	const auto all_rows = MatchRecognizeReportsRows(ref.config->rows_per_match);
	auto measures_binder = Binder::CreateBinder(context, this);
	auto spans_ref = make_uniq<SubqueryRef>(MakeSelectStatement(std::move(spans_node)));

	BoundSelectNode measures;
	measures.from_table = measures_binder->Bind(*spans_ref);
	ReserveNodeIndexes(*this, measures);

	// the DEFINE columns are an implementation detail, so they do not reach the output
	const case_insensitive_set_t hidden(hidden_columns.begin(), hidden_columns.end());
	for (auto &binding : measures_binder->bind_context.GetBindingsList()) {
		auto &column_names = binding->GetColumnNames();
		auto &column_types = binding->GetColumnTypes();
		for (idx_t i = 0; i < column_names.size(); i++) {
			if (hidden.count(column_names[i].GetIdentifierName())) {
				continue;
			}
			measures.select_list.push_back(make_uniq<BoundColumnRefExpression>(
			    column_names[i], column_types[i], ColumnBinding(binding->GetIndex(), ProjectionIndex(i))));
			measures.names.push_back(column_names[i]);
			measures.types.push_back(column_types[i]);
		}
	}

	// A measure is named twice: by the name the user gave it, which the output reports, and by one of
	// its own, which the projections below refer to it by - so a measure named after a column of the
	// input does not make that column unreachable. The alias map is what lets a measure name an
	// earlier one.
	for (idx_t i = 0; i < ref.config->measures_expression_list.size(); i++) {
		auto &expr = ref.config->measures_expression_list[i];
		measures.bind_state.alias_map[expr->GetAlias()] = i;
		measures.bind_state.original_expressions.push_back(expr->Copy());
	}

	MatchRecognizeMeasureNames measure_names;
	{
		MatchRecognizeMeasureBinder measure_expression_binder(*measures_binder, context, measures, state_column,
		                                                      *ref.config, measure_symbols, input_refs.universal,
		                                                      all_rows);
		for (auto &expr : ref.config->measures_expression_list) {
			D_ASSERT(!expr->GetAlias().empty());
			measure_names.aliases.push_back(expr->GetAlias());
			measure_names.columns.push_back(names.Reserve("__mr_measure_" + to_string(measure_names.columns.size())));
			auto bound = measure_expression_binder.Bind(expr);
			// one more measure is now there to be named by the ones after it
			measures.bound_column_count++;
			bound->SetAlias(Identifier(measure_names.columns.back()));
			measures.names.emplace_back(measure_names.columns.back());
			measures.types.push_back(bound->GetReturnType());
			measures.select_list.push_back(std::move(bound));
		}
	}
	MoveCorrelatedExpressions(*measures_binder);
	measures.column_count = measures.select_list.size();

	BoundStatement bound_measures;
	bound_measures.types = measures.types;
	bound_measures.names = measures.names;
	bound_measures.plan = CreatePlan(measures);

	// the level above sees the measures and not the columns they were computed from
	auto output_binder = Binder::CreateBinder(context, this);
	output_binder->bind_context.AddGenericBinding(measures.GetRootIndex(), Identifier(names.Reserve("__mr_measures")),
	                                              bound_measures.names, bound_measures.types);

	auto select_node = MakeSelectNode(make_uniq<BoundRefWrapper>(std::move(bound_measures), std::move(output_binder)));
	select_node->select_list.push_back(make_uniq<StarExpression>());

	select_node = BuildOutputNode(*ref.config, std::move(select_node), measure_names, reported.partitions,
	                              reported.leading, input_refs, state_column, has_exclusion);

	auto child_binder = Binder::CreateBinder(context, this);
	auto result = child_binder->Bind(*select_node);
	MoveCorrelatedExpressions(*child_binder);
	const auto alias = !ref.alias.empty() ? ref.alias : Identifier("__match_recognize_table");
	auto output_names = BindContext::AliasColumnNames(alias, result.names, ref.column_name_alias);
	bind_context.AddGenericBinding(result.plan->GetRootIndex(), alias, output_names, result.types);
	return result;
}

} // namespace duckdb
