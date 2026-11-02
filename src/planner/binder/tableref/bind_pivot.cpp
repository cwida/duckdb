#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/pivotref.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/planner/tableref/bound_pivotref.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_pivot.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression_binder/constant_binder.hpp"

namespace duckdb {

static void ConstructPivots(PivotRef &ref, vector<PivotValueElement> &pivot_values, idx_t pivot_idx = 0,
                            const PivotValueElement &current_value = PivotValueElement()) {
	auto &pivot = ref.pivots[pivot_idx];
	bool last_pivot = pivot_idx + 1 == ref.pivots.size();
	for (auto &entry : pivot.entries) {
		PivotValueElement new_value = current_value;
		string name = entry.alias.GetIdentifierName();
		D_ASSERT(entry.values.size() == pivot.pivot_expressions.size());
		for (idx_t v = 0; v < entry.values.size(); v++) {
			auto &value = entry.values[v];
			new_value.values.push_back(value);
			if (entry.alias.empty()) {
				if (name.empty()) {
					name = value.ToString();
				} else {
					name += "_" + value.ToString();
				}
			}
		}
		if (!current_value.name.empty()) {
			new_value.name = current_value.name + "_" + name;
		} else {
			new_value.name = std::move(name);
		}
		if (new_value.name.empty()) {
			// the pivot value is an empty string - column names cannot be empty
			new_value.name = "''";
		}
		if (last_pivot) {
			pivot_values.push_back(std::move(new_value));
		} else {
			// need to recurse
			ConstructPivots(ref, pivot_values, pivot_idx + 1, new_value);
		}
	}
}

static void ExtractPivotExpressions(ParsedExpression &root_expr, identifier_set_t &handled_columns,
                                    optional_ptr<DummyBinding> macro_binding) {
	ParsedExpressionIterator::VisitExpression<ColumnRefExpression>(
	    root_expr, [&](const ColumnRefExpression &child_colref) {
		    if (child_colref.IsQualified()) {
			    if (child_colref.ColumnNames()[0].StartsWith(DummyBinding::DUMMY_NAME) && macro_binding &&
			        macro_binding->HasMatchingBinding(Identifier(child_colref.GetColumnName()))) {
				    throw ParameterNotResolvedException();
			    }
			    throw BinderException(child_colref, "PIVOT expression cannot contain qualified columns");
		    }
		    handled_columns.insert(child_colref.GetColumnName());
	    });
}

struct ExtractPivotAggregateOperator {
	using TYPE = vector<reference<FunctionExpression>>;

	static void HandleAggregate(unique_ptr<ParsedExpression> &expr, FunctionExpression &aggr_function,
	                            TYPE &aggregates) {
		aggregates.push_back(aggr_function);
	}
};

struct ReplacePivotAggregateOperator {
	using TYPE = unique_ptr<ParsedExpression>;

	static void HandleAggregate(unique_ptr<ParsedExpression> &expr, FunctionExpression &aggr_function,
	                            TYPE &replacement_expr) {
		if (replacement_expr->GetExpressionType() != ExpressionType::COLUMN_REF) {
			throw BinderException(*expr, "Pivot expression can only have one aggregate");
		}
		auto aggr = std::move(expr);
		expr = std::move(replacement_expr);
		replacement_expr = std::move(aggr);
	}
};

template <class OP>
void TemplatedHandlePivotAggregate(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                   typename OP::TYPE &aggregates) {
	if (expr->GetExpressionType() == ExpressionType::FUNCTION) {
		auto &aggr_function = expr->Cast<FunctionExpression>();

		// check if this is an aggregate to ensure it is an aggregate and not a scalar function
		EntryLookupInfo lookup_info(CatalogType::AGGREGATE_FUNCTION_ENTRY, aggr_function.GetQualifiedName(), *expr);
		auto &entry = Catalog::GetEntry(context, lookup_info);
		if (entry.type == CatalogType::AGGREGATE_FUNCTION_ENTRY) {
			// aggregate
			OP::HandleAggregate(expr, aggr_function, aggregates);
			return;
		}
	}
	if (expr->GetExpressionType() == ExpressionType::COLUMN_REF) {
		throw BinderException(*expr, "Columns can only be referenced within the aggregate of a PIVOT expression");
	}
	ParsedExpressionIterator::EnumerateChildren(*expr, [&](unique_ptr<ParsedExpression> &child) {
		TemplatedHandlePivotAggregate<OP>(context, child, aggregates);
	});
}

void ExtractPivotAggregateExpression(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                     vector<reference<FunctionExpression>> &aggregates) {
	TemplatedHandlePivotAggregate<ExtractPivotAggregateOperator>(context, expr, aggregates);
}

void ReplacePivotAggregateExpression(ClientContext &context, unique_ptr<ParsedExpression> &expr,
                                     unique_ptr<ParsedExpression> &replacement_expr) {
	TemplatedHandlePivotAggregate<ReplacePivotAggregateOperator>(context, expr, replacement_expr);
}

static unique_ptr<SelectNode> ConstructInitialGrouping(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns,
                                                       const identifier_set_t &handled_columns) {
	auto subquery = make_uniq<SelectNode>();
	subquery->from_table = std::move(ref.source);
	if (ref.groups.empty()) {
		// if rows are not specified any columns that are not pivoted/aggregated on are added to the GROUP BY clause
		for (auto &entry : all_columns) {
			auto column_entry = Binder::GetResolvedColumnExpression(*entry);
			if (!column_entry) {
				throw InternalException("Unexpected child of pivot source - not a ColumnRef");
			}
			auto &columnref = column_entry->Cast<ColumnRefExpression>();
			if (handled_columns.find(columnref.GetColumnName()) == handled_columns.end()) {
				// not handled - add to grouping set
				subquery->groups.group_expressions.push_back(
				    ConstantExpression::Integer(NumericCast<int64_t>(subquery->select_list.size() + 1)));
				subquery->select_list.push_back(make_uniq<ColumnRefExpression>(columnref.GetColumnName()));
			}
		}
	} else {
		// if rows are specified only the columns mentioned in rows are added as groups
		for (auto &row : ref.groups) {
			subquery->groups.group_expressions.push_back(
			    ConstantExpression::Integer(NumericCast<int64_t>(subquery->select_list.size() + 1)));
			subquery->select_list.push_back(make_uniq<ColumnRefExpression>(row));
		}
	}
	return subquery;
}

static unique_ptr<SelectNode> PivotFilteredAggregate(ClientContext &context, PivotRef &ref,
                                                     vector<unique_ptr<ParsedExpression>> all_columns,
                                                     const identifier_set_t &handled_columns,
                                                     vector<PivotValueElement> pivot_values) {
	auto subquery = ConstructInitialGrouping(ref, std::move(all_columns), handled_columns);

	// push the filtered aggregates
	for (auto &pivot_value : pivot_values) {
		unique_ptr<ParsedExpression> filter;
		idx_t pivot_value_idx = 0;
		for (auto &pivot_column : ref.pivots) {
			for (auto &pivot_expr : pivot_column.pivot_expressions) {
				auto column_ref = make_uniq<CastExpression>(LogicalType::VARCHAR, pivot_expr->Copy());
				auto constant_value = ConstantExpression::FromValue(
				    pivot_value.values[pivot_value_idx++].DefaultCastAs(LogicalType::VARCHAR));
				auto comp_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_NOT_DISTINCT_FROM,
				                                                 std::move(column_ref), std::move(constant_value));
				if (filter) {
					filter = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(filter),
					                                          std::move(comp_expr));
				} else {
					filter = std::move(comp_expr);
				}
			}
		}
		for (auto &aggregate : ref.aggregates) {
			auto copied_aggr = aggregate->Copy();

			vector<reference<FunctionExpression>> aggregates;
			ExtractPivotAggregateExpression(context, copied_aggr, aggregates);
			D_ASSERT(aggregates.size() == 1);

			auto &aggr = aggregates[0].get().Cast<FunctionExpression>();
			if (aggr.FilterMutable()) {
				// keep the FILTER of the aggregate - only the rows that match both filters are aggregated
				aggr.FilterMutable() = make_uniq<ConjunctionExpression>(
				    ExpressionType::CONJUNCTION_AND, std::move(aggr.FilterMutable()), filter->Copy());
			} else {
				aggr.FilterMutable() = filter->Copy();
			}
			auto &aggr_name = aggregate->GetAlias();
			auto name = pivot_value.name;
			if (ref.aggregates.size() > 1 || !aggr_name.empty()) {
				// if there are multiple aggregates specified we add the name of the aggregate as well
				name += "_" +
				        (aggr_name.empty() ? aggregate->GetName().GetIdentifierName() : aggr_name.GetIdentifierName());
			}
			copied_aggr->SetAlias(Identifier(name));
			subquery->select_list.push_back(std::move(copied_aggr));
		}
	}
	return subquery;
}

