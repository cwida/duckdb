#include "duckdb/planner/expression_binder/match_recognize_binder.hpp"

#include "duckdb/function/function_binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"

#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/table_binding.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// What a value read out of the match is worth
//===--------------------------------------------------------------------===//
//! The field a value travels in while it is carried to the row that reports it
static constexpr const char *MATCH_RECOGNIZE_VALUE_FIELD = "v";

static unique_ptr<ParsedExpression> CreateStructExtract(unique_ptr<ParsedExpression> value, const string &child_name) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(value));
	children.push_back(ConstantExpression::String(child_name));
	return make_uniq<FunctionExpression>("struct_extract", std::move(children));
}

static unique_ptr<ParsedExpression> CreateStructExtract(const string &column_name, const string &child_name) {
	return CreateStructExtract(make_uniq<ColumnRefExpression>(Identifier(column_name)), child_name);
}

unique_ptr<ParsedExpression> MatchRecognizeStateField(const string &state, const string &field) {
	return CreateStructExtract(state, field);
}

//! read. The row the match is reported on is not one of them.
static unique_ptr<ParsedExpression> OnlyWhenMatched(const string &state, unique_ptr<ParsedExpression> value) {
	auto in_match = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, CreateStructExtract(state, "is_empty"));
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(in_match);
	check.then_expr = std::move(value);
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = ConstantExpression::Null();
	return std::move(result);
}

//! struct_pack(v := <value>) is never NULL, which keeps a NULL the value holds apart from the NULL that
//! masks a row the variable did not match - the one MatchScopedValue walks back over
static unique_ptr<ParsedExpression> PackValue(const string &state, unique_ptr<ParsedExpression> value) {
	value->SetAlias(Identifier(MATCH_RECOGNIZE_VALUE_FIELD));
	vector<unique_ptr<ParsedExpression>> fields;
	fields.push_back(std::move(value));
	auto packed = make_uniq<FunctionExpression>("struct_pack", std::move(fields));

	auto in_match = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, CreateStructExtract(state, "is_empty"));
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(in_match);
	check.then_expr = std::move(packed);
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = ConstantExpression::Null();
	return std::move(result);
}

//! CASE WHEN <classifier> IN (symbols) THEN <column> END - NULL on every row none of them matched.
//! A plain pattern variable stands for itself; a SUBSET name stands for all of its members.
static unique_ptr<ParsedExpression> ClassifierMatches(const string &state, const vector<string> &symbols) {
	D_ASSERT(!symbols.empty());
	vector<unique_ptr<ParsedExpression>> in_children;
	in_children.push_back(CreateStructExtract(state, "classifier"));
	for (auto &symbol : symbols) {
		in_children.push_back(ConstantExpression::String(symbol));
	}
	return make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(in_children));
}

static unique_ptr<ParsedExpression> ClassifiedValue(const string &state, const vector<string> &symbols,
                                                    unique_ptr<ParsedExpression> value) {
	auto matches_symbol = ClassifierMatches(state, symbols);
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(matches_symbol);
	check.then_expr = std::move(value);
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = ConstantExpression::Null();
	return std::move(result);
}

//! An ordering as the sorter will apply it: a reversed window has to spell both out, because "the
//! opposite of the default" is not something the enums can say
static void ResolveOrder(ClientContext &context, OrderType &type, OrderByNullType &null_order) {
	auto &config = DBConfig::GetConfig(context);
	type = config.ResolveOrder(context, type);
	null_order = config.ResolveNullOrder(context, type, null_order);
}

//! A reference to <symbol>.<column> resolves to that column on the last row the variable matched.
//! ONE ROW PER MATCH reports a finished match, so it sees the whole match (FINAL semantics); ALL ROWS
//! PER MATCH reports progress, so it only sees the match up to the current row (RUNNING semantics).
static void ScopeToMatch(ClientContext &context, const string &state, WindowExpression &window,
                         const MatchRecognizeConfig &config, bool running, bool reversed = false) {
	// walked backwards, the rows that came before the current one come after it
	window.WindowStartMutable() =
	    running && reversed ? WindowBoundary::CURRENT_ROW_ROWS : WindowBoundary::UNBOUNDED_PRECEDING;
	window.WindowEndMutable() =
	    running && !reversed ? WindowBoundary::CURRENT_ROW_ROWS : WindowBoundary::UNBOUNDED_FOLLOWING;

	// matches are numbered within a partition, so both are needed to identify one
	for (auto &expr : config.partition_expressions) {
		window.PartitionsMutable().push_back(expr->Copy());
	}
	window.PartitionsMutable().push_back(CreateStructExtract(state, "match_number"));
	// windows that leave the order unsaid can be grouped with one that says the opposite
	for (auto &order : config.order_by_expressions) {
		auto type = order.type;
		auto null_order = order.null_order;
		if (reversed) {
			ResolveOrder(context, type, null_order);
			type = type == OrderType::DESCENDING ? OrderType::ASCENDING : OrderType::DESCENDING;
			null_order =
			    null_order == OrderByNullType::NULLS_FIRST ? OrderByNullType::NULLS_LAST : OrderByNullType::NULLS_FIRST;
		}
		window.OrderByMutable().emplace_back(type, null_order, order.expression->Copy());
	}
	// a stable descending sort is not the reverse of a stable ascending one, so ordering on the row's
	// place in the partition last is what leaves the two directions nothing tied to disagree about
	window.OrderByMutable().emplace_back(reversed ? OrderType::DESCENDING : OrderType::ASCENDING,
	                                     OrderByNullType::NULLS_LAST, CreateStructExtract(state, "row_index"));
}

//! Reports a packed value from the first or last row of the match that carries one, which is what
//! IGNORE NULLS makes it rather than the first or last row of the match
static unique_ptr<ParsedExpression> MatchScopedValue(ClientContext &context, const string &state,
                                                     const MatchRecognizeConfig &config,
                                                     unique_ptr<ParsedExpression> packed, bool running,
                                                     bool first = false, idx_t offset = 0) {
	// nth_value counts from the frame's first row, so counting back from the match's last one is the
	// same window walked the other way round
	const auto reversed = !first && offset > 0;
	auto window =
	    make_uniq<WindowExpression>("", "", offset > 0 ? "nth_value" : (first ? "first_value" : "last_value"));
	window->GetArgumentsMutable().emplace_back(std::move(packed));
	if (offset > 0) {
		window->GetArgumentsMutable().emplace_back(ConstantExpression::Integer(NumericCast<int64_t>(offset + 1)));
	}
	window->HasIgnoreNullsMutable() = true;
	window->IgnoreNullsMutable() = true;
	ScopeToMatch(context, state, *window, config, running, reversed);
	return CreateStructExtract(std::move(window), MATCH_RECOGNIZE_VALUE_FIELD);
}

