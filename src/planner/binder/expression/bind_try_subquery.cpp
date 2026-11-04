#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/scalar_function_catalog_entry.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/generic_functions.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/expression/bound_subquery_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

#include <algorithm>

namespace duckdb {

static unique_ptr<Expression> WrapInTry(unique_ptr<Expression> child) {
	auto result = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_TRY, child->GetReturnType());
	result->GetChildrenMutable().push_back(std::move(child));
	return std::move(result);
}

//! Match the use-site re-raise PlanSubquery creates for a subquery lowered with an exposed error flag
static bool IsTryErrorRethrow(const Expression &expr) {
	if (expr.GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		return false;
	}
	return expr.Cast<BoundFunctionExpression>().Function().GetName() == TryRethrowFun::Name;
}

//! Collect the error flags of re-raises in unconditionally evaluated positions that no TRY or
//! HAS_ERROR inside the expression consumes. A flag from a conditionally evaluated position (a CASE
//! branch, a later COALESCE argument, the short-circuited operand of a selection conjunction) must
//! not join the gate: the gate fires eagerly, while evaluation may never reach that re-raise. The
//! split follows the executor: value evaluation computes every operand of conjunctions and IN lists,
//! while selection evaluation (a CASE WHEN condition) short-circuits conjunctions. Function arguments
//! count as unconditional even though the executor can skip later arguments after a constant NULL:
//! that eager reading of a nested subquery error is accepted and pinned in tests.
static void CollectTryErrorRethrowFlags(const Expression &expr, bool select_context,
                                        vector<unique_ptr<Expression>> &flags) {
	if (IsTryErrorRethrow(expr)) {
		flags.push_back(expr.Cast<BoundFunctionExpression>().GetChildren()[0]->Copy());
		return;
	}
	switch (expr.GetExpressionClass()) {
	case ExpressionClass::BOUND_CASE:
		// only the first WHEN condition is evaluated unconditionally, and it is evaluated as a selection
		CollectTryErrorRethrowFlags(*expr.Cast<BoundCaseExpression>().CaseChecks()[0].when_expr, true, flags);
		return;
	case ExpressionClass::BOUND_CONJUNCTION:
		if (select_context) {
			// selection evaluation short-circuits, so only the first operand always evaluates
			CollectTryErrorRethrowFlags(*expr.Cast<BoundConjunctionExpression>().GetChildren()[0], true, flags);
			return;
		}
		break;
	case ExpressionClass::BOUND_OPERATOR:
		switch (expr.GetExpressionType()) {
		case ExpressionType::OPERATOR_TRY:
		case ExpressionType::OPERATOR_HAS_ERROR:
			// the capture consumes re-raises below it
			return;
		case ExpressionType::OPERATOR_COALESCE:
			CollectTryErrorRethrowFlags(*expr.Cast<BoundOperatorExpression>().GetChildren()[0], false, flags);
			return;
		default:
			break;
		}
		break;
	default:
		break;
	}
	// everything else evaluates its children as values before producing a result
	ExpressionIterator::EnumerateChildren(
	    expr, [&](const Expression &child) { CollectTryErrorRethrowFlags(child, false, flags); });
}

static unique_ptr<Expression> OrErrorFlags(unique_ptr<Expression> gate, unique_ptr<Expression> flag) {
	if (!gate) {
		return flag;
	}
	return make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR, std::move(gate), std::move(flag));
}

//! Find the aggregate node of the TRY select node by its recorded table index. The search traverses
//! joins that subquery flattening may have inserted between the select list and the aggregate.
static optional_ptr<LogicalAggregate> FindAggregateOperator(LogicalOperator &op, TableIndex aggregate_index) {
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggr = op.Cast<LogicalAggregate>();
		if (aggr.aggregate_index == aggregate_index) {
			return &aggr;
		}
	}
	for (auto &child : op.children) {
		auto result = FindAggregateOperator(*child, aggregate_index);
		if (result) {
			return result;
		}
	}
	return nullptr;
}

