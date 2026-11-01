#include "duckdb/catalog/default/default_macro_qualifier.hpp"

#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/function/macro_function.hpp"
#include "duckdb/function/scalar_macro_function.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/lambda_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/column_qualifier.hpp"

namespace duckdb {

struct DefaultMacroQualifierState {
	DefaultMacroQualifierState(const Identifier &macro_schema_p, identifier_set_t parameters_p)
	    : macro_schema(macro_schema_p), parameters(std::move(parameters_p)) {
	}

	const Identifier &macro_schema;
	identifier_set_t parameters;
	vector<identifier_set_t> lambda_params;

	QualifiedName Resolve(const Identifier &name) const {
		if (!(macro_schema == Identifier::DefaultSchema()) &&
		    DefaultFunctionGenerator::IsDefaultMacro(macro_schema, name)) {
			return QualifiedName(Identifier::SystemCatalog(), macro_schema, name);
		}
		return QualifiedName(Identifier::SystemCatalog(), Identifier::DefaultSchema(), name);
	}

	bool IsBoundName(const Identifier &name) const {
		return parameters.count(name) || LambdaExpression::IsLambdaParameter(lambda_params, name);
	}
};

static void QualifyExpression(unique_ptr<ParsedExpression> &expr, DefaultMacroQualifierState &state);

static void QualifyQueryNode(QueryNode &node, DefaultMacroQualifierState &state) {
	ParsedExpressionIterator::EnumerateQueryNodeChildren(
	    node, [&](unique_ptr<ParsedExpression> &child) { QualifyExpression(child, state); });
}

static void QualifyLambdaArguments(FunctionExpression &function, DefaultMacroQualifierState &state) {
	for (auto &argument : function.GetArgumentsMutable()) {
		auto &child = argument.GetExpressionMutable();
		if (child->GetExpressionClass() != ExpressionClass::LAMBDA) {
			QualifyExpression(child, state);
			continue;
		}
		auto &lambda = child->Cast<LambdaExpression>();
		string error_message;
		auto column_refs = lambda.ExtractColumnRefExpressions(error_message);
		if (!error_message.empty()) {
			QualifyExpression(lambda.LeftMutable(), state);
			QualifyExpression(lambda.RightMutable(), state);
			continue;
		}
		state.lambda_params.emplace_back();
		for (auto &column_ref : column_refs) {
			state.lambda_params.back().insert(column_ref.get().Cast<ColumnRefExpression>().GetName());
		}
		QualifyExpression(lambda.RightMutable(), state);
		state.lambda_params.pop_back();
	}
}

static void QualifyExpression(unique_ptr<ParsedExpression> &expr, DefaultMacroQualifierState &state) {
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::COLUMN_REF: {
		// a bare SQL value function name binds as an unqualified call, spell it qualified
		auto &column_ref = expr->Cast<ColumnRefExpression>();
		if (column_ref.IsQualified() || state.IsBoundName(column_ref.GetColumnName())) {
			return;
		}
		auto value_function = ColumnQualifier::SQLValueFunctionName(column_ref.GetColumnName());
		if (value_function.empty()) {
			return;
		}
		auto function =
		    make_uniq<FunctionExpression>(state.Resolve(value_function), vector<unique_ptr<ParsedExpression>>());
		function->SetQueryLocation(column_ref.GetQueryLocation());
		function->SetAlias(expr->GetAlias());
		expr = std::move(function);
		return;
	}
	case ExpressionClass::FUNCTION: {
		auto &function = expr->Cast<FunctionExpression>();
		auto &name = function.GetQualifiedName();
		if (name.Catalog().empty()) {
			if (name.Schema().empty()) {
				function.SetQualifiedName(state.Resolve(function.FunctionName()));
			} else {
				function.SetQualifiedName(QualifiedName(Identifier::SystemCatalog(), name.Schema(), name.Name()));
			}
			// operator syntax renders without its qualification, the qualified call form keeps it
			function.IsOperatorMutable() = false;
		}
		if (function.IsLambdaFunction()) {
			QualifyLambdaArguments(function, state);
			if (function.FilterMutable()) {
				QualifyExpression(function.FilterMutable(), state);
			}
			if (function.OrderByMutable()) {
				for (auto &order : function.OrderByMutable()->orders) {
					QualifyExpression(order.expression, state);
				}
			}
			return;
		}
		break;
	}
	case ExpressionClass::OPERATOR: {
		// slice syntax binds through an array_slice lookup, spell it as the qualified call. Other
		// lowered operators (element extraction) choose their function by argument type and stay
		auto &op = expr->Cast<OperatorExpression>();
		if (op.GetExpressionType() == ExpressionType::ARRAY_SLICE) {
			vector<unique_ptr<ParsedExpression>> children;
			for (auto &child : op.GetChildrenMutable()) {
				children.push_back(std::move(child));
			}
			auto function =
			    make_uniq<FunctionExpression>(state.Resolve(Identifier("array_slice")), std::move(children));
			function->SetQueryLocation(op.GetQueryLocation());
			function->SetAlias(expr->GetAlias());
			expr = std::move(function);
			QualifyExpression(expr, state);
			return;
		}
		break;
	}
	case ExpressionClass::WINDOW: {
		auto &window = expr->Cast<WindowExpression>();
		auto &name = window.GetQualifiedName();
		if (name.Catalog().empty() && !name.Name().empty()) {
			if (name.Schema().empty()) {
				window.SetQualifiedName(state.Resolve(name.Name()));
			} else {
				window.SetQualifiedName(QualifiedName(Identifier::SystemCatalog(), name.Schema(), name.Name()));
			}
		}
		break;
	}
	case ExpressionClass::SUBQUERY: {
		auto &subquery = expr->Cast<SubqueryExpression>();
		QualifyQueryNode(*subquery.SubqueryMutable()->node, state);
		break;
	}
	default:
		break;
	}
	ParsedExpressionIterator::EnumerateChildren(
	    *expr, [&](unique_ptr<ParsedExpression> &child) { QualifyExpression(child, state); });
}

void DefaultMacroQualifier::Qualify(MacroFunction &macro, const Identifier &macro_schema) {
	identifier_set_t parameters;
	for (auto &parameter : macro.parameters) {
		parameters.insert(parameter->Cast<ColumnRefExpression>().GetColumnName());
	}
	DefaultMacroQualifierState state(macro_schema, std::move(parameters));
	for (auto &default_parameter : macro.default_parameters) {
		QualifyExpression(default_parameter.second, state);
	}
	if (macro.type == MacroType::SCALAR_MACRO) {
		QualifyExpression(macro.Cast<ScalarMacroFunction>().expression, state);
	} else if (macro.type == MacroType::TABLE_MACRO) {
		QualifyQueryNode(*macro.Cast<TableMacroFunction>().query_node, state);
	}
}

} // namespace duckdb