//! Whether a call is spelled the way only an aggregate can be
static bool HasAggregateModifiers(const FunctionExpression &function) {
	return function.Distinct() || function.Filter() || (function.OrderBy() && !function.OrderBy()->orders.empty());
}

idx_t MatchRecognizeNavigationOffset(const string &function_name, const ParsedExpression &offset_expr) {
	if (offset_expr.GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
		throw BinderException("The offset of %s() must be a constant", function_name);
	}
	auto offset_value = offset_expr.Cast<ConstantExpression>().GetLiteral().ToValue();
	if (offset_value.IsNull()) {
		throw BinderException("The offset of %s() must be a non-negative integer, not NULL", function_name);
	}
	if (!offset_value.DefaultTryCastAs(LogicalType::UBIGINT) || offset_value.IsNull()) {
		throw BinderException("The offset of %s() must be a non-negative integer", function_name);
	}
	const auto offset = offset_value.GetValue<uint64_t>();
	// MEASURES counts the offset from one, as the window function it lowers to takes it, and a match
	// no rows can reach is still an offset the two clauses have to agree on rather than wrap around
	if (offset >= NumericCast<uint64_t>(NumericLimits<int64_t>::Maximum())) {
		throw BinderException("The offset of %s() is larger than any match can have rows", function_name);
	}
	return NumericCast<idx_t>(offset);
}

unique_ptr<ParsedExpression> MatchRecognizeWithoutQualifier(const ColumnRefExpression &colref) {
	auto copy = colref.Copy();
	auto &copied = copy->Cast<ColumnRefExpression>();
	auto names = copied.ColumnNames();
	D_ASSERT(names.size() > 1);
	names.erase(names.begin());
	copied.ColumnNamesMutable() = std::move(names);
	return copy;
}

bool MatchRecognizeLambdaParameters(const ParsedExpression &expr, identifier_set_t &parameters) {
	auto &lambda = expr.Cast<LambdaExpression>();
	if (lambda.GetLambdaSyntaxType() != LambdaSyntaxType::LAMBDA_KEYWORD) {
		return false;
	}
	string error;
	auto column_refs = lambda.ExtractColumnRefExpressions(error);
	if (!error.empty()) {
		return false;
	}
	for (auto &column_ref : column_refs) {
		auto &names = column_ref.get().Cast<ColumnRefExpression>().ColumnNames();
		if (names.size() != 1) {
			return false;
		}
		parameters.insert(names[0]);
	}
	return true;
}

//! What a value that is not the candidate row is being computed for, for the message that says so
static const char *ScopeName(MatchRecognizeScope scope) {
	switch (scope) {
	case MatchRecognizeScope::WINDOW:
		return "a window function, which is computed over the whole partition before there is a match";
	case MatchRecognizeScope::NAVIGATED:
		return "the expression a navigation reads, which is computed for every row of the input";
	case MatchRecognizeScope::PARTITION_BY:
		return "PARTITION BY, which the matcher walks rather than produces";
	case MatchRecognizeScope::ORDER_BY:
		return "ORDER BY, which the matcher walks rather than produces";
	default:
		throw InternalException("MATCH_RECOGNIZE has no name for this scope");
	}
}

//! Binds one expression in a scope of its own, leaving the enclosing one behind afterwards
struct ScopedScope {
	ScopedScope(MatchRecognizeScope &current, MatchRecognizeScope scope) : current(current), saved(current) {
		current = scope;
	}
	~ScopedScope() {
		current = saved;
	}
	MatchRecognizeScope &current;
	const MatchRecognizeScope saved;
};

//! Whether an enclosing lambda binds this name. A pattern variable and a lambda parameter can be
//! spelled the same way, and the lambda's is the one in scope.
static bool BoundByLambda(optional_ptr<vector<DummyBinding>> lambda_bindings, const Identifier &name) {
	if (!lambda_bindings) {
		return false;
	}
	for (auto &binding : *lambda_bindings) {
		if (binding.HasMatchingBinding(name)) {
			return true;
		}
	}
	return false;
}

//===--------------------------------------------------------------------===//
// Reading the rows of a pattern variable
//===--------------------------------------------------------------------===//
//! A reference qualified by a pattern variable reads the rows that variable matched. The qualifier is
//! this clause's name rather than the input's, so it is resolved here: it is dropped from the
//! reference, and the variable it named is reported back through \p scope.
//! CLASSIFIER(), with or without a variable in front of it
static bool IsClassifier(const ParsedExpression &expr) {
	return expr.GetExpressionType() == ExpressionType::FUNCTION &&
	       StringUtil::CIEquals(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName(), "classifier") &&
	       expr.Cast<FunctionExpression>().GetArguments().size() <= 1;
}

//! What CLASSIFIER(X) has become once its variable has scoped it: the classifier of the row reached
static bool IsClassifierField(const ParsedExpression &expr) {
	return expr.GetExpressionType() == ExpressionType::COLUMN_REF &&
	       expr.Cast<ColumnRefExpression>().ColumnNames().size() == 1 &&
	       expr.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName() == MATCH_RECOGNIZE_CLASSIFIER_FIELD;
}

//! The classifier read off a row, whether the variable in front of it has been lifted off or there
//! never was one: CLASSIFIER() stays a call, which is what says it reads the whole match
static bool IsClassifierRead(const ParsedExpression &expr) {
	return IsClassifierField(expr) || (IsClassifier(expr) && expr.Cast<FunctionExpression>().GetArguments().empty());
}

bool MatchRecognizeContainsClassifier(const ParsedExpression &expr) {
	if (IsClassifier(expr) || IsClassifierField(expr)) {
		return true;
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || MatchRecognizeContainsClassifier(child); });
	return found;
}