bool ExpressionBinder::GateTryAggregates(const ParsedExpression &location, LogicalAggregate &aggregate,
                                         const vector<idx_t> &aggregate_positions, bool strict,
                                         unique_ptr<Expression> &gate) {
	// validate every position before mutating anything, so a non-strict caller can still fall back to
	// the plain subquery
	for (auto position : aggregate_positions) {
		auto &aggr_expr = aggregate.expressions[position]->Cast<BoundAggregateExpression>();
		if (aggr_expr.GetFilter()) {
			if (strict) {
				throw BinderException(location, "TRY over a subquery with an aggregate FILTER clause is not supported");
			}
			return false;
		}
		if (aggr_expr.GetOrderBys()) {
			if (strict) {
				throw BinderException(location, "TRY over a subquery with an ordered aggregate is not supported");
			}
			return false;
		}
		if (aggr_expr.StateExportMode() != AggregateStateExportMode::NONE) {
			if (strict) {
				throw BinderException(location,
				                      "TRY over a subquery with an exported aggregate state is not supported");
			}
			return false;
		}
		for (auto &child : aggr_expr.GetChildren()) {
			if (child->IsVolatile()) {
				if (strict) {
					throw BinderException(location, "TRY can not be used in combination with a volatile function");
				}
				return false;
			}
		}
	}
	// wrap the aggregate inputs in TRY and collect HAS_ERROR probes over the same inputs
	for (auto position : aggregate_positions) {
		auto &aggr_expr = aggregate.expressions[position]->Cast<BoundAggregateExpression>();
		for (auto &child : aggr_expr.GetChildrenMutable()) {
			auto probe = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_HAS_ERROR, LogicalType::BOOLEAN);
			probe->GetChildrenMutable().push_back(child->Copy());
			gate = OrErrorFlags(std::move(gate), std::move(probe));
			child = WrapInTry(std::move(child));
		}
	}
	if (!gate) {
		// aggregates without inputs, such as count(*), have nothing to mask
		return true;
	}
	// aggregate the probes, so that an error in any input row raises the gate
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	auto &bool_or = system_catalog.GetEntry<AggregateFunctionCatalogEntry>(
	    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "bool_or"));
	vector<pair<Identifier, unique_ptr<Expression>>> gate_children;
	gate_children.emplace_back(Identifier(), std::move(gate));
	ErrorData error;
	FunctionBinder function_binder(context);
	auto gate_aggregate = function_binder.BindAggregateFunction(bool_or, std::move(gate_children), error);
	if (!gate_aggregate) {
		error.AddQueryLocation(location);
		error.Throw();
	}
	ColumnBinding gate_binding(aggregate.aggregate_index, ProjectionIndex(aggregate.expressions.size()));
	aggregate.expressions.push_back(std::move(gate_aggregate));
	gate = make_uniq<BoundColumnRefExpression>(LogicalType::BOOLEAN, gate_binding);
	return true;
}

