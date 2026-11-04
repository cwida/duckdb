#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/result_modifier.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/tableref/bound_joinref.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/case_insensitive_map.hpp"
#include "duckdb/planner/expression_binder/lateral_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

static unique_ptr<Expression> BindColumn(Binder &binder, ClientContext &context, const BindingAlias &alias,
                                         const Identifier &column_name) {
	auto expr = make_uniq_base<ParsedExpression, ColumnRefExpression>(column_name, alias);
	ExpressionBinder expr_binder(binder, context);
	return expr_binder.Bind(expr);
}

//! Build the bound comparison of a USING/NATURAL column. Each side is bound against its own binder,
//! which is why the condition is built bound rather than as a parsed expression.
static unique_ptr<Expression> AddCondition(ClientContext &context, Binder &left_binder, Binder &right_binder,
                                           const BindingAlias &left_alias, const BindingAlias &right_alias,
                                           const Identifier &column_name, ExpressionType type) {
	ExpressionBinder expr_binder(left_binder, context);
	auto left = BindColumn(left_binder, context, left_alias, column_name);
	auto right = BindColumn(right_binder, context, right_alias, column_name);
	ErrorData error;
	auto condition = expr_binder.CreateBoundComparison(type, std::move(left), std::move(right), error);
	if (!condition) {
		error.Throw();
	}
	return condition;
}

bool Binder::TryFindBinding(const Identifier &using_column, const string &join_side, BindingAlias &result) {
	// for each using column, get the matching binding
	auto bindings = bind_context.GetMatchingBindings(using_column);
	if (bindings.empty()) {
		return false;
	}
	// find the join binding
	for (auto &binding : bindings) {
		if (result.IsSet()) {
			string error = "Column name \"";
			error += using_column;
			error += "\" is ambiguous: it exists more than once on ";
			error += join_side;
			error += " side of join.\nCandidates:";
			for (auto &binding_ref : bindings) {
				auto &other_binding = binding_ref.get();
				error += "\n\t";
				error += other_binding.GetAlias();
				error += ".";
				error += bind_context.GetActualColumnName(other_binding, using_column);
			}
			throw BinderException(error);
		} else {
			result = binding.get().GetBindingAlias();
		}
	}
	return true;
}

BindingAlias Binder::FindBinding(const Identifier &using_column, const string &join_side) {
	BindingAlias result;
	if (!TryFindBinding(using_column, join_side, result)) {
		throw BinderException("Column %s does not exist on %s side of join!", using_column, join_side);
	}
	return result;
}

static void AddUsingBindings(UsingColumnSet &set, optional_ptr<UsingColumnSet> input_set,
                             const BindingAlias &input_binding) {
	if (input_set) {
		for (auto &entry : input_set->bindings) {
			set.bindings.push_back(entry);
		}
	} else {
		set.bindings.push_back(input_binding);
	}
}

static void SetPrimaryBinding(UsingColumnSet &set, JoinType join_type, const BindingAlias &left_binding,
                              const BindingAlias &right_binding) {
	switch (join_type) {
	case JoinType::LEFT:
	case JoinType::INNER:
	case JoinType::SEMI:
	case JoinType::ANTI:
		set.primary_binding = left_binding;
		break;
	case JoinType::RIGHT:
	case JoinType::RIGHT_SEMI:
	case JoinType::RIGHT_ANTI:
		set.primary_binding = right_binding;
		break;
	default:
		break;
	}
}

BindingAlias Binder::RetrieveUsingBinding(Binder &current_binder, optional_ptr<UsingColumnSet> current_set,
                                          const Identifier &using_column, const string &join_side) {
	BindingAlias binding;
	if (!current_set) {
		binding = current_binder.FindBinding(using_column, join_side);
	} else {
		binding = current_set->primary_binding;
	}
	return binding;
}

static vector<Identifier> RemoveDuplicateUsingColumns(const vector<Identifier> &using_columns) {
	vector<Identifier> result;
	identifier_set_t handled_columns;
	for (auto &using_column : using_columns) {
		if (handled_columns.find(using_column) == handled_columns.end()) {
			handled_columns.insert(using_column);
			result.push_back(using_column);
		}
	}
	return result;
}

BoundStatement Binder::BindJoin(Binder &parent_binder, TableRef &ref) {
	unnamed_subquery_index = parent_binder.unnamed_subquery_index;
	auto result = Bind(ref);
	parent_binder.unnamed_subquery_index = unnamed_subquery_index;
	return result;
}