static void ScopeToVariable(unique_ptr<ParsedExpression> &expr, const MatchRecognizeIsSymbol &symbols,
                            case_insensitive_set_t &scope, vector<identifier_set_t> &lambda_parameters) {
	if (expr->GetExpressionClass() == ExpressionClass::LAMBDA) {
		identifier_set_t parameters;
		if (MatchRecognizeLambdaParameters(*expr, parameters)) {
			lambda_parameters.push_back(std::move(parameters));
			ScopeToVariable(expr->Cast<LambdaExpression>().RightMutable(), symbols, scope, lambda_parameters);
			lambda_parameters.pop_back();
			return;
		}
	}
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr->Cast<ColumnRefExpression>();
		auto &names = colref.ColumnNames();
		if (names.size() >= 2 && !LambdaExpression::IsLambdaParameter(lambda_parameters, names[0]) &&
		    symbols(names[0].GetIdentifierName())) {
			scope.insert(names[0].GetIdentifierName());
			expr = MatchRecognizeWithoutQualifier(colref);
		}
		return;
	}
	if (IsClassifier(*expr) && !expr->Cast<FunctionExpression>().GetArguments().empty()) {
		// CLASSIFIER(X) reads the rows of X the way X.c does (5.9): the variable scopes it and what is
		// left is the classifier read off the row reached
		auto &argument = expr->Cast<FunctionExpression>().GetArguments()[0].GetExpression();
		if (argument.GetExpressionType() != ExpressionType::COLUMN_REF ||
		    argument.Cast<ColumnRefExpression>().ColumnNames().size() != 1 ||
		    !symbols(argument.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName())) {
			throw BinderException("CLASSIFIER() takes a pattern variable, or nothing for the match as a whole");
		}
		scope.insert(argument.Cast<ColumnRefExpression>().GetColumnName().GetIdentifierName());
		expr = make_uniq<ColumnRefExpression>(Identifier(MATCH_RECOGNIZE_CLASSIFIER_FIELD));
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { ScopeToVariable(child, symbols, scope, lambda_parameters); });
}

static void ScopeToVariable(unique_ptr<ParsedExpression> &expr, const MatchRecognizeIsSymbol &symbols,
                            case_insensitive_set_t &scope) {
	vector<identifier_set_t> lambda_parameters;
	ScopeToVariable(expr, symbols, scope, lambda_parameters);
}

//! A reference the input itself answers is the universal row pattern variable's, and so is
//! CLASSIFIER() with nothing in front of it: both read every row of the match. What reads one
//! variable's rows reads a different set, so the two cannot be asked for at once - the name each was
//! written with is reported back so the error can say which reference it was.
static bool FindUniversalReference(const ParsedExpression &expr, const case_insensitive_map_t<string> &universal,
                                   string &spelled) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &names = expr.Cast<ColumnRefExpression>().ColumnNames();
		auto entry = names.size() == 1 ? universal.find(names[0].GetIdentifierName()) : universal.end();
		if (entry == universal.end()) {
			return false;
		}
		spelled = "\"" + entry->second + "\"";
		return true;
	}
	if (expr.GetExpressionType() == ExpressionType::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		if (function.GetArguments().empty() &&
		    StringUtil::CIEquals(function.FunctionName().GetIdentifierName(), "classifier")) {
			spelled = "CLASSIFIER()";
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](const ParsedExpression &child) {
		found = found || FindUniversalReference(child, universal, spelled);
	});
	return found;
}

//! What reads the rows of one pattern variable cannot also read the whole match
static void RejectUniversalReference(const ParsedExpression &expr, const case_insensitive_map_t<string> &universal,
                                     const string &function_name, const string &variable) {
	string spelled;
	if (FindUniversalReference(expr, universal, spelled)) {
		throw BinderException("%s() reads the rows of \"%s\", so %s cannot also read the whole match", function_name,
		                      variable, spelled);
	}
}

unique_ptr<ParsedExpression> MatchRecognizeSteppedNavigation::Rebuild(const string &variable, const string &column) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(variable.empty() ? make_uniq<ColumnRefExpression>(Identifier(column))
	                                    : make_uniq<ColumnRefExpression>(Identifier(column), Identifier(variable)));
	if (offset) {
		children.push_back(std::move(offset));
	}
	unique_ptr<ParsedExpression> result =
	    make_uniq<FunctionExpression>(Identifier(last ? "last" : "first"), std::move(children));
	if (marker) {
		marker->Cast<FunctionExpression>().GetArgumentsMutable()[0].GetExpressionMutable() = std::move(result);
		result = std::move(marker);
	}
	return result;
}

static bool ContainsStep(const ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::FUNCTION) {
		auto name = StringUtil::Upper(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName());
		if (name == "PREV" || name == "NEXT") {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ContainsStep(child); });
	return found;
}

static bool ContainsNavigation(const ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::FUNCTION) {
		auto name = StringUtil::Upper(expr.Cast<FunctionExpression>().FunctionName().GetIdentifierName());
		if (name == "PREV" || name == "NEXT" || name == "FIRST" || name == "LAST") {
			return true;
		}
	}
	bool found = false;
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { found = found || ContainsNavigation(child); });
	return found;
}

//! An aggregate reads the rows of the match and a navigation reads one of them, so one inside the
//! other has no reading (ISO/IEC 19075-5 5.5, 5.6)
void MatchRecognizeRejectNavigationInAggregate(const ParsedExpression &argument, const string &function_name) {
	if (ContainsNavigation(argument)) {
		throw BinderException("%s() aggregates the rows of the match, so a row pattern navigation cannot be nested "
		                      "inside it",
		                      StringUtil::Upper(function_name));
	}
}

void MatchRecognizeRejectNestedStep(const ParsedExpression &inner, const string &function_name) {
	if (ContainsStep(inner)) {
		throw BinderException("%s() steps from a row the match names, and a step inside it names none, so the two "
		                      "cannot be nested",
		                      function_name);
	}
}

MatchRecognizeSteppedNavigation MatchRecognizePeelStep(unique_ptr<ParsedExpression> &inner) {
	MatchRecognizeSteppedNavigation result;
	if (inner->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &marker = inner->Cast<FunctionExpression>();
		const auto is_final = marker.FunctionName() == MATCH_RECOGNIZE_FINAL_MARKER;
		if (is_final || marker.FunctionName() == MATCH_RECOGNIZE_RUNNING_MARKER) {
			auto unwrapped = std::move(marker.GetArgumentsMutable()[0].GetExpressionMutable());
			result.marker = std::move(inner);
			result.final_semantics = is_final;
			inner = std::move(unwrapped);
		}
	}
	if (inner->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &navigation = inner->Cast<FunctionExpression>();
		auto name = StringUtil::Upper(navigation.FunctionName().GetIdentifierName());
		if ((name == "FIRST" || name == "LAST") && !navigation.GetArguments().empty() &&
		    navigation.GetArguments().size() <= 2) {
			result.navigated = true;
			result.last = name == "LAST";
			if (navigation.GetArguments().size() == 2) {
				result.offset_value =
				    MatchRecognizeNavigationOffset(name, navigation.GetArguments()[1].GetExpression());
				result.offset = std::move(navigation.GetArgumentsMutable()[1].GetExpressionMutable());
			}
			// taken out before the navigation it sat in is dropped, which is what frees it
			auto base = std::move(navigation.GetArgumentsMutable()[0].GetExpressionMutable());
			inner = std::move(base);
		}
	}
	return result;
}

