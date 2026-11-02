#include "duckdb/common/enums/date_part_specifier.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/common/enums/subquery_type.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/optimizer/rule/date_trunc_simplification.hpp"
#include "duckdb/parser/peg/transformer/peg_transformer.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/between_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/positional_reference_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/default_expression.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

//! Only an unqualified star (e.g. COUNT(*)) refers to the rows themselves - a qualified star (e.g. COUNT(tbl.*))
//! refers to the columns of that relation and is expanded by the binder
static bool ExpressionIsUnqualifiedStar(const ParsedExpression &expr) {
	return PEGTransformerFactory::ExpressionIsEmptyStar(expr) && expr.Cast<StarExpression>().RelationName().empty();
}

//! Which syntactic form named the function: f(x) or x.f(y).
enum class ParserRewriteCallForm : uint8_t { PLAIN, METHOD_CHAIN };

//! Names that the parser rewrites into a syntax node instead of a catalog function call.
//! Every path that can name a function must go through this, or the paths disagree.
//! Returns nullptr when the name is an ordinary function.
static unique_ptr<ParsedExpression> TransformParserFunctionRewrite(const string &lowercase_name,
                                                                   vector<FunctionArgument> &function_children,
                                                                   ParserRewriteCallForm call_form) {
	auto reject_named_arguments = [&function_children](const char *context) {
		for (auto &arg : function_children) {
			if (arg.HasName()) {
				throw ParserException("Named arguments are not supported in %s", context);
			}
		}
	};

	if (lowercase_name == "unpack") {
		if (function_children.size() != 1) {
			throw ParserException("Wrong number of arguments to the UNPACK operator");
		}
		reject_named_arguments("UNPACK operator");
		auto expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_UNPACK);
		for (auto &arg : function_children) {
			expr->GetChildrenMutable().push_back(std::move(arg.GetExpressionMutable()));
		}
		return std::move(expr);
	}
	if (lowercase_name == "try") {
		if (function_children.size() != 1) {
			throw ParserException("Wrong number of arguments provided to TRY expression");
		}
		reject_named_arguments("TRY expression");
		auto try_expression = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_TRY);
		for (auto &arg : function_children) {
			try_expression->GetChildrenMutable().push_back(std::move(arg.GetExpressionMutable()));
		}
		return std::move(try_expression);
	}
	if (lowercase_name == "construct_array") {
		reject_named_arguments("array constructors");
		auto construct_array = make_uniq<OperatorExpression>(ExpressionType::ARRAY_CONSTRUCTOR);
		for (auto &arg : function_children) {
			construct_array->GetChildrenMutable().push_back(std::move(arg.GetExpressionMutable()));
		}
		return std::move(construct_array);
	}
	if (lowercase_name == "ifnull") {
		if (function_children.size() != 2) {
			throw ParserException("Wrong number of arguments to IFNULL.");
		}
		reject_named_arguments("IFNULL expressions");

		//  Two-argument COALESCE
		auto coalesce_op = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
		coalesce_op->GetChildrenMutable().push_back(std::move(function_children[0].GetExpressionMutable()));
		coalesce_op->GetChildrenMutable().push_back(std::move(function_children[1].GetExpressionMutable()));
		return std::move(coalesce_op);
	}
	// COALESCE has a grammar rule of its own (CoalesceExpression), which is how the plain path spells
	// it. Reaching the plain path under this name therefore means the caller wrote it as a function
	// call - quoted, or schema-qualified - which resolves in the catalog and lets a user macro named
	// coalesce win. The method chain has no grammar form, so the rewrite is its only route.
	if (lowercase_name == "coalesce" && call_form == ParserRewriteCallForm::METHOD_CHAIN) {
		reject_named_arguments("COALESCE expressions");
		auto coalesce_op = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
		for (auto &arg : function_children) {
			coalesce_op->GetChildrenMutable().push_back(std::move(arg.GetExpressionMutable()));
		}
		return std::move(coalesce_op);
	}
	if (lowercase_name == "date") {
		if (function_children.size() != 1) {
			throw ParserException("Wrong number of arguments provided to DATE function");
		}
		return std::move(
		    make_uniq<CastExpression>(LogicalType::DATE, std::move(function_children[0].GetExpressionMutable())));
	}
	return nullptr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformBaseExpression(PEGTransformer &transformer,
                                               unique_ptr<ParsedExpression> single_expression,
                                               optional<vector<unique_ptr<ParsedExpression>>> indirection_list) {
	auto expr = std::move(single_expression);
	if (!indirection_list) {
		return expr;
	}

	bool prev_indirection_was_cast = false;
	for (auto &indirection_expr : *indirection_list) {
		if (indirection_expr->GetExpressionClass() == ExpressionClass::CAST) {
			auto cast_expr = unique_ptr_cast<ParsedExpression, CastExpression>(std::move(indirection_expr));
			cast_expr->ChildMutable() = std::move(expr);
			expr = std::move(cast_expr);
			prev_indirection_was_cast = true;
		} else if (indirection_expr->GetExpressionClass() == ExpressionClass::OPERATOR) {
			if (prev_indirection_was_cast) {
				throw ParserException(
				    "Subscript/slice cannot be applied directly after a cast operator (e.g. x::TYPE[1:3] is not "
				    "allowed). Wrap the cast in parentheses: (x::TYPE)[1:3]");
			}
			auto operator_expr = unique_ptr_cast<ParsedExpression, OperatorExpression>(std::move(indirection_expr));
			operator_expr->GetChildrenMutable().insert(operator_expr->GetChildrenMutable().begin(), std::move(expr));
			expr = std::move(operator_expr);
			prev_indirection_was_cast = false;
		} else if (indirection_expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
			auto function_expr = unique_ptr_cast<ParsedExpression, FunctionExpression>(std::move(indirection_expr));
			function_expr->GetArgumentsMutable().insert(function_expr->GetArgumentsMutable().begin(), std::move(expr));
			// A method-chained call names a function too, so it takes the same rewrites.
			auto lowercase_name = StringUtil::Lower(function_expr->FunctionName().GetIdentifierName());
			const bool distinct = function_expr->Distinct();
			const bool has_order_by = function_expr->OrderBy() && !function_expr->OrderBy()->orders.empty();
			if (auto rewritten = TransformParserFunctionRewrite(lowercase_name, function_expr->GetArgumentsMutable(),
			                                                    ParserRewriteCallForm::METHOD_CHAIN)) {
				// The method grammar accepts DISTINCT and ORDER BY for every call, but a rewrite yields a
				// syntax node with nowhere to put them. Reject rather than drop them silently.
				auto upper_name = StringUtil::Upper(lowercase_name);
				if (distinct) {
					throw ParserException("DISTINCT is not supported in %s expressions", upper_name);
				}
				if (has_order_by) {
					throw ParserException("ORDER BY is not supported in %s expressions", upper_name);
				}
				expr = std::move(rewritten);
			} else {
				expr = std::move(function_expr);
			}
			prev_indirection_was_cast = false;
		} else if (indirection_expr->GetExpressionClass() == ExpressionClass::CONSTANT) {
			vector<unique_ptr<ParsedExpression>> struct_children;
			struct_children.push_back(std::move(expr));
			struct_children.push_back(std::move(indirection_expr));
			auto struct_expr =
			    make_uniq<OperatorExpression>(ExpressionType::STRUCT_EXTRACT, std::move(struct_children));
			expr = std::move(struct_expr);
			prev_indirection_was_cast = false;
		} else {
			throw NotImplementedException("Unhandled case for Base Expression with indirection");
		}
	}
	return expr;
}

// ColumnReference <- CatalogReservedSchemaTableColumnName / SchemaReservedTableColumnName / TableReservedColumnName /
// NestedColumnName
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColumnReference(PEGTransformer &transformer,
                                                                             unique_ptr<ColumnRefExpression> child) {
	return std::move(child);
}

unique_ptr<ColumnRefExpression> PEGTransformerFactory::TransformNestedSchemaTableColumnName(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_schema_qualification_1,
    const vector<Identifier> &reserved_schema_qualification_2, const Identifier &reserved_column_name) {
	vector<Identifier> column_names;
	column_names.push_back(catalog_qualification);
	column_names.push_back(reserved_schema_qualification);
	column_names.push_back(reserved_schema_qualification_1);
	for (auto &qualification : reserved_schema_qualification_2) {
		column_names.push_back(qualification);
	}
	column_names.push_back(reserved_column_name);
	return make_uniq<ColumnRefExpression>(std::move(column_names));
}

unique_ptr<ColumnRefExpression> PEGTransformerFactory::TransformCatalogReservedSchemaTableColumnName(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const Identifier &reserved_schema_qualification, const Identifier &reserved_table_qualification,
    const Identifier &reserved_column_name) {
	vector<Identifier> column_names;
	column_names.push_back(catalog_qualification);
	column_names.push_back(reserved_schema_qualification);
	column_names.push_back(reserved_table_qualification);
	column_names.push_back(reserved_column_name);
	return make_uniq<ColumnRefExpression>(std::move(column_names));
}

unique_ptr<ColumnRefExpression> PEGTransformerFactory::TransformSchemaReservedTableColumnName(
    PEGTransformer &transformer, const Identifier &schema_qualification, const Identifier &reserved_table_qualification,
    const Identifier &reserved_column_name) {
	vector<Identifier> column_names;
	column_names.push_back(schema_qualification);
	column_names.push_back(reserved_table_qualification);
	column_names.push_back(reserved_column_name);
	return make_uniq<ColumnRefExpression>(std::move(column_names));
}