//! The name under which the target of a NEAREST BY join remains visible to the outer query
static Identifier GetNearestTargetAlias(const TableRef &target) {
	if (!target.alias.empty()) {
		return target.alias;
	}
	if (target.type == TableReferenceType::BASE_TABLE) {
		return target.Cast<BaseTableRef>().Table();
	}
	return Identifier();
}

BoundStatement Binder::BindNearestJoin(JoinRef &ref) {
	// NEAREST BY is sugar for a lateral join against the top-k rows of the target:
	//   <left> JOIN (SELECT * FROM <target> WHERE <ranking> IS NOT NULL
	//                ORDER BY <ranking> LIMIT <k>) <target_alias> ON TRUE
	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(make_uniq<StarExpression>());
	auto target_alias = GetNearestTargetAlias(*ref.right);
	select_node->from_table = std::move(ref.right);

	// Drop candidates whose ranking expression is NULL (they never rank)
	select_node->where_clause =
	    make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, ref.ranking_expression->Copy());

	auto order_modifier = make_uniq<OrderModifier>();
	order_modifier->orders.emplace_back(ref.nearest_order_type, OrderByNullType::ORDER_DEFAULT,
	                                    std::move(ref.ranking_expression));
	select_node->modifiers.push_back(std::move(order_modifier));

	auto limit_modifier = make_uniq<LimitModifier>();
	limit_modifier->limit = ConstantExpression::Integer(NumericCast<int64_t>(ref.nearest_count));
	select_node->modifiers.push_back(std::move(limit_modifier));

	auto select_statement = make_uniq<SelectStatement>();
	select_statement->node = std::move(select_node);

	auto lateral_join = make_uniq<JoinRef>(JoinRefType::REGULAR);
	lateral_join->type = ref.type;
	lateral_join->left = std::move(ref.left);
	lateral_join->right = make_uniq<SubqueryRef>(std::move(select_statement), std::move(target_alias));
	lateral_join->condition = ConstantExpression::Boolean(true);
	lateral_join->query_location = ref.query_location;
	return Bind(*lateral_join);
}