struct PivotBindState {
	vector<Identifier> internal_group_names;
	vector<Identifier> group_names;
	vector<Identifier> aggregate_names;
	vector<Identifier> internal_aggregate_names;
};

static unique_ptr<SelectNode> PivotInitialAggregate(ClientContext &context, PivotBindState &bind_state, PivotRef &ref,
                                                    vector<unique_ptr<ParsedExpression>> all_columns,
                                                    const identifier_set_t &handled_columns) {
	auto subquery_stage1 = ConstructInitialGrouping(ref, std::move(all_columns), handled_columns);

	idx_t group_count = 0;
	for (auto &expr : subquery_stage1->select_list) {
		bind_state.group_names.push_back(expr->GetName());
		if (expr->GetAlias().empty()) {
			expr->SetAlias(Identifier("__internal_pivot_group" + std::to_string(++group_count)));
		}
		bind_state.internal_group_names.emplace_back(expr->GetAlias());
	}
	// group by all of the pivot values
	idx_t pivot_count = 0;
	for (auto &pivot_column : ref.pivots) {
		for (auto &pivot_expr : pivot_column.pivot_expressions) {
			if (pivot_expr->GetAlias().empty()) {
				pivot_expr->SetAlias(Identifier("__internal_pivot_ref" + std::to_string(++pivot_count)));
			}
			auto pivot_alias = pivot_expr->GetAlias();
			subquery_stage1->groups.group_expressions.push_back(
			    ConstantExpression::Integer(NumericCast<int64_t>(subquery_stage1->select_list.size() + 1)));
			subquery_stage1->select_list.push_back(std::move(pivot_expr));
			pivot_expr = make_uniq<ColumnRefExpression>(pivot_alias);
		}
	}
	idx_t aggregate_count = 0;
	// finally add the aggregates
	for (auto &aggregate : ref.aggregates) {
		auto aggregate_alias = "__internal_pivot_aggregate" + std::to_string(++aggregate_count);
		auto aggr_name = aggregate->GetAlias();
		if (aggr_name.empty() && ref.aggregates.size() > 1) {
			aggr_name = Identifier(aggregate->ToString());
		}
		if (!aggr_name.empty()) {
			aggr_name = Identifier("_" + aggr_name);
		}

		unique_ptr<ParsedExpression> aggregate_ref;
		aggregate_ref = make_uniq<ColumnRefExpression>(Identifier(aggregate_alias));
		ReplacePivotAggregateExpression(context, aggregate, aggregate_ref);

		bind_state.aggregate_names.emplace_back(std::move(aggr_name));
		bind_state.internal_aggregate_names.push_back(Identifier(aggregate_alias));
		aggregate_ref->SetAlias(Identifier(std::move(aggregate_alias)));
		subquery_stage1->select_list.push_back(std::move(aggregate_ref));
	}
	return subquery_stage1;
}

//! The key of a pivot value: the length of the value, followed by the value - or "N" for NULL values.
//! Prefixing the length makes the concatenated keys of multiple pivot values unambiguous.
static string PivotValueKey(const Value &value) {
	if (value.IsNull()) {
		return "N";
	}
	auto str = value.ToString();
	return to_string(str.size()) + ":" + str;
}

