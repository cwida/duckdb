#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/table_binding.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/expression_binder/table_function_binder.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/regexp.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"

namespace duckdb {

string GetColumnsStringValue(ParsedExpression &expr) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		return colref.GetColumnName().GetIdentifierName();
	} else {
		return expr.ToString();
	}
}

static const vector<FunctionArgument> &GetFunctionArguments(const ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		return expr.Cast<WindowExpression>().GetArguments();
	}
	return expr.Cast<FunctionExpression>().GetArguments();
}

static vector<FunctionArgument> &GetFunctionArgumentsMutable(ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		return expr.Cast<WindowExpression>().GetArgumentsMutable();
	}
	return expr.Cast<FunctionExpression>().GetArgumentsMutable();
}

StarExpressionType Binder::FindStarExpression(unique_ptr<ParsedExpression> &expr, StarExpression **star, bool is_root,
                                              bool in_columns) {
	StarExpressionType has_star = StarExpressionType::NONE;
	if (expr->GetExpressionType() == ExpressionType::OPERATOR_UNPACK) {
		auto &operator_expr = expr->Cast<OperatorExpression>();
		auto res = FindStarExpression(operator_expr.GetChildrenMutable()[0], star, is_root, in_columns);
		if (res != StarExpressionType::STAR && res != StarExpressionType::COLUMNS) {
			throw BinderException(
			    "UNPACK can only be used in combination with a STAR (*) expression or COLUMNS expression");
		}
		return StarExpressionType::UNPACKED;
	}
	if (expr->GetExpressionClass() == ExpressionClass::STAR) {
		auto &current_star = expr->Cast<StarExpression>();
		if (StarExpression::IsStar(*expr)) {
			if (is_root) {
				D_ASSERT(!in_columns);
				// At the root level
				*star = &current_star;
				return StarExpressionType::STAR;
			}

			if (!in_columns) {
				// '*' can only appear inside COLUMNS or at the root level
				throw BinderException(
				    "STAR expression is only allowed as the root element of an expression. Use COLUMNS(*) instead.");
			}

			if (!current_star.ReplaceList().empty()) {
				// '*' inside COLUMNS can not have a REPLACE list
				throw BinderException(
				    "STAR expression with REPLACE list is only allowed as the root element of COLUMNS");
			}
			if (!current_star.RenameList().empty()) {
				// '*' inside COLUMNS can not have a REPLACE list
				throw BinderException(
				    "STAR expression with RENAME list is only allowed as the root element of COLUMNS");
			}

			// '*' expression inside a COLUMNS - convert to a constant list of strings (column names)
			vector<unique_ptr<ParsedExpression>> star_list;
			bind_context.GenerateAllColumnExpressions(current_star, star_list);

			vector<Value> values;
			values.reserve(star_list.size());
			for (auto &element : star_list) {
				values.emplace_back(GetColumnsStringValue(*element));
			}
			D_ASSERT(!values.empty());
			expr = ConstantExpression::FromValue(Value::LIST(LogicalType::VARCHAR, values));
			return StarExpressionType::STAR;
		}
		if (in_columns) {
			throw BinderException("COLUMNS expression is not allowed inside another COLUMNS expression");
		}
		in_columns = true;

		if (*star) {
			// we can have multiple
			if (!(*star)->Equals(current_star)) {
				throw BinderException(*expr,
				                      "Multiple different STAR/COLUMNS in the same expression are not supported");
			}
			return StarExpressionType::STAR;
		}
		*star = &current_star;
		has_star = StarExpressionType::STAR;
	}
	// the star of COUNT(tbl.*) is rewritten when the aggregate is bound
	optional_ptr<const ParsedExpression> count_star;
	if (IsQualifiedCountStar(*expr)) {
		count_star = &GetFunctionArguments(*expr)[0].GetExpression();
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child_expr) {
		if (child_expr.get() == count_star.get()) {
			return;
		}
		auto res = FindStarExpression(child_expr, star, false, in_columns);
		if (res != StarExpressionType::NONE) {
			has_star = res;
		}
	});
	return has_star;
}

void Binder::ReplaceStarExpression(unique_ptr<ParsedExpression> &expr, unique_ptr<ParsedExpression> &replacement) {
	D_ASSERT(expr);
	if (StarExpression::IsColumns(*expr) || StarExpression::IsStar(*expr)) {
		D_ASSERT(replacement);
		auto alias = expr->GetAlias();
		expr = replacement->Copy();
		if (!alias.empty()) {
			expr->SetAlias(std::move(alias));
		}
		return;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child_expr) { ReplaceStarExpression(child_expr, replacement); });
}