Identifier PEGTransformerFactory::TransformReservedTableQualification(PEGTransformer &transformer,
                                                                      const Identifier &reserved_table_name) {
	return reserved_table_name;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformFunctionExpression(
    PEGTransformer &transformer, const QualifiedName &function_identifier,
    MethodArguments function_expression_arguments, optional<vector<OrderByNode>> within_group_clause,
    optional<unique_ptr<ParsedExpression>> filter_clause, const bool &has_result,
    optional<unique_ptr<WindowExpression>> over_clause) {
	const auto &qualified_function = function_identifier;
	bool export_clause = has_result;
	auto distinct = function_expression_arguments.distinct;
	auto function_children = std::move(function_expression_arguments.arguments);
	auto order_modifier = make_uniq<OrderModifier>();
	order_modifier->orders = std::move(function_expression_arguments.order_bys);

	unique_ptr<ParsedExpression> filter_expr;
	if (filter_clause) {
		filter_expr = std::move(*filter_clause);
	}
	if (within_group_clause) {
		if (distinct) {
			throw ParserException("DISTINCT is not allowed in combination with WITHIN GROUP");
		}
		if (!order_modifier->orders.empty()) {
			throw ParserException("Cannot use multiple ORDER BY statements with WITHIN GROUP");
		}
		order_modifier->orders = std::move(*within_group_clause);
		if (order_modifier->orders.size() != 1) {
			throw ParserException("Cannot use multiple ORDER BY clauses with WITHIN GROUP");
		}
	}

	if (function_children.size() == 1 && ExpressionIsUnqualifiedStar(function_children[0].GetExpression()) &&
	    !distinct && order_modifier->orders.empty()) {
		// COUNT(*) gets converted into COUNT()
		function_children.clear();
	}
	auto lowercase_name = StringUtil::Lower(qualified_function.Name().GetIdentifierName());

	if (over_clause) {
		if (transformer.in_window_definition) {
			throw ParserException("window functions are not allowed in window definitions");
		}
		//	We map first/last OVER() to first_value/last_value.
		//	Not sure the semantics match, but we are stuck with it.
		if (lowercase_name == "first" || lowercase_name == "last") {
			lowercase_name += "_value";
		}

		if (export_clause) {
			throw ParserException("EXPORT_STATE is not supported for window functions!");
		}

		transformer.in_window_definition = true;
		auto expr = std::move(*over_clause);
		expr->SetQualifiedName(qualified_function.WithName(Identifier()));
		expr->SetFunctionName(lowercase_name);

		for (auto &arg : function_children) {
			expr->GetArgumentsMutable().push_back(std::move(arg));
		}

		expr->HasIgnoreNullsMutable() = function_expression_arguments.has_ignore_nulls;
		expr->IgnoreNullsMutable() = function_expression_arguments.ignore_nulls;
		expr->FilterMutable() = std::move(filter_expr);
		expr->ArgOrdersMutable() = std::move(order_modifier->orders);
		expr->DistinctMutable() = distinct;
		transformer.in_window_definition = false;
		return std::move(expr);
	}
	if (lowercase_name == "count" && function_children.empty()) {
		lowercase_name = "count_star";
	}

	if (auto rewritten =
	        TransformParserFunctionRewrite(lowercase_name, function_children, ParserRewriteCallForm::PLAIN)) {
		return rewritten;
	}
	if (function_expression_arguments.has_ignore_nulls) {
		throw ParserException("RESPECT/IGNORE NULLS is not supported for non-window functions");
	}
	if (within_group_clause) {
		if (lowercase_name == "percentile_cont") {
			if (function_children.size() != 1) {
				throw ParserException("Wrong number of arguments for PERCENTILE_CONT");
			}
			lowercase_name = "quantile_cont";
		} else if (lowercase_name == "percentile_disc") {
			if (function_children.size() != 1) {
				throw ParserException("Wrong number of arguments for PERCENTILE_DISC");
			}
			lowercase_name = "quantile_disc";
		} else if (lowercase_name == "mode") {
			if (!function_children.empty()) {
				throw ParserException("Wrong number of arguments for MODE");
			}
			lowercase_name = "mode";
		} else {
			throw ParserException("Unknown ordered aggregate %s.", qualified_function.Name());
		}
	}
	auto result = make_uniq<FunctionExpression>(qualified_function.WithName(Identifier(lowercase_name)),
	                                            std::move(function_children), std::move(filter_expr),
	                                            std::move(order_modifier), distinct, false, export_clause);

	return std::move(result);
}

MethodArguments PEGTransformerFactory::TransformFunctionExpressionArgumentList(
    PEGTransformer &transformer, const optional<bool> &distinct_or_all,
    optional<vector<FunctionArgument>> function_argument_list, optional<vector<OrderByNode>> order_by_clause,
    const optional<bool> &ignore_or_respect_nulls) {
	if (distinct_or_all && !function_argument_list) {
		throw ParserException("%s requires at least one argument", *distinct_or_all ? "DISTINCT" : "ALL");
	}
	MethodArguments result;
	if (distinct_or_all) {
		result.distinct = *distinct_or_all;
	}
	if (function_argument_list) {
		result.arguments = std::move(*function_argument_list);
	}
	if (order_by_clause) {
		result.order_bys = std::move(*order_by_clause);
	}
	if (ignore_or_respect_nulls) {
		result.has_ignore_nulls = true;
		result.ignore_nulls = *ignore_or_respect_nulls;
	}
	return result;
}

MethodArguments
PEGTransformerFactory::TransformFunctionExpressionArguments(PEGTransformer &transformer,
                                                            MethodArguments function_expression_argument_list) {
	return function_expression_argument_list;
}

vector<FunctionArgument>
PEGTransformerFactory::TransformFunctionArgumentList(PEGTransformer &transformer,
                                                     vector<FunctionArgument> function_argument) {
	return function_argument;
}

vector<OrderByNode> PEGTransformerFactory::TransformWithinGroupClause(PEGTransformer &transformer,
                                                                      vector<OrderByNode> order_by_clause) {
	return order_by_clause;
}

bool PEGTransformerFactory::TransformDistinctKeyword(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformAllKeyword(PEGTransformer &transformer) {
	return false;
}

QualifiedName PEGTransformerFactory::TransformFunctionNameAsQualifiedName(PEGTransformer &transformer,
                                                                          const Identifier &function_name) {
	return QualifiedName(function_name);
}

QualifiedName PEGTransformerFactory::TransformSchemaReservedFunctionName(PEGTransformer &transformer,
                                                                         const Identifier &schema_qualification,
                                                                         const Identifier &reserved_function_name) {
	QualifiedName result({schema_qualification}, reserved_function_name);
	return result;
}

QualifiedName PEGTransformerFactory::TransformCatalogReservedSchemaFunctionName(
    PEGTransformer &transformer, const Identifier &catalog_qualification,
    const optional<vector<Identifier>> &reserved_schema_qualification, const Identifier &reserved_function_name) {
	vector<Identifier> qualification;
	qualification.push_back(catalog_qualification);
	if (reserved_schema_qualification) {
		for (auto &schema : *reserved_schema_qualification) {
			qualification.push_back(schema);
		}
	}
	return QualifiedName(std::move(qualification), reserved_function_name);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformArrayBoundedListExpression(
    PEGTransformer &transformer, const bool &has_result, vector<unique_ptr<ParsedExpression>> bounded_list_expression) {
	bool is_array = has_result;
	if (!is_array) {
		return make_uniq<FunctionExpression>("list_value", std::move(bounded_list_expression));
	}
	return make_uniq<OperatorExpression>(ExpressionType::ARRAY_CONSTRUCTOR, std::move(bounded_list_expression));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformFilterClause(PEGTransformer &transformer,
                                             unique_ptr<ParsedExpression> filter_clause_expression) {
	return filter_clause_expression;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformFilterClauseExpression(PEGTransformer &transformer,
                                                       unique_ptr<ParsedExpression> filter_clause_contents) {
	return filter_clause_contents;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformFilterClauseContents(PEGTransformer &transformer, const bool &has_result,
                                                     unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformParenthesisExpression(PEGTransformer &transformer,
                                                      optional<vector<unique_ptr<ParsedExpression>>> expression) {
	// ParenthesisExpression <- Parens(List(Expression)?)
	// Python-style tuples: (), (x,) and (a, b, ...) all build an (unnamed) row/TUPLE.
	// A single (x) without a trailing comma is grouping and is handled earlier by ParensExpression.
	vector<unique_ptr<ParsedExpression>> children;
	if (expression) {
		children = std::move(*expression);
	}
	return make_uniq<FunctionExpression>("row", std::move(children));
}

void PEGTransformerFactory::RemoveOrderQualificationRecursive(unique_ptr<ParsedExpression> &root_expr) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    *root_expr, [&](ColumnRefExpression &col_ref) {
		    auto &col_names = col_ref.ColumnNamesMutable();
		    if (col_names.size() > 1) {
			    col_names = vector<Identifier> {col_names.back()};
		    }
	    });
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformArrayParensSelect(PEGTransformer &transformer,
                                                  unique_ptr<SelectStatement> select_statement_internal) {
	auto subquery_expr = make_uniq<SubqueryExpression>();
	subquery_expr->SubqueryMutable() = std::move(select_statement_internal);
	// ARRAY expression
	// wrap subquery into
	// "SELECT CASE WHEN ARRAY_AGG(col) IS NULL THEN [] ELSE ARRAY_AGG(col) END FROM (...) tbl"
	auto select_node = make_uniq<SelectNode>();
	unique_ptr<ParsedExpression> array_agg_child;
	optional_ptr<SelectNode> sub_select;
	if (subquery_expr->Subquery()->node->type == QueryNodeType::SELECT_NODE) {
		// easy case - subquery is a SELECT
		sub_select = subquery_expr->Subquery()->node->Cast<SelectNode>();
		if (sub_select->select_list.size() != 1) {
			throw BinderException(*subquery_expr, "Subquery returns %zu columns - expected 1",
			                      sub_select->select_list.size());
		}
		array_agg_child = make_uniq<PositionalReferenceExpression>(1ULL);
	} else {
		// subquery is not a SELECT but a UNION or CTE
		// we can still support this but it is more challenging since we can't push columns for the ORDER BY
		auto columns_star = make_uniq<StarExpression>();
		columns_star->IsColumnsMutable() = true;
		array_agg_child = std::move(columns_star);
	}

	// ARRAY_AGG(COLUMNS(*))
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(array_agg_child));
	auto aggr = make_uniq<FunctionExpression>("array_agg", std::move(children));
	// push ORDER BY modifiers into the array_agg
	for (auto &modifier : subquery_expr->SubqueryMutable()->node->modifiers) {
		if (modifier->type == ResultModifierType::ORDER_MODIFIER) {
			aggr->OrderByMutable() = unique_ptr_cast<ResultModifier, OrderModifier>(modifier->Copy());
			break;
		}
	}
	// transform constants (e.g. ORDER BY 1) into positional references (ORDER BY #1)
	idx_t array_idx = 0;
	if (aggr->OrderBy()) {
		for (auto &order : aggr->OrderByMutable()->orders) {
			if (order.expression->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
				auto &constant_expr = order.expression->Cast<ConstantExpression>();
				int64_t order_index;
				if (constant_expr.GetLiteral().TryGetInt64(order_index)) {
					idx_t positional_index = order_index < 0 ? NumericLimits<idx_t>::Maximum() : idx_t(order_index);
					order.expression = make_uniq<PositionalReferenceExpression>(positional_index);
				}
			} else if (sub_select) {
				// if we have a SELECT we can push the ORDER BY clause into the SELECT list and reference it
				auto alias = "__array_internal_idx_" + to_string(++array_idx);
				order.expression->SetAlias(Identifier(alias));
				sub_select->select_list.push_back(std::move(order.expression));
				order.expression = make_uniq<ColumnRefExpression>(Identifier(alias));
			} else {
				// otherwise we remove order qualifications
				RemoveOrderQualificationRecursive(order.expression);
			}
		}
	}
	// ARRAY_AGG(COLUMNS(*)) IS NULL
	auto agg_is_null = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, aggr->Copy());
	// empty list
	vector<unique_ptr<ParsedExpression>> list_children;
	auto empty_list = make_uniq<FunctionExpression>("list_value", std::move(list_children));
	// CASE
	auto case_expr = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(agg_is_null);
	check.then_expr = std::move(empty_list);
	case_expr->CaseChecksMutable().push_back(std::move(check));
	case_expr->ElseMutable() = std::move(aggr);

	select_node->select_list.push_back(std::move(case_expr));

	// FROM (...) tbl
	auto child_subquery = make_uniq<SubqueryRef>(std::move(subquery_expr->SubqueryMutable()));
	select_node->from_table = std::move(child_subquery);

	auto new_subquery = make_uniq<SelectStatement>();
	new_subquery->node = std::move(select_node);
	subquery_expr->SubqueryMutable() = std::move(new_subquery);

	subquery_expr->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	return std::move(subquery_expr);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformStructExpression(PEGTransformer &transformer,
                                                 optional<vector<FunctionArgument>> struct_field) {
	// {} produces an empty STRUCT, {'a': 1, ...} a named STRUCT - both via struct_pack
	vector<FunctionArgument> fields;
	if (struct_field) {
		fields = std::move(*struct_field);
	}
	return make_uniq<FunctionExpression>("struct_pack", std::move(fields));
}

FunctionArgument PEGTransformerFactory::TransformStructField(PEGTransformer &transformer,
                                                             const Identifier &col_id_or_string,
                                                             unique_ptr<ParsedExpression> expression) {
	auto alias = col_id_or_string.GetIdentifierName();
	expression->SetAlias(Identifier(alias));

	return FunctionArgument(Identifier(std::move(alias)), std::move(expression));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformBoundedListExpression(PEGTransformer &transformer,
                                                      optional<vector<unique_ptr<ParsedExpression>>> expression) {
	if (expression) {
		return std::move(*expression);
	}
	return vector<unique_ptr<ParsedExpression>>();
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformColumnDefaultExpr(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> col_def_or_expr) {
	return col_def_or_expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformLambdaArrowExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_or_expression,
    optional<vector<unique_ptr<ParsedExpression>>> single_arrow_pair) {
	auto expr = std::move(logical_or_expression);
	if (!single_arrow_pair) {
		return expr;
	}
	for (auto &right_expr : *single_arrow_pair) {
		expr = make_uniq<LambdaExpression>(std::move(expr), std::move(right_expr));
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformSingleArrowPair(PEGTransformer &transformer,
                                                unique_ptr<ParsedExpression> logical_or_expression) {
	return logical_or_expression;
}

static unique_ptr<ParsedExpression> FoldConjunctionExpression(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> expression,
                                                              optional<vector<unique_ptr<ParsedExpression>>> tails,
                                                              ExpressionType conjunction_type) {
	auto expr = std::move(expression);
	if (!tails) {
		return expr;
	}
	auto depth_guard = transformer.StackCheck(1);
	vector<unique_ptr<ParsedExpression>> children;
	children.reserve(tails->size() + 1);
	children.push_back(std::move(expr));
	for (auto &tail : *tails) {
		children.push_back(std::move(tail));
	}
	return make_uniq<ConjunctionExpression>(conjunction_type, std::move(children));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformLogicalOrExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_and_expression,
    optional<vector<unique_ptr<ParsedExpression>>> logical_or_expression_tail) {
	return FoldConjunctionExpression(transformer, std::move(logical_and_expression),
	                                 std::move(logical_or_expression_tail), ExpressionType::CONJUNCTION_OR);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformLogicalOrExpressionTail(PEGTransformer &transformer,
                                                        unique_ptr<ParsedExpression> logical_and_expression) {
	return logical_and_expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformLogicalAndExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> logical_not_expression,
    optional<vector<unique_ptr<ParsedExpression>>> logical_and_expression_tail) {
	return FoldConjunctionExpression(transformer, std::move(logical_not_expression),
	                                 std::move(logical_and_expression_tail), ExpressionType::CONJUNCTION_AND);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformLogicalAndExpressionTail(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> logical_not_expression) {
	return logical_not_expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColDefOrExpr(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> col_def_and_expr,
    optional<vector<unique_ptr<ParsedExpression>>> col_def_or_expression_tail) {
	return FoldConjunctionExpression(transformer, std::move(col_def_and_expr), std::move(col_def_or_expression_tail),
	                                 ExpressionType::CONJUNCTION_OR);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformColDefOrExpressionTail(PEGTransformer &transformer,
                                                       unique_ptr<ParsedExpression> col_def_and_expr) {
	return col_def_and_expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColDefAndExpr(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> is_distinct_from_expression,
    optional<vector<unique_ptr<ParsedExpression>>> col_def_and_expression_tail) {
	return FoldConjunctionExpression(transformer, std::move(is_distinct_from_expression),
	                                 std::move(col_def_and_expression_tail), ExpressionType::CONJUNCTION_AND);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformColDefAndExpressionTail(PEGTransformer &transformer,
                                                        unique_ptr<ParsedExpression> is_distinct_from_expression) {
	return is_distinct_from_expression;
}

static unique_ptr<ParsedExpression> ApplyNotKeywords(PEGTransformer &transformer, const vector<bool> &not_keywords,
                                                     unique_ptr<ParsedExpression> expr) {
	auto not_depth_guard = transformer.StackCheck(not_keywords.size());
	for (idx_t i = 0; i < not_keywords.size(); i++) {
		vector<unique_ptr<ParsedExpression>> inner_list_children;
		inner_list_children.push_back(std::move(expr));
		expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(inner_list_children));
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformLogicalNotExpression(PEGTransformer &transformer,
                                                     const optional<vector<bool>> &not_expression,
                                                     unique_ptr<ParsedExpression> is_expression) {
	if (!not_expression) {
		return is_expression;
	}
	return ApplyNotKeywords(transformer, *not_expression, std::move(is_expression));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformNotComparisonOperand(PEGTransformer &transformer, const vector<bool> &not_expression,
                                                     unique_ptr<ParsedExpression> is_expression) {
	return ApplyNotKeywords(transformer, not_expression, std::move(is_expression));
}

vector<bool> PEGTransformerFactory::TransformNotExpression(PEGTransformer &transformer,
                                                           const vector<bool> &not_keyword) {
	return not_keyword;
}

static unique_ptr<ParsedExpression> ApplyIsTest(unique_ptr<ParsedExpression> expr,
                                                unique_ptr<ParsedExpression> is_expr) {
	if (is_expr->GetExpressionClass() == ExpressionClass::COMPARISON) {
		auto compare_expr = unique_ptr_cast<ParsedExpression, ComparisonExpression>(std::move(is_expr));
		compare_expr->LeftMutable() = make_uniq<CastExpression>(LogicalType::BOOLEAN, std::move(expr));
		return std::move(compare_expr);
	}
	if (is_expr->GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto operator_expr = unique_ptr_cast<ParsedExpression, OperatorExpression>(std::move(is_expr));
		operator_expr->GetChildrenMutable().insert(operator_expr->GetChildrenMutable().begin(), std::move(expr));
		return std::move(operator_expr);
	}
	throw InternalException("Unexpected expression encountered in IsExpression: %s",
	                        ExpressionClassToString(is_expr->GetExpressionClass()));
}

static unique_ptr<ParsedExpression> ApplyCastOperator(unique_ptr<ParsedExpression> expr,
                                                      unique_ptr<ParsedExpression> cast_operator) {
	auto cast_expr = unique_ptr_cast<ParsedExpression, CastExpression>(std::move(cast_operator));
	cast_expr->ChildMutable() = std::move(expr);
	return std::move(cast_expr);
}

static unique_ptr<ParsedExpression> ApplyIsDistinctFromTail(unique_ptr<ParsedExpression> expr,
                                                            IsDistinctFromTail tail) {
	return make_uniq<ComparisonExpression>(tail.comparison_type, std::move(expr), std::move(tail.expression));
}

static unique_ptr<ParsedExpression> ApplyComparisonTail(unique_ptr<ParsedExpression> expr,
                                                        ComparisonExpressionTail tail) {
	return make_uniq<ComparisonExpression>(tail.comparison_type, std::move(expr), std::move(tail.expression));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIsExpression(PEGTransformer &transformer,
                                             unique_ptr<ParsedExpression> is_distinct_from_expression,
                                             optional<vector<IsExpressionTail>> is_expression_continuation) {
	auto expr = std::move(is_distinct_from_expression);
	if (!is_expression_continuation) {
		return expr;
	}
	auto previous_type = ExpressionTailType::IS_TEST;
	for (auto &tail : *is_expression_continuation) {
		if (tail.type == previous_type &&
		    (tail.type == ExpressionTailType::DISTINCT || tail.type == ExpressionTailType::COMPARISON)) {
			throw ParserException("Chained comparisons are not supported, use AND to combine comparisons");
		}
		switch (tail.type) {
		case ExpressionTailType::IS_TEST:
			expr = ApplyIsTest(std::move(expr), std::move(tail.test));
			break;
		case ExpressionTailType::DISTINCT:
			expr = ApplyIsDistinctFromTail(std::move(expr), std::move(tail.distinct));
			break;
		case ExpressionTailType::COMPARISON:
			expr = ApplyComparisonTail(std::move(expr), std::move(tail.comparison));
			break;
		case ExpressionTailType::OTHER_OPERATOR: {
			vector<OtherOperatorTail> other_tail;
			other_tail.push_back(std::move(tail.other));
			expr = TransformInfixOtherOperatorExpression(transformer, std::move(expr), std::move(other_tail));
			break;
		}
		case ExpressionTailType::CAST:
			expr = ApplyCastOperator(std::move(expr), std::move(tail.cast));
			break;
		}
		previous_type = tail.type;
	}
	return expr;
}

vector<IsExpressionTail>
PEGTransformerFactory::TransformIsExpressionContinuation(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> is_test,
                                                         optional<vector<IsExpressionTail>> is_expression_tail) {
	vector<IsExpressionTail> result;
	result.push_back(TransformIsTestTail(transformer, std::move(is_test)));
	if (is_expression_tail) {
		for (auto &tail : *is_expression_tail) {
			result.push_back(std::move(tail));
		}
	}
	return result;
}

IsExpressionTail PEGTransformerFactory::TransformIsTestTail(PEGTransformer &transformer,
                                                            unique_ptr<ParsedExpression> is_test) {
	IsExpressionTail result;
	result.test = std::move(is_test);
	return result;
}

IsExpressionTail PEGTransformerFactory::TransformIsDistinctTail(PEGTransformer &transformer,
                                                                IsDistinctFromTail is_distinct_from_tail) {
	IsExpressionTail result;
	result.type = ExpressionTailType::DISTINCT;
	result.distinct = std::move(is_distinct_from_tail);
	return result;
}

IsExpressionTail PEGTransformerFactory::TransformIsComparisonTail(PEGTransformer &transformer,
                                                                  ComparisonExpressionTail comparison_expression_tail) {
	IsExpressionTail result;
	result.type = ExpressionTailType::COMPARISON;
	result.comparison = std::move(comparison_expression_tail);
	return result;
}

IsExpressionTail PEGTransformerFactory::TransformIsOtherOperatorTail(PEGTransformer &transformer,
                                                                     OtherOperatorTail other_operator_tail) {
	IsExpressionTail result;
	result.type = ExpressionTailType::OTHER_OPERATOR;
	result.other = std::move(other_operator_tail);
	return result;
}

IsExpressionTail PEGTransformerFactory::TransformIsCastTail(PEGTransformer &transformer,
                                                            unique_ptr<ParsedExpression> cast_operator) {
	IsExpressionTail result;
	result.type = ExpressionTailType::CAST;
	result.cast = std::move(cast_operator);
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformIsLiteral(PEGTransformer &transformer,
                                                                       const bool &has_result,
                                                                       const Value &is_literal_value) {
	if (is_literal_value.IsNull()) {
		auto expr_type = has_result ? ExpressionType::OPERATOR_IS_NOT_NULL : ExpressionType::OPERATOR_IS_NULL;
		return make_uniq<OperatorExpression>(expr_type, nullptr);
	}
	auto expr_type = has_result ? ExpressionType::COMPARE_DISTINCT_FROM : ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	return make_uniq<ComparisonExpression>(expr_type, nullptr, ConstantExpression::FromValue(is_literal_value));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNotNullKeyword(PEGTransformer &transformer) {
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, nullptr);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformNotNullOperator(PEGTransformer &transformer) {
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, nullptr);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformIsNullOperator(PEGTransformer &transformer) {
	return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL, nullptr);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformIsNull(PEGTransformer &transformer,
                                                                    unique_ptr<ParsedExpression> is_null_operator) {
	return is_null_operator;
}

bool PEGTransformerFactory::TransformNotKeyword(PEGTransformer &transformer) {
	return true;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIsDistinctFromExpression(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> comparison_expression,
                                                         optional<vector<IsDistinctFromTail>> is_distinct_from_tail) {
	auto expr = std::move(comparison_expression);
	if (!is_distinct_from_tail) {
		return expr;
	}
	if (is_distinct_from_tail->size() > 1) {
		throw ParserException("Chained comparisons are not supported, use AND to combine comparisons");
	}
	for (auto &is_distinct : *is_distinct_from_tail) {
		expr = ApplyIsDistinctFromTail(std::move(expr), std::move(is_distinct));
	}
	return expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformComparisonExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> between_in_like_expression,
    optional<vector<ComparisonExpressionTail>> comparison_expression_tail) {
	auto expr = std::move(between_in_like_expression);
	if (!comparison_expression_tail) {
		return expr;
	}
	if (comparison_expression_tail->size() > 1) {
		throw ParserException("Chained comparisons are not supported, use AND to combine comparisons");
	}
	auto cmp_depth_guard = transformer.StackCheck(comparison_expression_tail->size());
	for (auto &comparison_expr : *comparison_expression_tail) {
		expr = ApplyComparisonTail(std::move(expr), std::move(comparison_expr));
	}
	return expr;
}

IsDistinctFromTail
PEGTransformerFactory::TransformIsDistinctFromTail(PEGTransformer &transformer,
                                                   const ExpressionType &is_distinct_from_op,
                                                   unique_ptr<ParsedExpression> comparison_expression) {
	return {is_distinct_from_op, std::move(comparison_expression)};
}

ComparisonExpressionTail
PEGTransformerFactory::TransformComparisonExpressionTail(PEGTransformer &transformer,
                                                         const ExpressionType &comparison_operator,
                                                         unique_ptr<ParsedExpression> comparison_operand) {
	ComparisonExpressionTail result;
	result.comparison_type = comparison_operator;
	result.expression = std::move(comparison_operand);
	return result;
}

ExpressionType PEGTransformerFactory::TransformOperatorEqual(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_EQUAL;
}

ExpressionType PEGTransformerFactory::TransformOperatorNotEqual(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_NOTEQUAL;
}

ExpressionType PEGTransformerFactory::TransformOperatorLessThan(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_LESSTHAN;
}

ExpressionType PEGTransformerFactory::TransformOperatorGreaterThan(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_GREATERTHAN;
}

ExpressionType PEGTransformerFactory::TransformOperatorLessThanEquals(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_LESSTHANOREQUALTO;
}

ExpressionType PEGTransformerFactory::TransformOperatorGreaterThanEquals(PEGTransformer &transformer) {
	return ExpressionType::COMPARE_GREATERTHANOREQUALTO;
}

bool TryNegateLikeFunction(Identifier &function_name) {
	if (function_name == "~~") {
		function_name = "!~~";
		return true;
	} else if (function_name == "~~*") {
		function_name = "!~~*";
		return true;
	} else if (function_name == "~~~") {
		return false;
	} else if (function_name == "regexp_matches") {
		return false;
	} else if (function_name == "regexp_full_match") {
		return false;
	}
	return false;
}

static string RegexMatchOperatorFunctionName(PEGTransformer &transformer) {
	if (transformer.options.regex_match_operator_semantics == RegexMatchOperatorSemantics::FULL) {
		return "regexp_full_match";
	}
	return "regexp_matches";
}

static bool IsRegexMatchFunctionName(const string &function_name) {
	auto name = function_name;
	if (StringUtil::StartsWith(name, "!")) {
		name = name.substr(1);
	}
	return name == "regexp_matches" || name == "regexp_full_match";
}

static bool TryRemoveRegexOperatorNegation(Identifier &function_name) {
	if (function_name == "!regexp_matches") {
		function_name = "regexp_matches";
		return true;
	}
	if (function_name == "!regexp_full_match") {
		function_name = "regexp_full_match";
		return true;
	}
	return false;
}

static bool TryRemoveRegexCaseInsensitiveSuffix(string &function_name) {
	static constexpr const char *REGEX_CASE_INSENSITIVE_SUFFIX = "__case_insensitive";
	if (!StringUtil::EndsWith(function_name, REGEX_CASE_INSENSITIVE_SUFFIX)) {
		return false;
	}
	function_name =
	    function_name.substr(0, function_name.size() - std::char_traits<char>::length(REGEX_CASE_INSENSITIVE_SUFFIX));
	return true;
}

static bool TryGetRegexMatchOperator(const string &op_string, PEGTransformer &transformer, string &function_name,
                                     bool &negated, bool &case_insensitive) {
	if (op_string == "~") {
		function_name = RegexMatchOperatorFunctionName(transformer);
		negated = false;
		case_insensitive = false;
		return true;
	}
	if (op_string == "!~") {
		function_name = RegexMatchOperatorFunctionName(transformer);
		negated = true;
		case_insensitive = false;
		return true;
	}
	if (op_string == "~*") {
		function_name = RegexMatchOperatorFunctionName(transformer);
		negated = false;
		case_insensitive = true;
		return true;
	}
	if (op_string == "!~*") {
		function_name = RegexMatchOperatorFunctionName(transformer);
		negated = true;
		case_insensitive = true;
		return true;
	}
	return false;
}

static unique_ptr<ParsedExpression> MakeFunctionExpression(const string &name,
                                                           vector<unique_ptr<ParsedExpression>> children) {
	return make_uniq<FunctionExpression>(Identifier(name), std::move(children));
}

static unique_ptr<ParsedExpression> MakeBooleanConstant(bool value) {
	return ConstantExpression::Boolean(value);
}

static unique_ptr<ParsedExpression> MakeListContains(unique_ptr<ParsedExpression> list, bool value) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(list));
	children.push_back(MakeBooleanConstant(value));
	return MakeFunctionExpression("list_contains", std::move(children));
}

static unique_ptr<ParsedExpression> MakeListHasNull(unique_ptr<ParsedExpression> list) {
	auto is_null = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NULL,
	                                             make_uniq<ColumnRefExpression>("__regex_match"));

	auto null_check_lambda = make_uniq<LambdaExpression>(vector<string> {"__regex_match"}, std::move(is_null));
	vector<unique_ptr<ParsedExpression>> filter_children;
	filter_children.push_back(std::move(list));
	filter_children.push_back(std::move(null_check_lambda));
	auto null_matches = MakeFunctionExpression("list_filter", std::move(filter_children));

	vector<unique_ptr<ParsedExpression>> length_children;
	length_children.push_back(std::move(null_matches));
	auto null_count = MakeFunctionExpression("len", std::move(length_children));

	return make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN, std::move(null_count),
	                                       ConstantExpression::Integer(0));
}

static unique_ptr<ParsedExpression> TransformRegexAnyAllList(unique_ptr<ParsedExpression> left_expr,
                                                             unique_ptr<ParsedExpression> right_expr,
                                                             const string &function_name, bool negated,
                                                             bool case_insensitive, bool is_any) {
	vector<unique_ptr<ParsedExpression>> regex_children;
	regex_children.push_back(std::move(left_expr));
	regex_children.push_back(make_uniq<ColumnRefExpression>("__regex_pattern"));
	if (case_insensitive) {
		regex_children.push_back(ConstantExpression::String("i"));
	}
	unique_ptr<ParsedExpression> regex_match = MakeFunctionExpression(function_name, std::move(regex_children));
	if (negated) {
		regex_match = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(regex_match));
	}

	auto pattern_lambda = make_uniq<LambdaExpression>(vector<string> {"__regex_pattern"}, std::move(regex_match));
	vector<unique_ptr<ParsedExpression>> transform_children;
	transform_children.push_back(std::move(right_expr));
	transform_children.push_back(std::move(pattern_lambda));
	auto match_list = MakeFunctionExpression("list_transform", std::move(transform_children));

	auto result = make_uniq<CaseExpression>();
	CaseCheck has_decisive_value;
	has_decisive_value.when_expr = MakeListContains(match_list->Copy(), is_any);
	has_decisive_value.then_expr = MakeBooleanConstant(is_any);
	result->CaseChecksMutable().push_back(std::move(has_decisive_value));

	CaseCheck has_null_value;
	has_null_value.when_expr = MakeListHasNull(match_list->Copy());
	has_null_value.then_expr = ConstantExpression::Null();
	result->CaseChecksMutable().push_back(std::move(has_null_value));

	result->ElseMutable() = MakeBooleanConstant(!is_any);
	return std::move(result);
}

static unique_ptr<ParsedExpression> ApplyBetweenInLikeOperator(unique_ptr<ParsedExpression> expr,
                                                               BetweenInLikeOperator &predicate) {
	auto between_in_like_expr = std::move(predicate.expression);
	bool has_not = predicate.has_not;
	if (between_in_like_expr->GetExpressionClass() == ExpressionClass::BETWEEN) {
		auto between_expr = unique_ptr_cast<ParsedExpression, BetweenExpression>(std::move(between_in_like_expr));
		between_expr->InputMutable() = std::move(expr);
		if (has_not) {
			expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(between_expr));
		} else {
			expr = std::move(between_expr);
		}
	} else if (between_in_like_expr->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto func_expr = unique_ptr_cast<ParsedExpression, FunctionExpression>(std::move(between_in_like_expr));
		if (func_expr->FunctionName() == "contains") {
			func_expr->GetArgumentsMutable().push_back(std::move(expr));
		} else {
			func_expr->GetArgumentsMutable().insert(func_expr->GetArgumentsMutable().begin(), std::move(expr));
		}
		auto function_name = func_expr->FunctionName();
		auto regex_operator_negated = TryRemoveRegexOperatorNegation(function_name);
		bool negated_like = has_not && !regex_operator_negated && TryNegateLikeFunction(function_name);
		func_expr->SetQualifiedName(func_expr->GetQualifiedName().WithName(std::move(function_name)));
		if (has_not) {
			if (regex_operator_negated) {
				expr = std::move(func_expr);
			} else if (!negated_like) {
				// If it wasn't a special "Like" function, wrap it in a standard NOT operator
				expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(func_expr));
			} else {
				expr = std::move(func_expr);
			}
		} else if (regex_operator_negated) {
			expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(func_expr));
		} else {
			expr = std::move(func_expr);
		}
	} else if (between_in_like_expr->GetExpressionClass() == ExpressionClass::OPERATOR) {
		auto &operator_expr = between_in_like_expr->Cast<OperatorExpression>();
		operator_expr.GetChildrenMutable().insert(operator_expr.GetChildrenMutable().begin(), std::move(expr));
		if (has_not) {
			expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(between_in_like_expr));
		} else {
			expr = std::move(between_in_like_expr);
		}
	} else if (between_in_like_expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = between_in_like_expr->Cast<SubqueryExpression>();
		subquery_expr.GetChildMutable() = std::move(expr);
		if (has_not) {
			expr = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(between_in_like_expr));
		} else {
			expr = std::move(between_in_like_expr);
		}
	}
	return expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformBetweenInLikeExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> other_operator_expression,
    optional<vector<BetweenInLikeOperator>> in_predicate, optional<BetweenInLikeOperator> between_like_op) {
	auto expr = std::move(other_operator_expression);
	if (!in_predicate && !between_like_op) {
		return expr;
	}
	idx_t predicate_count = 0;
	if (in_predicate) {
		for (auto &predicate : *in_predicate) {
			predicate_count += 1 + predicate.casts.size();
		}
	}
	if (between_like_op) {
		predicate_count++;
	}
	auto depth_guard = transformer.StackCheck(predicate_count);
	if (in_predicate) {
		for (auto &predicate : *in_predicate) {
			expr = ApplyBetweenInLikeOperator(std::move(expr), predicate);
			for (auto &cast : predicate.casts) {
				expr = ApplyCastOperator(std::move(expr), std::move(cast));
			}
		}
	}
	if (between_like_op) {
		expr = ApplyBetweenInLikeOperator(std::move(expr), *between_like_op);
	}
	return expr;
}

BetweenInLikeOperator
PEGTransformerFactory::TransformInPredicate(PEGTransformer &transformer, const bool &has_result,
                                            unique_ptr<ParsedExpression> in_clause,
                                            optional<vector<unique_ptr<ParsedExpression>>> cast_operator) {
	BetweenInLikeOperator result {has_result, std::move(in_clause)};
	if (cast_operator) {
		result.casts = std::move(*cast_operator);
	}
	return result;
}

BetweenInLikeOperator
PEGTransformerFactory::TransformBetweenLikeOp(PEGTransformer &transformer, const bool &has_result,
                                              unique_ptr<ParsedExpression> between_like_op_expression) {
	if (has_result && between_like_op_expression->GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function_name = between_like_op_expression->Cast<FunctionExpression>().FunctionName();
		if (StringUtil::StartsWith(function_name.GetIdentifierName(), "!")) {
			throw ParserException("NOT cannot precede an operator that is already negated (!~~, !~~*, !~ or !~*)");
		}
	}
	return {has_result, std::move(between_like_op_expression)};
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformInClause(PEGTransformer &transformer,
                                                                      unique_ptr<ParsedExpression> in_expression) {
	return in_expression;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformInContainsExpression(PEGTransformer &transformer,
                                                     unique_ptr<ParsedExpression> other_operator_expression) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(other_operator_expression));
	return make_uniq<FunctionExpression>("contains", std::move(children));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformInExpressionList(PEGTransformer &transformer,
                                                 vector<unique_ptr<ParsedExpression>> expression) {
	auto in_children = std::move(expression);
	if (in_children.size() == 1 && in_children[0]->GetExpressionClass() == ExpressionClass::SUBQUERY) {
		auto &subquery_expr = in_children[0]->Cast<SubqueryExpression>();
		auto result = make_uniq<SubqueryExpression>();
		result->GetSubqueryTypeMutable() = SubqueryType::ANY;
		result->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
		result->SubqueryMutable() = std::move(subquery_expr.SubqueryMutable());
		return std::move(result);
	}
	auto result = make_uniq<OperatorExpression>(ExpressionType::COMPARE_IN, std::move(in_children));
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformInSelectStatement(PEGTransformer &transformer,
                                                  unique_ptr<SelectStatement> select_statement_internal) {
	auto result = make_uniq<SubqueryExpression>();
	result->GetSubqueryTypeMutable() = SubqueryType::ANY;
	result->GetComparisonTypeMutable() = ExpressionType::COMPARE_EQUAL;
	result->SubqueryMutable() = std::move(select_statement_internal);
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformBetweenClause(PEGTransformer &transformer, const bool &,
                                              unique_ptr<ParsedExpression> other_operator_expression,
                                              unique_ptr<ParsedExpression> other_operator_expression_1) {
	auto result = make_uniq<BetweenExpression>(nullptr, std::move(other_operator_expression),
	                                           std::move(other_operator_expression_1));
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformLikeClause(PEGTransformer &transformer, const string &like_variations,
                                           unique_ptr<ParsedExpression> other_operator_expression,
                                           optional<unique_ptr<ParsedExpression>> escape_clause) {
	string like_variation = like_variations;
	bool case_insensitive_regex = TryRemoveRegexCaseInsensitiveSuffix(like_variation);
	bool is_regex_operator = IsRegexMatchFunctionName(like_variation);
	vector<unique_ptr<ParsedExpression>> like_children;
	like_children.push_back(std::move(other_operator_expression));
	if (case_insensitive_regex && escape_clause) {
		throw ParserException(
		    "ESCAPE clause is not supported with case-insensitive regular expression match operators");
	}
	if (is_regex_operator && escape_clause) {
		throw ParserException("ESCAPE clause is not supported with SIMILAR TO and regular expression match operators");
	}
	if (escape_clause) {
		if (like_variation == "~~") {
			like_variation = "like_escape";
		} else if (like_variation == "~~*") {
			like_variation = "ilike_escape";
		}
		like_children.push_back(std::move(*escape_clause));
	}
	if (case_insensitive_regex) {
		like_children.push_back(ConstantExpression::String("i"));
	}
	auto result = make_uniq<FunctionExpression>(Identifier(like_variation), std::move(like_children));
	if (!is_regex_operator) {
		result->IsOperatorMutable() = true;
	}
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformEscapeClause(PEGTransformer &transformer,
                                             unique_ptr<ParsedExpression> other_operator_expression) {
	return other_operator_expression;
}

string PEGTransformerFactory::TransformLikeToken(PEGTransformer &transformer) {
	return "~~";
}

string PEGTransformerFactory::TransformILikeToken(PEGTransformer &transformer) {
	return "~~*";
}

string PEGTransformerFactory::TransformGlobToken(PEGTransformer &transformer) {
	return "~~~";
}

string PEGTransformerFactory::TransformSimilarToToken(PEGTransformer &transformer) {
	return "regexp_full_match";
}

string PEGTransformerFactory::TransformRegexMatchToken(PEGTransformer &transformer) {
	return RegexMatchOperatorFunctionName(transformer);
}

string PEGTransformerFactory::TransformRegexInsensitiveMatchToken(PEGTransformer &transformer) {
	return RegexMatchOperatorFunctionName(transformer) + "__case_insensitive";
}

string PEGTransformerFactory::TransformNotILikeOp(PEGTransformer &transformer) {
	return "!~~*";
}

string PEGTransformerFactory::TransformNotLikeOp(PEGTransformer &transformer) {
	return "!~~";
}

string PEGTransformerFactory::TransformNotRegexInsensitiveMatchOp(PEGTransformer &transformer) {
	return "!" + RegexMatchOperatorFunctionName(transformer) + "__case_insensitive";
}

string PEGTransformerFactory::TransformNotSimilarToOp(PEGTransformer &transformer) {
	return "!" + RegexMatchOperatorFunctionName(transformer);
}

static unique_ptr<ParsedExpression> TransformOperatorFunction(const string &operator_name,
                                                              vector<unique_ptr<ParsedExpression>> children) {
	vector split_operator = StringUtil::Split(operator_name, ".");
	string schema_name;
	string function_name;
	if (split_operator.size() == 1) {
		function_name = std::move(split_operator[0]);
	} else if (split_operator.size() == 2) {
		schema_name = std::move(split_operator[0]);
		function_name = std::move(split_operator[1]);
	} else {
		throw ParserException("Too many identifiers found, expected schema.operator or operator");
	}

	auto result = make_uniq<FunctionExpression>(
	    QualifiedName(Identifier(), Identifier(std::move(schema_name)), Identifier(std::move(function_name))),
	    std::move(children));
	result->IsOperatorMutable() = true;
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformInfixOtherOperatorExpression(PEGTransformer &transformer,
                                                             unique_ptr<ParsedExpression> bitwise_expression,
                                                             optional<vector<OtherOperatorTail>> other_operator_tail) {
	auto expr = std::move(bitwise_expression);
	if (!other_operator_tail) {
		return expr;
	}
	for (auto &other_operator_expr : *other_operator_tail) {
		auto right_expr = std::move(other_operator_expr.expression);
		if (other_operator_expr.op.is_any_all) {
			auto op_string = other_operator_expr.op.name;
			auto is_any = other_operator_expr.op.is_any;

			// Map operator string to ExpressionType (INVALID if not a comparison operator)
			auto expression_type = OperatorToExpressionType(op_string);

			auto subquery_expr = make_uniq<SubqueryExpression>();
			if (right_expr->GetExpressionClass() == ExpressionClass::SUBQUERY) {
				if (expression_type == ExpressionType::INVALID) {
					throw ParserException("ANY and ALL operators require one of =,<>,>,<,>=,<= comparisons!");
				}
				subquery_expr->GetSubqueryTypeMutable() = SubqueryType::ANY;
				subquery_expr->GetComparisonTypeMutable() = expression_type;
				auto &right_expr_subquery = right_expr->Cast<SubqueryExpression>();
				subquery_expr->SubqueryMutable() = std::move(right_expr_subquery.SubqueryMutable());
				subquery_expr->GetChildMutable() = std::move(expr);
				if (!is_any) {
					// ALL sublink is equivalent to NOT(ANY) with inverted comparison
					// e.g. [= ALL()] is equivalent to [NOT(<> ANY())]
					// first invert the comparison type
					subquery_expr->GetComparisonTypeMutable() =
					    NegateComparisonExpression(subquery_expr->GetComparisonType());
					return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subquery_expr));
				}
				expr = std::move(subquery_expr);
			} else {
				string regex_function_name;
				bool regex_negated;
				bool regex_case_insensitive;
				if (TryGetRegexMatchOperator(op_string, transformer, regex_function_name, regex_negated,
				                             regex_case_insensitive)) {
					expr = TransformRegexAnyAllList(std::move(expr), std::move(right_expr), regex_function_name,
					                                regex_negated, regex_case_insensitive, is_any);
					continue;
				}
				// left=ANY(right)
				// we turn this into left=ANY((SELECT UNNEST(right)))
				if (expression_type == ExpressionType::INVALID) {
					throw ParserException("Unsupported comparison \"%s\" for ANY/ALL subquery", op_string);
				}
				auto select_statement = make_uniq<SelectStatement>();
				auto select_node = make_uniq<SelectNode>();
				vector<unique_ptr<ParsedExpression>> children;
				children.push_back(std::move(right_expr));

				select_node->select_list.push_back(make_uniq<FunctionExpression>("UNNEST", std::move(children)));
				select_node->from_table = make_uniq<EmptyTableRef>();
				select_statement->node = std::move(select_node);
				subquery_expr->SubqueryMutable() = std::move(select_statement);
				subquery_expr->GetSubqueryTypeMutable() = SubqueryType::ANY;
				subquery_expr->GetChildMutable() = std::move(expr);
				subquery_expr->GetComparisonTypeMutable() = expression_type;
				if (!is_any) {
					// ALL sublink is equivalent to NOT(ANY) with inverted comparison
					// e.g. [= ALL()] is equivalent to [NOT(<> ANY())]
					// first invert the comparison type
					subquery_expr->GetComparisonTypeMutable() =
					    NegateComparisonExpression(subquery_expr->GetComparisonType());
					return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(subquery_expr));
				}
				return std::move(subquery_expr);
			}
		} else {
			auto other_operator = std::move(other_operator_expr.op.name);
			vector<unique_ptr<ParsedExpression>> children_function;
			children_function.push_back(std::move(expr));
			children_function.push_back(std::move(right_expr));
			expr = TransformOperatorFunction(other_operator, std::move(children_function));
		}
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformCustomPrefixExpression(PEGTransformer &transformer, const string &operator_literal,
                                                       unique_ptr<ParsedExpression> other_operator_expression) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(other_operator_expression));
	return TransformOperatorFunction(operator_literal, std::move(children));
}

OtherOperatorTail PEGTransformerFactory::TransformOtherOperatorTail(PEGTransformer &transformer,
                                                                    ParsedOperator other_operator,
                                                                    unique_ptr<ParsedExpression> bitwise_expression) {
	OtherOperatorTail result;
	result.op = std::move(other_operator);
	result.expression = std::move(bitwise_expression);
	return result;
}

ParsedOperator PEGTransformerFactory::TransformAnyAllParsedOperator(PEGTransformer &transformer,
                                                                    const pair<string, bool> &any_all_operator) {
	ParsedOperator result;
	result.name = any_all_operator.first;
	result.is_any_all = true;
	result.is_any = any_all_operator.second;
	return result;
}

ParsedOperator PEGTransformerFactory::TransformNamedOtherOperator(PEGTransformer &transformer, const string &child) {
	ParsedOperator result;
	result.name = child;
	return result;
}

string PEGTransformerFactory::TransformQualifiedOperator(PEGTransformer &transformer,
                                                         const string &qualified_operator_contents) {
	return qualified_operator_contents;
}

string PEGTransformerFactory::TransformQualifiedOperatorContents(PEGTransformer &transformer,
                                                                 const optional<vector<string>> &col_id_dot,
                                                                 const string &any_operator_literal) {
	vector<string> result;
	if (col_id_dot) {
		result = *col_id_dot;
	}
	result.push_back(any_operator_literal);
	return StringUtil::Join(result, ".");
}

pair<string, bool> PEGTransformerFactory::TransformAnyAllOperator(PEGTransformer &transformer,
                                                                  const string &any_operator_literal,
                                                                  const bool &any_or_all) {
	return make_pair(any_operator_literal, any_or_all);
}

bool PEGTransformerFactory::TransformSubqueryAny(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformSubqueryAll(PEGTransformer &transformer) {
	return false;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformBitwiseExpression(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> tilde_expression,
                                                  optional<vector<BinaryExpressionTail>> bitwise_expression_tail) {
	auto expr = std::move(tilde_expression);
	if (!bitwise_expression_tail) {
		return expr;
	}
	auto bit_depth_guard = transformer.StackCheck(bitwise_expression_tail->size());
	for (auto &bit_expr : *bitwise_expression_tail) {
		vector<unique_ptr<ParsedExpression>> bit_children;
		bit_children.push_back(std::move(expr));
		bit_children.push_back(std::move(bit_expr.expression));
		auto func_expr = make_uniq<FunctionExpression>(Identifier(std::move(bit_expr.op)), std::move(bit_children));
		func_expr->IsOperatorMutable() = true;
		expr = std::move(func_expr);
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformAdditiveExpression(PEGTransformer &transformer,
                                                   unique_ptr<ParsedExpression> multiplicative_expression,
                                                   optional<vector<BinaryExpressionTail>> additive_expression_tail) {
	auto expr = std::move(multiplicative_expression);
	if (!additive_expression_tail) {
		return expr;
	}
	auto add_depth_guard = transformer.StackCheck(additive_expression_tail->size());
	for (auto &term_expr : *additive_expression_tail) {
		vector<unique_ptr<ParsedExpression>> term_children;
		term_children.push_back(std::move(expr));
		term_children.push_back(std::move(term_expr.expression));
		auto func_expr = make_uniq<FunctionExpression>(Identifier(std::move(term_expr.op)), std::move(term_children));
		func_expr->IsOperatorMutable() = true;
		if (term_expr.query_location.IsValid()) {
			transformer.SetQueryLocation(*func_expr, term_expr.query_location);
		}
		expr = std::move(func_expr);
	}
	return expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformMultiplicativeExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> exponentiation_expression,
    optional<vector<BinaryExpressionTail>> multiplicative_expression_tail) {
	auto expr = std::move(exponentiation_expression);
	if (!multiplicative_expression_tail) {
		return expr;
	}
	auto mul_depth_guard = transformer.StackCheck(multiplicative_expression_tail->size());
	for (auto &factor_expr : *multiplicative_expression_tail) {
		auto factor = std::move(factor_expr.op);
		if (factor == "/" && transformer.options.integer_division) {
			factor = "//";
		}
		vector<unique_ptr<ParsedExpression>> factor_children;
		factor_children.push_back(std::move(expr));
		factor_children.push_back(std::move(factor_expr.expression));
		auto func_expr = make_uniq<FunctionExpression>(Identifier(std::move(factor)), std::move(factor_children));
		func_expr->IsOperatorMutable() = true;
		expr = std::move(func_expr);
	}
	return expr;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExponentiationExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> collate_expression,
    optional<vector<BinaryExpressionTail>> exponentiation_expression_tail) {
	auto expr = std::move(collate_expression);
	if (!exponentiation_expression_tail) {
		return expr;
	}
	for (auto &exponent_expr : *exponentiation_expression_tail) {
		vector<unique_ptr<ParsedExpression>> exponent_children;
		exponent_children.push_back(std::move(expr));
		exponent_children.push_back(std::move(exponent_expr.expression));
		auto func_expr =
		    make_uniq<FunctionExpression>(Identifier(std::move(exponent_expr.op)), std::move(exponent_children));
		func_expr->IsOperatorMutable() = true;
		expr = std::move(func_expr);
	}
	return expr;
}

BinaryExpressionTail
PEGTransformerFactory::TransformBitwiseExpressionTail(PEGTransformer &transformer, const string &bit_operator,
                                                      unique_ptr<ParsedExpression> tilde_expression) {
	return {bit_operator, std::move(tilde_expression), optional_idx()};
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformTildeExpression(PEGTransformer &transformer,
                                                optional<vector<string>> tilde_prefix_operator,
                                                unique_ptr<ParsedExpression> additive_expression) {
	auto expr = std::move(additive_expression);
	if (!tilde_prefix_operator) {
		return expr;
	}
	auto tilde_depth_guard = transformer.StackCheck(tilde_prefix_operator->size());
	for (const auto &prefix : *tilde_prefix_operator) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(expr));
		expr = TransformOperatorFunction(prefix, std::move(children));
	}
	return expr;
}

BinaryExpressionTail
PEGTransformerFactory::TransformAdditiveExpressionTail(PEGTransformer &transformer, const string &term,
                                                       unique_ptr<ParsedExpression> multiplicative_expression,
                                                       optional_idx query_location) {
	return {term, std::move(multiplicative_expression), query_location};
}

BinaryExpressionTail
PEGTransformerFactory::TransformMultiplicativeExpressionTail(PEGTransformer &transformer, const string &factor,
                                                             unique_ptr<ParsedExpression> exponentiation_expression) {
	return {factor, std::move(exponentiation_expression), optional_idx()};
}

BinaryExpressionTail PEGTransformerFactory::TransformExponentiationExpressionTail(
    PEGTransformer &transformer, const string &exponent_operator, unique_ptr<ParsedExpression> collate_expression) {
	return {exponent_operator, std::move(collate_expression), optional_idx()};
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCollateExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> at_time_zone_expression,
    optional<vector<unique_ptr<ParsedExpression>>> collate_expression_tail) {
	auto expr = std::move(at_time_zone_expression);
	if (!collate_expression_tail) {
		return expr;
	}
	for (auto &collate_string_expr : *collate_expression_tail) {
		string collate_string;
		if (collate_string_expr->GetExpressionClass() == ExpressionClass::CONSTANT) {
			auto &const_expr = collate_string_expr->Cast<ConstantExpression>();
			collate_string = const_expr.GetLiteral().ToValue().GetValue<string>();
		} else if (collate_string_expr->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
			auto &col_ref = collate_string_expr->Cast<ColumnRefExpression>();
			collate_string = StringUtil::Join(col_ref.ColumnNames(), ".");
		} else {
			throw NotImplementedException("Unexpected expression encountered for collate, %s",
			                              EnumUtil::ToString(collate_string_expr->GetExpressionClass()));
		}
		auto collate_expr = make_uniq<CollateExpression>(collate_string, std::move(expr));
		expr = std::move(collate_expr);
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformCollateExpressionTail(PEGTransformer &transformer,
                                                      unique_ptr<ParsedExpression> at_time_zone_expression) {
	return at_time_zone_expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformAtTimeZoneExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> prefix_expression,
    optional<vector<unique_ptr<ParsedExpression>>> at_time_zone_expression_tail) {
	auto expr = std::move(prefix_expression);
	if (!at_time_zone_expression_tail) {
		return expr;
	}
	for (auto &time_zone_expr : *at_time_zone_expression_tail) {
		vector<unique_ptr<ParsedExpression>> time_zone_children;
		time_zone_children.push_back(std::move(time_zone_expr));
		time_zone_children.push_back(std::move(expr));
		auto func_expr = make_uniq<FunctionExpression>("timezone", std::move(time_zone_children));
		expr = std::move(func_expr);
	}
	return expr;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformAtTimeZoneExpressionTail(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> prefix_expression) {
	return prefix_expression;
}

void PEGTransformerFactory::InitializePrefixExpressionTrampoline(PEGTransformer &transformer,
                                                                 GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &prefix_opt = list_pr.Child<OptionalParseResult>(0);
	idx_t prefix_count = 0;
	if (prefix_opt.HasResult()) {
		auto &prefix_repeat = prefix_opt.GetResult().Cast<RepeatParseResult>();
		auto prefix_children = prefix_repeat.GetChildren();
		prefix_count = prefix_children.size();
		process.ReserveChildSlots(1 + prefix_count);
		for (idx_t i = prefix_children.size(); i > 0; i--) {
			auto child_idx = i - 1;
			process.PushChild({prefix_children[child_idx].get()}, 1 + child_idx);
		}
	} else {
		process.ReserveChildSlots(1);
	}
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizePrefixExpressionTrampoline(PEGTransformer &transformer,
                                                          GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &prefix_opt = list_pr.Child<OptionalParseResult>(0);
	auto expr = process.TakeResult<unique_ptr<ParsedExpression>>(0);

	if (!prefix_opt.HasResult()) {
		return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(expr));
	}

	auto &prefix_repeat = prefix_opt.GetResult().Cast<RepeatParseResult>();
	vector<string> prefixes;
	auto prefix_children = prefix_repeat.GetChildren();
	for (idx_t i = 0; i < prefix_children.size(); i++) {
		prefixes.push_back(process.TakeResult<string>(1 + i));
	}

	for (auto it = prefixes.rbegin(); it != prefixes.rend(); ++it) {
		const string &prefix = *it;
		if (prefix == "-" && expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
			// fold the sign into the number text so -9223372036854775808 stays exact
			auto &literal = expr->Cast<ConstantExpression>().GetLiteral();
			if (literal.IsNumeric()) {
				expr = make_uniq<ConstantExpression>(literal.Negate());
				continue;
			}
		}
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(expr));
		auto func_expr = make_uniq<FunctionExpression>(Identifier(prefix), std::move(children));
		func_expr->IsOperatorMutable() = true;
		expr = std::move(func_expr);
	}
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(expr));
}
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformAnonymousParameter(PEGTransformer &transformer) {
	// AnonymousParameter <- '?'
	auto expr = make_uniq<ParameterExpression>();

	// Auto-increment the parameter count
	idx_t known_param_index = transformer.ParamCount() + 1;
	string identifier = StringUtil::Format("%d", known_param_index);

	// Register it
	transformer.SetParam(Identifier(identifier), known_param_index, PreparedParamType::AUTO_INCREMENT);
	transformer.SetParamCount(MaxValue<idx_t>(transformer.ParamCount(), known_param_index));
	transformer.has_anonymous_parameters = true;

	expr->IdentifierMutable() = Identifier(identifier);
	return std::move(expr);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformQuestionMarkNumberedParameter(PEGTransformer &transformer,
                                                              unique_ptr<ParsedExpression> number_literal) {
	// QuestionMarkNumberedParameter <- '?' NumberLiteral
	auto &const_expr = number_literal->Cast<ConstantExpression>();
	int64_t param_number;
	if (!const_expr.GetLiteral().TryGetInt64(param_number) || param_number <= 0) {
		throw ParserException("Parameter numbers must be greater than 0");
	}

	auto expr = make_uniq<ParameterExpression>();
	string identifier = std::to_string(param_number);
	idx_t known_param_index = DConstants::INVALID_INDEX;

	transformer.GetParam(Identifier(identifier), known_param_index, PreparedParamType::POSITIONAL);

	if (known_param_index == DConstants::INVALID_INDEX) {
		known_param_index = NumericCast<idx_t>(param_number);
		transformer.SetParam(Identifier(identifier), known_param_index, PreparedParamType::POSITIONAL);
	}

	expr->IdentifierMutable() = Identifier(identifier);
	transformer.SetParamCount(MaxValue<idx_t>(transformer.ParamCount(), known_param_index));
	return std::move(expr);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformNumberedParameter(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> number_literal) {
	// NumberedParameter <- '$' NumberLiteral
	auto &const_expr = number_literal->Cast<ConstantExpression>();
	int64_t param_number;
	if (!const_expr.GetLiteral().TryGetInt64(param_number) || param_number <= 0) {
		throw ParserException("Parameter numbers must be greater than 0");
	}

	auto expr = make_uniq<ParameterExpression>();
	string identifier = std::to_string(param_number);
	idx_t known_param_index = DConstants::INVALID_INDEX;

	transformer.GetParam(Identifier(identifier), known_param_index, PreparedParamType::POSITIONAL);

	if (known_param_index == DConstants::INVALID_INDEX) {
		known_param_index = NumericCast<idx_t>(param_number);
		transformer.SetParam(Identifier(identifier), known_param_index, PreparedParamType::POSITIONAL);
	}

	expr->IdentifierMutable() = Identifier(identifier);
	transformer.SetParamCount(MaxValue<idx_t>(transformer.ParamCount(), known_param_index));
	transformer.has_anonymous_parameters = true;
	return std::move(expr);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformColLabelParameter(PEGTransformer &transformer,
                                                                               const string &col_label) {
	// ColLabelParameter <- '$' ColLabel
	const string &identifier = col_label;

	auto expr = make_uniq<ParameterExpression>();
	idx_t known_param_index = DConstants::INVALID_INDEX;

	transformer.GetParam(Identifier(identifier), known_param_index, PreparedParamType::NAMED);

	if (known_param_index == DConstants::INVALID_INDEX) {
		// New named parameter gets the next available index
		known_param_index = transformer.ParamCount() + 1;
		transformer.SetParam(Identifier(identifier), known_param_index, PreparedParamType::NAMED);
	}

	expr->IdentifierMutable() = Identifier(identifier);
	transformer.SetParamCount(MaxValue<idx_t>(transformer.ParamCount(), known_param_index));
	return std::move(expr);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformPositionalExpression(PEGTransformer &transformer,
                                                     unique_ptr<ParsedExpression> number_literal) {
	auto &const_expr = number_literal->Cast<ConstantExpression>();
	int64_t index;
	if (!const_expr.GetLiteral().TryGetInt64(index) || index <= 0) {
		throw ParserException("Positional reference node needs to be >= 1");
	}
	return make_uniq<PositionalReferenceExpression>(NumericCast<idx_t>(index));
}

// LiteralExpression <- StringLiteral / NumberLiteral / 'NULL' / 'TRUE' / 'FALSE'
unique_ptr<ParsedExpression> PEGTransformerFactory::TransformLiteralExpression(PEGTransformer &transformer,
                                                                               ParseResult &choice_result) {
	if (choice_result.Name() == "StringLiteral") {
		auto &string_literal = choice_result.Cast<StringLiteralParseResult>();
		return string_literal.ToExpression();
	}
	return transformer.Transform<unique_ptr<ParsedExpression>>(choice_result);
}

void PEGTransformerFactory::InitializeLiteralExpressionTrampoline(PEGTransformer &transformer,
                                                                  GeneratedTransformProcess &process) {
	process.ReserveChildSlots(0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeLiteralExpressionTrampoline(PEGTransformer &transformer,
                                                           GeneratedTransformProcess &process) {
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	auto &choice_pr = list_pr.Child<ChoiceParseResult>(0);
	auto &choice_result = choice_pr.GetResult();
	unique_ptr<ParsedExpression> result;
	if (choice_result.Name() == "StringLiteral") {
		auto &string_literal = choice_result.Cast<StringLiteralParseResult>();
		result = string_literal.ToExpression();
	} else if (choice_result.Name() == "NumberLiteral") {
		result = TransformNumberLiteral(transformer, choice_result);
	} else {
		auto &constant_list_pr = choice_result.Cast<ListParseResult>();
		auto &constant_choice_pr = constant_list_pr.Child<ChoiceParseResult>(0);
		auto &constant_result = constant_choice_pr.GetResult();
		Value value;
		if (constant_result.Name() == "NullLiteral") {
			value = TransformNullLiteral(transformer);
		} else if (constant_result.Name() == "TrueLiteral") {
			value = TransformTrueLiteral(transformer);
		} else if (constant_result.Name() == "FalseLiteral") {
			value = TransformFalseLiteral(transformer);
		} else {
			throw InternalException("Unexpected literal expression process child '%s'", constant_result.Name());
		}
		result = TransformConstantLiteral(transformer, value);
	}
	return transformer.MakeResult<unique_ptr<ParsedExpression>>(std::move(result));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformParensExpression(PEGTransformer &transformer,
                                                                              unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformConstantLiteral(PEGTransformer &transformer,
                                                                             const Value &child) {
	return ConstantExpression::FromValue(child);
}

Value PEGTransformerFactory::TransformFalseLiteral(PEGTransformer &transformer) {
	return Value(false);
}

Value PEGTransformerFactory::TransformTrueLiteral(PEGTransformer &transformer) {
	return Value(true);
}

Value PEGTransformerFactory::TransformNullLiteral(PEGTransformer &transformer) {
	return Value();
}

Value PEGTransformerFactory::TransformUnknownLiteral(PEGTransformer &transformer) {
	return Value();
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformPostfixOperator(PEGTransformer &transformer) {
	vector<unique_ptr<ParsedExpression>> func_children;
	return make_uniq<FunctionExpression>("factorial", std::move(func_children));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformIndirectionList(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> indirection) {
	return indirection;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCastOperator(PEGTransformer &transformer,
                                                                          const LogicalType &type) {
	// We input a dummy constant expression but replace this later with the real expression that precedes this post-fix
	// castOperator
	return make_uniq<CastExpression>(type, ConstantExpression::Null());
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformDotMethodOperator(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> method_expression) {
	return method_expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDotColumnOperator(PEGTransformer &transformer,
                                                                               const string &col_label) {
	return ConstantExpression::String(col_label);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformMethodExpression(PEGTransformer &transformer, const string &col_label,
                                                 MethodArguments method_expression_arguments) {
	if (method_expression_arguments.arguments.size() == 1 &&
	    ExpressionIsUnqualifiedStar(method_expression_arguments.arguments[0].GetExpression())) {
		// COUNT(*) gets converted into COUNT()
		method_expression_arguments.arguments.clear();
	}
	if (method_expression_arguments.has_ignore_nulls) {
		throw ParserException("RESPECT/IGNORE NULLS is not supported for non-window functions");
	}
	auto result =
	    make_uniq<FunctionExpression>(Identifier(col_label), std::move(method_expression_arguments.arguments));
	result->DistinctMutable() = method_expression_arguments.distinct;
	if (!method_expression_arguments.order_bys.empty()) {
		auto order_by_modifier = make_uniq<OrderModifier>();
		order_by_modifier->orders = std::move(method_expression_arguments.order_bys);
		result->OrderByMutable() = std::move(order_by_modifier);
	}
	return std::move(result);
}

MethodArguments
PEGTransformerFactory::TransformMethodExpressionArguments(PEGTransformer &transformer,
                                                          MethodArguments method_expression_argument_list) {
	return method_expression_argument_list;
}

MethodArguments PEGTransformerFactory::TransformMethodExpressionArgumentList(
    PEGTransformer &transformer, const optional<bool> &distinct_or_all,
    optional<vector<FunctionArgument>> method_function_arguments, optional<vector<OrderByNode>> order_by_clause,
    const optional<bool> &ignore_or_respect_nulls) {
	MethodArguments result;
	if (distinct_or_all) {
		result.distinct = *distinct_or_all;
	}
	if (method_function_arguments) {
		result.arguments = std::move(*method_function_arguments);
	}
	if (order_by_clause) {
		result.order_bys = std::move(*order_by_clause);
	}
	if (ignore_or_respect_nulls) {
		result.has_ignore_nulls = true;
		result.ignore_nulls = *ignore_or_respect_nulls;
	}
	return result;
}

vector<FunctionArgument>
PEGTransformerFactory::TransformMethodFunctionArguments(PEGTransformer &transformer,
                                                        vector<FunctionArgument> function_argument) {
	return function_argument;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformSliceExpression(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> slice_bound) {
	if (slice_bound.empty()) {
		throw ParserException("Empty subscript '[]' is not allowed");
	}
	if (slice_bound.size() == 1) {
		return make_uniq<OperatorExpression>(ExpressionType::ARRAY_EXTRACT, std::move(slice_bound));
	}
	return make_uniq<OperatorExpression>(ExpressionType::ARRAY_SLICE, std::move(slice_bound));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSliceBound(PEGTransformer &transformer,
                                           optional<unique_ptr<ParsedExpression>> expression,
                                           optional<vector<unique_ptr<ParsedExpression>>> slice_upper_bound) {
	vector<unique_ptr<ParsedExpression>> slice_bounds;
	if (!slice_upper_bound) {
		if (expression && *expression) {
			slice_bounds.push_back(std::move(*expression));
		}
		return slice_bounds;
	}
	if (expression && *expression) {
		slice_bounds.push_back(std::move(*expression));
	} else {
		slice_bounds.push_back(OperatorExpression::EmptySliceBound());
	}
	for (auto &bound : *slice_upper_bound) {
		slice_bounds.push_back(std::move(bound));
	}
	return slice_bounds;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformMinusStepSliceBound(PEGTransformer &transformer,
                                                    unique_ptr<ParsedExpression> step_slice_bound) {
	vector<unique_ptr<ParsedExpression>> slice_bounds;
	slice_bounds.push_back(OperatorExpression::EmptySliceBound());
	slice_bounds.push_back(std::move(step_slice_bound));
	return slice_bounds;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformEndStepSliceBound(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> end_slice_bound,
                                                  optional<unique_ptr<ParsedExpression>> step_slice_bound) {
	vector<unique_ptr<ParsedExpression>> slice_bounds;
	slice_bounds.push_back(std::move(end_slice_bound));
	if (step_slice_bound && *step_slice_bound) {
		slice_bounds.push_back(std::move(*step_slice_bound));
	}
	return slice_bounds;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformEndSliceBound(PEGTransformer &transformer,
                                              optional<unique_ptr<ParsedExpression>> expression) {
	// If either the lower or upper bound is not specified, we use an empty constant LIST,
	// which we handle in the execution.
	if (expression && *expression) {
		return std::move(*expression);
	}
	return OperatorExpression::EmptySliceBound();
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformStepSliceBound(PEGTransformer &transformer,
                                               optional<unique_ptr<ParsedExpression>> expression) {
	if (expression && *expression) {
		return std::move(*expression);
	}
	return OperatorExpression::EmptySliceBound();
}

unique_ptr<ColumnRefExpression> PEGTransformerFactory::TransformTableReservedColumnName(
    PEGTransformer &transformer, const Identifier &table_qualification, const Identifier &reserved_column_name) {
	return make_uniq<ColumnRefExpression>(reserved_column_name, table_qualification);
}

Identifier PEGTransformerFactory::TransformTableQualification(PEGTransformer &transformer,
                                                              const Identifier &table_name) {
	return table_name;
}

string PEGTransformerFactory::TransformColIdDot(PEGTransformer &transformer, const Identifier &col_id) {
	return col_id.GetIdentifierName();
}

vector<string> PEGTransformerFactory::TransformStarQualifierList(PEGTransformer &transformer,
                                                                 const vector<string> &col_id_dot) {
	return col_id_dot;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformStarExpression(
    PEGTransformer &transformer, const optional<vector<string>> &star_qualifier_list,
    const optional<qualified_column_set_t> &exclude_list,
    optional<case_insensitive_map_t<unique_ptr<ParsedExpression>>> replace_list,
    const optional<qualified_column_map_t<string>> &rename_list) {
	auto result = make_uniq<StarExpression>();
	if (star_qualifier_list) {
		if (star_qualifier_list->size() > 1) {
			throw ParserException("Did not expect more than one column in front of a star expression");
		}
		result->RelationNameMutable() = Identifier((*star_qualifier_list)[0]);
	}
	if (exclude_list) {
		result->ExcludeListMutable() = *exclude_list;
	}
	if (replace_list) {
		for (auto &replace_entry : *replace_list) {
			result->ReplaceListMutable()[Identifier(replace_entry.first)] = std::move(replace_entry.second);
		}
		for (auto &replace_entry : result->ReplaceList()) {
			if (result->ExcludeList().find(QualifiedColumnName(replace_entry.first)) != result->ExcludeList().end()) {
				throw ParserException("Column %s cannot occur in both EXCLUDE and REPLACE list", replace_entry.first);
			}
		}
	}
	if (rename_list) {
		for (auto &rename_entry : *rename_list) {
			result->RenameListMutable()[rename_entry.first] = Identifier(rename_entry.second);
		}
		for (auto &rename_column : result->RenameList()) {
			if (result->ExcludeList().find(rename_column.first) != result->ExcludeList().end()) {
				throw ParserException("Column \"%s\" cannot occur in both EXCLUDE and RENAME list",
				                      rename_column.first.ToString());
			}
			if (result->ReplaceList().find(rename_column.first.column) != result->ReplaceList().end()) {
				throw ParserException("Column \"%s\" cannot occur in both REPLACE and RENAME list",
				                      rename_column.first.ToString());
			}
		}
	}
	return std::move(result);
}

qualified_column_set_t PEGTransformerFactory::TransformExcludeList(PEGTransformer &transformer,
                                                                   const qualified_column_set_t &exclude_names) {
	return exclude_names;
}

qualified_column_set_t
PEGTransformerFactory::TransformExcludeNameList(PEGTransformer &transformer,
                                                const vector<QualifiedColumnName> &exclude_name) {
	qualified_column_set_t result;
	for (auto &exclude_column : exclude_name) {
		if (result.find(exclude_column) != result.end()) {
			throw ParserException("Duplicate entry \"%s\" in EXCLUDE list", exclude_column.ToString());
		}
		result.insert(exclude_column);
	}
	return result;
}

qualified_column_set_t PEGTransformerFactory::TransformExcludeNameSingle(PEGTransformer &transformer,
                                                                         const QualifiedColumnName &exclude_name) {
	qualified_column_set_t result;
	result.insert(exclude_name);
	return result;
}

QualifiedColumnName PEGTransformerFactory::TransformExcludeDottedName(PEGTransformer &transformer,
                                                                      const vector<string> &dotted_identifier) {
	return StringToQualifiedColumnName(dotted_identifier);
}

QualifiedColumnName PEGTransformerFactory::TransformExcludeColumnName(PEGTransformer &transformer,
                                                                      const Identifier &col_id_or_string) {
	return QualifiedColumnName(col_id_or_string);
}

void PEGTransformerFactory::InitializeOverClauseTrampoline(PEGTransformer &transformer,
                                                           GeneratedTransformProcess &process) {
	if (transformer.in_window_definition) {
		throw ParserException("window functions are not allowed in window definitions");
	}
	auto &list_pr = process.parse_result.Cast<ListParseResult>();
	transformer.in_window_definition = true;
	process.ReserveChildSlots(1);
	process.PushChild({list_pr.GetChild(1)}, 0);
}

arena_ptr<TransformResultValue>
PEGTransformerFactory::FinalizeOverClauseTrampoline(PEGTransformer &transformer, GeneratedTransformProcess &process) {
	auto result = process.TakeResult<unique_ptr<WindowExpression>>(0);
	transformer.in_window_definition = false;
	return transformer.MakeResult<unique_ptr<WindowExpression>>(std::move(result));
}

unique_ptr<WindowExpression> PEGTransformerFactory::TransformIdentifierWindowFrame(PEGTransformer &transformer,
                                                                                   const Identifier &identifier) {
	return transformer.GetWindowClause(identifier);
}

unique_ptr<WindowExpression> PEGTransformerFactory::TransformParensIdentifier(PEGTransformer &transformer,
                                                                              const Identifier &identifier) {
	auto window_name = identifier.GetIdentifierName();
	auto window_clause = transformer.GetWindowClause(identifier);
	if (window_clause->StartExpr() || window_clause->EndExpr() ||
	    !transformer.IsWindowFrameDefault(window_clause->WindowStart(), window_clause->WindowEnd())) {
		throw ParserException("cannot copy window \"%s\" because it has a frame clause", window_name);
	}
	return window_clause;
}

unique_ptr<WindowExpression>
PEGTransformerFactory::TransformWindowFrameContentsParens(PEGTransformer &transformer,
                                                          unique_ptr<WindowExpression> window_frame_contents) {
	return window_frame_contents;
}

unique_ptr<WindowExpression>
PEGTransformerFactory::TransformWindowFrameNameContentsParens(PEGTransformer &transformer,
                                                              unique_ptr<WindowExpression> window_frame_name_contents) {
	return window_frame_name_contents;
}

unique_ptr<WindowExpression>
PEGTransformerFactory::TransformWindowFrameNameContents(PEGTransformer &transformer,
                                                        const optional<Identifier> &base_window_name,
                                                        unique_ptr<WindowExpression> window_frame_contents) {
	if (!base_window_name) {
		return window_frame_contents;
	}
	auto window_name = base_window_name->GetIdentifierName();
	auto lower_name = StringUtil::Lower(window_name);
	if (lower_name == "partition" || lower_name == "range" || lower_name == "rows" || lower_name == "groups") {
		throw ParserException("Invalid window name \"%s\"", window_name);
	}
	auto copied_window = transformer.GetWindowClause(*base_window_name);
	if (copied_window->StartExpr() || copied_window->EndExpr() ||
	    !transformer.IsWindowFrameDefault(copied_window->WindowStart(), copied_window->WindowEnd())) {
		throw ParserException("cannot copy window \"%s\" because it has a frame clause", window_name);
	}
	copied_window->WindowStartMutable() = window_frame_contents->WindowStart();
	copied_window->WindowEndMutable() = window_frame_contents->WindowEnd();
	copied_window->WindowExcludeMutable() = window_frame_contents->WindowExclude();
	copied_window->StartExprMutable() = std::move(window_frame_contents->StartExprMutable());
	copied_window->EndExprMutable() = std::move(window_frame_contents->EndExprMutable());

	if (!copied_window->OrderBy().empty() && !window_frame_contents->OrderBy().empty()) {
		throw ParserException("Cannot override ORDER BY clause of window \"%s\"", window_name);
	}
	if (copied_window->OrderBy().empty()) {
		copied_window->OrderByMutable() = std::move(window_frame_contents->OrderByMutable());
	}
	if (!copied_window->Partitions().empty() && !window_frame_contents->Partitions().empty()) {
		throw ParserException("Cannot override PARTITION BY clause of window \"%s\"", window_name);
	}
	if (copied_window->Partitions().empty()) {
		copied_window->PartitionsMutable() = std::move(window_frame_contents->PartitionsMutable());
	}
	return copied_window;
}

Identifier PEGTransformerFactory::TransformBaseWindowName(PEGTransformer &transformer, const Identifier &identifier) {
	return identifier;
}

unique_ptr<WindowExpression> PEGTransformerFactory::TransformWindowFrameContents(
    PEGTransformer &transformer, optional<vector<unique_ptr<ParsedExpression>>> window_partition,
    optional<vector<OrderByNode>> order_by_clause, optional<WindowFrame> frame_clause) {
	//! Create a dummy result to add modifiers to
	auto result = make_uniq<WindowExpression>(string(), string(), string());
	if (window_partition) {
		result->PartitionsMutable() = std::move(*window_partition);
	}
	if (order_by_clause) {
		result->OrderByMutable() = std::move(*order_by_clause);
		for (auto &order : result->OrderByMutable()) {
			if (order.expression->GetExpressionType() == ExpressionType::STAR) {
				auto &star = order.expression->Cast<StarExpression>();
				if (!star.Expression()) {
					throw ParserException("Cannot ORDER BY ALL in a window expression");
				}
			}
		}
	}
	if (frame_clause) {
		result->WindowStartMutable() = frame_clause->start;
		result->WindowEndMutable() = frame_clause->end;
		result->StartExprMutable() = std::move(frame_clause->start_expr);
		result->EndExprMutable() = std::move(frame_clause->end_expr);
		result->WindowExcludeMutable() = frame_clause->exclude_clause;
	} else {
		result->WindowStartMutable() = WindowBoundary::UNBOUNDED_PRECEDING;
		result->WindowEndMutable() = WindowBoundary::CURRENT_ROW_RANGE;
	}
	return result;
}

WindowFrame PEGTransformerFactory::TransformFrameClause(PEGTransformer &transformer, const string &framing,
                                                        vector<WindowBoundaryExpression> frame_extent,
                                                        const optional<WindowExcludeMode> &window_exclude_clause) {
	WindowFrame result;
	if (frame_extent[0].boundary == WindowBoundary::CURRENT_ROW_RANGE &&
	    frame_extent[1].boundary == WindowBoundary::EXPR_PRECEDING_RANGE) {
		throw ParserException("Frame starting from current row cannot have preceding rows");
	}
	if (frame_extent[0].boundary == WindowBoundary::EXPR_FOLLOWING_RANGE &&
	    (frame_extent[1].boundary == WindowBoundary::CURRENT_ROW_RANGE ||
	     frame_extent[1].boundary == WindowBoundary::EXPR_PRECEDING_RANGE)) {
		throw ParserException("Frame starting from following row cannot have preceding rows");
	}
	for (auto &frame : frame_extent) {
		if (StringUtil::CIEquals(framing, "rows")) {
			if (frame.boundary == WindowBoundary::CURRENT_ROW_RANGE) {
				frame.boundary = WindowBoundary::CURRENT_ROW_ROWS;
			} else if (frame.boundary == WindowBoundary::EXPR_PRECEDING_RANGE) {
				frame.boundary = WindowBoundary::EXPR_PRECEDING_ROWS;
			} else if (frame.boundary == WindowBoundary::EXPR_FOLLOWING_RANGE) {
				frame.boundary = WindowBoundary::EXPR_FOLLOWING_ROWS;
			} else if (frame.boundary == WindowBoundary::INVALID) {
				frame.boundary = WindowBoundary::CURRENT_ROW_ROWS;
			}
		} else if (StringUtil::CIEquals(framing, "groups")) {
			if (frame.boundary == WindowBoundary::CURRENT_ROW_RANGE) {
				frame.boundary = WindowBoundary::CURRENT_ROW_GROUPS;
			} else if (frame.boundary == WindowBoundary::EXPR_PRECEDING_RANGE) {
				frame.boundary = WindowBoundary::EXPR_PRECEDING_GROUPS;
			} else if (frame.boundary == WindowBoundary::EXPR_FOLLOWING_RANGE) {
				frame.boundary = WindowBoundary::EXPR_FOLLOWING_GROUPS;
			} else if (frame.boundary == WindowBoundary::INVALID) {
				frame.boundary = WindowBoundary::CURRENT_ROW_GROUPS;
			}
		} else if (StringUtil::CIEquals(framing, "range")) {
			if (frame.boundary == WindowBoundary::INVALID) {
				frame.boundary = WindowBoundary::CURRENT_ROW_RANGE;
			}
		} else {
			throw ParserException("Invalid result from frame: %s", framing);
		}
	}
	if (frame_extent[0].boundary == WindowBoundary::UNBOUNDED_FOLLOWING) {
		throw ParserException("Frame start cannot be UNBOUNDED FOLLOWING");
	}
	result.start = frame_extent[0].boundary;
	if (frame_extent[0].expr) {
		result.start_expr = std::move(frame_extent[0].expr);
	}
	if (frame_extent.size() == 2) {
		if (frame_extent[1].boundary == WindowBoundary::UNBOUNDED_PRECEDING) {
			throw ParserException("Frame end cannot be UNBOUNDED PRECEDING");
		}
		result.end = frame_extent[1].boundary;
		if (frame_extent[1].expr) {
			result.end_expr = std::move(frame_extent[1].expr);
		}
	}
	if (window_exclude_clause) {
		result.exclude_clause = *window_exclude_clause;
	}
	return result;
}

vector<WindowBoundaryExpression>
PEGTransformerFactory::TransformBetweenFrameExtent(PEGTransformer &transformer, WindowBoundaryExpression frame_bound,
                                                   WindowBoundaryExpression frame_bound_1) {
	vector<WindowBoundaryExpression> result;
	result.push_back(std::move(frame_bound));
	result.push_back(std::move(frame_bound_1));
	return result;
}

vector<WindowBoundaryExpression>
PEGTransformerFactory::TransformSingleFrameExtent(PEGTransformer &transformer, WindowBoundaryExpression frame_bound) {
	if (frame_bound.boundary == WindowBoundary::EXPR_FOLLOWING_RANGE) {
		throw ParserException("Frame starting from following row cannot end with current row");
	}
	vector<WindowBoundaryExpression> result;
	result.push_back(std::move(frame_bound));
	WindowBoundaryExpression end_current_row;
	end_current_row.boundary = WindowBoundary::INVALID;
	result.push_back(std::move(end_current_row));
	return result;
}

WindowBoundaryExpression PEGTransformerFactory::TransformFrameUnbounded(PEGTransformer &transformer,
                                                                        const bool &preceding_or_following) {
	WindowBoundaryExpression result;
	if (preceding_or_following) {
		result.boundary = WindowBoundary::UNBOUNDED_PRECEDING;
	} else {
		result.boundary = WindowBoundary::UNBOUNDED_FOLLOWING;
	}
	return result;
}

WindowBoundaryExpression PEGTransformerFactory::TransformFrameExpression(PEGTransformer &transformer,
                                                                         unique_ptr<ParsedExpression> expression,
                                                                         const bool &preceding_or_following) {
	WindowBoundaryExpression result;
	result.expr = std::move(expression);
	if (preceding_or_following) {
		// These are placeholders and will be converted to groups/rows/range later
		result.boundary = WindowBoundary::EXPR_PRECEDING_RANGE;
	} else {
		result.boundary = WindowBoundary::EXPR_FOLLOWING_RANGE;
	}
	return result;
}

WindowBoundaryExpression PEGTransformerFactory::TransformFrameCurrentRow(PEGTransformer &transformer) {
	WindowBoundaryExpression result;
	// These are placeholders and will be converted to groups/rows/range later
	result.boundary = WindowBoundary::CURRENT_ROW_RANGE;
	return result;
}

bool PEGTransformerFactory::TransformPrecedingFrame(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformFollowingFrame(PEGTransformer &transformer) {
	return false;
}

WindowExcludeMode PEGTransformerFactory::TransformWindowExcludeClause(PEGTransformer &transformer,
                                                                      const WindowExcludeMode &window_exclude_element) {
	return window_exclude_element;
}

string PEGTransformerFactory::TransformRowsFraming(PEGTransformer &transformer) {
	return "ROWS";
}

string PEGTransformerFactory::TransformRangeFraming(PEGTransformer &transformer) {
	return "RANGE";
}

string PEGTransformerFactory::TransformGroupsFraming(PEGTransformer &transformer) {
	return "GROUPS";
}

WindowExcludeMode PEGTransformerFactory::TransformExcludeCurrentRow(PEGTransformer &transformer) {
	return WindowExcludeMode::CURRENT_ROW;
}

WindowExcludeMode PEGTransformerFactory::TransformExcludeGroup(PEGTransformer &transformer) {
	return WindowExcludeMode::GROUP;
}

WindowExcludeMode PEGTransformerFactory::TransformExcludeTies(PEGTransformer &transformer) {
	return WindowExcludeMode::TIES;
}

WindowExcludeMode PEGTransformerFactory::TransformExcludeNoOthers(PEGTransformer &transformer) {
	return WindowExcludeMode::NO_OTHER;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformWindowPartition(PEGTransformer &transformer,
                                                vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformCoalesceExpression(PEGTransformer &transformer,
                                                   vector<unique_ptr<ParsedExpression>> expression) {
	auto result = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE);
	for (auto &expr : expression) {
		result->GetChildrenMutable().push_back(std::move(expr));
	}
	return std::move(result);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformUnpackExpression(PEGTransformer &transformer,
                                                                              unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_UNPACK);
	result->GetChildrenMutable().push_back(std::move(expression));
	return std::move(result);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformTryExpression(PEGTransformer &transformer,
                                                                           unique_ptr<ParsedExpression> expression) {
	auto result = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_TRY);
	result->GetChildrenMutable().push_back(std::move(expression));
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformColumnsExpression(PEGTransformer &transformer, const bool &has_result,
                                                  unique_ptr<ParsedExpression> expression) {
	bool unpack = has_result;
	auto result = make_uniq<StarExpression>();
	if (expression->GetExpressionType() == ExpressionType::STAR) {
		auto star_expr = unique_ptr_cast<ParsedExpression, StarExpression>(std::move(expression));
		if (star_expr->IsColumns()) {
			result->ExpressionMutable() = std::move(star_expr);
		} else {
			result = std::move(star_expr);
		}
	} else if (expression->GetExpressionType() == ExpressionType::LAMBDA) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(make_uniq<StarExpression>());
		children.push_back(std::move(expression));
		auto list_filter = make_uniq<FunctionExpression>("list_filter", std::move(children));
		result->ExpressionMutable() = std::move(list_filter);
	} else {
		result->ExpressionMutable() = std::move(expression);
	}
	result->IsColumnsMutable() = true;
	if (unpack) {
		return make_uniq<OperatorExpression>(ExpressionType::OPERATOR_UNPACK, std::move(result));
	}
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformExtractExpression(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> extract_arguments) {
	return make_uniq<FunctionExpression>("date_part", std::move(extract_arguments));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformExtractArguments(PEGTransformer &transformer,
                                                 unique_ptr<ParsedExpression> extract_argument,
                                                 unique_ptr<ParsedExpression> expression) {
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(std::move(extract_argument));
	result.push_back(std::move(expression));
	return result;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformExtractDatePartArgument(PEGTransformer &transformer,
                                                        const DatePartSpecifier &extract_date_part) {
	return ConstantExpression::String(EnumUtil::ToString(extract_date_part));
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExtractIdentifierArgument(PEGTransformer &transformer,
                                                                                       const Identifier &identifier) {
	return ConstantExpression::String(identifier.GetIdentifierName());
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformExtractStringArgument(PEGTransformer &transformer,
                                                                                   const string &string_literal) {
	return ConstantExpression::String(string_literal);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformLambdaExpression(
    PEGTransformer &transformer, const vector<Identifier> &col_id_or_string, unique_ptr<ParsedExpression> expression) {
	vector<string> parameters;
	for (auto &parameter : col_id_or_string) {
		parameters.push_back(parameter.GetIdentifierName());
	}
	auto result = make_uniq<LambdaExpression>(parameters, std::move(expression));
	return std::move(result);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformNullIfExpression(PEGTransformer &transformer,
                                                 vector<unique_ptr<ParsedExpression>> null_if_arguments) {
	return make_uniq<FunctionExpression>("nullif", std::move(null_if_arguments));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformNullIfArguments(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
                                                unique_ptr<ParsedExpression> expression_1) {
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(std::move(expression));
	result.push_back(std::move(expression_1));
	return result;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformRowExpression(PEGTransformer &transformer,
                                              optional<vector<unique_ptr<ParsedExpression>>> expression) {
	if (!expression) {
		return make_uniq<FunctionExpression>("row", vector<unique_ptr<ParsedExpression>>());
	}
	auto func_expr = make_uniq<FunctionExpression>("row", std::move(*expression));
	return std::move(func_expr);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformSubstringExpression(PEGTransformer &transformer,
                                                    vector<unique_ptr<ParsedExpression>> substring_arguments) {
	return make_uniq<FunctionExpression>("substring", std::move(substring_arguments));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSubstringExpressionList(PEGTransformer &transformer,
                                                        vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSubstringParameters(PEGTransformer &transformer,
                                                    unique_ptr<ParsedExpression> expression,
                                                    vector<unique_ptr<ParsedExpression>> substring_from_for) {
	vector<unique_ptr<ParsedExpression>> results;
	results.push_back(std::move(expression));
	for (auto &arg : substring_from_for) {
		results.push_back(std::move(arg));
	}
	return results;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSubstringFromOptionalFor(PEGTransformer &transformer,
                                                         unique_ptr<ParsedExpression> from_expression,
                                                         optional<unique_ptr<ParsedExpression>> for_expression) {
	vector<unique_ptr<ParsedExpression>> results;
	results.push_back(std::move(from_expression));
	if (for_expression && *for_expression) {
		results.push_back(std::move(*for_expression));
	}
	return results;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformSubstringFor(PEGTransformer &transformer, unique_ptr<ParsedExpression> for_expression) {
	vector<unique_ptr<ParsedExpression>> results;
	results.push_back(ConstantExpression::Integer(1));
	results.push_back(std::move(for_expression));
	return results;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformTrimExpression(PEGTransformer &transformer,
                                                                            TrimArguments trim_arguments) {
	string function_name = "trim";
	if (trim_arguments.trim_direction) {
		function_name = *trim_arguments.trim_direction;
	}
	return make_uniq<FunctionExpression>(Identifier(function_name), std::move(trim_arguments.expressions));
}

TrimArguments PEGTransformerFactory::TransformTrimArguments(PEGTransformer &transformer,
                                                            const optional<string> &trim_direction,
                                                            optional<unique_ptr<ParsedExpression>> trim_source,
                                                            vector<unique_ptr<ParsedExpression>> expression) {
	TrimArguments result;
	result.trim_direction = trim_direction;
	result.expressions = std::move(expression);
	if (trim_source && *trim_source) {
		result.expressions.push_back(std::move(*trim_source));
	}
	return result;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformTrimSource(PEGTransformer &transformer,
                                           optional<unique_ptr<ParsedExpression>> expression) {
	if (expression) {
		return std::move(*expression);
	}
	return nullptr;
}

string PEGTransformerFactory::TransformTrimBoth(PEGTransformer &transformer) {
	return "trim";
}

string PEGTransformerFactory::TransformTrimLeading(PEGTransformer &transformer) {
	return "ltrim";
}

string PEGTransformerFactory::TransformTrimTrailing(PEGTransformer &transformer) {
	return "rtrim";
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformOverlayExpression(PEGTransformer &transformer,
                                                  vector<unique_ptr<ParsedExpression>> overlay_arguments) {
	return make_uniq<FunctionExpression>("overlay", std::move(overlay_arguments));
}

vector<unique_ptr<ParsedExpression>> PEGTransformerFactory::TransformOverlayParameters(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, unique_ptr<ParsedExpression> expression_1,
    unique_ptr<ParsedExpression> from_expression, optional<unique_ptr<ParsedExpression>> for_expression) {
	vector<unique_ptr<ParsedExpression>> results;
	results.push_back(std::move(expression));
	results.push_back(std::move(expression_1));
	results.push_back(std::move(from_expression));
	if (for_expression && *for_expression) {
		results.push_back(std::move(*for_expression));
	}
	return results;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformFromExpression(PEGTransformer &transformer,
                                                                            unique_ptr<ParsedExpression> expression) {
	return expression;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformForExpression(PEGTransformer &transformer,
                                                                           unique_ptr<ParsedExpression> expression) {
	return expression;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformOverlayExpressionList(PEGTransformer &transformer,
                                                      vector<unique_ptr<ParsedExpression>> expression) {
	return expression;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformPositionExpression(PEGTransformer &transformer,
                                                   vector<unique_ptr<ParsedExpression>> position_arguments) {
	return make_uniq<FunctionExpression>("position", std::move(position_arguments));
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformPositionArguments(PEGTransformer &transformer,
                                                  unique_ptr<ParsedExpression> other_operator_expression,
                                                  unique_ptr<ParsedExpression> expression) {
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(std::move(expression));
	result.push_back(std::move(other_operator_expression));
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCastExpression(PEGTransformer &transformer,
                                                                            const bool &cast_or_try_cast,
                                                                            CastArguments cast_arguments) {
	return make_uniq<CastExpression>(cast_arguments.type, std::move(cast_arguments.expression), cast_or_try_cast);
}

CastArguments PEGTransformerFactory::TransformCastArguments(PEGTransformer &transformer,
                                                            unique_ptr<ParsedExpression> expression,
                                                            const LogicalType &type) {
	CastArguments result;
	result.expression = std::move(expression);
	result.type = type;
	return result;
}

bool PEGTransformerFactory::TransformCastKeyword(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformTryCastKeyword(PEGTransformer &transformer) {
	return true;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCaseExpression(
    PEGTransformer &transformer, optional<unique_ptr<ParsedExpression>> expression, vector<CaseCheck> case_when_then,
    optional<unique_ptr<ParsedExpression>> case_else) {
	auto result = make_uniq<CaseExpression>();

	for (auto &case_expr : case_when_then) {
		CaseCheck new_case;
		if (expression) {
			new_case.when_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_EQUAL, (*expression)->Copy(),
			                                                     std::move(case_expr.when_expr));
		} else {
			new_case.when_expr = std::move(case_expr.when_expr);
		}
		new_case.then_expr = std::move(case_expr.then_expr);
		result->CaseChecksMutable().push_back(std::move(new_case));
	}
	if (case_else) {
		result->ElseMutable() = std::move(*case_else);
	} else {
		result->ElseMutable() = ConstantExpression::Null();
	}
	return std::move(result);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformCaseElse(PEGTransformer &transformer,
                                                                      unique_ptr<ParsedExpression> expression) {
	return expression;
}

CaseCheck PEGTransformerFactory::TransformCaseWhenThen(PEGTransformer &transformer,
                                                       unique_ptr<ParsedExpression> expression,
                                                       unique_ptr<ParsedExpression> expression_1) {
	CaseCheck result;
	result.when_expr = std::move(expression);
	result.then_expr = std::move(expression_1);
	return result;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformTypeLiteral(PEGTransformer &transformer,
                                                                         const LogicalType &type,
                                                                         const string &string_literal) {
	auto child = ConstantExpression::String(string_literal);
	auto result = make_uniq<CastExpression>(type, std::move(child));
	return std::move(result);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformDefaultExpression(PEGTransformer &transformer) {
	return make_uniq<DefaultExpression>();
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformIntervalLiteral(PEGTransformer &transformer,
                                                unique_ptr<ParsedExpression> interval_parameter,
                                                const optional<DatePartSpecifier> &interval) {
	DatePartSpecifier interval_unit = DatePartSpecifier::INVALID;
	if (interval) {
		interval_unit = *interval;
	}
	auto expr = std::move(interval_parameter);
	auto func_name = DateTruncSimplificationRule::DatePartToFunc(interval_unit);
	if (func_name.empty()) {
		expr = make_uniq<CastExpression>(LogicalType::INTERVAL, std::move(expr));
		return expr;
	}
	LogicalType parse_type = LogicalType::DOUBLE;
	expr = make_uniq<CastExpression>(parse_type, std::move(expr));
	auto target_type = GetIntervalTargetType(interval_unit);
	if (target_type != parse_type) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(expr));
		expr = make_uniq<FunctionExpression>("trunc", std::move(children));
		expr = make_uniq<CastExpression>(target_type, std::move(expr));
	}
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(expr));
	auto result = make_uniq<FunctionExpression>(Identifier(func_name), std::move(children));
	return std::move(result);
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformIntervalStringParameter(PEGTransformer &transformer,
                                                                                     const string &string_literal) {
	return ConstantExpression::String(string_literal);
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformSubqueryExpression(PEGTransformer &transformer, const optional<bool> &subquery_not,
                                                   const optional<bool> &subquery_exists,
                                                   unique_ptr<TableRef> subquery_reference) {
	bool is_not = subquery_not ? *subquery_not : false;
	bool is_exists = subquery_exists ? *subquery_exists : false;
	auto result = make_uniq<SubqueryExpression>();
	if (is_exists) {
		result->GetSubqueryTypeMutable() = SubqueryType::EXISTS;
	} else {
		result->GetSubqueryTypeMutable() = SubqueryType::SCALAR;
	}
	if (subquery_reference->type == TableReferenceType::SUBQUERY) {
		auto &subquery_ref = subquery_reference->Cast<SubqueryRef>();
		result->SubqueryMutable() = std::move(subquery_ref.subquery);
	} else {
		auto select_statement = make_uniq<SelectStatement>();
		auto select_node = make_uniq<SelectNode>();
		select_node->select_list.push_back(make_uniq<StarExpression>());
		select_node->from_table = std::move(subquery_reference);
		select_statement->node = std::move(select_node);
		result->SubqueryMutable() = std::move(select_statement);
	}
	if (is_not) {
		vector<unique_ptr<ParsedExpression>> children;
		children.push_back(std::move(result));
		auto not_operator = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_NOT, std::move(children));
		return std::move(not_operator);
	}
	return std::move(result);
}

bool PEGTransformerFactory::TransformSubqueryNot(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformSubqueryExists(PEGTransformer &transformer) {
	return true;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformMapExpression(PEGTransformer &transformer,
                                              vector<unique_ptr<ParsedExpression>> map_struct_expression) {
	return make_uniq<FunctionExpression>("map", std::move(map_struct_expression));
}

vector<unique_ptr<ParsedExpression>> PEGTransformerFactory::TransformMapStructExpression(
    PEGTransformer &transformer, optional<vector<vector<unique_ptr<ParsedExpression>>>> map_struct_field) {
	vector<unique_ptr<ParsedExpression>> keys;
	vector<unique_ptr<ParsedExpression>> values;

	if (map_struct_field) {
		for (auto &key_val_pair : *map_struct_field) {
			keys.push_back(std::move(key_val_pair[0]));
			values.push_back(std::move(key_val_pair[1]));
		}
	}
	vector<unique_ptr<ParsedExpression>> result;
	result.push_back(make_uniq<FunctionExpression>("list_value", std::move(keys)));
	result.push_back(make_uniq<FunctionExpression>("list_value", std::move(values)));
	return result;
}

vector<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformMapStructField(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
                                               unique_ptr<ParsedExpression> expression_1) {
	vector<unique_ptr<ParsedExpression>> fields;
	fields.push_back(std::move(expression));
	fields.push_back(std::move(expression_1));
	return fields;
}

unique_ptr<ParsedExpression> PEGTransformerFactory::TransformListComprehensionExpression(
    PEGTransformer &transformer, unique_ptr<ParsedExpression> expression, const vector<Identifier> &col_id_or_string,
    unique_ptr<ParsedExpression> expression_1, optional<unique_ptr<ParsedExpression>> list_comprehension_filter) {
	auto result_expr = std::move(expression);
	auto in_expr = std::move(expression_1);
	vector<string> lambda_columns;
	for (auto &col : col_id_or_string) {
		lambda_columns.push_back(col.GetIdentifierName());
	}

	if (!list_comprehension_filter || !*list_comprehension_filter) {
		auto lambda_expression = make_uniq<LambdaExpression>(lambda_columns, std::move(result_expr));
		vector<unique_ptr<ParsedExpression>> apply_children;
		apply_children.push_back(std::move(in_expr));
		apply_children.push_back(std::move(lambda_expression));

		return make_uniq<FunctionExpression>("list_apply", std::move(apply_children));
	}

	auto filter_expr = std::move(*list_comprehension_filter);

	// STAGE 1: list_apply(in_expr, x -> struct_pack(filter := ..., result := ...))
	vector<FunctionArgument> struct_children;
	struct_children.emplace_back("filter", std::move(filter_expr));
	struct_children.emplace_back("result", std::move(result_expr));
	auto struct_pack = make_uniq<FunctionExpression>("struct_pack", std::move(struct_children));

	auto stage1_lambda = make_uniq<LambdaExpression>(lambda_columns, std::move(struct_pack));
	vector<unique_ptr<ParsedExpression>> stage1_apply_args;
	stage1_apply_args.push_back(std::move(in_expr));
	stage1_apply_args.push_back(std::move(stage1_lambda));
	auto stage1_apply = make_uniq<FunctionExpression>("list_apply", std::move(stage1_apply_args));

	// STAGE 2: list_filter(stage1, elem -> struct_extract(elem, 'filter'))
	auto elem_ref_filter = make_uniq<ColumnRefExpression>("elem");
	auto filter_const = ConstantExpression::String("filter");
	vector<unique_ptr<ParsedExpression>> extract_filter_args;
	extract_filter_args.push_back(std::move(elem_ref_filter));
	extract_filter_args.push_back(std::move(filter_const));
	auto filter_extract = make_uniq<FunctionExpression>("struct_extract", std::move(extract_filter_args));

	auto stage2_lambda = make_uniq<LambdaExpression>(vector<string> {"elem"}, std::move(filter_extract));
	vector<unique_ptr<ParsedExpression>> stage2_filter_args;
	stage2_filter_args.push_back(std::move(stage1_apply));
	stage2_filter_args.push_back(std::move(stage2_lambda));
	auto stage2_filter = make_uniq<FunctionExpression>("list_filter", std::move(stage2_filter_args));

	// STAGE 3: list_apply(stage2, elem -> struct_extract(elem, 'result'))
	auto elem_ref_result = make_uniq<ColumnRefExpression>("elem");
	auto result_const = ConstantExpression::String("result");
	vector<unique_ptr<ParsedExpression>> extract_result_args;
	extract_result_args.push_back(std::move(elem_ref_result));
	extract_result_args.push_back(std::move(result_const));
	auto result_extract = make_uniq<FunctionExpression>("struct_extract", std::move(extract_result_args));

	auto stage3_lambda = make_uniq<LambdaExpression>(vector<string> {"elem"}, std::move(result_extract));
	vector<unique_ptr<ParsedExpression>> stage3_apply_args;
	stage3_apply_args.push_back(std::move(stage2_filter));
	stage3_apply_args.push_back(std::move(stage3_lambda));

	return make_uniq<FunctionExpression>("list_apply", std::move(stage3_apply_args));
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformListComprehensionFilter(PEGTransformer &transformer,
                                                        unique_ptr<ParsedExpression> expression) {
	return expression;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformReplaceList(PEGTransformer &transformer,
                                            case_insensitive_map_t<unique_ptr<ParsedExpression>> replace_entries) {
	return replace_entries;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformReplaceEntrySingle(PEGTransformer &transformer,
                                                   pair<string, unique_ptr<ParsedExpression>> replace_entry) {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> entry_map;
	entry_map.insert(std::move(replace_entry));
	return entry_map;
}

case_insensitive_map_t<unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformReplaceEntryList(PEGTransformer &transformer,
                                                 vector<pair<string, unique_ptr<ParsedExpression>>> replace_entry) {
	case_insensitive_map_t<unique_ptr<ParsedExpression>> entry_map;
	for (auto &entry : replace_entry) {
		if (entry_map.find(entry.first) != entry_map.end()) {
			throw ParserException("Duplicate entry \"%s\" in REPLACE list", entry.first);
		}
		entry_map.insert(std::move(entry));
	}
	return entry_map;
}

pair<string, unique_ptr<ParsedExpression>>
PEGTransformerFactory::TransformReplaceEntry(PEGTransformer &transformer, unique_ptr<ParsedExpression> expression,
                                             unique_ptr<ParsedExpression> column_reference) {
	if (column_reference->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
		throw InternalException("Expected a column reference in the replace entry");
	}
	auto &col_ref = column_reference->Cast<ColumnRefExpression>();
	auto column_name = col_ref.GetColumnName();
	return make_pair(column_name.GetIdentifierName(), std::move(expression));
}

ExpressionType PEGTransformerFactory::TransformIsDistinctFromOp(PEGTransformer &transformer, const bool &has_result) {
	if (has_result) {
		return ExpressionType::COMPARE_NOT_DISTINCT_FROM;
	}
	return ExpressionType::COMPARE_DISTINCT_FROM;
}

unique_ptr<ParsedExpression>
PEGTransformerFactory::TransformGroupingExpression(PEGTransformer &transformer, const bool &grouping_or_grouping_id,
                                                   optional<vector<unique_ptr<ParsedExpression>>> expression) {
	vector<unique_ptr<ParsedExpression>> grouping_expressions;
	if (expression) {
		grouping_expressions = std::move(*expression);
	}

	auto result = make_uniq<OperatorExpression>(ExpressionType::GROUPING_FUNCTION, std::move(grouping_expressions));
	return std::move(result);
}

bool PEGTransformerFactory::TransformGroupingKeyword(PEGTransformer &transformer) {
	return false;
}

bool PEGTransformerFactory::TransformGroupingIdKeyword(PEGTransformer &transformer) {
	return true;
}

qualified_column_map_t<string>
PEGTransformerFactory::TransformRenameList(PEGTransformer &transformer,
                                           const qualified_column_map_t<string> &rename_entries) {
	return rename_entries;
}

qualified_column_map_t<string>
PEGTransformerFactory::TransformRenameEntryList(PEGTransformer &transformer,
                                                const vector<pair<QualifiedColumnName, string>> &rename_entry) {
	qualified_column_map_t<string> result;
	for (auto &entry : rename_entry) {
		result[entry.first] = entry.second;
	}
	return result;
}

qualified_column_map_t<string>
PEGTransformerFactory::TransformSingleRenameEntry(PEGTransformer &transformer,
                                                  const pair<QualifiedColumnName, string> &rename_entry) {
	qualified_column_map_t<string> result;
	result[rename_entry.first] = rename_entry.second;
	return result;
}

pair<QualifiedColumnName, string> PEGTransformerFactory::TransformRenameEntry(PEGTransformer &transformer,
                                                                              const QualifiedColumnName &exclude_name,
                                                                              const Identifier &identifier) {
	return make_pair(exclude_name, identifier.GetIdentifierName());
}

bool PEGTransformerFactory::TransformIgnoreNulls(PEGTransformer &transformer) {
	return true;
}

bool PEGTransformerFactory::TransformRespectNulls(PEGTransformer &transformer) {
	return false;
}

} // namespace duckdb
