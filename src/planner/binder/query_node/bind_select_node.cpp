#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception/parser_exception.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/aggregate/distributive_functions.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/main/config.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/expression/subquery_expression.hpp"
#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/tableref/joinref.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_expanded_expression.hpp"
#include "duckdb/planner/expression_binder/column_alias_binder.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"
#include "duckdb/planner/expression_binder/group_binder.hpp"
#include "duckdb/planner/expression_binder/having_binder.hpp"
#include "duckdb/planner/expression_binder/order_binder.hpp"
#include "duckdb/planner/expression_binder/qualify_binder.hpp"
#include "duckdb/planner/expression_binder/select_bind_state.hpp"
#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/planner/expression_binder/where_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/operator/logical_sample.hpp"
#include "duckdb/common/enums/dialect_compatibility_mode.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

unique_ptr<Expression> Binder::BindOrderExpression(OrderBinder &order_binder, unique_ptr<ParsedExpression> expr) {
	// we treat the distinct list as an ORDER BY
	auto bound_expr = order_binder.Bind(std::move(expr));
	if (!bound_expr) {
		// DISTINCT ON non-integer constant
		// remove the expression from the DISTINCT ON list
		return nullptr;
	}
	D_ASSERT(bound_expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT);
	return bound_expr;
}

BoundLimitNode Binder::BindLimitValue(OrderBinder &order_binder, unique_ptr<ParsedExpression> limit_val,
                                      LimitValueType value_type, bool is_offset) {
	auto new_binder = Binder::CreateBinder(context, this);
	ExpressionBinder expr_binder(*new_binder, context);
	bool is_percentage = value_type == LimitValueType::PERCENTAGE;
	auto target_type = is_percentage ? LogicalType::DOUBLE : LogicalType::BIGINT;
	expr_binder.target_type = target_type;
	auto original_limit = limit_val->Copy();
	auto expr = expr_binder.Bind(limit_val);
	if (expr->HasSubquery()) {
		if (!order_binder.HasExtraList()) {
			throw BinderException("Subquery in LIMIT/OFFSET not supported in set operation");
		}
		auto bound_limit = order_binder.CreateExtraReference(std::move(original_limit));
		if (is_percentage) {
			return BoundLimitNode::ExpressionPercentage(std::move(bound_limit));
		} else {
			return BoundLimitNode::ExpressionValue(std::move(bound_limit));
		}
	}
	if (expr->IsFoldable()) {
		//! this is a constant
		auto val = ExpressionExecutor::EvaluateScalar(context, *expr).CastAs(context, target_type);
		if (is_percentage) {
			D_ASSERT(!is_offset);
			double percentage_val;
			if (val.IsNull()) {
				percentage_val = 100.0;
			} else {
				percentage_val = val.GetValue<double>();
			}
			if (Value::IsNan(percentage_val) || percentage_val < 0 || percentage_val > 100) {
				throw OutOfRangeException("Limit percent out of range, should be between 0% and 100%");
			}
			return BoundLimitNode::ConstantPercentage(percentage_val);
		} else {
			int64_t constant_val;
			if (val.IsNull()) {
				constant_val = is_offset ? 0 : NumericLimits<int64_t>::Maximum();
			} else {
				constant_val = val.GetValue<int64_t>();
			}
			if (constant_val < 0) {
				throw BinderException(expr->GetQueryLocation(), "LIMIT/OFFSET cannot be negative");
			}
			return BoundLimitNode::ConstantValue(constant_val);
		}
	}
	if (!new_binder->correlated_columns.empty()) {
		throw BinderException("Correlated columns not supported in LIMIT/OFFSET");
	}
	// move any correlated columns to this binder
	MoveCorrelatedExpressions(*new_binder);
	if (is_percentage) {
		return BoundLimitNode::ExpressionPercentage(std::move(expr));
	} else {
		return BoundLimitNode::ExpressionValue(std::move(expr));
	}
}

duckdb::unique_ptr<BoundResultModifier> Binder::BindLimit(OrderBinder &order_binder, LimitModifier &limit_mod) {
	auto result = make_uniq<BoundLimitModifier>();
	if (limit_mod.limit) {
		result->limit_val = BindLimitValue(order_binder, std::move(limit_mod.limit), limit_mod.limit_type, false);
	}
	if (limit_mod.offset) {
		result->offset_val = BindLimitValue(order_binder, std::move(limit_mod.offset), LimitValueType::ROW_COUNT, true);
	}
	return std::move(result);
}