unique_ptr<ParsedExpression> ConstructPivotExpression(unique_ptr<ParsedExpression> pivot_expr) {
	// coalesce(strlen(pivot::VARCHAR)::VARCHAR || ':' || pivot::VARCHAR, 'N'), see PivotValueKey
	auto cast = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(pivot_expr));
	vector<unique_ptr<ParsedExpression>> strlen_children;
	strlen_children.push_back(cast->Copy());
	auto length = make_uniq<CastExpression>(LogicalType::VARCHAR,
	                                        make_uniq<FunctionExpression>("strlen", std::move(strlen_children)));
	vector<unique_ptr<ParsedExpression>> prefix_children;
	prefix_children.push_back(std::move(length));
	prefix_children.push_back(ConstantExpression::String(":"));
	auto prefix = make_uniq<FunctionExpression>("||", std::move(prefix_children));
	vector<unique_ptr<ParsedExpression>> key_children;
	key_children.push_back(std::move(prefix));
	key_children.push_back(std::move(cast));
	auto key = make_uniq<FunctionExpression>("||", std::move(key_children));

	vector<unique_ptr<ParsedExpression>> coalesce_children;
	coalesce_children.push_back(std::move(key));
	coalesce_children.push_back(ConstantExpression::String("N"));
	auto coalesce = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_COALESCE, std::move(coalesce_children));
	return std::move(coalesce);
}

static unique_ptr<SelectNode> PivotListAggregate(PivotBindState &bind_state, PivotRef &ref,
                                                 unique_ptr<SelectNode> subquery_stage1) {
	auto subquery_stage2 = make_uniq<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_uniq<SelectStatement>();
	subquery_select->node = std::move(subquery_stage1);
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(subquery_select));

	// add all of the groups
	for (idx_t gr = 0; gr < bind_state.internal_group_names.size(); gr++) {
		subquery_stage2->groups.group_expressions.push_back(
		    ConstantExpression::Integer(NumericCast<int64_t>(subquery_stage2->select_list.size() + 1)));
		auto group_reference = make_uniq<ColumnRefExpression>(bind_state.internal_group_names[gr]);
		group_reference->SetAlias(bind_state.internal_group_names[gr]);
		subquery_stage2->select_list.push_back(std::move(group_reference));
	}

	// construct the list aggregates
	for (idx_t aggr = 0; aggr < bind_state.internal_aggregate_names.size(); aggr++) {
		auto colref = make_uniq<ColumnRefExpression>(bind_state.internal_aggregate_names[aggr]);
		vector<unique_ptr<ParsedExpression>> list_children;
		list_children.push_back(std::move(colref));
		auto aggregate = make_uniq<FunctionExpression>("list", std::move(list_children));
		aggregate->SetAlias(bind_state.internal_aggregate_names[aggr]);
		subquery_stage2->select_list.push_back(std::move(aggregate));
	}
	// construct the pivot list
	auto pivot_name = "__internal_pivot_name";
	unique_ptr<ParsedExpression> expr;
	for (auto &pivot : ref.pivots) {
		for (auto &pivot_expr : pivot.pivot_expressions) {
			// coalesce(pivot::VARCHAR, 'NULL')
			auto coalesce = ConstructPivotExpression(std::move(pivot_expr));
			if (!expr) {
				expr = std::move(coalesce);
			} else {
				// string concat
				vector<unique_ptr<ParsedExpression>> concat_children;
				concat_children.push_back(std::move(expr));
				concat_children.push_back(ConstantExpression::String("_"));
				concat_children.push_back(std::move(coalesce));
				auto concat = make_uniq<FunctionExpression>("concat", std::move(concat_children));
				expr = std::move(concat);
			}
		}
	}
	// list(coalesce)
	vector<unique_ptr<ParsedExpression>> list_children;
	list_children.push_back(std::move(expr));
	auto aggregate = make_uniq<FunctionExpression>("list", std::move(list_children));

	aggregate->SetAlias(Identifier(pivot_name));
	subquery_stage2->select_list.push_back(std::move(aggregate));

	subquery_stage2->from_table = std::move(subquery_ref);
	return subquery_stage2;
}

void ReplacePivotColumnRef(ParsedExpression &root_expr, const Identifier &name) {
	ParsedExpressionIterator::VisitExpressionMutable<ColumnRefExpression>(
	    root_expr, [&](ColumnRefExpression &colref) { colref.ColumnNamesMutable()[0] = name; });
}

static unique_ptr<SelectNode> PivotFinalOperator(PivotBindState &bind_state, PivotRef &ref,
                                                 unique_ptr<SelectNode> subquery,
                                                 vector<PivotValueElement> pivot_values) {
	auto final_pivot_operator = make_uniq<SelectNode>();
	// wrap the subquery of stage 1
	auto subquery_select = make_uniq<SelectStatement>();
	subquery_select->node = std::move(subquery);
	auto subquery_ref = make_uniq<SubqueryRef>(std::move(subquery_select));

	auto bound_pivot = make_uniq<PivotRef>();
	bound_pivot->bound_pivot_values = std::move(pivot_values);
	bound_pivot->bound_group_names = std::move(bind_state.group_names);
	bound_pivot->bound_aggregate_names = std::move(bind_state.aggregate_names);
	bound_pivot->source = std::move(subquery_ref);

	for (auto &group_name : bound_pivot->bound_group_names) {
		final_pivot_operator->select_list.push_back(make_uniq<ColumnRefExpression>(group_name));
	}
	// gather aggregate names - these are deduplicated together with the group names, as they are all output columns
	vector<Identifier> aggregate_names = bound_pivot->bound_group_names;
	const auto group_count = aggregate_names.size();
	for (auto &pivot_value : bound_pivot->bound_pivot_values) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.aggregates.size(); aggr_idx++) {
			auto aggr = ref.aggregates[aggr_idx]->Copy();
			auto &aggr_name = bound_pivot->bound_aggregate_names[aggr_idx];
			auto pivot_aggr_name = pivot_value.name + aggr_name.GetIdentifierName();
			aggregate_names.push_back(Identifier(std::move(pivot_aggr_name)));
		}
	}
	QueryResult::DeduplicateColumns(aggregate_names);
	aggregate_names.erase(aggregate_names.begin(), aggregate_names.begin() + NumericCast<int64_t>(group_count));

	idx_t aggr_name_idx = 0;
	for (idx_t pivot_idx = 0; pivot_idx < bound_pivot->bound_pivot_values.size(); pivot_idx++) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.aggregates.size(); aggr_idx++) {
			auto aggr = ref.aggregates[aggr_idx]->Copy();
			auto &pivot_aggr_name = aggregate_names[aggr_name_idx++];
			// replace column ref with name
			ReplacePivotColumnRef(*aggr, pivot_aggr_name);
			aggr->SetAlias(pivot_aggr_name);

			final_pivot_operator->select_list.push_back(std::move(aggr));
		}
	}

	final_pivot_operator->from_table = std::move(bound_pivot);
	return final_pivot_operator;
}