string MatchRecognizeNavigationVariable(unique_ptr<ParsedExpression> &inner, const MatchRecognizeIsSymbol &symbols,
                                        const case_insensitive_map_t<string> &universal, const string &function_name) {
	case_insensitive_set_t scope;
	ScopeToVariable(inner, symbols, scope);
	if (scope.size() > 1) {
		throw BinderException("%s() reads one row of the match, so \"%s\" cannot also read a row of \"%s\"",
		                      function_name, *scope.begin(), *std::next(scope.begin()));
	}
	if (!scope.empty()) {
		RejectUniversalReference(*inner, universal, function_name, *scope.begin());
	}
	return scope.empty() ? string() : *scope.begin();
}

//===--------------------------------------------------------------------===//
// DEFINE
//===--------------------------------------------------------------------===//
unique_ptr<Expression> MatchRecognizeConditionInputs::Project(unique_ptr<Expression> value, const string &base) {
	return ProjectAs(std::move(value), generated.Reserve(base));
}

unique_ptr<Expression> MatchRecognizeConditionInputs::ProjectAs(unique_ptr<Expression> value, const string &name) {
	// A projection reads what its child produces: the input and the windows below it. Neither a column
	// of this projection nor a field only the matcher supplies is one, and the binder decides that
	// where the expression is bound - this is the boundary that holds it to the decision.
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(*value, [&](const BoundColumnRefExpression &column) {
		if (column.Binding().table_index == projection.projection_index) {
			throw InternalException("MATCH_RECOGNIZE projected \"%s\" from another column of the same projection",
			                        name);
		}
		if (column.Binding().table_index == match_number_index) {
			throw InternalException("MATCH_RECOGNIZE projected \"%s\", which reads a field only the matcher supplies",
			                        name);
		}
	});
	auto type = value->GetReturnType();
	const auto index = projection.select_list.size();
	value->SetAlias(Identifier(name));
	projection.select_list.push_back(std::move(value));
	projection.names.emplace_back(name);
	projection.types.push_back(type);
	hidden.push_back(name);
	return make_uniq<BoundColumnRefExpression>(Identifier(name), type,
	                                           ColumnBinding(projection.projection_index, ProjectionIndex(index)));
}

MatchRecognizeDefineBinder::MatchRecognizeDefineBinder(Binder &binder, ClientContext &context, BoundSelectNode &node,
                                                       MatchRecognizeConditionInputs &inputs_p,
                                                       const WindowExpression &window_template_p,
                                                       const case_insensitive_set_t &symbols_p,
                                                       const case_insensitive_map_t<string> &universal_p,
                                                       const unique_ptr<Expression> &match_number_p)
    : SelectBinder(binder, context, node), inputs(inputs_p), window_template(window_template_p), symbols(symbols_p),
      universal(universal_p), match_number(match_number_p) {
}

BindResult MatchRecognizeDefineBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                      bool root_expression) {
	auto &expr = *expr_ptr;
	if (expr.GetExpressionType() == ExpressionType::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		auto function_name = StringUtil::Upper(function.FunctionName().GetIdentifierName());
		// A condition is decided while the match is still being assembled, so running is the only
		// semantics it has: RUNNING may be written for clarity, FINAL asks for a match that is not
		// there yet
		if (function.FunctionName() == MATCH_RECOGNIZE_RUNNING_MARKER) {
			expr_ptr = std::move(function.GetArgumentsMutable()[0].GetExpressionMutable());
			return BindExpression(expr_ptr, depth, root_expression);
		}
		if (function.FunctionName() == MATCH_RECOGNIZE_FINAL_MARKER) {
			throw BinderException("FINAL reads the whole match, which a DEFINE condition is still assembling, so "
			                      "only RUNNING is available there");
		}
		if (function_name == "CLASSIFIER" && function.GetArguments().empty()) {
			OutsideMatch("CLASSIFIER()");
			// the row being tested is the one this DEFINE decides on, so it classifies as this symbol
			expr_ptr = ConstantExpression::String(define_name);
			return SelectBinder::BindExpression(expr_ptr, depth, root_expression);
		}
		if (function_name == "CLASSIFIER" && function.GetArguments().size() == 1) {
			// the classifier of the last row mapped to the variable so far, which is what LAST() reads
			auto variable = MatchRecognizeNavigationVariable(
			    expr_ptr, [&](const string &name) { return symbols.count(name) > 0; }, universal, "CLASSIFIER");
			return BindNavigated(std::move(expr_ptr), MatchRecognizeDefineColumn(variable), true, 0, depth);
		}
		if (function_name == "MATCH_NUMBER" && function.GetArguments().empty()) {
			OutsideMatch("MATCH_NUMBER()");
			return BindResult(match_number->Copy());
		}
		if (function_name == "PREV" || function_name == "NEXT") {
			if (scope == MatchRecognizeScope::PARTITION_BY || scope == MatchRecognizeScope::ORDER_BY) {
				throw BinderException("%s() reads a neighbour in the order the matcher walks, so it cannot be part "
				                      "of %s",
				                      function_name, ScopeName(scope));
			}
			return BindNeighbour(function, function_name, expr_ptr, depth);
		}
		if (function_name == "FIRST" || function_name == "LAST") {
			return BindNavigation(function, function_name, depth);
		}
	}
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		// a window's binding is the window operator's own output, which the projection over it can read
		const ScopedScope window(scope, MatchRecognizeScope::WINDOW);
		return SelectBinder::BindExpression(expr_ptr, depth, root_expression);
	}
	if (IsClassifierField(expr)) {
		// CLASSIFIER(X) only reaches here inside something that scoped it, and only a navigation can
		// read it off a row: an aggregate over it has no row to read it off
		throw BinderException("CLASSIFIER() cannot be aggregated in a DEFINE condition, which is still assembling "
		                      "the match");
	}
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto &names = colref.ColumnNames();
		const auto qualifier =
		    names.size() >= 2 && !BoundByLambda(lambda_bindings, names[0]) ? names[0].GetIdentifierName() : string();
		if (!qualifier.empty() && StringUtil::CIEquals(qualifier, define_name)) {
			// the variable being defined is the row being tested, so it is the row itself
			expr_ptr = MatchRecognizeWithoutQualifier(colref);
			return SelectBinder::BindExpression(expr_ptr, depth, root_expression);
		}
		if (!qualifier.empty() && symbols.count(qualifier)) {
			// naming another variable means its value on the last row matched to it so far, which
			// is what LAST() means
			return BindNavigated(MatchRecognizeWithoutQualifier(colref), MatchRecognizeDefineColumn(qualifier), true, 0,
			                     depth);
		}
	}
	return SelectBinder::BindExpression(expr_ptr, depth, root_expression);
}