BoundStatement Binder::Bind(JoinRef &ref) {
	if (ref.ref_type == JoinRefType::NEAREST) {
		return BindNearestJoin(ref);
	}
	auto result = make_uniq<BoundJoinRef>(ref.ref_type);
	result->left_binder = Binder::CreateBinder(context, this);
	result->right_binder = Binder::CreateBinder(context, this);
	auto &left_binder = *result->left_binder;
	auto &right_binder = *result->right_binder;

	result->type = ref.type;
	result->left = left_binder.BindJoin(*this, *ref.left);
	result->delim_flipped = ref.delim_flipped;

	{
		LateralBinder lateral_binder(left_binder, context);

		right_binder.BeginSubqueryBind(left_binder, lateral_binder);
		result->right = right_binder.BindJoin(*this, *ref.right);
		if (!ref.duplicate_eliminated_columns.empty()) {
			if (ref.delim_flipped) {
				// We gotta use the expression binder of the right side
				ExpressionBinder expr_binder(right_binder, context);
				for (auto &col : ref.duplicate_eliminated_columns) {
					result->duplicate_eliminated_columns.emplace_back(expr_binder.Bind(col));
				}
			} else {
				// We use the left side
				ExpressionBinder expr_binder(left_binder, context);
				for (auto &col : ref.duplicate_eliminated_columns) {
					result->duplicate_eliminated_columns.emplace_back(expr_binder.Bind(col));
				}
			}
		}
		right_binder.FinishSubqueryBind();
		bool is_lateral = false;
		// Store the correlated columns in the right binder in bound ref for planning of LATERALs
		// Ignore the correlated columns in the left binder, flattening handles those correlations
		result->correlated_columns = right_binder.correlated_columns;
		// Find correlations for the current join
		for (auto &cor_col : result->correlated_columns) {
			if (cor_col.depth == 1) {
				// Depth 1 indicates columns binding from the left indicating a lateral join
				is_lateral = true;
				break;
			}
		}
		result->lateral = is_lateral;
		if (result->lateral) {
			// lateral join: can only be an INNER or LEFT join
			if (result->type != JoinType::INNER && result->type != JoinType::LEFT) {
				throw BinderException("The combining JOIN type must be INNER or LEFT for a LATERAL reference");
			}
		}
	}

	vector<unique_ptr<Expression>> extra_conditions;
	vector<Identifier> extra_using_columns;
	switch (ref.ref_type) {
	case JoinRefType::NATURAL: {
		// natural join, figure out which column names are present in both sides of the join
		// first bind the left hand side and get a list of all the tables and column names
		identifier_set_t lhs_columns;
		auto &lhs_binding_list = left_binder.bind_context.GetBindingsList();
		for (auto &binding : lhs_binding_list) {
			for (auto &column_name : binding->GetColumnNames()) {
				lhs_columns.insert(column_name);
			}
		}
		// now bind the rhs
		for (auto &column_name : lhs_columns) {
			auto right_using_binding = right_binder.bind_context.GetUsingBinding(column_name);

			BindingAlias right_binding;
			// loop over the set of lhs columns, and figure out if there is a table in the rhs with the same name
			if (!right_using_binding) {
				if (!right_binder.TryFindBinding(column_name, "right", right_binding)) {
					// no match found for this column on the rhs: skip
					continue;
				}
			}
			extra_using_columns.push_back(column_name);
		}
		if (extra_using_columns.empty()) {
			// no matching bindings found in natural join: throw an exception
			string error_msg = "No columns found to join on in NATURAL JOIN.\n";
			error_msg += "Use CROSS JOIN if you intended for this to be a cross-product.";
			// gather all left/right candidates
			string left_candidates, right_candidates;
			auto &rhs_binding_list = right_binder.bind_context.GetBindingsList();
			for (auto &binding_ref : lhs_binding_list) {
				auto &binding = *binding_ref;
				for (auto &column_name : binding.GetColumnNames()) {
					if (!left_candidates.empty()) {
						left_candidates += ", ";
					}
					left_candidates += binding.GetAlias() + "." + column_name;
				}
			}
			for (auto &binding_ref : rhs_binding_list) {
				auto &binding = *binding_ref;
				for (auto &column_name : binding.GetColumnNames()) {
					if (!right_candidates.empty()) {
						right_candidates += ", ";
					}
					right_candidates += binding.GetAlias() + "." + column_name;
				}
			}
			error_msg += "\n   Left candidates: " + left_candidates;
			error_msg += "\n   Right candidates: " + right_candidates;
			throw BinderException(ref, error_msg);
		}
		break;
	}
	case JoinRefType::REGULAR:
	case JoinRefType::ASOF:
		if (!ref.using_columns.empty()) {
			// USING columns
			D_ASSERT(!result->condition);
			extra_using_columns = ref.using_columns;
		}
		break;

	case JoinRefType::CROSS:
	case JoinRefType::POSITIONAL:
	case JoinRefType::DEPENDENT:
		break;
	case JoinRefType::NEAREST:
		throw InternalException("NEAREST BY should have been rewritten by BindNearestJoin");
	}
	extra_using_columns = RemoveDuplicateUsingColumns(extra_using_columns);

	if (!extra_using_columns.empty()) {
		vector<optional_ptr<UsingColumnSet>> left_using_bindings;
		vector<optional_ptr<UsingColumnSet>> right_using_bindings;
		for (idx_t i = 0; i < extra_using_columns.size(); i++) {
			auto &using_column = extra_using_columns[i];
			// we check if there is ALREADY a using column of the same name in the left and right set
			// this can happen if we chain USING clauses
			// e.g. x JOIN y USING (c) JOIN z USING (c)
			auto left_using_binding = left_binder.bind_context.GetUsingBinding(using_column);
			auto right_using_binding = right_binder.bind_context.GetUsingBinding(using_column);
			if (!left_using_binding) {
				left_binder.bind_context.GetMatchingBinding(using_column);
			}
			if (!right_using_binding) {
				right_binder.bind_context.GetMatchingBinding(using_column);
			}
			left_using_bindings.push_back(left_using_binding);
			right_using_bindings.push_back(right_using_binding);
		}

		for (idx_t i = 0; i < extra_using_columns.size(); i++) {
			auto &using_column = extra_using_columns[i];
			BindingAlias left_binding;
			BindingAlias right_binding;

			auto set = make_uniq<UsingColumnSet>();
			auto &left_using_binding = left_using_bindings[i];
			auto &right_using_binding = right_using_bindings[i];
			left_binding = RetrieveUsingBinding(left_binder, left_using_binding, using_column, "left");
			right_binding = RetrieveUsingBinding(right_binder, right_using_binding, using_column, "right");

			// Last column of ASOF JOIN ... USING is >=
			const auto type = (ref.ref_type == JoinRefType::ASOF && i == extra_using_columns.size() - 1)
			                      ? ExpressionType::COMPARE_GREATERTHANOREQUALTO
			                      : ExpressionType::COMPARE_EQUAL;

			extra_conditions.push_back(
			    AddCondition(context, left_binder, right_binder, left_binding, right_binding, using_column, type));

			AddUsingBindings(*set, left_using_binding, left_binding);
			AddUsingBindings(*set, right_using_binding, right_binding);
			SetPrimaryBinding(*set, result->type, left_binding, right_binding);
			bind_context.TransferUsingBinding(left_binder.bind_context, left_using_binding, *set, using_column);
			bind_context.TransferUsingBinding(right_binder.bind_context, right_using_binding, *set, using_column);
			AddUsingBindingSet(std::move(set));
		}
	}

	// a positional join pads the shorter side with NULLs
	const bool full_outer = result->type == JoinType::OUTER || ref.ref_type == JoinRefType::POSITIONAL;
	if (result->type == JoinType::LEFT || full_outer) {
		for (auto &binding : right_binder.bind_context.GetBindingsList()) {
			binding->SetNullExtended();
		}
	}
	if (result->type == JoinType::RIGHT || full_outer) {
		for (auto &binding : left_binder.bind_context.GetBindingsList()) {
			binding->SetNullExtended();
		}
	}
	auto right_bindings = right_binder.bind_context.GetBindingAliases();
	auto left_bindings = left_binder.bind_context.GetBindingAliases();
	bind_context.AddContext(std::move(left_binder.bind_context));
	bind_context.AddContext(std::move(right_binder.bind_context));
	if (ref.condition) {
		WhereBinder condition_binder(*this, context);
		result->condition = condition_binder.Bind(ref.condition);
	}
	// AND the USING/NATURAL conditions onto the condition written in the query, in that order
	for (auto &condition : extra_conditions) {
		if (result->condition) {
			auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
			conjunction->GetChildrenMutable().push_back(std::move(result->condition));
			conjunction->GetChildrenMutable().push_back(std::move(condition));
			result->condition = std::move(conjunction);
		} else {
			result->condition = std::move(condition);
		}
	}

	// Update the correlated columns for the parent binder
	// For the left binder, depth >= 1 indicates correlations from the parent binder
	for (const auto &col : left_binder.correlated_columns) {
		if (col.depth >= 1) {
			AddCorrelatedColumn(col);
		}
	}
	// For the right binder, depth > 1 indicates correlations from the parent binder
	// (depth = 1 indicates correlations from the left side of the join)
	for (auto col : right_binder.correlated_columns) {
		if (col.depth > 1) {
			// Decrement the depth to account for the effect of the lateral binder
			col.depth--;
			AddCorrelatedColumn(col);
		}
	}

	if (result->type == JoinType::SEMI || result->type == JoinType::ANTI || result->type == JoinType::MARK) {
		bind_context.RemoveContext(right_bindings);
		if (result->type == JoinType::MARK) {
			auto mark_join_idx = GenerateTableIndex();
			string mark_join_alias = "__internal_mark_join_ref" + to_string(mark_join_idx.index);
			bind_context.AddGenericBinding(mark_join_idx, Identifier(mark_join_alias), {"__mark_join_marker"},
			                               {LogicalType::BOOLEAN});
			result->mark_index = mark_join_idx;
		}
	}
	if (result->type == JoinType::RIGHT_SEMI || result->type == JoinType::RIGHT_ANTI) {
		bind_context.RemoveContext(left_bindings);
	}

	BoundStatement result_stmt;
	result_stmt.types.insert(result_stmt.types.end(), result->left.types.begin(), result->left.types.end());
	result_stmt.types.insert(result_stmt.types.end(), result->right.types.begin(), result->right.types.end());
	result_stmt.names.insert(result_stmt.names.end(), result->left.names.begin(), result->left.names.end());
	result_stmt.names.insert(result_stmt.names.end(), result->right.names.begin(), result->right.names.end());
	result_stmt.plan = CreatePlan(*result);
	return result_stmt;
}

} // namespace duckdb