void Binder::PrepareModifiers(OrderBinder &order_binder, QueryNode &statement, BoundQueryNode &result) {
	for (auto &mod : statement.modifiers) {
		unique_ptr<BoundResultModifier> bound_modifier;
		switch (mod->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct = mod->Cast<DistinctModifier>();
			auto bound_distinct = make_uniq<BoundDistinctModifier>();
			bound_distinct->distinct_type =
			    distinct.distinct_on_targets.empty() ? DistinctType::DISTINCT : DistinctType::DISTINCT_ON;
			if (distinct.distinct_on_targets.empty()) {
				for (idx_t i = 0; i < result.names.size(); i++) {
					distinct.distinct_on_targets.push_back(ConstantExpression::Integer(NumericCast<int64_t>(1 + i)));
				}
			}
			order_binder.SetQueryComponent("DISTINCT ON");
			auto &order_binders = order_binder.GetBinders();
			for (auto &distinct_on_target : distinct.distinct_on_targets) {
				vector<unique_ptr<ParsedExpression>> target_list;
				order_binders[0].get().ExpandStarExpression(std::move(distinct_on_target), target_list);
				for (auto &target : target_list) {
					auto expr = BindOrderExpression(order_binder, std::move(target));
					if (!expr) {
						continue;
					}
					// Skip duplicates
					bool duplicate = false;
					for (auto &existing : bound_distinct->target_distincts) {
						if (expr->Equals(*existing)) {
							duplicate = true;
							break;
						}
					}
					if (!duplicate) {
						bound_distinct->target_distincts.push_back(std::move(expr));
					}
				}
			}
			order_binder.SetQueryComponent();

			bound_modifier = std::move(bound_distinct);
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order = mod->Cast<OrderModifier>();
			auto bound_order = make_uniq<BoundOrderModifier>();
			auto &config = DBConfig::GetConfig(context);
			D_ASSERT(!order.orders.empty());
			auto &order_binders = order_binder.GetBinders();
			if (order.orders.size() == 1 && order.orders[0].expression->GetExpressionType() == ExpressionType::STAR) {
				auto &star = order.orders[0].expression->Cast<StarExpression>();
				if (star.ExcludeList().empty() && star.ReplaceList().empty() && !star.Expression()) {
					// ORDER BY ALL
					// replace the order list with the all elements in the SELECT list
					auto order_type = config.ResolveOrder(context, order.orders[0].type);
					auto null_order = config.ResolveNullOrder(context, order_type, order.orders[0].null_order);
					auto constant_expr = make_uniq<BoundConstantExpression>(Value("ALL"));
					bound_order->orders.emplace_back(order_type, null_order, std::move(constant_expr));
					bound_modifier = std::move(bound_order);
					break;
				}
			}
			// Spark Compatibility Mode: when ALL is not reserved, ORDER BY ALL is parsed as a column reference.
			// If no column named "all" exists, treat as ORDER BY ALL.
			if (Settings::Get<DialectCompatibilityModeSetting>(context) == DialectCompatibilityMode::SPARK &&
			    order.orders.size() == 1 &&
			    order.orders[0].expression->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
				auto &colref = order.orders[0].expression->Cast<ColumnRefExpression>();
				if (colref.ColumnNames().size() == 1 &&
				    StringUtil::CIEquals(colref.ColumnNames()[0].GetIdentifierName(), "all")) {
					auto matching = bind_context.GetMatchingBindings("all");
					if (matching.empty()) {
						auto order_type = config.ResolveOrder(context, order.orders[0].type);
						auto null_order = config.ResolveNullOrder(context, order_type, order.orders[0].null_order);
						auto constant_expr = make_uniq<BoundConstantExpression>(Value("ALL"));
						bound_order->orders.emplace_back(order_type, null_order, std::move(constant_expr));
						bound_modifier = std::move(bound_order);
						break;
					}
				}
			}
			if (config.options.debug_order_verification == DebugOrderVerification::CREATE_SORT_KEY) {
				// When this verification is enabled, replace ORDER BY x, y with ORDER BY create_sort_key(x, y)
				// note that we don't enable this during actual verification since it doesn't always work
				// e.g. it breaks EXPLAIN output on queries
				bool can_replace = true;
				for (auto &order_node : order.orders) {
					if (order_node.expression->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
						// we cannot replace the sort key when we order by literals (e.g. ORDER BY 1, 2`
						can_replace = false;
						break;
					}
				}
				if (!order_binder.HasExtraList()) {
					// we can only do the replacement when we can order by elements that are not in the selection list
					can_replace = false;
				}
				if (can_replace) {
					vector<unique_ptr<ParsedExpression>> sort_key_parameters;
					for (auto &order_node : order.orders) {
						sort_key_parameters.push_back(std::move(order_node.expression));
						auto type = config.ResolveOrder(context, order_node.type);
						auto null_order = config.ResolveNullOrder(context, type, order_node.null_order);
						string sort_param = EnumUtil::ToString(type) + " " + EnumUtil::ToString(null_order);
						sort_key_parameters.push_back(ConstantExpression::String(sort_param));
					}
					order.orders.clear();
					auto create_sort_key =
					    make_uniq<FunctionExpression>("create_sort_key", std::move(sort_key_parameters));
					order.orders.emplace_back(OrderType::ASCENDING, OrderByNullType::NULLS_LAST,
					                          std::move(create_sort_key));
				}
			}
			for (auto &order_node : order.orders) {
				vector<unique_ptr<ParsedExpression>> order_list;
				order_binders[0].get().ExpandStarExpression(std::move(order_node.expression), order_list);

				auto type = config.ResolveOrder(context, order_node.type);
				auto null_order = config.ResolveNullOrder(context, type, order_node.null_order);
				for (auto &order_expr : order_list) {
					auto bound_expr = BindOrderExpression(order_binder, std::move(order_expr));
					if (!bound_expr) {
						continue;
					}
					bound_order->orders.emplace_back(type, null_order, std::move(bound_expr));
				}
			}
			if (!bound_order->orders.empty()) {
				bound_modifier = std::move(bound_order);
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER:
			bound_modifier = BindLimit(order_binder, mod->Cast<LimitModifier>());
			break;
		default:
			throw InternalException("Unsupported result modifier");
		}
		if (bound_modifier) {
			result.modifiers.push_back(std::move(bound_modifier));
		}
	}
}

static unique_ptr<Expression> CreateOrderExpression(unique_ptr<Expression> expr, const vector<Identifier> &names,
                                                    const vector<LogicalType> &sql_types, TableIndex table_index,
                                                    ProjectionIndex index) {
	if (index >= sql_types.size()) {
		throw BinderException(*expr, "ORDER term out of range - should be between 1 and %lld", sql_types.size());
	}
	Identifier alias;
	if (index < names.size()) {
		alias = names[index];
	} else {
		alias = expr->GetAlias();
	}
	auto result =
	    make_uniq<BoundColumnRefExpression>(std::move(alias), sql_types[index], ColumnBinding(table_index, index));
	return std::move(result);
}

static unique_ptr<Expression> FinalizeBindOrderExpression(unique_ptr<Expression> expr, TableIndex table_index,
                                                          const vector<Identifier> &names,
                                                          const vector<LogicalType> &sql_types,
                                                          const SelectBindState &bind_state) {
	auto &constant = expr->Cast<BoundConstantExpression>();
	switch (constant.GetValue().type().id()) {
	case LogicalTypeId::UBIGINT: {
		// index
		auto index = UBigIntValue::Get(constant.GetValue());
		return CreateOrderExpression(std::move(expr), names, sql_types, table_index, bind_state.GetFinalIndex(index));
	}
	case LogicalTypeId::VARCHAR: {
		// ORDER BY ALL
		return nullptr;
	}
	case LogicalTypeId::STRUCT: {
		// collation
		auto &struct_values = StructValue::GetChildren(constant.GetValue());
		if (struct_values.size() > 2) {
			throw InternalException("Expected one or two children: index and optional collation");
		}
		auto index = UBigIntValue::Get(struct_values[0]);
		string collation;
		if (struct_values.size() == 2) {
			collation = StringValue::Get(struct_values[1]);
		}
		auto result = CreateOrderExpression(std::move(expr), names, sql_types, table_index, ProjectionIndex(index));
		if (!collation.empty()) {
			if (sql_types[index].id() != LogicalTypeId::VARCHAR) {
				throw BinderException(*result, "COLLATE can only be applied to varchar columns");
			}
			result->SetReturnType(LogicalType::VARCHAR_COLLATION(std::move(collation)));
		}
		return result;
	}
	default:
		throw InternalException("Unknown type in FinalizeBindOrderExpression");
	}
}

static void AssignReturnType(unique_ptr<Expression> &expr, TableIndex table_index, const vector<Identifier> &names,
                             const vector<LogicalType> &sql_types, const SelectBindState &bind_state) {
	if (!expr) {
		return;
	}
	if (expr->GetExpressionType() == ExpressionType::VALUE_CONSTANT) {
		expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
	}
	if (expr->GetExpressionType() != ExpressionType::BOUND_COLUMN_REF) {
		return;
	}
	auto &bound_colref = expr->Cast<BoundColumnRefExpression>();
	bound_colref.SetReturnType(sql_types[bound_colref.Binding().column_index]);
}

void Binder::BindModifiers(BoundQueryNode &result, TableIndex table_index, const vector<Identifier> &names,
                           const vector<LogicalType> &sql_types, const SelectBindState &bind_state) {
	for (auto &bound_mod : result.modifiers) {
		switch (bound_mod->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct = bound_mod->Cast<BoundDistinctModifier>();
			// set types of distinct targets
			for (auto &expr : distinct.target_distincts) {
				expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
				if (!expr) {
					throw InternalException("DISTINCT ON ORDER BY ALL not supported");
				}
			}
			for (auto &expr : distinct.target_distincts) {
				ExpressionBinder::PushCollation(context, expr, expr->GetReturnType());
			}
			break;
		}
		case ResultModifierType::LIMIT_MODIFIER: {
			auto &limit = bound_mod->Cast<BoundLimitModifier>();
			AssignReturnType(limit.limit_val.GetExpression(), table_index, names, sql_types, bind_state);
			AssignReturnType(limit.offset_val.GetExpression(), table_index, names, sql_types, bind_state);
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order = bound_mod->Cast<BoundOrderModifier>();
			bool order_by_all = false;
			for (auto &order_node : order.orders) {
				auto &expr = order_node.expression;
				expr = FinalizeBindOrderExpression(std::move(expr), table_index, names, sql_types, bind_state);
				if (!expr) {
					order_by_all = true;
				}
			}
			if (order_by_all) {
				D_ASSERT(order.orders.size() == 1);
				auto order_type = order.orders[0].type;
				auto null_order = order.orders[0].null_order;
				order.orders.clear();
				for (idx_t i = 0; i < sql_types.size(); i++) {
					auto expr = make_uniq<BoundColumnRefExpression>(sql_types[i],
					                                                ColumnBinding(table_index, ProjectionIndex(i)));
					if (i < names.size()) {
						expr->SetAlias(names[i]);
					}
					order.orders.emplace_back(order_type, null_order, std::move(expr));
				}
			}
			auto &config = DBConfig::GetConfig(context);
			for (auto &order_node : order.orders) {
				auto &expr = order_node.expression;
				if (config.options.debug_order_verification == DebugOrderVerification::VARIANT &&
				    expr->GetReturnType().id() != LogicalTypeId::VARIANT) {
					// when this verification is enabled, cast every ORDER BY expression to VARIANT (the
					// PushCollation below then routes ordering through the VARIANT comparator) to verify it
					// produces the same ordering as the regular comparison. We do this on the *bound*
					// expression so alias / positional / GROUP BY ALL resolution is unaffected.
					order_node.expression = BoundCastExpression::AddCastToType(
					    context, std::move(order_node.expression), LogicalType::VARIANT());
				}
				ExpressionBinder::PushCollation(context, order_node.expression, order_node.expression->GetReturnType());
			}
			break;
		}
		default:
			break;
		}
	}
}