string Binder::ReplaceColumnsAlias(const string &alias, const string &column_name,
                                   optional_ptr<duckdb_re2::RE2> regex) {
	string result;
	result.reserve(alias.size());
	for (idx_t c = 0; c < alias.size(); c++) {
		if (alias[c] == '\\') {
			c++;
			if (c >= alias.size()) {
				throw BinderException("Unterminated backslash in COLUMNS(*) \"%s\" alias. Backslashes must either be "
				                      "escaped or followed by a number",
				                      alias);
			}
			if (alias[c] == '\\') {
				result += "\\";
				continue;
			}
			if (alias[c] < '0' || alias[c] > '9') {
				throw BinderException("Invalid backslash code in COLUMNS(*) \"%s\" alias. Backslashes must either be "
				                      "escaped or followed by a number",
				                      alias);
			}
			if (alias[c] == '0') {
				result += column_name;
			} else if (!regex) {
				throw BinderException(
				    "Only the backslash escape code \\0 can be used when no regex is supplied to COLUMNS(*)");
			} else {
				string extracted;
				RE2::Extract(column_name, *regex, "\\" + alias.substr(c, 1), &extracted);
				result += extracted;
			}
		} else {
			result += alias[c];
		}
	}
	return result;
}

void TryTransformStarLike(unique_ptr<ParsedExpression> &root) {
	// detect "* LIKE [literal]" and similar expressions
	bool inverse = root->GetExpressionType() == ExpressionType::OPERATOR_NOT;
	auto &expr = inverse ? root->Cast<OperatorExpression>().GetChildrenMutable()[0] : root;
	if (!expr) {
		return;
	}
	if (expr->GetExpressionClass() != ExpressionClass::FUNCTION) {
		return;
	}
	auto &function = expr->Cast<FunctionExpression>();
	if (function.GetArguments().size() < 2 || function.GetArguments().size() > 3) {
		return;
	}
	auto &left = function.GetArgumentsMutable()[0];
	// expression must have a star on the LHS, and a literal on the RHS
	if (left.GetExpression().GetExpressionClass() != ExpressionClass::STAR) {
		return;
	}
	auto &star = left.GetExpressionMutable()->Cast<StarExpression>();
	if (star.IsColumns()) {
		// COLUMNS(*) has different semantics
		return;
	}
	unordered_set<string> supported_ops {"~~",
	                                     "!~~",
	                                     "~~~",
	                                     "!~~~",
	                                     "~~*",
	                                     "!~~*",
	                                     "regexp_full_match",
	                                     "regexp_matches",
	                                     "not_like_escape",
	                                     "ilike_escape",
	                                     "not_ilike_escape",
	                                     "like_escape"};
	if (supported_ops.count(function.FunctionName().GetIdentifierName()) == 0) {
		// unsupported op for * expression
		throw BinderException(*root, "Function %s cannot be applied to a star expression", function.FunctionName());
	}
	auto &right = function.GetArgumentsMutable()[1];
	if (right.GetExpression().GetExpressionClass() != ExpressionClass::CONSTANT) {
		throw BinderException(*root, "Pattern applied to a star expression must be a constant");
	}
	if (!star.RenameList().empty()) {
		throw BinderException(*root, "Rename list cannot be combined with a filtering operation");
	}
	if (!star.ReplaceList().empty()) {
		throw BinderException(*root, "Replace list cannot be combined with a filtering operation");
	}
	auto original_alias = root->GetAlias();
	auto star_expr = std::move(left);
	unique_ptr<ParsedExpression> child_expr;
	if (!inverse && function.FunctionName() == "regexp_full_match" && function.GetArguments().size() == 2 &&
	    star.ExcludeList().empty()) {
		// * SIMILAR TO '[regex]' is equivalent to COLUMNS('^(?:[regex])$') - COLUMNS matches partially
		auto &pattern = right.GetExpression().Cast<ConstantExpression>();
		if (pattern.GetLiteral().kind == LiteralKind::STRING) {
			child_expr = ConstantExpression::String("^(?:" + pattern.GetLiteral().text + ")$");
		} else {
			vector<unique_ptr<ParsedExpression>> prefix_children;
			prefix_children.push_back(ConstantExpression::String("^(?:"));
			prefix_children.push_back(std::move(right.GetExpressionMutable()));
			vector<unique_ptr<ParsedExpression>> anchored_children;
			anchored_children.push_back(make_uniq<FunctionExpression>("||", std::move(prefix_children)));
			anchored_children.push_back(ConstantExpression::String(")$"));
			child_expr = make_uniq<FunctionExpression>("||", std::move(anchored_children));
		}
	} else {
		// for other expressions -> generate a columns expression
		// "* LIKE '%literal%'
		// -> COLUMNS(list_filter(*, x -> x LIKE '%literal%'))
		vector<string> named_parameters;
		named_parameters.push_back("__lambda_col");
		function.GetArgumentsMutable()[0] = FunctionArgument(make_uniq<ColumnRefExpression>("__lambda_col"));
		function.GetArgumentsMutable()[1] = std::move(right);

		unique_ptr<ParsedExpression> lambda_body = std::move(expr);
		if (inverse) {
			vector<unique_ptr<ParsedExpression>> root_children;
			root_children.push_back(std::move(lambda_body));
			lambda_body = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(root_children));
		}
		auto lambda = make_uniq<LambdaExpression>(std::move(named_parameters), std::move(lambda_body));

		vector<unique_ptr<ParsedExpression>> filter_children;
		filter_children.push_back(std::move(star_expr.GetExpressionMutable()));
		filter_children.push_back(std::move(lambda));
		child_expr = make_uniq<FunctionExpression>("list_filter", std::move(filter_children));
	}

	auto columns_expr = make_uniq<StarExpression>(star.RelationName());
	columns_expr->IsColumnsMutable() = true;
	columns_expr->ExpressionMutable() = std::move(child_expr);
	columns_expr->SetAlias(std::move(original_alias));
	root = std::move(columns_expr);
}