void ExtractPivotAggregates(BoundStatement &node, vector<unique_ptr<Expression>> &aggregates) {
	reference<LogicalOperator> op(*node.plan);
	bool found_first_aggregate = false;
	while (true) {
		if (op.get().type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
			if (found_first_aggregate) {
				break;
			}
			found_first_aggregate = true;
		}
		if (op.get().children.size() != 1) {
			throw InternalException("Pivot - expected an aggregate");
		}
		op = *op.get().children[0];
	}
	auto &aggr_op = op.get().Cast<LogicalAggregate>();
	for (auto &aggr : aggr_op.expressions) {
		if (aggr->GetAlias() == "__collated_group") {
			continue;
		}
		aggregates.push_back(aggr->Copy());
	}
}

string GetPivotAggregateName(const PivotValueElement &pivot_value, const string &aggr_name, idx_t aggregate_count) {
	auto name = pivot_value.name;
	if (aggregate_count > 1 || !aggr_name.empty()) {
		// if there are multiple aggregates specified we add the name of the aggregate as well
		name += "_" + aggr_name;
	}
	return name;
}

BoundStatement Binder::BindBoundPivot(PivotRef &ref) {
	// bind the child table in a child binder
	BoundPivotRef result;
	result.bind_index = GenerateTableIndex();
	result.child_binder = Binder::CreateBinder(context, this);
	result.child = result.child_binder->Bind(*ref.source);
	if (!result.child_binder->correlated_columns.empty()) {
		throw BinderException("PIVOT is not supported in correlated subqueries yet");
	}

	auto &aggregates = result.bound_pivot.aggregates;
	ExtractPivotAggregates(result.child, aggregates);

	if (aggregates.size() < ref.bound_aggregate_names.size()) {
		vector<string> unique_names;
		identifier_set_t seen;
		for (auto &name : ref.bound_aggregate_names) {
			if (seen.find(name) == seen.end()) {
				unique_names.push_back(name.GetIdentifierName());
				seen.insert(name);
			}
		}

		if (unique_names.size() != aggregates.size()) {
			// identical aggregates with different names are bound to a single aggregate
			throw BinderException("PIVOT: identical aggregates must have the same name - use a single aggregate, or "
			                      "make the aggregates differ");
		}

		identifier_map_t<idx_t> name_to_position;
		for (idx_t i = 0; i < unique_names.size(); i++) {
			name_to_position[Identifier(unique_names[i])] = i;
		}

		vector<unique_ptr<Expression>> expanded_aggs;
		for (auto &expected_name : ref.bound_aggregate_names) {
			idx_t position = name_to_position[expected_name];
			expanded_aggs.push_back(aggregates[position]->Copy());
		}

		result.bound_pivot.aggregates = std::move(expanded_aggs);
	}

	if (aggregates.size() != ref.bound_aggregate_names.size()) {
		throw InternalException("Pivot aggregate count mismatch (expected %llu, found %llu)",
		                        ref.bound_aggregate_names.size(), aggregates.size());
	}

	vector<Identifier> child_names;
	vector<LogicalType> child_types;
	result.child_binder->bind_context.GetTypesAndNames(child_names, child_types);

	vector<Identifier> names;
	vector<LogicalType> types;
	// emit the groups
	for (idx_t i = 0; i < ref.bound_group_names.size(); i++) {
		names.push_back(ref.bound_group_names[i]);
		types.push_back(child_types[i]);
	}
	// emit the pivot columns
	for (auto &pivot_value : ref.bound_pivot_values) {
		for (idx_t aggr_idx = 0; aggr_idx < ref.bound_aggregate_names.size(); aggr_idx++) {
			auto &aggr = aggregates[aggr_idx];
			auto &aggr_name = ref.bound_aggregate_names[aggr_idx];
			auto name = pivot_value.name + aggr_name;

			string pivot_str;
			for (idx_t value_idx = 0; value_idx < pivot_value.values.size(); value_idx++) {
				if (value_idx > 0) {
					pivot_str += "_";
				}
				pivot_str += PivotValueKey(pivot_value.values[value_idx]);
			}
			result.bound_pivot.pivot_values.push_back(std::move(pivot_str));
			names.push_back(Identifier(std::move(name)));
			types.push_back(aggr->GetReturnType());
		}
	}
	result.bound_pivot.group_count = ref.bound_group_names.size();
	result.bound_pivot.types = types;
	Identifier subquery_alias = ref.alias.empty() ? Identifier("__unnamed_pivot") : ref.alias;
	QueryResult::DeduplicateColumns(names);
	bind_context.AddGenericBinding(result.bind_index, subquery_alias, names, types);

	MoveCorrelatedExpressions(*result.child_binder);

	BoundStatement result_statement;
	result_statement.plan =
	    make_uniq<LogicalPivot>(result.bind_index, std::move(result.child.plan), std::move(result.bound_pivot));
	return result_statement;
}

static void BindPivotConstantInList(unique_ptr<ParsedExpression> &expr, vector<Value> &values, Binder &binder) {
	Value val;
	ConstantBinder const_binder(binder, binder.context, "PIVOT IN list");
	auto bound_expr = const_binder.Bind(expr);
	if (!bound_expr->IsFoldable()) {
		throw BinderException(expr->GetQueryLocation(), "PIVOT IN list must contain constant expressions");
	}
	auto folded_value = ExpressionExecutor::EvaluateScalar(binder.context, *bound_expr);
	values.push_back(folded_value);
}