//! Returns a first(x) aggregate that selects the uncollated value x of a collated group
static unique_ptr<Expression> BindUncollatedFirst(Binder &binder, unique_ptr<Expression> uncollated_expr) {
	auto first_fun = GetBuiltinAggregateFunction(binder.context, FirstFun::Name, {uncollated_expr->GetReturnType()});
	vector<unique_ptr<Expression>> first_children;
	first_children.push_back(std::move(uncollated_expr));
	FunctionBinder function_binder(binder);
	auto function = function_binder.BindAggregateFunction(first_fun, std::move(first_children));
	function->SetAlias("__collated_group");
	return std::move(function);
}

BoundStatement Binder::BindNode(SelectNode &statement) {
	D_ASSERT(statement.from_table);

	// first bind the FROM table statement
	auto from = std::move(statement.from_table);
	auto from_table = Bind(*from);
	return BindSelectNode(statement, std::move(from_table));
}

void Binder::BindWhereStarExpression(unique_ptr<ParsedExpression> &expr) {
	// expand any expressions in the upper AND recursively
	if (expr->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
		auto &conj = expr->Cast<ConjunctionExpression>();
		for (auto &child : conj.GetChildrenMutable()) {
			BindWhereStarExpression(child);
		}
		return;
	}
	if (expr->GetExpressionType() == ExpressionType::STAR) {
		auto &star = expr->Cast<StarExpression>();
		if (!star.IsColumns()) {
			throw ParserException("STAR expression is not allowed in the WHERE clause. Use COLUMNS(*) instead.");
		}
	}
	// expand the stars for this expression
	vector<unique_ptr<ParsedExpression>> new_conditions;
	ExpandStarExpression(std::move(expr), new_conditions);
	if (new_conditions.empty()) {
		throw ParserException("COLUMNS expansion resulted in empty set of columns");
	}

	// set up an AND conjunction between the expanded conditions
	expr = std::move(new_conditions[0]);
	for (idx_t i = 1; i < new_conditions.size(); i++) {
		auto and_conj = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(expr),
		                                                 std::move(new_conditions[i]));
		expr = std::move(and_conj);
	}
}