optional_ptr<ParsedExpression> Binder::GetResolvedColumnExpression(ParsedExpression &root_expr) {
	optional_ptr<ParsedExpression> expr = &root_expr;
	while (expr) {
		if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
			break;
		}
		if (expr->GetExpressionType() == ExpressionType::OPERATOR_COALESCE) {
			expr = expr->Cast<OperatorExpression>().GetChildrenMutable()[0].get();
		} else {
			// unknown expression
			return nullptr;
		}
	}
	return expr;
}

static bool IsQualifiedStar(const ParsedExpression &expr) {
	if (!StarExpression::IsStar(expr)) {
		return false;
	}
	auto &star = expr.Cast<StarExpression>();
	if (star.RelationName().empty()) {
		return false;
	}
	if (!star.ExcludeList().empty()) {
		return false;
	}
	if (!star.ReplaceList().empty()) {
		return false;
	}
	if (!star.RenameList().empty()) {
		return false;
	}
	return true;
}

static bool IsCount(const Identifier &function_name, bool distinct) {
	return function_name == "count" && !distinct;
}

bool Binder::IsQualifiedCountStar(const ParsedExpression &expr) {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::FUNCTION: {
		auto &function = expr.Cast<FunctionExpression>();
		if (!IsCount(function.FunctionName(), function.Distinct())) {
			return false;
		}
		break;
	}
	case ExpressionClass::WINDOW: {
		auto &window = expr.Cast<WindowExpression>();
		if (!IsCount(window.FunctionName(), window.Distinct())) {
			return false;
		}
		break;
	}
	default:
		return false;
	}
	auto &arguments = GetFunctionArguments(expr);
	return arguments.size() == 1 && IsQualifiedStar(arguments[0].GetExpression());
}

//! CASE WHEN col1 IS NOT NULL OR ... OR coln IS NOT NULL THEN 1 END
static unique_ptr<ParsedExpression> AnyColumnNotNull(vector<unique_ptr<ParsedExpression>> columns) {
	vector<unique_ptr<ParsedExpression>> checks;
	for (auto &column : columns) {
		checks.push_back(make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(column)));
	}
	auto case_expr = make_uniq<CaseExpression>();
	CaseCheck case_check;
	case_check.when_expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(checks));
	case_check.then_expr = ConstantExpression::FromValue(Value::BIGINT(1));
	case_expr->CaseChecksMutable().push_back(std::move(case_check));
	case_expr->ElseMutable() = ConstantExpression::FromValue(Value());
	return std::move(case_expr);
}

static unique_ptr<ParsedExpression> GetRowIdColumn(Binding &binding) {
	if (binding.GetBindingType() != BindingType::TABLE) {
		return nullptr;
	}
	auto &virtual_columns = binding.Cast<TableBinding>().virtual_columns;
	auto entry = virtual_columns.find(COLUMN_IDENTIFIER_ROW_ID);
	if (entry == virtual_columns.end()) {
		return nullptr;
	}
	column_t column_index;
	if (!binding.TryGetBindingIndex(entry->second.name, column_index) || column_index != COLUMN_IDENTIFIER_ROW_ID) {
		// the row id is shadowed by a regular column
		return nullptr;
	}
	return make_uniq<ColumnRefExpression>(entry->second.name, binding.GetBindingAlias());
}