optional<LogicalType> ExpressionBinder::LowerTrySubquery(const ParsedExpression &location,
                                                         const TrySubqueryPlanIndexes &indexes, LogicalOperator &root,
                                                         bool expose_error_flag, bool tolerant) {
	D_ASSERT(indexes.recorded);
	// a fallback to the plain subquery would strand the re-raises of nested subqueries that were already
	// lowered with an exposed flag, so such shapes are reported even when a fallback is allowed
	const bool strict = !tolerant || indexes.nested_transport;
	// walk from the plan root to the select list projection of the TRY select node
	auto binding = root.GetColumnBindings()[0];
	reference<LogicalOperator> current(root);
	optional_ptr<LogicalProjection> select_projection;
	while (!select_projection) {
		auto &current_op = current.get();
		switch (current_op.type) {
		case LogicalOperatorType::LOGICAL_ORDER_BY:
		case LogicalOperatorType::LOGICAL_TOP_N:
		case LogicalOperatorType::LOGICAL_DISTINCT:
			if (expose_error_flag) {
				// these modifiers reference or deduplicate the select list column; a struct transport
				// would leave their references scalar or change DISTINCT semantics (an error-masked
				// NULL must not be distinguishable from a plain NULL)
				if (strict) {
					throw BinderException(location, "TRY over a subquery with result modifiers is only supported "
					                                "when the subquery is the direct argument of TRY");
				}
				return optional<LogicalType>();
			}
			current = *current_op.children[0];
			continue;
		case LogicalOperatorType::LOGICAL_LIMIT:
			// constant LIMIT and OFFSET forward the child columns without referencing them; a LIMIT
			// computed by a scalar subquery adds a hidden select list entry and is handled by the
			// prune rejection below. USING SAMPLE attaches below the select list projection and never
			// appears on this path
			current = *current_op.children[0];
			continue;
		case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE:
			current = *current_op.children[1];
			continue;
		case LogicalOperatorType::LOGICAL_PROJECTION: {
			auto &projection = current_op.Cast<LogicalProjection>();
			if (projection.table_index == indexes.prune_index) {
				if (expose_error_flag) {
					// pruning accompanies hidden select list entries (ORDER BY columns or a LIMIT
					// computed by a scalar subquery), whose references would stay typed scalar
					if (strict) {
						throw BinderException(location, "TRY over a subquery with result modifiers is only supported "
						                                "when the subquery is the direct argument of TRY");
					}
					return optional<LogicalType>();
				}
				// the prune projection forwards the select list columns
				binding = projection.expressions[binding.column_index]->Cast<BoundColumnRefExpression>().Binding();
				current = *projection.children[0];
				continue;
			}
			if (projection.table_index != indexes.projection_index) {
				throw InternalException("TRY subquery: unexpected projection above the select list");
			}
			select_projection = &projection;
			continue;
		}
		default:
			throw InternalException("TRY subquery: unexpected operator above the select list projection");
		}
	}

	// classify the projected expression against the recorded indexes of the TRY select node, so that
	// the lowering never crosses into derived tables, flattened subqueries or other query levels
	auto &expr = select_projection->expressions[binding.column_index];
	vector<ColumnBinding> refs;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(*expr, [&](const BoundColumnRefExpression &colref) {
		if (colref.Depth() == 0) {
			refs.push_back(colref.Binding());
		}
	});
	vector<idx_t> aggregate_positions;
	for (auto &ref : refs) {
		if (ref.table_index == indexes.window_index) {
			if (strict) {
				throw BinderException(location,
				                      "TRY over a subquery with a window function in its select list is not supported");
			}
			return optional<LogicalType>();
		}
		for (auto &unnest_index : indexes.unnest_indexes) {
			if (ref.table_index == unnest_index) {
				if (strict) {
					throw BinderException(location,
					                      "TRY over a subquery with UNNEST in its select list is not supported");
				}
				return optional<LogicalType>();
			}
		}
		if (ref.table_index == indexes.aggregate_index) {
			aggregate_positions.push_back(ref.column_index);
		}
	}
	// error_flag is only produced when the caller re-raises captured errors at the use site
	unique_ptr<Expression> error_flag;
	if (!aggregate_positions.empty()) {
		// the select item aggregates in this node: gate it on HAS_ERROR probes of the aggregate inputs
		auto aggregate = FindAggregateOperator(*select_projection->children[0], indexes.aggregate_index);
		if (!aggregate) {
			throw InternalException("TRY subquery: the aggregate node was not found below the select list");
		}
		std::sort(aggregate_positions.begin(), aggregate_positions.end());
		aggregate_positions.erase(std::unique(aggregate_positions.begin(), aggregate_positions.end()),
		                          aggregate_positions.end());
		unique_ptr<Expression> gate;
		if (!GateTryAggregates(location, *aggregate, aggregate_positions, strict, gate)) {
			return optional<LogicalType>();
		}
		// re-raises of nested subqueries next to the aggregates have no consuming TRY above them: their
		// flags join the gate, and the gated CASE never evaluates the re-raise on an error row; a
		// re-raise in a conditionally evaluated position stays out of the gate and raises if reached
		vector<unique_ptr<Expression>> rethrow_flags;
		CollectTryErrorRethrowFlags(*expr, false, rethrow_flags);
		for (auto &flag : rethrow_flags) {
			gate = OrErrorFlags(std::move(gate), std::move(flag));
		}
		if (gate) {
			if (expose_error_flag) {
				// the error flag carries the gate; the CASE below still guards the projected
				// expression, whose evaluation can depend on the gated aggregate results
				error_flag = gate->Copy();
			}
			// gate the projected expression: an input error turns the aggregated result into NULL
			auto case_expr = make_uniq<BoundCaseExpression>(expr->GetReturnType());
			BoundCaseCheck check;
			check.when_expr = std::move(gate);
			check.then_expr = make_uniq<BoundConstantExpression>(Value(expr->GetReturnType()));
			case_expr->CaseChecksMutable().push_back(std::move(check));
			case_expr->ElseMutable() = std::move(expr);
			expr = std::move(case_expr);
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		// a bare column computed outside this node's select list (a group, a FROM or derived table
		// column, a flattened nested subquery): TRY has nothing to mask, errors there propagate
	} else {
		// a plain projection: push TRY onto the projected expression; TRY consumes the re-raises of
		// nested subqueries per row, and the HAS_ERROR probe observes them when the flag is exposed
		if (expr->IsVolatile()) {
			if (strict) {
				throw BinderException(location, "TRY can not be used in combination with a volatile function");
			}
			return optional<LogicalType>();
		}
		if (expose_error_flag) {
			auto probe = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_HAS_ERROR, LogicalType::BOOLEAN);
			probe->GetChildrenMutable().push_back(expr->Copy());
			error_flag = std::move(probe);
		}
		expr = WrapInTry(std::move(expr));
	}
	if (!expose_error_flag) {
		return expr->GetReturnType();
	}
	if (!error_flag) {
		error_flag = make_uniq<BoundConstantExpression>(Value::BOOLEAN(false));
	}
	// pack the masked value and the error flag into a single struct column for the use site
	expr->SetAlias("value");
	error_flag->SetAlias("error");
	vector<pair<Identifier, unique_ptr<Expression>>> pack_children;
	pack_children.emplace_back(Identifier(), std::move(expr));
	pack_children.emplace_back(Identifier(), std::move(error_flag));
	auto &system_catalog = Catalog::GetSystemCatalog(context);
	auto &struct_pack = system_catalog.GetEntry<ScalarFunctionCatalogEntry>(
	    context, QualifiedName(system_catalog.GetName(), DEFAULT_SCHEMA, "struct_pack"));
	ErrorData error;
	FunctionBinder function_binder(context);
	auto packed = function_binder.BindScalarFunction(struct_pack, std::move(pack_children), error, false, &binder);
	if (!packed) {
		error.AddQueryLocation(location);
		error.Throw();
	}
	auto struct_type = packed->GetReturnType();
	select_projection->expressions[binding.column_index] = std::move(packed);
	return struct_type;
}