void Binder::NormalizeFilterStarExpression(ParsedExpression &expr) {
	if (expr.GetExpressionClass() == ExpressionClass::FUNCTION) {
		auto &function = expr.Cast<FunctionExpression>();
		if (function.Filter()) {
			BindWhereStarExpression(function.FilterMutable());
		}
	} else if (expr.GetExpressionClass() == ExpressionClass::WINDOW) {
		auto &window = expr.Cast<WindowExpression>();
		if (window.Filter()) {
			BindWhereStarExpression(window.FilterMutable());
		}
	}

	ParsedExpressionIterator::EnumerateChildren(expr,
	                                            [&](ParsedExpression &child) { NormalizeFilterStarExpression(child); });
}

void Binder::NormalizeFilterStarExpressions(SelectNode &statement) {
	for (auto &select_element : statement.select_list) {
		NormalizeFilterStarExpression(*select_element);
	}

	for (auto &modifier : statement.modifiers) {
		switch (modifier->type) {
		case ResultModifierType::DISTINCT_MODIFIER: {
			auto &distinct = modifier->Cast<DistinctModifier>();
			for (auto &target : distinct.distinct_on_targets) {
				NormalizeFilterStarExpression(*target);
			}
			break;
		}
		case ResultModifierType::ORDER_MODIFIER: {
			auto &order = modifier->Cast<OrderModifier>();
			for (auto &order_node : order.orders) {
				NormalizeFilterStarExpression(*order_node.expression);
			}
			break;
		}
		default:
			break;
		}
	}

	if (statement.having) {
		NormalizeFilterStarExpression(*statement.having);
	}
	if (statement.qualify) {
		NormalizeFilterStarExpression(*statement.qualify);
	}
}