//! Returns the argument that COUNT(tbl.*) counts, or nullptr if every row of tbl is counted
static unique_ptr<ParsedExpression> GetCountStarArgument(StarExpression &star, BindContext &bind_context) {
	if (bind_context.GetBindingsList().empty()) {
		// nothing to resolve the relation against (e.g. when verifying a macro definition)
		return nullptr;
	}
	ErrorData error;
	auto binding = bind_context.GetBinding(star.RelationName(), error);
	if (binding) {
		if (!binding->IsNullExtended()) {
			return nullptr;
		}
		auto row_id = GetRowIdColumn(*binding);
		if (row_id) {
			// the row id is only NULL for NULL-extended rows
			return row_id;
		}
		// without a row id, a row in which all columns are NULL is indistinguishable from a NULL-extended row
		vector<unique_ptr<ParsedExpression>> columns;
		bind_context.GenerateAllColumnExpressions(star, columns);
		return AnyColumnNotNull(std::move(columns));
	}
	// struct.*
	auto struct_binding = bind_context.GetMatchingBinding(star.RelationName(), star);
	if (!struct_binding) {
		error.Throw();
	}
	if (!struct_binding->IsNullExtended()) {
		return nullptr;
	}
	return make_uniq<ColumnRefExpression>(star.RelationName(), struct_binding->GetBindingAlias());
}

//! COUNT(tbl.*) counts every row of tbl, except for the rows that an outer join NULL-extended
unique_ptr<ParsedExpression> Binder::TryRewriteQualifiedCountStar(const ParsedExpression &expr) {
	if (!IsQualifiedCountStar(expr)) {
		return nullptr;
	}
	auto result = expr.Copy();
	if (result->GetAlias().empty()) {
		// the rewrite is an implementation detail - keep the original expression as the name
		result->SetAlias(Identifier(expr.ToString()));
	}
	auto &arguments = GetFunctionArgumentsMutable(*result);
	auto &star = arguments[0].GetExpressionMutable()->Cast<StarExpression>();
	auto count_argument = GetCountStarArgument(star, bind_context);
	if (count_argument) {
		arguments[0] = FunctionArgument(std::move(count_argument));
	} else {
		arguments.clear();
	}
	return result;
}