static bool TryExtractUnpivotList(ParsedExpression &expr, vector<Identifier> &column_names) {
	auto initial_size = column_names.size();
	switch (expr.GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = expr.Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			return false;
		}
		column_names.push_back(colref.GetColumnName());
		return true;
	}
	case ExpressionType::VALUE_CONSTANT: {
		auto &constant = expr.Cast<ConstantExpression>();
		column_names.push_back(Identifier(constant.GetLiteral().ToValue().ToString()));
		return true;
	}
	case ExpressionType::FUNCTION: {
		auto &function = expr.Cast<FunctionExpression>();
		if (function.FunctionName() != "row" || function.GetArgumentsMutable().empty()) {
			return false;
		}
		for (auto &child : function.GetArgumentsMutable()) {
			if (!TryExtractUnpivotList(*child.GetExpressionMutable(), column_names)) {
				column_names.resize(initial_size);
				return false;
			}
		}
		return true;
	}
	default:
		return false;
	}
}

static void BindPivotInList(unique_ptr<ParsedExpression> &expr, vector<Value> &values, Binder &binder) {
	switch (expr->GetExpressionType()) {
	case ExpressionType::COLUMN_REF: {
		auto &colref = expr->Cast<ColumnRefExpression>();
		if (colref.IsQualified()) {
			throw BinderException(expr->GetQueryLocation(), "PIVOT IN list cannot contain qualified column references");
		}
		values.emplace_back(colref.GetColumnName());
	} break;
	case ExpressionType::FUNCTION: {
		auto &function = expr->Cast<FunctionExpression>();
		if (function.FunctionName() == "row") {
			for (auto &child : function.GetArgumentsMutable()) {
				BindPivotInList(child.GetExpressionMutable(), values, binder);
			}
		} else {
			BindPivotConstantInList(expr, values, binder);
		}
	} break;
	default: {
		BindPivotConstantInList(expr, values, binder);
	} break;
	}
}

unique_ptr<SelectNode> Binder::BindPivot(PivotRef &ref, vector<unique_ptr<ParsedExpression>> all_columns) {
	// keep track of the columns by which we pivot/aggregate
	// any columns which are not pivoted/aggregated on are added to the GROUP BY clause
	identifier_set_t handled_columns;

	vector<reference<FunctionExpression>> pivot_aggregates;
	// parse the aggregate, and extract the referenced columns from the aggregate
	for (auto &aggr : ref.aggregates) {
		if (aggr->HasSubquery()) {
			throw BinderException(*aggr, "Pivot expression cannot contain subqueries");
		}
		if (aggr->IsWindow()) {
			throw BinderException(*aggr, "Pivot expression cannot contain window functions");
		}
		idx_t aggregate_count = pivot_aggregates.size();
		ExtractPivotAggregateExpression(context, aggr, pivot_aggregates);
		if (pivot_aggregates.size() != aggregate_count + 1) {
			string error_str = pivot_aggregates.size() == aggregate_count
			                       ? "but no aggregates were found"
			                       : "but " + to_string(pivot_aggregates.size() - aggregate_count) + " were found";
			throw BinderException(*aggr, "Pivot expression must contain exactly one aggregate, %s", error_str);
		}
	}
	for (auto &aggr : pivot_aggregates) {
		ExtractPivotExpressions(aggr.get(), handled_columns, macro_binding);
	}

	// process the in-lists
	for (auto &pivot_column : ref.pivots) {
		D_ASSERT(pivot_column.unpivot_names.empty());

		for (auto &pivot_entry : pivot_column.entries) {
			// bind the expressions in the IN list
			if (!pivot_entry.values.empty()) {
				continue;
			}
			if (!pivot_entry.expr) {
				throw BinderException("PIVOT IN list - expected an expression or value");
			}

			BindPivotInList(pivot_entry.expr, pivot_entry.values, *this);

			// check that we have the expected number of values
			const auto expected_size = pivot_column.pivot_expressions.size();
			const auto values_size = pivot_entry.values.size();

			if (values_size != expected_size) {
				throw BinderException("PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
				                      expected_size, values_size);
			}

			// clear the expression after binding, we only need the values from now
			pivot_entry.expr = nullptr;
		}
	}

	// first add all pivots to the set of handled columns, and check for duplicates
	idx_t total_pivots = 1;
	for (auto &pivot : ref.pivots) {
		if (!pivot.pivot_enum.empty()) {
			auto &type_entry = Catalog::GetEntry<TypeCatalogEntry>(
			    context, QualifiedName(Identifier::InvalidCatalog(), Identifier::InvalidSchema(), pivot.pivot_enum));
			auto type = type_entry.user_type;
			if (type.id() != LogicalTypeId::ENUM) {
				throw BinderException(ref, "Pivot must reference an ENUM type: %s is of type \"%s\"", pivot.pivot_enum,
				                      type.ToString());
			}
			if (!type.IsComplete()) {
				throw BinderException("ENUM type is incomplete");
			}
			auto enum_size = EnumType::GetSize(type);
			for (idx_t i = 0; i < enum_size; i++) {
				auto enum_value = EnumType::GetValue(Value::ENUM(i, type));
				PivotColumnEntry entry;
				entry.values.emplace_back(enum_value);
				entry.alias = Identifier(std::move(enum_value));
				pivot.entries.push_back(std::move(entry));
			}
		}
		total_pivots *= pivot.entries.size();
		// add the pivoted column to the columns that have been handled
		for (auto &pivot_name : pivot.pivot_expressions) {
			ExtractPivotExpressions(*pivot_name, handled_columns, macro_binding);
		}
		value_set_t pivots;
		for (auto &entry : pivot.entries) {
			D_ASSERT(!entry.expr);
			Value val;
			if (entry.values.size() == 1) {
				val = entry.values[0];
			} else {
				val = Value::LIST(LogicalType::VARCHAR, entry.values);
			}
			if (pivots.find(val) != pivots.end()) {
				throw BinderException(ref, "The value \"%s\" was specified multiple times in the IN clause",
				                      val.ToString());
			}
			if (entry.values.size() != pivot.pivot_expressions.size()) {
				throw BinderException(ref, "PIVOT IN list - inconsistent amount of rows - expected %d but got %d",
				                      pivot.pivot_expressions.size(), entry.values.size());
			}
			pivots.insert(val);
		}
	}
	auto pivot_limit = Settings::Get<PivotLimitSetting>(context);
	if (total_pivots >= pivot_limit) {
		throw BinderException(ref, "Pivot column limit of %llu exceeded. Use SET pivot_limit=X to increase the limit.",
		                      pivot_limit);
	}

	// construct the required pivot values recursively
	vector<PivotValueElement> pivot_values;
	ConstructPivots(ref, pivot_values);

	unique_ptr<SelectNode> pivot_node;
	// pivots have three components
	// - the pivots (i.e. future column names)
	// - the groups (i.e. the future row names
	// - the aggregates (i.e. the values of the pivot columns)

	// we have two ways of executing a pivot statement
	// (1) the straightforward manner of filtered aggregates SUM(..) FILTER (pivot_value=X)
	// (2) computing the aggregates once, then using LIST to group the aggregates together with the PIVOT operator
	// -> filtered aggregates are faster when there are FEW pivot values
	// -> LIST is faster when there are MANY pivot values
	// we switch dynamically based on the number of pivots to compute
	auto pivot_filter_threshold = Settings::Get<PivotFilterThresholdSetting>(context);
	if (pivot_values.size() <= pivot_filter_threshold) {
		// use a set of filtered aggregates
		pivot_node =
		    PivotFilteredAggregate(context, ref, std::move(all_columns), handled_columns, std::move(pivot_values));
	} else {
		// executing a pivot statement happens in three stages
		// 1) execute the query "SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
		// this computes all values that are required in the final result, but not yet in the correct orientation
		// 2) execute the query "SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
		// this pushes all pivots and aggregates that belong to a specific group together in an aligned manner
		// 3) push a PIVOT operator, that performs the actual pivoting of the values into the different columns

		PivotBindState bind_state;
		// Pivot Stage 1
		// SELECT {groups}, {pivots}, {aggregates} FROM {from_clause} GROUP BY {groups}, {pivots}
		auto subquery_stage1 = PivotInitialAggregate(context, bind_state, ref, std::move(all_columns), handled_columns);

		// Pivot stage 2
		// SELECT {groups}, LIST({pivots}), LIST({aggregates}) FROM [Q1] GROUP BY {groups}
		auto subquery_stage2 = PivotListAggregate(bind_state, ref, std::move(subquery_stage1));

		// Pivot stage 3
		// construct the final pivot operator
		pivot_node = PivotFinalOperator(bind_state, ref, std::move(subquery_stage2), std::move(pivot_values));
	}
	return pivot_node;
}