BindResult MatchRecognizeDefineBinder::BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function,
                                                     idx_t depth) {
	const auto function_name = StringUtil::Upper(expr.FunctionName().GetIdentifierName());
	if (scope != MatchRecognizeScope::CANDIDATE_ROW) {
		return BindResult(BinderException(expr, "An aggregate reads the rows of the match, so it cannot be part of %s",
		                                  ScopeName(scope)));
	}
	// what these mean over a set that grows as the match is assembled is not settled here yet
	if (expr.Distinct() || expr.Filter() || (expr.OrderBy() && !expr.OrderBy()->orders.empty())) {
		return BindResult(BinderException(
		    expr, "DISTINCT, FILTER and ORDER BY are not supported on an aggregate in a DEFINE condition"));
	}
	auto &arguments = expr.GetArgumentsMutable();
	if (arguments.size() > 1) {
		return BindResult(BinderException(
		    expr,
		    "An aggregate in a DEFINE condition reads one expression of the match's rows, so %s() is not "
		    "supported there yet",
		    function_name));
	}

	// A condition is settled while the match is still being assembled, so the only semantics an
	// aggregate has here is running: the rows mapped to the variable up to and including this one
	// (ISO/IEC 19075-5 5.5). COUNT(*) reads no expression at all, and its star is the universal row
	// pattern variable, so it counts the rows of the match rather than of any one variable.
	string variable;
	const MatchRecognizeIsSymbol is_symbol = [&](const string &name) {
		return symbols.count(name) > 0;
	};
	unique_ptr<ParsedExpression> inner;
	if (!arguments.empty()) {
		MatchRecognizeRejectNavigationInAggregate(arguments[0].GetExpression(), function_name);
		inner = std::move(arguments[0].GetExpressionMutable());
		variable = MatchRecognizeNavigationVariable(inner, is_symbol, universal, function_name);
	}

	// the matcher folds rows into the aggregate itself, so what is bound here is the function rather
	// than a tree over the row: its one child says no more than what a folded value looks like
	optional_idx column;
	vector<pair<Identifier, unique_ptr<Expression>>> children;
	if (inner) {
		BindResult operand;
		{
			const ScopedScope navigated(scope, MatchRecognizeScope::NAVIGATED);
			operand = BindExpression(inner, depth, false);
		}
		if (operand.HasError()) {
			return operand;
		}
		const auto operand_type = operand.expression->GetReturnType();
		inputs.Project(std::move(operand.expression), "__mr_agg");
		column = inputs.projection.select_list.size() - 1;
		children.emplace_back(Identifier(), make_uniq<BoundReferenceExpression>(operand_type, 0U));
	}
	FunctionBinder function_binder(binder);
	ErrorData error;
	auto bound = function_binder.BindAggregateFunction(function, std::move(children), error);
	if (!bound) {
		error.AddQueryLocation(expr);
		error.Throw();
	}
	const auto return_type = bound->GetReturnType();
	const auto slot = inputs.supplied++;
	unique_ptr<Expression> aggregate = std::move(bound);
	// the matcher knows a variable by the name its condition is bound under, the same as a navigation
	auto symbol = variable.empty() ? string() : MatchRecognizeDefineColumn(variable);
	inputs.aggregates.push_back(MatchRecognizeAggregate {std::move(symbol), column, std::move(aggregate), slot});

	// read back as a field the matcher supplies, which is where the match number already lives
	auto &supplied = match_number->Cast<BoundColumnRefExpression>();
	return BindResult(make_uniq<BoundColumnRefExpression>(
	    Identifier("__mr_agg"), return_type, ColumnBinding(supplied.Binding().table_index, ProjectionIndex(slot))));
}

string MatchRecognizeDefineBinder::UnsupportedAggregateMessage() {
	return "A MATCH_RECOGNIZE condition decides one row at a time, so it cannot be an aggregate";
}

void MatchRecognizeDefineBinder::OutsideMatch(const string &what) const {
	if (scope == MatchRecognizeScope::CANDIDATE_ROW) {
		return;
	}
	throw BinderException("%s only means something while the matcher is assembling a match, so it cannot be part of %s",
	                      what, ScopeName(scope));
}