void Binder::ExpandStarExpression(unique_ptr<ParsedExpression> expr,
                                  vector<unique_ptr<ParsedExpression>> &new_select_list) {
	TryTransformStarLike(expr);

	StarExpression *star = nullptr;

	//! Check the current 'expr' to find out if it contains a star-expression
	//! that needs to be expanded
	auto star_type = FindStarExpression(expr, &star, true, false);
	if (star_type == StarExpressionType::NONE) {
		// no star expression: add it as-is
		D_ASSERT(!star);
		new_select_list.push_back(std::move(expr));
		return;
	}
	D_ASSERT(star);
	vector<unique_ptr<ParsedExpression>> star_list;
	//! Expand the star that we found into a new list of expressions
	bind_context.GenerateAllColumnExpressions(*star, star_list);

	unique_ptr<duckdb_re2::RE2> regex;
	if (star->Expression()) {
		// COLUMNS with an expression
		// two options:
		// VARCHAR parameter <- this is a regular expression
		// LIST of VARCHAR parameters <- this is a set of columns
		TableFunctionBinder binder(*this, context);
		auto child = star->Expression()->Copy();
		auto result = binder.Bind(child);
		if (!result->IsFoldable()) {
			// cannot resolve parameters here
			if (star->Expression()->HasParameter()) {
				throw ParameterNotResolvedException();
			} else {
				throw BinderException("Unsupported expression in COLUMNS");
			}
		}
		auto val = ExpressionExecutor::EvaluateScalar(context, *result);
		if (val.type().id() == LogicalTypeId::VARCHAR) {
			// regex
			if (val.IsNull()) {
				throw BinderException("COLUMNS does not support NULL as regex argument");
			}
			auto &regex_str = StringValue::Get(val);
			regex = make_uniq<duckdb_re2::RE2>(regex_str);
			if (!regex->error().empty()) {
				auto err = StringUtil::Format("Failed to compile regex \"%s\": %s", regex_str, regex->error());
				throw BinderException(*star, err);
			}
			vector<unique_ptr<ParsedExpression>> new_list;
			for (auto &expanded_expr : star_list) {
				auto child_expr = GetResolvedColumnExpression(*expanded_expr);
				if (!child_expr) {
					continue;
				}
				auto &colref = child_expr->Cast<ColumnRefExpression>();
				if (!RE2::PartialMatch(colref.GetColumnName().GetIdentifierName(), *regex)) {
					continue;
				}
				new_list.push_back(std::move(expanded_expr));
			}
			if (new_list.empty()) {
				vector<string> candidates;
				for (auto &expanded_expr : star_list) {
					auto child_expr = GetResolvedColumnExpression(*expanded_expr);
					if (!child_expr) {
						continue;
					}
					auto &colref = child_expr->Cast<ColumnRefExpression>();
					candidates.emplace_back(colref.GetColumnName());
				}
				string candidate_str;
				if (!candidates.empty()) {
					candidate_str = "\n" + StringUtil::CandidatesErrorMessage(candidates, regex_str, "Did you mean");
				}
				auto err =
				    StringUtil::Format("No matching columns found that match regex \"%s\"%s", regex_str, candidate_str);
				throw BinderException(*star, err);
			}
			star_list = std::move(new_list);
		} else if (val.type().id() == LogicalTypeId::LIST &&
		           ListType::GetChildType(val.type()).id() == LogicalTypeId::VARCHAR) {
			// list of varchar columns
			if (val.IsNull() || ListValue::GetChildren(val).empty()) {
				auto err =
				    StringUtil::Format("Star expression \"%s\" resulted in an empty set of columns", star->ToString());
				throw BinderException(*star, err);
			}
			auto &children = ListValue::GetChildren(val);
			vector<unique_ptr<ParsedExpression>> new_list;
			// scan the list for all selected columns and construct a lookup table
			case_insensitive_map_t<bool> selected_set;
			for (auto &child : children) {
				if (child.IsNull()) {
					throw BinderException(*star, "Columns expression does not support NULL input parameters");
				}
				selected_set.insert(make_pair(StringValue::Get(child), false));
			}
			// now check the list of all possible expressions and select which ones make it in
			for (auto &expr : star_list) {
				auto str = GetColumnsStringValue(*expr);
				auto entry = selected_set.find(str);
				if (entry != selected_set.end()) {
					new_list.push_back(std::move(expr));
					entry->second = true;
				}
			}
			// check if all expressions found a match
			for (auto &entry : selected_set) {
				if (!entry.second) {
					throw BinderException("Column \"%s\" was selected but was not found in the FROM clause",
					                      entry.first);
				}
			}
			star_list = std::move(new_list);
		} else {
			throw BinderException(
			    *star, "COLUMNS expects either a VARCHAR argument (regex) or a LIST of VARCHAR (list of columns)");
		}
	}
	//! We make a copy because we will be replacing the original star
	//! If we have identical stars that also need to be replaced
	//! This causes a heap-use-after-free or other corruption if we don't make a copy to reference instead.
	auto copied_star_p = star->Copy();
	auto &copied_star = copied_star_p->Cast<StarExpression>();

	// now perform the replacement
	if (star_type == StarExpressionType::UNPACKED) {
		if (StarExpression::IsColumnsUnpacked(*expr)) {
			throw BinderException("*COLUMNS not allowed at the root level, use COLUMNS instead");
		}
		ReplaceUnpackedStarExpression(expr, star_list, copied_star, regex.get());
		new_select_list.push_back(std::move(expr));
		return;
	}
	for (idx_t i = 0; i < star_list.size(); i++) {
		auto new_expr = expr->Copy();
		ReplaceStarExpression(new_expr, star_list[i]);
		if (StarExpression::IsColumns(copied_star)) {
			auto expr = GetResolvedColumnExpression(*star_list[i]);
			if (expr) {
				auto &colref = expr->Cast<ColumnRefExpression>();
				if (new_expr->GetAlias().empty()) {
					new_expr->SetAlias(colref.GetColumnName());
				} else {
					new_expr->SetAlias(
					    Identifier(ReplaceColumnsAlias(new_expr->GetAlias().GetIdentifierName(),
					                                   colref.GetColumnName().GetIdentifierName(), regex.get())));
				}
			}
		}
		new_select_list.push_back(std::move(new_expr));
	}
}

void Binder::ExpandStarExpressions(vector<unique_ptr<ParsedExpression>> &select_list,
                                   vector<unique_ptr<ParsedExpression>> &new_select_list) {
	for (auto &select_element : select_list) {
		ExpandStarExpression(std::move(select_element), new_select_list);
	}
}

} // namespace duckdb