struct UnpivotEntry {
	Identifier alias;
	vector<Identifier> column_names;
	vector<unique_ptr<ParsedExpression>> expressions;
};

void Binder::ExtractUnpivotEntries(Binder &child_binder, PivotColumnEntry &entry,
                                   vector<UnpivotEntry> &unpivot_entries) {
	// Try to bind the entry expression as values - but only when it is composed purely of column
	// references and constants (a column list). Entries that contain other expressions are
	// unpivoted as expressions instead of being folded into column names.
	if (entry.expr) {
		vector<Identifier> column_list;
		if (TryExtractUnpivotList(*entry.expr, column_list)) {
			try {
				auto expr_copy = entry.expr->Copy();
				BindPivotInList(expr_copy, entry.values, child_binder);
				// successfully bound as values - clear the expression
				entry.expr = nullptr;
			} catch (...) {
				// ignore binder exceptions here - we fall back to expression mode
				entry.values.clear();
			}
		}
	}

	if (!entry.expr) {
		// pivot entry without an expression - generate one
		UnpivotEntry unpivot_entry;
		unpivot_entry.alias = Identifier(entry.alias.GetIdentifierName());
		for (auto &val : entry.values) {
			auto column_name = val.ToString();
			if (column_name.empty()) {
				throw BinderException("UNPIVOT - empty column name not supported");
			}
			unpivot_entry.expressions.push_back(make_uniq<ColumnRefExpression>(Identifier(column_name)));
		}
		unpivot_entries.push_back(std::move(unpivot_entry));
		return;
	}

	vector<Identifier> column_names;
	if (TryExtractUnpivotList(*entry.expr, column_names)) {
		UnpivotEntry unpivot_entry;
		unpivot_entry.alias = Identifier(entry.alias.GetIdentifierName());
		for (auto &column_name : column_names) {
			if (column_name.empty()) {
				throw BinderException("UNPIVOT - empty column name not supported");
			}
			unpivot_entry.expressions.push_back(make_uniq<ColumnRefExpression>(column_name));
		}
		unpivot_entries.push_back(std::move(unpivot_entry));
		return;
	}

	D_ASSERT(entry.values.empty());
	// expand star expressions (if any)
	vector<unique_ptr<ParsedExpression>> star_columns;
	child_binder.ExpandStarExpression(std::move(entry.expr), star_columns);

	for (auto &expr : star_columns) {
		// create one pivot entry per result column
		UnpivotEntry unpivot_entry;
		if (!expr->GetAlias().empty()) {
			unpivot_entry.alias = Identifier(expr->GetAlias().GetIdentifierName());
		}
		unpivot_entry.expressions.push_back(std::move(expr));
		unpivot_entries.push_back(std::move(unpivot_entry));
	}
}

void Binder::ExtractUnpivotColumnName(ParsedExpression &expr, vector<Identifier> &result) {
	if (expr.GetExpressionType() == ExpressionType::COLUMN_REF) {
		auto &colref = expr.Cast<ColumnRefExpression>();
		result.push_back(colref.GetColumnName());
		return;
	}
	if (expr.GetExpressionType() == ExpressionType::SUBQUERY) {
		throw BinderException(expr, "UNPIVOT list cannot contain subqueries");
	}
	ParsedExpressionIterator::EnumerateChildren(
	    expr, [&](ParsedExpression &child) { ExtractUnpivotColumnName(child, result); });
}

