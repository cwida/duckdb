#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/parser/expression_map.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/query_node/recursive_cte_node.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/planner/operator/logical_set_operation.hpp"
#include "duckdb/planner/operator/logical_recursive_cte.hpp"
#include "duckdb/planner/expression_binder.hpp"

namespace duckdb {

BoundStatement Binder::BindNode(RecursiveCTENode &statement) {
	// first recursively visit the recursive CTE operations
	// the left side is visited first and is added to the BindContext of the right side
	D_ASSERT(statement.left);
	D_ASSERT(statement.right);

	auto is_using_key = !statement.key_targets.empty();

	auto ctename = statement.ctename;
	auto union_all = statement.union_all;
	auto setop_index = GenerateTableIndex();

	auto left_binder = Binder::CreateBinder(context, this);
	auto left = left_binder->BindNode(*statement.left);

	BoundStatement result;
	// the result types of the CTE are the types of the LHS
	result.types = left.types;
	vector<LogicalType> internal_types = left.types;
	vector<unique_ptr<Expression>> key_targets, payload_aggregates;

	// names are picked from the LHS, unless aliases are explicitly specified
	result.names = left.names;
	for (idx_t i = 0; i < statement.aliases.size() && i < result.names.size(); i++) {
		if (statement.aliases[i].empty()) {
			throw BinderException("column alias cannot be empty");
		}
		result.names[i] = statement.aliases[i];
	}

	// This allows the right side to reference the CTE recursively
	bind_context.AddGenericBinding(setop_index, statement.ctename, result.names, result.types);

	// Create temporary binder to bind expressions
	auto aggregate_binder = Binder::CreateBinder(context, nullptr);
	FunctionBinder function_binder(*aggregate_binder);
	aggregate_binder->bind_context.AddGenericBinding(setop_index, statement.ctename, result.names, result.types);
	ExpressionBinder expression_binder(*aggregate_binder, context);

	// Set contains column indices that are already bound
	unordered_set<ProjectionIndex> key_references;
	unordered_map<ProjectionIndex, unique_ptr<Expression>> payload_references;
	// Temporary copy of return types that we can modify without having a conflict with binding the aggregates
	vector<LogicalType> return_types = result.types;

	// Bind specified keys to the referenced column
	for (idx_t expr_idx = 0; expr_idx < statement.key_targets.size(); expr_idx++) {
		auto &expr = statement.key_targets[expr_idx];

		if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
			if (expr->HasAlias()) {
				throw BinderException(expr->GetQueryLocation(),
				                      "In USING KEY, only direct calls to an aggregate function can have an alias.");
			}

			auto bound_expr = expression_binder.Bind(expr);
			auto &bound_ref = bound_expr->Cast<BoundColumnRefExpression>();

			auto column_index = bound_ref.Binding().column_index;
			if (key_references.find(column_index) != key_references.end()) {
				continue;
			}

			key_references.insert(column_index);
			key_targets.push_back(std::move(bound_expr));
		} else if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
			auto &func_expr = expr->Cast<FunctionExpression>();

			if (func_expr.Filter()) {
				throw BinderException(func_expr.Filter()->GetQueryLocation(),
				                      "FILTER clause is not yet supported for aggregates in USING KEY");
			}

			if (!func_expr.OrderBy()->orders.empty()) {
				throw BinderException(func_expr.GetQueryLocation(),
				                      "ORDER BY clause is not yet supported for aggregates in USING KEY");
			}

			if (func_expr.Distinct()) {
				throw BinderException(func_expr.GetQueryLocation(),
				                      "DISTINCT is not yet supported for aggregates in USING KEY");
			}

			QueryErrorContext error_context(expr->GetQueryLocation());

			auto function_name = func_expr.GetQualifiedName();
			BindSchemaOrCatalog(function_name);
			EntryLookupInfo function_lookup(CatalogType::AGGREGATE_FUNCTION_ENTRY, BindTableName(function_name),
			                                error_context);
			auto entry = GetCatalogEntry(function_lookup, OnEntryNotFound::RETURN_NULL);

			if (!entry || entry->type != CatalogType::AGGREGATE_FUNCTION_ENTRY) {
				throw BinderException(
				    expr->GetQueryLocation(),
				    "'%s' can't be used in the USING KEY clause.\n"
				    "It has to be either a column name as a key or a direct call to an aggregate function.",
				    expr->ToString());
			}
			auto &func = entry->Cast<AggregateFunctionCatalogEntry>();

			vector<unique_ptr<Expression>> bound_children;

			// Bind the children of the aggregate function
			for (auto &child : func_expr.GetArgumentsMutable()) {
				bound_children.push_back(expression_binder.Bind(child.GetExpressionMutable()));
			}

			ProjectionIndex aggregate_idx;
			// If user provided an alias, prioritize that.
			// Otherwise, we try to infer the target column from the first argument
			if (func_expr.HasAlias()) {
				auto names_iter = find(result.names.begin(), result.names.end(), func_expr.GetAlias());
				if (names_iter == result.names.end()) {
					throw BinderException(expr->GetQueryLocation(),
					                      "Could not find column with name %s to bind aggregate to.",
					                      func_expr.GetAlias());
				}
				aggregate_idx = ProjectionIndex(NumericCast<idx_t>(std::distance(result.names.begin(), names_iter)));
			} else {
				if (bound_children.empty() ||
				    bound_children[0]->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
					// No alias and no way to infer target column through first argument
					throw BinderException(
					    expr->GetQueryLocation(),
					    "In USING KEY, an aggregate must either have a column reference or an alias.");
				}
				aggregate_idx = bound_children[0]->Cast<BoundColumnRefExpression>().Binding().column_index;
			}

			// Bind the best matching aggregate function
			ErrorData error;
			auto aggregate = function_binder.BindAggregateFunction(func, std::move(bound_children), error);
			if (!aggregate) {
				throw BinderException("No matching aggregate function\n%s", error.Message());
			}

			if (payload_references.find(aggregate_idx) != payload_references.end()) {
				throw BinderException(func_expr.GetQueryLocation(),
				                      "Column %s referenced multiple times in USING KEY clause.\n"
				                      "Try using an alias for one of the aggregates.",
				                      result.names[aggregate_idx]);
			}

			if (key_references.find(aggregate_idx) != key_references.end()) {
				throw BinderException(func_expr.GetQueryLocation(),
				                      "Column %s cannot be used as both key and aggregate in USING KEY clause.\n"
				                      "Try using an alias for the aggregation.",
				                      result.names[aggregate_idx]);
			}

			return_types[aggregate_idx] = aggregate->GetReturnType();
			payload_references[aggregate_idx] = std::move(aggregate);
		} else {
			throw BinderException(
			    expr->GetQueryLocation(),
			    "'%s' can't be used in the USING KEY clause.\n"
			    "It has to be either a column name as a key or a direct call to an aggregate function.",
			    expr->ToString());
		}
	}

	if (key_targets.empty() && !payload_references.empty()) {
		throw BinderException("USING KEY clause requires at least one key column.");
	}

	// Now that we have finished binding all aggregates, we can update the operator types
	result.types = std::move(return_types);

	// If we have key targets, then all the other columns must be aggregated
	if (!key_targets.empty()) {
		// Bind every column that is neither referenced as a key nor by an aggregate to a LAST aggregate
		for (idx_t i = 0; i < left.types.size(); i++) {
			if (key_references.find(ProjectionIndex(i)) == key_references.end()) {
				auto payload_entry = payload_references.find(ProjectionIndex(i));
				if (payload_entry == payload_references.end()) {
					// Create a new bound column reference for the missing columns
					vector<unique_ptr<Expression>> first_children;
					auto bound = make_uniq<BoundColumnRefExpression>(result.types[i],
					                                                 ColumnBinding(setop_index, ProjectionIndex(i)));
					first_children.push_back(std::move(bound));

					// Create a last aggregate for the newly bound column reference
					auto last_fun = GetBuiltinAggregateFunction(context, LastFun::Name, {result.types[i]});
					auto first_aggregate = function_binder.BindAggregateFunction(last_fun, std::move(first_children),
					                                                             nullptr, AggregateType::NON_DISTINCT);

					payload_aggregates.push_back(std::move(first_aggregate));
				} else {
					payload_aggregates.push_back(std::move(payload_entry->second));
				}
			}
		}
	}

	auto right_binder = Binder::CreateBinder(context, this);

	// Add bindings of left side to temporary CTE bindings context
	BindingAlias cte_alias(statement.ctename);
	auto &recursive_types = is_using_key && !union_all ? result.types : internal_types;
	right_binder->bind_context.AddCTEBinding(setop_index, std::move(cte_alias), result.names, recursive_types);

	BindingAlias recurring_alias("recurring", statement.ctename);
	right_binder->bind_context.AddCTEBinding(setop_index, std::move(recurring_alias), result.names, result.types);

	auto right = right_binder->BindNode(*statement.right);
	for (auto &c : left_binder->correlated_columns) {
		right_binder->AddCorrelatedColumn(c);
	}

	// move the correlated expressions from the child binders to this binder
	MoveCorrelatedExpressions(*left_binder);
	MoveCorrelatedExpressions(*right_binder);

	// now both sides have been bound we can resolve types
	if (left.types.size() != right.types.size()) {
		throw BinderException("Set operations can only apply to expressions with the "
		                      "same number of result columns");
	}

	if (!statement.modifiers.empty()) {
		throw NotImplementedException("FIXME: bind modifiers in recursive CTE");
	}

	// Generate the logical plan for the left and right sides of the set operation
	left_binder->is_outside_flattened = is_outside_flattened;
	right_binder->is_outside_flattened = is_outside_flattened;

	auto left_node = std::move(left.plan);
	auto right_node = std::move(right.plan);

	// for both the left and right sides, cast them to the same types
	left_node = CastLogicalOperatorToTypes(left.types, internal_types, std::move(left_node));
	right_node = CastLogicalOperatorToTypes(right.types, internal_types, std::move(right_node));

	auto recurring_binding = right_binder->GetCTEBinding(BindingAlias("recurring", ctename));
	bool ref_recurring = recurring_binding && recurring_binding->IsReferenced();

	// Check if there is a reference to the recursive or recurring table, if not create a set operator.
	auto cte_binding = right_binder->GetCTEBinding(BindingAlias(ctename));
	bool ref_cte = cte_binding && cte_binding->IsReferenced();
	if (!ref_cte && !ref_recurring) {
		auto root = make_uniq<LogicalSetOperation>(setop_index, internal_types.size(), std::move(left_node),
		                                           std::move(right_node), LogicalOperatorType::LOGICAL_UNION,
		                                           is_using_key || union_all);
		if (!is_using_key) {
			result.plan = std::move(root);
			return result;
		}

		auto group_index = GenerateTableIndex();
		auto aggregate_index = GenerateTableIndex();
		unordered_map<ProjectionIndex, ProjectionIndex> group_bindings;
		unordered_map<ProjectionIndex, ProjectionIndex> collated_group_bindings;
		for (idx_t group_idx = 0; group_idx < key_targets.size(); group_idx++) {
			auto &group = key_targets[group_idx]->Cast<BoundColumnRefExpression>();
			const auto column_index = group.Binding().column_index;
			const auto group_type = group.GetReturnType();
			auto uncollated_group = key_targets[group_idx]->Copy();
			if (ExpressionBinder::PushCollation(context, key_targets[group_idx], group_type)) {
				vector<unique_ptr<Expression>> first_children;
				first_children.push_back(std::move(uncollated_group));
				FunctionBinder function_binder(*this);
				auto first_fun = GetBuiltinAggregateFunction(context, FirstFun::Name, {group_type});
				auto first = function_binder.BindAggregateFunction(first_fun, std::move(first_children));
				first->SetAlias("__collated_group");
				collated_group_bindings[column_index] = ProjectionIndex(payload_aggregates.size());
				payload_aggregates.push_back(std::move(first));
			} else {
				group_bindings[column_index] = ProjectionIndex(group_idx);
			}
		}
		auto aggregate = make_uniq<LogicalAggregate>(group_index, aggregate_index, std::move(payload_aggregates));
		aggregate->groups = std::move(key_targets);
		aggregate->AddChild(std::move(root));

		vector<unique_ptr<Expression>> projections;
		projections.reserve(result.types.size());
		idx_t payload_idx = 0;
		for (idx_t column_idx = 0; column_idx < result.types.size(); column_idx++) {
			auto group_entry = group_bindings.find(ProjectionIndex(column_idx));
			auto collated_group_entry = collated_group_bindings.find(ProjectionIndex(column_idx));
			if (group_entry != group_bindings.end()) {
				projections.push_back(make_uniq<BoundColumnRefExpression>(
				    result.types[column_idx], ColumnBinding(group_index, group_entry->second)));
			} else if (collated_group_entry != collated_group_bindings.end()) {
				projections.push_back(make_uniq<BoundColumnRefExpression>(
				    result.types[column_idx], ColumnBinding(aggregate_index, collated_group_entry->second)));
			} else {
				projections.push_back(make_uniq<BoundColumnRefExpression>(
				    result.types[column_idx], ColumnBinding(aggregate_index, ProjectionIndex(payload_idx++))));
			}
		}
		auto projection = make_uniq<LogicalProjection>(GenerateTableIndex(), std::move(projections));
		projection->AddChild(std::move(aggregate));
		result.plan = std::move(projection);
	} else {
		auto root = make_uniq<LogicalRecursiveCTE>(ctename, setop_index, result.types.size(), union_all,
		                                           std::move(key_targets), std::move(left_node), std::move(right_node));
		root->ref_recurring = ref_recurring;
		root->internal_types = std::move(internal_types);
		root->payload_aggregates = std::move(payload_aggregates);
		result.plan = std::move(root);
	}
	return result;
}

} // namespace duckdb