void ExpressionBinder::CheckTrySubqueryNode(const SubqueryExpression &subquery_expr, const ParsedExpression &location) {
	if (subquery_expr.Subquery()->node->type != QueryNodeType::SELECT_NODE) {
		throw BinderException(location, "TRY over a subquery is only supported for plain SELECT subqueries");
	}
}

//! A select item that is itself a scalar subquery takes the direct TRY lowering recursively: unlike the
//! exposed-flag transport, the direct form also covers result modifiers inside the nested subquery
static void PushTryIntoBareSubqueryItem(SubqueryExpression &subquery_expr) {
	auto &node = *subquery_expr.SubqueryMutable()->node;
	D_ASSERT(node.type == QueryNodeType::SELECT_NODE);
	auto &select_node = node.Cast<SelectNode>();
	if (select_node.select_list.size() != 1) {
		return;
	}
	auto &select_item = select_node.select_list[0];
	if (select_item->GetExpressionClass() != ExpressionClass::SUBQUERY ||
	    select_item->Cast<SubqueryExpression>().GetSubqueryType() != SubqueryType::SCALAR) {
		return;
	}
	auto alias = select_item->GetAlias();
	auto try_item = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_TRY);
	try_item->SetQueryLocation(select_item->GetQueryLocation());
	try_item->GetChildrenMutable().push_back(std::move(select_item));
	if (!alias.empty()) {
		try_item->SetAlias(alias);
	}
	select_item = std::move(try_item);
}

//! TRY over a scalar subquery binds the subquery as an ordinary subquery and lowers the TRY onto the bound
//! plan before the subquery is flattened, so names, types, overloads and aggregate ownership resolve exactly
//! as without TRY. In the plain projection case, TRY moves onto the projected expression, which yields a NULL
//! result under the one-row contract of a scalar subquery. In the aggregate case, the projected expression is
//! gated on HAS_ERROR probes of the aggregate inputs, so that an input error anywhere turns the aggregated
//! result into NULL instead of a partially aggregated value. The probes cover the aggregate inputs only: an
//! error raised inside aggregate execution itself (state update, combine, finalize) propagates. Scalar
//! subqueries nested inside the select item are lowered with an exposed error flag, so their errors turn the
//! whole result into NULL as well; shapes whose flag cannot be transported fall back to the plain subquery.
//! Errors raised outside the maskable positions, such as in the WHERE clause, in aggregate or window
//! arguments or by the one-row cardinality check, propagate deliberately. Volatile computation inside a
//! maskable position is rejected. A volatile value computed by a nested subquery is evaluated once in its
//! flattened subplan and referenced as a column, so the TRY input and its HAS_ERROR probe observe one
//! consistent value.
BindResult ExpressionBinder::BindTrySubquery(OperatorExpression &op, idx_t depth) {
	D_ASSERT(op.GetChildrenMutable().size() == 1);
	auto &subquery_expr = op.GetChildrenMutable()[0]->Cast<SubqueryExpression>();
	D_ASSERT(subquery_expr.GetSubqueryType() == SubqueryType::SCALAR);
	CheckTrySubqueryNode(subquery_expr, op);
	PushTryIntoBareSubqueryItem(subquery_expr);

	// bind as an ordinary scalar subquery, so inner scopes, overload resolution and aggregate
	// ownership resolve exactly as without TRY
	const auto was_inside_try = inside_try;
	inside_try = false;
	bind_try_subquery = true;
	auto result = BindExpression(subquery_expr, depth);
	inside_try = was_inside_try;
	if (result.HasError()) {
		return result;
	}
	auto &bound_subquery = result.expression->Cast<BoundSubqueryExpression>();
	if (bound_subquery.SubqueryMutable().plan) {
		auto &indexes = bound_subquery.GetBinder()->GetTrySubqueryPlanIndexes();
		if (!indexes.recorded) {
			throw InternalException("TRY subquery: no plan indexes were recorded for the top select node");
		}
		LowerTrySubquery(op, indexes, *bound_subquery.SubqueryMutable().plan, false, false);
	}
	return result;
}

} // namespace duckdb