unique_ptr<SelectNode> Binder::BindUnpivot(Binder &child_binder, PivotRef &ref,
                                           vector<unique_ptr<ParsedExpression>> all_columns,
                                           unique_ptr<ParsedExpression> &where_clause) {
	D_ASSERT(ref.groups.empty());
	D_ASSERT(ref.pivots.size() == 1);

	auto select_node = make_uniq<SelectNode>();
	select_node->from_table = std::move(ref.source);

	// handle the pivot
	auto &unpivot = ref.pivots[0];

	// handle star expressions in any entries
	vector<UnpivotEntry> unpivot_entries;
	for (auto &entry : unpivot.entries) {
		ExtractUnpivotEntries(child_binder, entry, unpivot_entries);
	}
	if (unpivot_entries.empty()) {
		throw BinderException(ref, "UNPIVOT clause must unpivot on at least one column - zero were provided");
	}

	identifier_set_t handled_columns;
	identifier_map_t<Identifier> name_map;
	for (auto &entry : unpivot_entries) {
		for (auto &unpivot_expr : entry.expressions) {
			vector<Identifier> result;
			ExtractUnpivotColumnName(*unpivot_expr, result);
			if (result.empty()) {
				throw BinderException(
				    *unpivot_expr,
				    "UNPIVOT clause must contain exactly one column - expression \"%s\" does not contain any",
				    unpivot_expr->ToString());
			}
			if (result.size() > 1) {
				throw BinderException(
				    *unpivot_expr,
				    "UNPIVOT clause must contain exactly one column - expression \"%s\" contains multiple (%s)",
				    unpivot_expr->ToString(), StringUtil::Join(result, ", "));
			}
			handled_columns.insert(result[0]);
			entry.column_names.push_back(std::move(result[0]));
		}
	}

	vector<Identifier> select_names;
	for (auto &col_expr : all_columns) {
		if (col_expr->GetExpressionType() != ExpressionType::COLUMN_REF) {
			throw InternalException("Unexpected child of pivot source - not a ColumnRef");
		}
		auto &columnref = col_expr->Cast<ColumnRefExpression>();
		auto &column_name = columnref.GetColumnName();
		auto entry = handled_columns.find(column_name);
		if (entry == handled_columns.end()) {
			// not handled - add to the set of regularly selected columns
			select_names.push_back(col_expr->GetName());
			select_node->select_list.push_back(std::move(col_expr));
		} else {
			name_map[column_name] = column_name;
			handled_columns.erase(entry);
		}
	}
	if (!handled_columns.empty()) {
		for (auto &entry : handled_columns) {
			throw BinderException(ref, "Column %s referenced in UNPIVOT but no matching entry was found in the table",
			                      entry);
		}
	}
	vector<Value> unpivot_names;
	for (auto &entry : unpivot_entries) {
		string generated_name;
		D_ASSERT(entry.expressions.size() == entry.column_names.size());
		for (idx_t c = 0; c < entry.expressions.size(); c++) {
			auto name_entry = name_map.find(entry.column_names[c]);
			if (name_entry == name_map.end()) {
				throw InternalException("Unpivot - could not find column name in name map");
			}
			if (!generated_name.empty()) {
				generated_name += "_";
			}
			generated_name += name_entry->second;
		}
		unpivot_names.emplace_back(!entry.alias.empty() ? entry.alias : Identifier(generated_name));
	}
	vector<vector<unique_ptr<ParsedExpression>>> unpivot_expressions;
	for (idx_t v_idx = 1; v_idx < unpivot_entries.size(); v_idx++) {
		if (unpivot_entries[v_idx].expressions.size() != unpivot_entries[0].expressions.size()) {
			throw BinderException(
			    ref,
			    "UNPIVOT value count mismatch - entry has %llu values, but expected all entries to have %llu values",
			    unpivot_entries[v_idx].expressions.size(), unpivot_entries[0].expressions.size());
		}
	}

	for (idx_t v_idx = 0; v_idx < unpivot_entries[0].expressions.size(); v_idx++) {
		vector<unique_ptr<ParsedExpression>> expressions;
		expressions.reserve(unpivot_entries.size());
		for (auto &entry : unpivot_entries) {
			expressions.push_back(std::move(entry.expressions[v_idx]));
		}
		unpivot_expressions.push_back(std::move(expressions));
	}

	idx_t column_count = select_names.size();
	idx_t unnest_count = unpivot_expressions.size();
	// add the names for the generated unpivot lists
	select_names.push_back("unpivot_names");
	for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
		if (i > 0) {
			select_names.push_back(Identifier("unpivot_list_" + std::to_string(i + 1)));
		} else {
			select_names.push_back(Identifier("unpivot_list"));
		}
	}

	// now de-duplicate the names
	QueryResult::DeduplicateColumns(select_names);

	// construct the UNNEST expression for the set of names (constant)
	auto unpivot_list = Value::LIST(LogicalType::VARCHAR, std::move(unpivot_names));
	auto unpivot_name_expr = ConstantExpression::FromValue(unpivot_list);
	unpivot_name_expr->SetAlias(select_names[column_count]);
	select_node->select_list.push_back(std::move(unpivot_name_expr));

	// construct the unpivot lists for the set of unpivoted columns
	if (ref.unpivot_names.size() != unpivot_expressions.size()) {
		throw BinderException(ref, "UNPIVOT name count mismatch - got %d names but %d expressions",
		                      ref.unpivot_names.size(), unpivot_expressions.size());
	}
	for (idx_t i = 0; i < unpivot_expressions.size(); i++) {
		auto list_expr = make_uniq<FunctionExpression>("unpivot_list", std::move(unpivot_expressions[i]));
		list_expr->SetAlias(select_names[column_count + 1 + i]);
		select_node->select_list.push_back(std::move(list_expr));
	}

	// move the unpivot lists into a subquery
	auto result_node = make_uniq<SelectNode>();
	auto sub_select = make_uniq<SelectStatement>();
	sub_select->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(sub_select));
	subquery->alias = "unpivot";

	result_node->from_table = std::move(subquery);

	// the names of the columns that are passed through (select_names is moved from below)
	const vector<Identifier> passthrough_names(select_names.begin(),
	                                           select_names.begin() + NumericCast<int64_t>(column_count));

	// construct the final UNNEST calls which generate the final unpivot result
	for (idx_t i = 0; i < column_count; i++) {
		auto select_col = make_uniq<ColumnRefExpression>(std::move(select_names[i]));
		result_node->select_list.push_back(std::move(select_col));
	}

	auto unpivot_name_list = make_uniq<ColumnRefExpression>(std::move(select_names[column_count]));
	vector<unique_ptr<ParsedExpression>> unnest_name_children;
	unnest_name_children.push_back(std::move(unpivot_name_list));
	auto unnest_name_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_name_children));
	// when excluding NULLs, the unnested columns get internal names: the final names can collide with other columns
	// and the NULL filter has to be applied to exactly these columns
	const bool exclude_nulls = !ref.include_nulls;
	const Identifier internal_name_alias("__unpivot_name");
	unnest_name_expr->SetAlias(exclude_nulls ? internal_name_alias : unpivot.unpivot_names[0]);
	result_node->select_list.push_back(std::move(unnest_name_expr));
	vector<pair<Identifier, Identifier>> value_aliases;

	for (idx_t i = 0; i < unnest_count; i++) {
		auto unpivot_internal_name = std::move(select_names[column_count + 1 + i]);

		auto unpivot_list_ref = make_uniq<ColumnRefExpression>(std::move(unpivot_internal_name));
		vector<unique_ptr<ParsedExpression>> unnest_val_children;
		unnest_val_children.push_back(std::move(unpivot_list_ref));
		auto unnest_val_expr = make_uniq<FunctionExpression>("unnest", std::move(unnest_val_children));
		auto &unnest_name = i < ref.column_name_alias.size() ? ref.column_name_alias[i] : ref.unpivot_names[i];
		if (exclude_nulls) {
			Identifier internal_value_alias("__unpivot_value_" + to_string(i));
			unnest_val_expr->SetAlias(internal_value_alias);
			value_aliases.emplace_back(internal_value_alias, unnest_name);
		} else {
			unnest_val_expr->SetAlias(unnest_name);
		}
		result_node->select_list.push_back(std::move(unnest_val_expr));
	}
	if (!exclude_nulls) {
		return result_node;
	}

	// if we are running with EXCLUDE NULLS we need to add an IS NOT NULL filter on the unnested columns,
	// and then restore the final column names in an outer projection
	auto filtered_node = make_uniq<SelectNode>();
	for (auto &passthrough_name : passthrough_names) {
		filtered_node->select_list.push_back(make_uniq<ColumnRefExpression>(passthrough_name));
	}
	auto name_ref = make_uniq<ColumnRefExpression>(internal_name_alias);
	name_ref->SetAlias(unpivot.unpivot_names[0]);
	filtered_node->select_list.push_back(std::move(name_ref));
	for (auto &value_alias : value_aliases) {
		auto value_ref = make_uniq<ColumnRefExpression>(value_alias.first);
		value_ref->SetAlias(value_alias.second);
		filtered_node->select_list.push_back(std::move(value_ref));

		auto colref = make_uniq<ColumnRefExpression>(value_alias.first);
		auto filter = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_IS_NOT_NULL, std::move(colref));
		if (filtered_node->where_clause) {
			filtered_node->where_clause = make_uniq<ConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, std::move(filtered_node->where_clause), std::move(filter));
		} else {
			filtered_node->where_clause = std::move(filter);
		}
	}
	auto inner_select = make_uniq<SelectStatement>();
	inner_select->node = std::move(result_node);
	auto inner_ref = make_uniq<SubqueryRef>(std::move(inner_select));
	inner_ref->alias = "unpivot_unnested";
	filtered_node->from_table = std::move(inner_ref);
	return filtered_node;
}