BindResult MatchRecognizeDefineBinder::BindNeighbour(FunctionExpression &function, const string &function_name,
                                                     unique_ptr<ParsedExpression> &expr_ptr, idx_t depth) {
	auto &arguments = function.GetArgumentsMutable();
	if (arguments.empty() || arguments.size() > 2) {
		throw BinderException("%s() takes an expression and an optional offset", function_name);
	}
	// a step is a fixed distance through the partition, so the offset is a non-negative constant - a
	// negative one would step the other way and a column a different way per row
	if (arguments.size() == 2) {
		MatchRecognizeNavigationOffset(function_name, arguments[1].GetExpression());
	}
	MatchRecognizeRejectNestedStep(arguments[0].GetExpression(), function_name);
	// A step names the row it starts from rather than the one it reaches: a pattern variable in front
	// of it is the row that variable denotes (5.6.2), and FIRST or LAST within it says which of that
	// variable's rows (5.6.4). Stepping and then reading the row reached is the same as reading the
	// already-stepped column off the row it started from, so the step is computed below the matcher
	// and a navigation reads it from there.
	auto inner = std::move(arguments[0].GetExpressionMutable());
	auto variable = MatchRecognizeNavigationVariable(
	    inner, [&](const string &name) { return symbols.count(name) > 0; }, universal, function_name);
	auto stepped = MatchRecognizePeelStep(inner);
	if (stepped.final_semantics) {
		throw BinderException("FINAL reads the whole match, which a DEFINE condition is still assembling, so "
		                      "only RUNNING is available there");
	}
	if (MatchRecognizeContainsClassifier(*inner)) {
		if (!IsClassifierRead(*inner)) {
			throw BinderException("%s() over CLASSIFIER() reads the classifier of the row it steps to, and nothing "
			                      "else of it",
			                      function_name);
		}
		// the classifier of the row a fixed distance from the row navigated to, which the matcher
		// reads off the attempt under way (5.9); a step naming no variable starts from the row tested
		const auto distance =
		    arguments.size() == 2
		        ? NumericCast<int64_t>(MatchRecognizeNavigationOffset(function_name, arguments[1].GetExpression()))
		        : int64_t(1);
		auto symbol = variable.empty() ? string() : MatchRecognizeDefineColumn(variable);
		return BindNavigated(std::move(inner), std::move(symbol), !stepped.navigated || stepped.last,
		                     stepped.offset_value, depth, function_name == "PREV" ? -distance : distance);
	}
	auto neighbour = window_template.Copy();
	auto &window = neighbour->Cast<WindowExpression>();
	window.SetFunctionName(function_name == "PREV" ? "lag" : "lead");
	window.GetArgumentsMutable().push_back(std::move(inner));
	if (arguments.size() == 2) {
		window.GetArgumentsMutable().push_back(std::move(arguments[1].GetExpressionMutable()));
	}
	// While a variable's own condition is being settled, the row it denotes is the row being tested:
	// its rows are the ones up to and including that one, so the last of them is that one. A step
	// naming it therefore reads exactly what a step naming nothing reads, and can be the same plain
	// window rather than a position the matcher resolves per candidate row.
	const bool own_row =
	    !variable.empty() && StringUtil::CIEquals(variable, define_name) && stepped.last && stepped.offset_value == 0;
	if ((variable.empty() && !stepped.navigated) || own_row) {
		expr_ptr = std::move(neighbour);
		// it is a window like any other from here on, so it is bound like one
		return BindExpression(expr_ptr, depth, false);
	}
	auto symbol = variable.empty() ? string() : MatchRecognizeDefineColumn(variable);
	return BindNavigated(std::move(neighbour), std::move(symbol), stepped.last, stepped.offset_value, depth);
}

BindResult MatchRecognizeDefineBinder::BindNavigation(FunctionExpression &function, const string &function_name,
                                                      idx_t depth) {
	auto &arguments = function.GetArgumentsMutable();
	if (arguments.empty() || arguments.size() > 2) {
		throw BinderException("%s() takes an expression and an optional offset", function_name);
	}
	idx_t offset = 0;
	if (arguments.size() == 2) {
		offset = MatchRecognizeNavigationOffset(function_name, arguments[1].GetExpression());
	}
	auto inner = std::move(arguments[0].GetExpressionMutable());
	auto variable = MatchRecognizeNavigationVariable(
	    inner, [&](const string &name) { return symbols.count(name) > 0; }, universal, function_name);
	// the qualifiers are gone, so what is left is read off the row this navigates to
	auto symbol = variable.empty() ? string() : MatchRecognizeDefineColumn(variable);
	return BindNavigated(std::move(inner), std::move(symbol), function_name == "LAST", offset, depth);
}

BindResult MatchRecognizeDefineBinder::BindNavigated(unique_ptr<ParsedExpression> inner, string symbol, bool last,
                                                     idx_t offset, idx_t depth, int64_t step) {
	if (scope == MatchRecognizeScope::NAVIGATED) {
		throw BinderException("Nested row pattern navigation is not supported");
	}
	OutsideMatch("Reading a row of the match");
	if (IsClassifierRead(*inner)) {
		// nothing to compute below the matcher: the classifier is the matcher's own, supplied in a
		// field of its own like an aggregate's value
		const auto slot = inputs.supplied++;
		MatchRecognizeNavigation navigation {last, std::move(symbol), DConstants::INVALID_INDEX, offset};
		navigation.classifier = true;
		navigation.slot = slot;
		navigation.step = step;
		inputs.navigations.push_back(std::move(navigation));
		auto &supplied = match_number->Cast<BoundColumnRefExpression>();
		return BindResult(
		    make_uniq<BoundColumnRefExpression>(Identifier("__mr_classifier"), LogicalType::VARCHAR,
		                                        ColumnBinding(supplied.Binding().table_index, ProjectionIndex(slot))));
	}
	if (step != 0) {
		throw InternalException("MATCH_RECOGNIZE stepped a navigation that is not over CLASSIFIER()");
	}
	BindResult bound;
	{
		const ScopedScope navigated(scope, MatchRecognizeScope::NAVIGATED);
		bound = BindExpression(inner, depth, false);
	}
	if (bound.HasError()) {
		return bound;
	}
	auto column = inputs.Project(std::move(bound.expression), "__mr_nav");
	inputs.navigations.push_back(
	    MatchRecognizeNavigation {last, std::move(symbol), inputs.projection.select_list.size() - 1, offset});
	return BindResult(std::move(column));
}

//===--------------------------------------------------------------------===//
// MEASURES
//===--------------------------------------------------------------------===//
MatchRecognizeMeasureBinder::MatchRecognizeMeasureBinder(Binder &binder, ClientContext &context, BoundSelectNode &node,
                                                         string state_p, const MatchRecognizeConfig &config_p,
                                                         const case_insensitive_map_t<vector<string>> &symbols_p,
                                                         const case_insensitive_map_t<string> &universal_p,
                                                         bool all_rows)
    : SelectBinder(binder, context, node), state(std::move(state_p)), config(config_p), symbols(symbols_p),
      universal(universal_p), one_row(!all_rows), running(all_rows) {
}

