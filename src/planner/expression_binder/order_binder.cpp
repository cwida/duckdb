#include "duckdb/planner/expression_binder/order_binder.hpp"

#include "duckdb/parser/expression/collate_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/parameter_expression.hpp"
#include "duckdb/parser/expression/positional_reference_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

OrderBinder::OrderBinder(vector<reference<Binder>> binders, SelectBindState &bind_state)
    : binders(std::move(binders)), extra_list(nullptr), bind_state(bind_state) {
}
OrderBinder::OrderBinder(vector<reference<Binder>> binders, SelectNode &node, SelectBindState &bind_state)
    : binders(std::move(binders)), bind_state(bind_state) {
	this->extra_list = &node.select_list;
}

unique_ptr<Expression> OrderBinder::CreateProjectionReference(ParsedExpression &expr, const idx_t index) {
	Identifier alias;
	if (extra_list && index < extra_list->size()) {
		alias = Identifier(extra_list->at(index)->ToString());
	} else {
		if (!expr.GetAlias().empty()) {
			alias = expr.GetAlias();
		}
	}
	auto result = make_uniq<BoundConstantExpression>(Value::UBIGINT(index));
	result->SetAlias(std::move(alias));
	result->SetQueryLocation(expr.GetQueryLocation());
	return std::move(result);
}

unique_ptr<Expression> OrderBinder::CreateExtraReference(unique_ptr<ParsedExpression> expr) {
	if (!extra_list) {
		throw InternalException("CreateExtraReference called without extra_list");
	}
	bind_state.projection_map[*expr] = extra_list->size();
	auto result = CreateProjectionReference(*expr, extra_list->size());
	extra_list->push_back(std::move(expr));
	return result;
}

optional_idx OrderBinder::TryGetProjectionReference(ParsedExpression &expr) const {
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::CONSTANT: {
		auto &constant = expr.Cast<ConstantExpression>();
		// ORDER BY a constant
		int64_t order_value;
		if (!constant.GetLiteral().TryGetInt64(order_value)) {
			// non-integral expression
			// ORDER BY <constant> has no effect
			// this is disabled by default (matching Postgres) - but we can control this with a setting
			auto order_by_non_integer_literal =
			    Settings::Get<OrderByNonIntegerLiteralSetting>(binders[0].get().context);
			if (!order_by_non_integer_literal) {
				throw BinderException(expr,
				                      "%s non-integer literal has no effect.\n* SET "
				                      "order_by_non_integer_literal=true to allow this behavior.",
				                      query_component);
			}
			break;
		}
		// INTEGER constant: we use the integer as an index into the select list (e.g. ORDER BY 1)
		return static_cast<idx_t>(order_value <= 0 ? NumericLimits<int64_t>::Maximum() : order_value - 1);
	}
	case ExpressionClass::COLUMN_REF: {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (!ExpressionBinder::IsPotentialAlias(colref)) {
			break;
		}

		auto &alias_name = colref.ColumnNames().back();
		// check the alias list
		auto entry = bind_state.alias_map.find(alias_name);
		if (entry != bind_state.alias_map.end()) {
			// this is an alias - return the index
			return entry->second;
		}
		// check the expression list
		vector<idx_t> matching_columns;
		for (idx_t i = 0; i < bind_state.original_expressions.size(); i++) {
			if (bind_state.original_expressions[i]->GetExpressionType() != ExpressionType::COLUMN_REF) {
				continue;
			}
			auto &colref = bind_state.original_expressions[i]->Cast<ColumnRefExpression>();
			if (colref.HasAlias()) {
				continue;
			}
			if (colref.GetColumnName() == alias_name) {
				matching_columns.push_back(i);
			}
		}
		if (matching_columns.size() == 1) {
			return matching_columns[0];
		}
		break;
	}
	case ExpressionClass::POSITIONAL_REFERENCE: {
		auto &posref = expr.Cast<PositionalReferenceExpression>();
		return posref.Index() - 1;
	}
	default:
		break;
	}
	return optional_idx();
}

void OrderBinder::SetQueryComponent(string component) {
	if (component.empty()) {
		query_component = "ORDER BY";
	} else {
		query_component = std::move(component);
	}
}

unique_ptr<Expression> OrderBinder::BindConstant(ParsedExpression &expr) {
	auto index = TryGetProjectionReference(expr);
	if (!index.IsValid()) {
		return nullptr;
	}
	child_list_t<Value> values;
	values.emplace_back(make_pair("index", Value::UBIGINT(index.GetIndex())));
	auto result = make_uniq<BoundConstantExpression>(Value::STRUCT(std::move(values)));
	result->SetAlias(expr.GetAlias());
	result->SetQueryLocation(expr.GetQueryLocation());
	return std::move(result);
}

unique_ptr<Expression> OrderBinder::Bind(unique_ptr<ParsedExpression> expr) {
	// in the ORDER BY clause we do not bind children
	// we bind ONLY to the select list
	// if there is no matching entry in the SELECT list already, we add the expression to the SELECT list and refer the
	// new expression the new entry will then be bound later during the binding of the SELECT list we also don't do type
	// resolution here: this only happens after the SELECT list has been bound
	switch (expr->GetExpressionClass()) {
	case ExpressionClass::CONSTANT: {
		// ORDER BY constant
		// is the ORDER BY expression a constant integer? (e.g. ORDER BY 1)
		return BindConstant(*expr);
	}
	case ExpressionClass::COLUMN_REF:
	case ExpressionClass::POSITIONAL_REFERENCE: {
		// COLUMN REF expression
		// check if we can bind it to an alias in the select list
		auto index = TryGetProjectionReference(*expr);
		if (index.IsValid()) {
			return CreateProjectionReference(*expr, index.GetIndex());
		}
		break;
	}
	case ExpressionClass::PARAMETER: {
		throw ParameterNotAllowedException("Parameter not supported in %s clause", query_component);
	}
	case ExpressionClass::COLLATE: {
		auto &collation = expr->Cast<CollateExpression>();
		auto collation_index = TryGetProjectionReference(*collation.ChildMutable());
		if (collation_index.IsValid()) {
			child_list_t<Value> values;
			values.emplace_back(make_pair("index", Value::UBIGINT(collation_index.GetIndex())));
			values.emplace_back(make_pair("collation", Value(collation.Collation())));
			return make_uniq<BoundConstantExpression>(Value::STRUCT(std::move(values)));
		}
		break;
	}
	default:
		break;
	}
	// general case
	// first bind the table names of this entry
	for (auto &binder : binders) {
		ExpressionBinder::QualifyColumnNames(binder.get(), expr);
	}
	// first check if the ORDER BY clause already points to an entry in the projection list
	auto entry = bind_state.projection_map.find(*expr);
	if (entry != bind_state.projection_map.end()) {
		if (entry->second == DConstants::INVALID_INDEX) {
			throw BinderException("Ambiguous reference to column");
		}
		// there is a matching entry in the projection list
		// just point to that entry
		return CreateProjectionReference(*expr, entry->second);
	}
	if (!extra_list) {
		// no extra list specified: we cannot push an extra ORDER BY clause
		throw BinderException("Could not ORDER BY column \"%s\": add the expression/function to every SELECT, or move "
		                      "the UNION into a FROM clause.",
		                      expr->ToString());
	}
	// otherwise we need to push the ORDER BY entry into the select list
	return CreateExtraReference(std::move(expr));
}

} // namespace duckdb