Identifier Binder::GetExpressionName(const ParsedExpression &expr) {
	if (!expr.GetAlias().empty()) {
		return expr.GetAlias();
	}
	return expr.GetName();
}

BoundStatement Binder::BindSelectNode(SelectNode &statement, BoundStatement from_table) {
	D_ASSERT(from_table.plan);
	D_ASSERT(!statement.from_table);
	auto result_ptr = make_uniq<BoundSelectNode>();
	auto &result = *result_ptr;
	result.projection_index = GenerateTableIndex();
	result.group_index = GenerateTableIndex();
	result.aggregate_index = GenerateTableIndex();
	result.groupings_index = GenerateTableIndex();
	result.window_index = GenerateTableIndex();
	result.prune_index = GenerateTableIndex();

	result.from_table = std::move(from_table);
	// bind the sample clause
	if (statement.sample) {
		result.from_table.plan =
		    make_uniq<LogicalSample>(std::move(statement.sample), std::move(result.from_table.plan));
	}

	// do this before column expansion to preserve FILTER semantics,
	// without normalization the enclosing expression would be duplicated once per column
	NormalizeFilterStarExpressions(statement);

	// visit the select list and expand any "*" statements
	vector<unique_ptr<ParsedExpression>> new_select_list;
	ExpandStarExpressions(statement.select_list, new_select_list);

	if (new_select_list.empty()) {
		throw BinderException("SELECT list is empty after resolving * expressions!");
	}
	statement.select_list = std::move(new_select_list);

	auto &bind_state = result.bind_state;
	for (idx_t i = 0; i < statement.select_list.size(); i++) {
		auto &expr = statement.select_list[i];
		result.names.emplace_back(GetExpressionName(*expr));
		ExpressionBinder::QualifyColumnNames(*this, expr);
		if (!expr->GetAlias().empty()) {
			bind_state.alias_map[expr->GetAlias()] = i;
			result.names[i] = expr->GetAlias();
		}
		bind_state.projection_map[*expr] = i;
		bind_state.original_expressions.push_back(expr->Copy());
	}
	result.column_count = statement.select_list.size();

	if (statement.where_clause) {
		// bind any star expressions in the WHERE clause
		BindWhereStarExpression(statement.where_clause);
	}
	for (idx_t i = 0; i < statement.groups.group_expressions.size(); i++) {
		auto &grp = statement.groups.group_expressions[i];
		ExpressionBinder::QualifyColumnNames(*this, grp);

		GroupBinder::ReplaceSelectRef(statement, bind_state, ProjectionIndex(i), grp);

		// set up a mapping of expression -> group index so we can map expressions in the select list / having to groups
		auto grp_copy = grp->Copy();
		bind_state.group_map[*grp_copy] = ProjectionIndex(i);
		if (grp_copy->GetExpressionClass() != ExpressionClass::COLUMN_REF) {
			bind_state.has_compound_groups = true;
		}
		bind_state.unbound_groups.push_back(std::move(grp_copy));
	}

	if (statement.qualify) {
		ExpressionBinder::QualifyColumnNames(*this, statement.qualify);
	}

	// prepare binding of all the result modifiers; including DISTINCT and ORDER BY targets
	OrderBinder order_binder({*this}, statement, bind_state);
	PrepareModifiers(order_binder, statement, result);

	// first visit the WHERE clause
	// the WHERE clause happens before the GROUP BY, PROJECTION or HAVING clauses
	if (statement.where_clause) {
		ColumnAliasBinder alias_binder(bind_state);
		WhereBinder where_binder(*this, context, alias_binder);
		unique_ptr<ParsedExpression> condition = std::move(statement.where_clause);
		auto where_clause = where_binder.Bind(condition);
		result.from_table.plan = PlanFilter(std::move(where_clause), std::move(result.from_table.plan));
	}

	auto &group_expressions = statement.groups.group_expressions;

	// Spark Compatibility Mode: when ALL is not a reserved keyword, GROUP BY ALL is parsed as a column reference
	// instead of the special GROUP BY ALL syntax. Detect this and convert to FORCE_AGGREGATES
	// if no column named "all" actually exists in scope.
	if (Settings::Get<DialectCompatibilityModeSetting>(context) == DialectCompatibilityMode::SPARK &&
	    group_expressions.size() == 1 && group_expressions[0]->GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		auto &colref = group_expressions[0]->Cast<ColumnRefExpression>();
		if (colref.ColumnNames().size() == 1 &&
		    StringUtil::CIEquals(colref.ColumnNames()[0].GetIdentifierName(), "all")) {
			auto matching = bind_context.GetMatchingBindings("all");
			if (matching.empty()) {
				statement.aggregate_handling = AggregateHandling::FORCE_AGGREGATES;
				group_expressions.clear();
				statement.groups.grouping_sets.clear();
			}
		}
	}

	if (!group_expressions.empty()) {
		// the statement has a GROUP BY clause, bind it
		GroupBinder group_binder(*this, context, result, bind_state);
		// Allow NULL constants in GROUP BY to maintain their SQLNULL type
		auto prev_can_contain_nulls = CanContainNulls();
		SetCanContainNulls(true);
		for (idx_t i = 0; i < group_expressions.size(); i++) {
			// bind the groups
			LogicalType group_type;
			// bound subqueries cannot be copied, so keep the parsed expression in case we need to bind it again
			auto parsed_copy = group_expressions[i]->Copy();
			auto bound_expr = group_binder.Bind(group_expressions[i], &group_type);
			if (bound_expr->GetExpressionType() == ExpressionType::BOUND_EXPANDED) {
				throw BinderException("UNNEST of struct cannot be used in GROUP BY clause");
			}
			D_ASSERT(bound_expr->GetReturnType().id() != LogicalTypeId::INVALID);

			// the group and the first(x) aggregate below are separate expression trees, so we need our own
			// copy - it must be made before pushing the collation, which can replace the expression
			bool contains_subquery = bound_expr->HasSubquery();
			unique_ptr<Expression> uncollated_expr = contains_subquery ? nullptr : bound_expr->Copy();

			// push a potential collation, if necessary
			bool requires_collation = ExpressionBinder::PushCollation(context, bound_expr, group_type);
			if (requires_collation && contains_subquery) {
				uncollated_expr = group_binder.Bind(parsed_copy);
			}
			if (requires_collation) {
				// if there is a collation on a group x, we should group by the collated expr,
				// but also push a first(x) aggregate in case x is selected (uncollated)
				auto uncollated_first = BindUncollatedFirst(*this, std::move(uncollated_expr));
				auto collated_idx = ColumnBinding::PushExpression(result.aggregates, std::move(uncollated_first));
				bind_state.collated_groups[ProjectionIndex(i)] = collated_idx;
			}
			result.groups.group_expressions.push_back(std::move(bound_expr));
		}
		SetCanContainNulls(prev_can_contain_nulls);
	}
	result.groups.grouping_sets = std::move(statement.groups.grouping_sets);

	// bind the QUALIFY clause, if any
	vector<BoundColumnReferenceInfo> bound_qualify_columns;
	if (statement.qualify) {
		if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
			throw BinderException("Combining QUALIFY with GROUP BY ALL is not supported yet");
		}
		QualifyBinder qualify_binder(*this, context, result);
		result.qualify = qualify_binder.Bind(statement.qualify);
		if (qualify_binder.HasBoundColumns()) {
			if (qualify_binder.BoundAggregates()) {
				throw BinderException("Cannot mix aggregates with non-aggregated columns!");
			}
			bound_qualify_columns = qualify_binder.GetBoundColumns();
		}
	}

	// after that, we bind to the SELECT list
	SelectBinder select_binder(*this, context, result);

	// if we expand select-list expressions, e.g., via UNNEST, then we need to possibly
	// adjust the column index of the already bound ORDER BY modifiers, and not only set their types
	vector<idx_t> group_by_all_indexes;
	vector<Identifier> new_names;
	vector<LogicalType> internal_sql_types;

	for (idx_t i = 0; i < statement.select_list.size(); i++) {
		bool is_window = statement.select_list[i]->IsWindow();
		idx_t unnest_count = result.unnests.SelectList().size();
		LogicalType result_type;
		// the select item of a TRY subquery transports the error flags of nested scalar subqueries to
		// the TRY lowering of this node; GROUP BY ALL may move the item into the group expressions,
		// where the flag would have no consumer, so such items bind as plain subqueries
		select_binder.SetTransportTryErrors(i == 0 && IsBindingTrySubquery() &&
		                                    statement.aggregate_handling != AggregateHandling::FORCE_AGGREGATES);
		auto expr = select_binder.Bind(statement.select_list[i], &result_type, true);
		select_binder.SetTransportTryErrors(false);
		bool is_original_column = i < result.column_count;
		bool can_group_by_all =
		    statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES && is_original_column;
		result.bound_column_count++;

		if (expr->GetExpressionType() == ExpressionType::BOUND_EXPANDED) {
			if (!is_original_column) {
				throw BinderException("UNNEST of struct cannot be used in ORDER BY/DISTINCT ON clause");
			}
			if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
				throw BinderException("UNNEST of struct cannot be combined with GROUP BY ALL");
			}

			auto &expanded = expr->Cast<BoundExpandedExpression>();
			auto &struct_expressions = expanded.GetChildrenMutable();
			if (struct_expressions.empty()) {
				throw BinderException("UNNEST of an empty struct is not supported");
			}

			for (auto &struct_expr : struct_expressions) {
				new_names.emplace_back(struct_expr->GetName());
				result.types.push_back(struct_expr->GetReturnType());
				internal_sql_types.push_back(struct_expr->GetReturnType());
				result.select_list.push_back(std::move(struct_expr));
			}
			bind_state.AddExpandedColumn(struct_expressions.size());
			continue;
		}

		if (expr->IsVolatile()) {
			bind_state.SetExpressionIsVolatile(i);
		}
		if (expr->HasSubquery()) {
			bind_state.SetExpressionHasSubquery(i);
		}
		bind_state.AddRegularColumn();

		if (can_group_by_all && result.unnests.SelectList().size() > unnest_count) {
			throw BinderException("Cannot group on an UNNEST or UNLIST clause");
		}
		if (can_group_by_all && select_binder.HasBoundColumns()) {
			if (select_binder.BoundAggregates()) {
				throw BinderException("Cannot mix aggregates with non-aggregated columns!");
			}
			if (is_window) {
				// a window over aggregates (e.g. sum(sum(x)) OVER ()) is fine, a window over a group is not
				throw BinderException("Cannot group on a window clause");
			}
			// we are forcing aggregates, and the node has columns bound
			// this entry becomes a group
			group_by_all_indexes.push_back(i);
		}

		result.select_list.push_back(std::move(expr));
		if (is_original_column) {
			new_names.push_back(std::move(result.names[i]));
			result.types.push_back(result_type);
		}
		internal_sql_types.push_back(result_type);

		if (can_group_by_all) {
			select_binder.ResetBindings();
		}
	}

	// push the GROUP BY ALL expressions into the group set
	vector<std::pair<idx_t, ProjectionIndex>> group_by_all_groups;
	for (auto &group_by_all_index : group_by_all_indexes) {
		auto &expr = result.select_list[group_by_all_index];
		auto return_type = expr->GetReturnType();
		// group by the collated expression, and select the uncollated value through a first(x) aggregate
		unique_ptr<Expression> uncollated_first;
		// avoid evaluating volatile groups twice
		if (!expr->IsVolatile()) {
			// copy before pushing the collation, which can replace the expression
			bool contains_subquery = expr->HasSubquery();
			auto uncollated_expr = contains_subquery ? nullptr : expr->Copy();
			if (ExpressionBinder::PushCollation(context, expr, return_type)) {
				if (contains_subquery) {
					// bound subqueries cannot be copied, so bind the select-list expression again
					auto parsed_copy = bind_state.original_expressions[group_by_all_index]->Copy();
					auto bound_column_count = select_binder.GetBoundColumns().size();
					uncollated_expr = select_binder.Bind(parsed_copy);
					select_binder.TruncateBoundColumns(bound_column_count);
				}
				uncollated_first = BindUncollatedFirst(*this, std::move(uncollated_expr));
			}
		}
		auto group_proj_idx = ColumnBinding::PushExpression(result.groups.group_expressions, std::move(expr));
		if (uncollated_first) {
			auto aggregate_idx = ColumnBinding::PushExpression(result.aggregates, std::move(uncollated_first));
			expr =
			    make_uniq<BoundColumnRefExpression>(return_type, ColumnBinding(result.aggregate_index, aggregate_idx));
			bind_state.collated_groups[group_proj_idx] = aggregate_idx;
		} else {
			expr = make_uniq<BoundColumnRefExpression>(return_type, ColumnBinding(result.group_index, group_proj_idx));
		}
		group_by_all_groups.emplace_back(group_by_all_index, group_proj_idx);
	}
	// map the select expressions to their groups, so HAVING binds to them
	// this happens after all select expressions are bound, since binding them again must not resolve to a group
	for (auto &entry : group_by_all_groups) {
		auto group_copy = bind_state.original_expressions[entry.first]->Copy();
		bind_state.group_map[*group_copy] = entry.second;
		bind_state.unbound_groups.push_back(std::move(group_copy));
	}

	// bind the HAVING clause, if any, after the SELECT list and its groups
	if (statement.having) {
		HavingBinder having_binder(*this, context, result, statement.aggregate_handling);
		ExpressionBinder::QualifyColumnNames(having_binder, statement.having);
		result.having = having_binder.Bind(statement.having);
	}
	set<ProjectionIndex> group_by_all_indexes_set;
	if (!group_by_all_indexes.empty()) {
		idx_t num_set_indexes = result.groups.group_expressions.size();
		for (idx_t i = 0; i < num_set_indexes; i++) {
			group_by_all_indexes_set.emplace(i);
		}
		D_ASSERT(result.groups.grouping_sets.empty());
		result.groups.grouping_sets.push_back(group_by_all_indexes_set);
	}
	result.column_count = new_names.size();
	result.names = std::move(new_names);
	result.need_prune = result.select_list.size() > result.column_count;

	// in the normal select binder, we bind columns as if there is no aggregation
	// i.e. in the query [SELECT i, SUM(i) FROM integers;] the "i" will be bound as a normal column
	// since we have an aggregation, we need to either (1) throw an error, or (2) wrap the column in a FIRST() aggregate
	// we choose the former one [CONTROVERSIAL: this is the PostgreSQL behavior]
	if (!result.groups.group_expressions.empty() || !result.aggregates.empty() || statement.having ||
	    !result.groups.grouping_sets.empty()) {
		if (statement.aggregate_handling == AggregateHandling::NO_AGGREGATES_ALLOWED) {
			throw BinderException("Aggregates cannot be present in a Project relation!");
		} else {
			vector<BoundColumnReferenceInfo> bound_columns;
			if (select_binder.HasBoundColumns()) {
				bound_columns = select_binder.GetBoundColumns();
			}
			for (auto &bound_qualify_col : bound_qualify_columns) {
				bound_columns.push_back(bound_qualify_col);
			}
			if (!bound_columns.empty()) {
				string error;
				error = "column %s must appear in the GROUP BY clause or must be part of an aggregate function.";
				if (statement.aggregate_handling == AggregateHandling::FORCE_AGGREGATES) {
					error += "\nGROUP BY ALL will only group entries in the SELECT list. Add it to the SELECT list or "
					         "GROUP BY this entry explicitly.";
					throw BinderException(bound_columns[0].query_location, error, bound_columns[0].name);
				} else {
					error += "\nEither add it to the GROUP BY list, or use ANY_VALUE(%s) if the exact value of %s "
					         "is not important.";
					throw BinderException(bound_columns[0].query_location, error, bound_columns[0].name,
					                      bound_columns[0].name, bound_columns[0].name);
				}
			}
		}
	}

	// QUALIFY clause requires at least one window function to be specified in at least one of the SELECT column list or
	// the filter predicate of the QUALIFY clause
	if (statement.qualify && result.windows.empty()) {
		throw BinderException("at least one window function must appear in the SELECT column or QUALIFY clause");
	}

	// now that the SELECT list is bound, we set the types of DISTINCT/ORDER BY expressions
	BindModifiers(result, result.projection_index, result.names, internal_sql_types, bind_state);

	BoundStatement result_statement;
	result_statement.types = result.types;
	result_statement.names = result.names;
	result_statement.plan = CreatePlan(result);
	result_statement.extra_info.original_expressions = std::move(result.bind_state.original_expressions);
	return result_statement;
}

} // namespace duckdb