BindResult MatchRecognizeMeasureBinder::BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                       bool root_expression) {
	auto &expr = *expr_ptr;
	if (expr.GetExpressionType() == ExpressionType::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		auto function_name = StringUtil::Upper(function.FunctionName().GetIdentifierName());
		// RUNNING and FINAL choose how much of the match the measure sees, which ONE ROW PER MATCH
		// makes the same thing: its current row is the last one
		const auto is_running = function.FunctionName() == MATCH_RECOGNIZE_RUNNING_MARKER;
		if (is_running || function.FunctionName() == MATCH_RECOGNIZE_FINAL_MARKER) {
			expr_ptr = std::move(function.GetArgumentsMutable()[0].GetExpressionMutable());
			const auto saved = running;
			running = one_row ? false : is_running;
			auto result = BindExpression(expr_ptr, depth, root_expression);
			running = saved;
			return result;
		}
		if (function_name == "CLASSIFIER" && function.GetArguments().empty()) {
			expr_ptr = StateField("classifier");
			return BindGenerated(expr_ptr, depth, root_expression);
		}
		if (function_name == "CLASSIFIER" && function.GetArguments().size() == 1) {
			// the classifier of the last row mapped to the variable, which is what LAST() reads (5.9)
			auto variable = MatchRecognizeNavigationVariable(
			    expr_ptr, [&](const string &name) { return symbols.find(name) != symbols.end(); }, universal,
			    "CLASSIFIER");
			auto entry = symbols.find(variable);
			D_ASSERT(entry != symbols.end());
			expr_ptr = MatchScopedValue(
			    context, state, config,
			    ClassifiedValue(state, entry->second, PackValue(state, StateField("classifier"))), running);
			return BindGenerated(expr_ptr, depth, root_expression);
		}
		if (function_name == "MATCH_NUMBER" && function.GetArguments().empty()) {
			expr_ptr = StateField("match_number");
			return BindGenerated(expr_ptr, depth, root_expression);
		}
		// a step over CLASSIFIER() is not computed below the matcher like one over a column, since no
		// row is classified there yet: it is read off the finished match instead
		if ((function_name == "PREV" || function_name == "NEXT") && !function.GetArguments().empty() &&
		    MatchRecognizeContainsClassifier(function.GetArguments()[0].GetExpression())) {
			return BindClassifierStep(function, function_name, expr_ptr, depth, root_expression);
		}
		// LAST(X.c) is what an unadorned X.c already means, so only the end they read from differs
		if ((function_name == "FIRST" || function_name == "LAST") && !function.GetArguments().empty() &&
		    function.GetArguments().size() <= 2) {
			return BindNavigation(function, function_name, expr_ptr, depth, root_expression);
		}
		// only an aggregate can carry these, so a call that does is one - including a macro standing
		// for one, which the window unfolds. Without them it reaches BindAggregate below instead.
		if (HasAggregateModifiers(function)) {
			return BindOverMatch(function, depth);
		}
	}
	if (IsClassifierField(expr)) {
		// CLASSIFIER() once the variable in front of it, if any, has scoped the rows it reads
		expr_ptr = StateField("classifier");
		return BindGenerated(expr_ptr, depth, root_expression);
	}
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF && !scoped &&
	    !BoundByLambda(lambda_bindings, expr.Cast<ColumnRefExpression>().ColumnNames()[0])) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		auto &names = colref.ColumnNames();
		auto entry = names.size() >= 2 ? symbols.find(names[0].GetIdentifierName()) : symbols.end();
		if (entry != symbols.end()) {
			// a pattern variable scopes the column to the rows it matched
			expr_ptr = MatchScopedValue(
			    context, state, config,
			    ClassifiedValue(state, entry->second, PackValue(state, MatchRecognizeWithoutQualifier(colref))),
			    running);
			return BindGenerated(expr_ptr, depth, root_expression);
		}
		if (ClaimsAlias(colref)) {
			// A measure may name an earlier measure, but a column of the input takes precedence, and
			// which of the two this is only shows once it is bound. Binding a column ref twice is free.
			auto probe = expr_ptr->Copy();
			auto bound = SelectBinder::BindExpression(probe, depth, root_expression);
			if (bound.HasError() || bound.expression->GetExpressionClass() != ExpressionClass::BOUND_COLUMN_REF) {
				return bound;
			}
		}
		// an empty match covers no rows, so a column of the input has no row here to be read from
		expr_ptr = OnlyWhenMatched(state, std::move(expr_ptr));
		return BindGenerated(expr_ptr, depth, root_expression);
	}
	return SelectBinder::BindExpression(expr_ptr, depth, root_expression);
}

BindResult MatchRecognizeMeasureBinder::BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function,
                                                      idx_t depth) {
	return BindOverMatch(expr, depth);
}

BindResult MatchRecognizeMeasureBinder::BindOverMatch(FunctionExpression &expr, idx_t depth) {
	// naming a variable restricts the aggregate to the rows it matched
	case_insensitive_set_t scope;
	const MatchRecognizeIsSymbol is_symbol = [&](const string &name) {
		return symbols.find(name) != symbols.end();
	};
	for (auto &argument : expr.GetArgumentsMutable()) {
		MatchRecognizeRejectNavigationInAggregate(argument.GetExpression(), expr.FunctionName().GetIdentifierName());
		ScopeToVariable(argument.GetExpressionMutable(), is_symbol, scope);
	}
	if (expr.OrderByMutable()) {
		for (auto &order : expr.OrderByMutable()->orders) {
			ScopeToVariable(order.expression, is_symbol, scope);
		}
	}
	// the filter decides which of the match's rows the aggregate sees, so it reads the match the
	// same way the arguments do
	if (expr.FilterMutable()) {
		ScopeToVariable(expr.FilterMutable(), is_symbol, scope);
	}
	if (scope.size() > 1) {
		throw BinderException("An aggregate in MEASURES reads the rows of one pattern variable, so \"%s\" "
		                      "cannot also read those of \"%s\"",
		                      *scope.begin(), *std::next(scope.begin()));
	}
	if (!scope.empty()) {
		const auto &name = expr.FunctionName().GetIdentifierName();
		for (auto &argument : expr.GetArguments()) {
			RejectUniversalReference(argument.GetExpression(), universal, name, *scope.begin());
		}
		if (expr.OrderByMutable()) {
			for (auto &order : expr.OrderByMutable()->orders) {
				RejectUniversalReference(*order.expression, universal, name, *scope.begin());
			}
		}
		if (expr.FilterMutable()) {
			RejectUniversalReference(*expr.FilterMutable(), universal, name, *scope.begin());
		}
	}

	auto &qualified = expr.GetQualifiedName();
	auto window =
	    make_uniq<WindowExpression>(qualified.Catalog().GetIdentifierName(), qualified.Schema().GetIdentifierName(),
	                                qualified.Name().GetIdentifierName());
	window->GetArgumentsMutable() = std::move(expr.GetArgumentsMutable());
	window->DistinctMutable() = expr.Distinct();
	// an ordered aggregate's input order is its own, and match order is no substitute
	if (expr.OrderByMutable()) {
		window->ArgOrdersMutable() = std::move(expr.OrderByMutable()->orders);
	}
	// the row carrying an empty match must not reach the aggregate at all: passed as NULL, an aggregate
	// that keeps NULLs would still see it
	unique_ptr<ParsedExpression> in_match =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, StateField("is_empty"));
	if (!scope.empty()) {
		auto classified = symbols.find(*scope.begin());
		D_ASSERT(classified != symbols.end());
		in_match = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(in_match),
		                                            ClassifierMatches(state, classified->second));
	}
	if (expr.FilterMutable()) {
		window->FilterMutable() = make_uniq<ConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, std::move(expr.FilterMutable()), std::move(in_match));
	} else {
		window->FilterMutable() = std::move(in_match);
	}
	ScopeToMatch(context, state, *window, config, running);

	// the frame above already restricts the rows, so the values are not masked as well
	const auto saved = scoped;
	scoped = true;
	auto result = BindWindowExpression(*window, depth);
	scoped = saved;
	return result;
}