BoundStatement Binder::Bind(PivotRef &ref) {
	if (!ref.source) {
		throw InternalException("Pivot without a source!?");
	}
	if (!ref.bound_pivot_values.empty() || !ref.bound_group_names.empty() || !ref.bound_aggregate_names.empty()) {
		// bound pivot
		return BindBoundPivot(ref);
	}

	// bind the source of the pivot
	// we need to do this to be able to expand star expressions
	if (ref.source->type == TableReferenceType::SUBQUERY && ref.source->alias.empty()) {
		ref.source->alias = Identifier("__internal_pivot_alias_" + to_string(GenerateTableIndex().index));
	}
	auto copied_source = ref.source->Copy();
	auto star_binder = Binder::CreateBinder(context, this);
	star_binder->Bind(*copied_source);

	// figure out the set of column names that are in the source of the pivot
	vector<unique_ptr<ParsedExpression>> all_columns;
	star_binder->ExpandStarExpression(make_uniq<StarExpression>(), all_columns);

	unique_ptr<SelectNode> select_node;
	unique_ptr<ParsedExpression> where_clause;
	if (!ref.aggregates.empty()) {
		select_node = BindPivot(ref, std::move(all_columns));
	} else {
		select_node = BindUnpivot(*star_binder, ref, std::move(all_columns), where_clause);
	}
	auto child_binder = Binder::CreateBinder(context, this);
	auto result = child_binder->BindNode(*select_node);
	auto root_index = result.plan->GetRootIndex();

	MoveCorrelatedExpressions(*child_binder);
	Identifier subquery_alias = ref.alias.empty() ? Identifier("__unnamed_pivot") : ref.alias;
	SubqueryRef subquery_ref(nullptr, subquery_alias);
	subquery_ref.column_name_alias = std::move(ref.column_name_alias);
	if (where_clause) {
		// if a WHERE clause was provided - bind a subquery holding the WHERE clause
		// we need to bind a new subquery here because the WHERE clause has to be applied AFTER the unnest
		child_binder = Binder::CreateBinder(context, this);
		child_binder->bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, result);
		auto where_query = make_uniq<SelectNode>();
		where_query->select_list.push_back(make_uniq<StarExpression>());
		where_query->where_clause = std::move(where_clause);
		result = child_binder->BindSelectNode(*where_query, std::move(result));
		root_index = result.plan->GetRootIndex();
	}
	bind_context.AddSubquery(root_index, subquery_ref.alias, subquery_ref, result);
	return result;
}

} // namespace duckdb
