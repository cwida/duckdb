#include "planner/expression_binder.hpp"

#include "main/client_context.hpp"
#include "main/database.hpp"
#include "parser/expression/columnref_expression.hpp"
#include "parser/expression/subquery_expression.hpp"
#include "parser/parsed_expression_iterator.hpp"
#include "planner/binder.hpp"
#include "planner/expression/bound_cast_expression.hpp"
#include "planner/expression/bound_default_expression.hpp"
#include "planner/expression/bound_parameter_expression.hpp"
#include "planner/expression/bound_subquery_expression.hpp"
#include "planner/expression_iterator.hpp"

using namespace duckdb;
using namespace std;

ExpressionBinder::ExpressionBinder(Binder &binder, ClientContext &context, bool replace_binder)
    : binder(binder), context(context), stored_binder(nullptr) {
	if (replace_binder) {
		stored_binder = binder.GetActiveBinder();
		binder.SetActiveBinder(this);
	} else {
		binder.PushExpressionBinder(this);
	}
}

ExpressionBinder::~ExpressionBinder() {
	if (binder.HasActiveBinder()) {
		if (stored_binder) {
			binder.SetActiveBinder(stored_binder);
		} else {
			binder.PopExpressionBinder();
		}
	}
}

BindResult ExpressionBinder::BindExpression(ParsedExpression &expr, uint32_t depth, bool root_expression) {
	switch (expr.expression_class) {
	case ExpressionClass::CASE:
		return BindExpression((CaseExpression &)expr, depth);
	case ExpressionClass::CAST:
		return BindExpression((CastExpression &)expr, depth);
	case ExpressionClass::COLUMN_REF:
		return BindExpression((ColumnRefExpression &)expr, depth);
	case ExpressionClass::COMPARISON:
		return BindExpression((ComparisonExpression &)expr, depth);
	case ExpressionClass::CONJUNCTION:
		return BindExpression((ConjunctionExpression &)expr, depth);
	case ExpressionClass::CONSTANT:
		return BindExpression((ConstantExpression &)expr, depth);
	case ExpressionClass::FUNCTION:
		return BindExpression((FunctionExpression &)expr, depth);
	case ExpressionClass::OPERATOR:
		return BindExpression((OperatorExpression &)expr, depth);
	case ExpressionClass::SUBQUERY:
		return BindExpression((SubqueryExpression &)expr, depth);
	default:
		assert(expr.GetExpressionClass() == ExpressionClass::PARAMETER);
		return BindExpression((ParameterExpression &)expr, depth);
	}
}

bool ExpressionBinder::BindCorrelatedColumns(unique_ptr<ParsedExpression> &expr) {
	// try to bind in one of the outer queries, if the binding error occurred in a subquery
	auto &active_binders = binder.GetActiveBinders();
	// make a copy of the set of binders, so we can restore it later
	auto binders = active_binders;
	active_binders.pop_back();
	uint64_t depth = 1;
	bool success = false;
	while (active_binders.size() > 0) {
		auto &next_binder = active_binders.back();
		assert(depth < numeric_limits<uint32_t>::max());
		auto bind_result = next_binder->Bind(&expr, depth);
		if (bind_result.empty()) {
			success = true;
			break;
		}
		depth++;
		active_binders.pop_back();
	}
	active_binders = binders;
	return success;
}

void ExpressionBinder::BindChild(unique_ptr<ParsedExpression> &expr, uint32_t depth, string &error) {
	if (expr.get()) {
		string bind_error = Bind(&expr, depth);
		if (error.empty()) {
			error = bind_error;
		}
	}
}

void ExpressionBinder::ExtractCorrelatedExpressions(Binder &binder, Expression &expr) {
	if (expr.type == ExpressionType::BOUND_COLUMN_REF) {
		auto &bound_colref = (BoundColumnRefExpression &)expr;
		if (bound_colref.depth > 0) {
			binder.AddCorrelatedColumn(CorrelatedColumnInfo(bound_colref));
		}
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](Expression &child) { ExtractCorrelatedExpressions(binder, child); });
}

unique_ptr<Expression> ExpressionBinder::Bind(unique_ptr<ParsedExpression> &expr, SQLType *result_type,
                                              bool root_expression) {
	// bind the main expression
	auto error_msg = Bind(&expr, 0, root_expression);
	if (!error_msg.empty()) {
		// failed to bind: try to bind correlated columns in the expression (if any)
		bool success = BindCorrelatedColumns(expr);
		if (!success) {
			throw BinderException(error_msg);
		}
		auto bound_expr = (BoundExpression *)expr.get();
		ExtractCorrelatedExpressions(binder, *bound_expr->expr);
	}
	assert(expr->expression_class == ExpressionClass::BOUND_EXPRESSION);
	auto bound_expr = (BoundExpression *)expr.get();
	unique_ptr<Expression> result = move(bound_expr->expr);
	if (target_type.id != SQLTypeId::INVALID) {
		// the binder has a specific target type: add a cast to that type
		result = AddCastToType(move(result), bound_expr->sql_type, target_type);
	} else {
		if (bound_expr->sql_type.id == SQLTypeId::SQLNULL) {
			// SQL NULL type is only used internally in the binder
			// cast to INTEGER if we encounter it outside of the binder
			bound_expr->sql_type = SQLType(SQLTypeId::INTEGER);
			result = AddCastToType(move(result), bound_expr->sql_type, bound_expr->sql_type);
		}
	}
	if (result_type) {
		*result_type = bound_expr->sql_type;
	}
	return result;
}

string ExpressionBinder::Bind(unique_ptr<ParsedExpression> *expr, uint32_t depth, bool root_expression) {
	// bind the node, but only if it has not been bound yet
	auto &expression = **expr;
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_EXPRESSION) {
		// already bound, don't bind it again
		return string();
	}
	// bind the expression
	BindResult result = BindExpression(**expr, depth, root_expression);
	if (result.HasError()) {
		return result.error;
	} else {
		// successfully bound: replace the node with a BoundExpression
		*expr = make_unique<BoundExpression>(move(result.expression), result.sql_type);
		return string();
	}
}

void ExpressionBinder::BindTableNames(ParsedExpression &expr) {
	if (expr.type == ExpressionType::COLUMN_REF) {
		auto &colref = (ColumnRefExpression &)expr;
		if (colref.table_name.empty()) {
			// no table name: find a binding that contains this
			colref.table_name = binder.bind_context.GetMatchingBinding(colref.column_name);
		}
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](const ParsedExpression &child) { BindTableNames((ParsedExpression &)child); });
}

namespace duckdb {
unique_ptr<Expression> AddCastToType(unique_ptr<Expression> expr, SQLType source_type, SQLType target_type) {
	assert(expr);
	if (expr->expression_class == ExpressionClass::BOUND_PARAMETER) {
		auto &parameter = (BoundParameterExpression &)*expr;
		parameter.sql_type = target_type;
		parameter.return_type = GetInternalType(target_type);
	} else if (expr->expression_class == ExpressionClass::BOUND_DEFAULT) {
		auto &def = (BoundDefaultExpression &)*expr;
		def.sql_type = target_type;
		def.return_type = GetInternalType(target_type);
	} else if (source_type != target_type) {
		return make_unique<BoundCastExpression>(GetInternalType(target_type), move(expr), source_type, target_type);
	}
	return expr;
}

} // namespace duckdb