//! a + b or a - b over parsed expressions
static unique_ptr<ParsedExpression> Arithmetic(const char *op, unique_ptr<ParsedExpression> left,
                                               unique_ptr<ParsedExpression> right) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	return make_uniq<FunctionExpression>(Identifier(op), std::move(children));
}

BindResult MatchRecognizeMeasureBinder::BindClassifierStep(FunctionExpression &function, const string &function_name,
                                                           unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                           bool root_expression) {
	auto &arguments = function.GetArgumentsMutable();
	if (arguments.size() > 2) {
		throw BinderException("%s() takes an expression and an optional offset", function_name);
	}
	MatchRecognizeRejectNestedStep(arguments[0].GetExpression(), function_name);
	int64_t distance = 1;
	if (arguments.size() == 2) {
		distance = NumericCast<int64_t>(MatchRecognizeNavigationOffset(function_name, arguments[1].GetExpression()));
	}
	auto inner = std::move(arguments[0].GetExpressionMutable());
	auto variable = MatchRecognizeNavigationVariable(
	    inner, [&](const string &name) { return symbols.find(name) != symbols.end(); }, universal, function_name);
	auto stepped = MatchRecognizePeelStep(inner);
	if (!IsClassifierRead(*inner)) {
		throw BinderException("%s() over CLASSIFIER() reads the classifier of the row it steps to, and nothing else "
		                      "of it",
		                      function_name);
	}
	vector<string> symbol;
	if (!variable.empty()) {
		symbol = symbols.find(variable)->second;
	}

	// The row stepped from is a row of the match: the one the measure is read on, or the one a
	// variable or a FIRST/LAST of it denotes. Its place in the partition is a field of the state, so
	// it navigates the way a column does.
	unique_ptr<ParsedExpression> from;
	if (!variable.empty() || stepped.navigated) {
		auto packed = PackValue(state, StateField("row_index"));
		auto masked = symbol.empty() ? std::move(packed) : ClassifiedValue(state, symbol, std::move(packed));
		const auto see = one_row ? false : (stepped.marker ? !stepped.final_semantics : running);
		from = MatchScopedValue(context, state, config, std::move(masked), see, stepped.navigated && !stepped.last,
		                        stepped.offset_value);
	} else {
		from = StateField("row_index");
	}
	auto as_signed = [](unique_ptr<ParsedExpression> value) {
		return make_uniq_base<ParsedExpression, CastExpression>(LogicalType::BIGINT, std::move(value));
	};
	auto target = Arithmetic(function_name == "PREV" ? "-" : "+", as_signed(std::move(from)),
	                         ConstantExpression::Integer(distance));

	// The classifiers of the whole match, in match order, are what any row of it reads its own
	// from: a step lands on a row of the match or on nothing, since a row outside it is not
	// classified by it (5.9). The list is final by construction; the step is what running limits.
	auto classifiers = make_uniq<WindowExpression>("", "", "list");
	classifiers->GetArgumentsMutable().emplace_back(StateField("classifier"));
	ScopeToMatch(context, state, *classifiers, config, false);
	auto place = Arithmetic("+", Arithmetic("-", target->Copy(), as_signed(StateField("match_start"))),
	                        ConstantExpression::Integer(1));
	vector<unique_ptr<ParsedExpression>> extract_children;
	extract_children.push_back(std::move(classifiers));
	extract_children.push_back(std::move(place));
	auto at = make_uniq<FunctionExpression>(Identifier("list_extract"), std::move(extract_children));
	auto within = make_uniq<BetweenExpression>(std::move(target), as_signed(StateField("match_start")),
	                                           as_signed(StateField("match_end")));
	auto result = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(within);
	check.then_expr = std::move(at);
	result->CaseChecksMutable().push_back(std::move(check));
	result->ElseMutable() = ConstantExpression::Null();
	expr_ptr = std::move(result);
	return BindGenerated(expr_ptr, depth, root_expression);
}

unique_ptr<ParsedExpression> MatchRecognizeMeasureBinder::StateField(const string &field) {
	return CreateStructExtract(make_uniq<ColumnRefExpression>(Identifier(state)), Identifier(field));
}

BindResult MatchRecognizeMeasureBinder::BindGenerated(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                      bool root_expression) {
	const auto saved = scoped;
	scoped = true;
	auto result = BindExpression(expr_ptr, depth, root_expression);
	scoped = saved;
	return result;
}

BindResult MatchRecognizeMeasureBinder::BindNavigation(FunctionExpression &function, const string &function_name,
                                                       unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
                                                       bool root_expression) {
	idx_t offset = 0;
	if (function.GetArguments().size() == 2) {
		offset = MatchRecognizeNavigationOffset(function_name, function.GetArguments()[1].GetExpression());
	}
	auto inner = std::move(function.GetArgumentsMutable()[0].GetExpressionMutable());
	auto variable = MatchRecognizeNavigationVariable(
	    inner, [&](const string &name) { return symbols.find(name) != symbols.end(); }, universal, function_name);
	vector<string> symbol;
	if (!variable.empty()) {
		auto entry = symbols.find(variable);
		D_ASSERT(entry != symbols.end());
		symbol = entry->second;
	}
	// what is navigated is an expression of the clause's own, so it is read the way one is
	auto packed = PackValue(state, std::move(inner));
	auto masked = symbol.empty() ? std::move(packed) : ClassifiedValue(state, symbol, std::move(packed));
	expr_ptr = MatchScopedValue(context, state, config, std::move(masked), running, function_name == "FIRST", offset);
	return BindGenerated(expr_ptr, depth, root_expression);
}

} // namespace duckdb
