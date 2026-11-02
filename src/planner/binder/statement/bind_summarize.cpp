#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/comparison_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/case_expression.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/parser/tableref/bound_ref_wrapper.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/tableref/showref.hpp"
#include "duckdb/parser/tableref/basetableref.hpp"
#include "duckdb/parser/expression/star_expression.hpp"

namespace duckdb {

static unique_ptr<ParsedExpression> SummarizeWrapUnnest(vector<unique_ptr<ParsedExpression>> &children,
                                                        const string &alias) {
	auto list_function = make_uniq<FunctionExpression>("list_value", std::move(children));
	vector<unique_ptr<ParsedExpression>> unnest_children;
	unnest_children.push_back(std::move(list_function));
	auto unnest_function = make_uniq<FunctionExpression>("unnest", std::move(unnest_children));
	unnest_function->SetAlias(Identifier(alias));
	return std::move(unnest_function);
}

static unique_ptr<ParsedExpression> SummarizeCreateAggregate(const string &aggregate, Identifier column_name) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(std::move(column_name)));
	auto aggregate_function = make_uniq<FunctionExpression>(Identifier(aggregate), std::move(children));
	auto cast_function = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(aggregate_function));
	return std::move(cast_function);
}

static unique_ptr<ParsedExpression> SummarizeCreateAggregate(const string &aggregate, Identifier column_name,
                                                             const Value &modifier) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(make_uniq<ColumnRefExpression>(std::move(column_name)));
	children.push_back(ConstantExpression::FromValue(modifier));
	auto aggregate_function = make_uniq<FunctionExpression>(Identifier(aggregate), std::move(children));
	auto cast_function = make_uniq<CastExpression>(LogicalType::VARCHAR, std::move(aggregate_function));
	return std::move(cast_function);
}

static unique_ptr<ParsedExpression> SummarizeCreateCountStar() {
	vector<unique_ptr<ParsedExpression>> children;
	auto aggregate_function = make_uniq<FunctionExpression>("count_star", std::move(children));
	return std::move(aggregate_function);
}

static unique_ptr<ParsedExpression> SummarizeCreateBinaryFunction(const string &op, unique_ptr<ParsedExpression> left,
                                                                  unique_ptr<ParsedExpression> right) {
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(std::move(left));
	children.push_back(std::move(right));
	auto binary_function = make_uniq<FunctionExpression>(Identifier(op), std::move(children));
	return std::move(binary_function);
}

static unique_ptr<ParsedExpression> SummarizeCreateNullPercentage(Identifier column_name) {
	auto count_star = make_uniq<CastExpression>(LogicalType::DOUBLE, SummarizeCreateCountStar());
	auto count =
	    make_uniq<CastExpression>(LogicalType::DOUBLE, SummarizeCreateAggregate("count", std::move(column_name)));
	auto null_percentage = SummarizeCreateBinaryFunction("/", std::move(count), std::move(count_star));
	auto negate_x =
	    SummarizeCreateBinaryFunction("-", ConstantExpression::FromValue(Value::DOUBLE(1)), std::move(null_percentage));
	auto percentage_x =
	    SummarizeCreateBinaryFunction("*", std::move(negate_x), ConstantExpression::FromValue(Value::DOUBLE(100)));

	auto comp_expr = make_uniq<ComparisonExpression>(ExpressionType::COMPARE_GREATERTHAN, SummarizeCreateCountStar(),
	                                                 ConstantExpression::FromValue(Value::BIGINT(0)));
	auto case_expr = make_uniq<CaseExpression>();
	CaseCheck check;
	check.when_expr = std::move(comp_expr);
	check.then_expr = std::move(percentage_x);
	case_expr->CaseChecksMutable().push_back(std::move(check));
	case_expr->ElseMutable() = ConstantExpression::Null();

	return make_uniq<CastExpression>(LogicalType::DECIMAL(9, 2), std::move(case_expr));
}

BoundStatement Binder::BindSummarize(ShowRef &ref) {
	unique_ptr<QueryNode> query;
	if (ref.query) {
		query = std::move(ref.query);
	} else {
		auto table_name = QualifiedName::Parse(ref.GetTableName().GetIdentifierName());
		auto node = make_uniq<SelectNode>();
		node->select_list.push_back(make_uniq<StarExpression>());
		auto basetableref = make_uniq<BaseTableRef>();
		basetableref->SetQualifiedName(table_name);
		node->from_table = std::move(basetableref);
		query = std::move(node);
	}

	// Bind the source once to discover its columns and retain the plan for the summarize query.
	auto source_select = make_uniq<SelectStatement>();
	source_select->node = std::move(query);
	auto source_ref = make_uniq<SubqueryRef>(std::move(source_select), "summarize_tbl");
	auto child_binder = Binder::CreateBinder(context, this);
	auto plan = child_binder->Bind(*source_ref);
	D_ASSERT(plan.types.size() == plan.names.size());
	vector<unique_ptr<ParsedExpression>> name_children;
	vector<unique_ptr<ParsedExpression>> type_children;
	vector<unique_ptr<ParsedExpression>> min_children;
	vector<unique_ptr<ParsedExpression>> max_children;
	vector<unique_ptr<ParsedExpression>> unique_children;
	vector<unique_ptr<ParsedExpression>> avg_children;
	vector<unique_ptr<ParsedExpression>> std_children;
	vector<unique_ptr<ParsedExpression>> q25_children;
	vector<unique_ptr<ParsedExpression>> q50_children;
	vector<unique_ptr<ParsedExpression>> q75_children;
	vector<unique_ptr<ParsedExpression>> count_children;
	vector<unique_ptr<ParsedExpression>> null_percentage_children;
	for (idx_t i = 0; i < plan.names.size(); i++) {
		name_children.push_back(ConstantExpression::String(plan.names[i].GetIdentifierName()));
		type_children.push_back(ConstantExpression::String(plan.types[i].ToString()));
		min_children.push_back(SummarizeCreateAggregate("min", plan.names[i]));
		max_children.push_back(SummarizeCreateAggregate("max", plan.names[i]));
		unique_children.push_back(make_uniq<CastExpression>(
		    LogicalType::BIGINT, SummarizeCreateAggregate("approx_count_distinct", plan.names[i])));
		if (plan.types[i].IsNumeric() || plan.types[i].IsTemporal()) {
			avg_children.push_back(SummarizeCreateAggregate("avg", plan.names[i]));
		} else {
			avg_children.push_back(ConstantExpression::Null());
		}
		if (plan.types[i].IsNumeric()) {
			std_children.push_back(SummarizeCreateAggregate("stddev", plan.names[i]));
		} else {
			std_children.push_back(ConstantExpression::Null());
		}
		if (plan.types[i].IsNumeric() || plan.types[i].IsTemporal()) {
			q25_children.push_back(SummarizeCreateAggregate("approx_quantile", plan.names[i], Value::FLOAT(0.25)));
			q50_children.push_back(SummarizeCreateAggregate("approx_quantile", plan.names[i], Value::FLOAT(0.50)));
			q75_children.push_back(SummarizeCreateAggregate("approx_quantile", plan.names[i], Value::FLOAT(0.75)));
		} else {
			q25_children.push_back(ConstantExpression::Null());
			q50_children.push_back(ConstantExpression::Null());
			q75_children.push_back(ConstantExpression::Null());
		}
		count_children.push_back(SummarizeCreateCountStar());
		null_percentage_children.push_back(SummarizeCreateNullPercentage(plan.names[i]));
	}

	auto select_node = make_uniq<SelectNode>();
	select_node->select_list.push_back(SummarizeWrapUnnest(name_children, "column_name"));
	select_node->select_list.push_back(SummarizeWrapUnnest(type_children, "column_type"));
	select_node->select_list.push_back(SummarizeWrapUnnest(min_children, "min"));
	select_node->select_list.push_back(SummarizeWrapUnnest(max_children, "max"));
	select_node->select_list.push_back(SummarizeWrapUnnest(unique_children, "approx_unique"));
	select_node->select_list.push_back(SummarizeWrapUnnest(avg_children, "avg"));
	select_node->select_list.push_back(SummarizeWrapUnnest(std_children, "std"));
	select_node->select_list.push_back(SummarizeWrapUnnest(q25_children, "q25"));
	select_node->select_list.push_back(SummarizeWrapUnnest(q50_children, "q50"));
	select_node->select_list.push_back(SummarizeWrapUnnest(q75_children, "q75"));
	select_node->select_list.push_back(SummarizeWrapUnnest(count_children, "count"));
	select_node->select_list.push_back(SummarizeWrapUnnest(null_percentage_children, "null_percentage"));
	MoveCorrelatedExpressions(*child_binder);
	select_node->from_table = make_uniq<BoundRefWrapper>(std::move(plan), std::move(child_binder));

	auto select_stmt = make_uniq<SelectStatement>();
	select_stmt->node = std::move(select_node);
	auto subquery = make_uniq<SubqueryRef>(std::move(select_stmt));
	return Bind(*subquery);
}

} // namespace duckdb
