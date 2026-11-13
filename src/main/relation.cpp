#include "duckdb/main/relation.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/relation/aggregate_relation.hpp"
#include "duckdb/main/relation/cross_product_relation.hpp"
#include "duckdb/main/relation/distinct_relation.hpp"
#include "duckdb/main/relation/explain_relation.hpp"
#include "duckdb/main/relation/filter_relation.hpp"
#include "duckdb/main/relation/insert_relation.hpp"
#include "duckdb/main/relation/limit_relation.hpp"
#include "duckdb/main/relation/order_relation.hpp"
#include "duckdb/main/relation/projection_relation.hpp"
#include "duckdb/main/relation/setop_relation.hpp"
#include "duckdb/main/relation/subquery_relation.hpp"
#include "duckdb/main/relation/table_function_relation.hpp"
#include "duckdb/main/relation/create_table_relation.hpp"
#include "duckdb/main/relation/create_view_relation.hpp"
#include "duckdb/main/relation/write_csv_relation.hpp"
#include "duckdb/main/relation/write_parquet_relation.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/parser/expression/conjunction_expression.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/main/relation/join_relation.hpp"
#include "duckdb/main/relation/value_relation.hpp"
#include "duckdb/parser/statement/explain_statement.hpp"

namespace duckdb {

shared_ptr<Relation> Relation::Project(const string &select_list) {
	return Project(select_list, vector<string>());
}

shared_ptr<Relation> Relation::Project(const string &expression, const string &alias) {
	return Project(expression, vector<string>({alias}));
}

shared_ptr<Relation> Relation::Project(const string &select_list, const vector<string> &aliases) {
	auto expressions = Parser(*context->GetContext()).ParseExpressionList(select_list);
	return make_shared_ptr<ProjectionRelation>(shared_from_this(), std::move(expressions),
	                                           StringsToIdentifiers(aliases));
}

shared_ptr<Relation> Relation::Project(const vector<string> &expressions) {
	vector<string> aliases;
	return Project(expressions, aliases);
}

shared_ptr<Relation> Relation::Project(vector<unique_ptr<ParsedExpression>> expressions,
                                       const vector<string> &aliases) {
	return make_shared_ptr<ProjectionRelation>(shared_from_this(), std::move(expressions),
	                                           StringsToIdentifiers(aliases));
}

static vector<unique_ptr<ParsedExpression>> StringListToExpressionList(ClientContext &context,
                                                                       const vector<string> &expressions) {
	if (expressions.empty()) {
		throw ParserException("Zero expressions provided");
	}
	vector<unique_ptr<ParsedExpression>> result_list;
	for (auto &expr : expressions) {
		auto expression_list = Parser(context).ParseExpressionList(expr);
		if (expression_list.size() != 1) {
			throw ParserException("Expected a single expression in the expression list");
		}
		result_list.push_back(std::move(expression_list[0]));
	}
	return result_list;
}

shared_ptr<Relation> Relation::Project(const vector<string> &expressions, const vector<string> &aliases) {
	auto result_list = StringListToExpressionList(*context->GetContext(), expressions);
	return make_shared_ptr<ProjectionRelation>(shared_from_this(), std::move(result_list),
	                                           StringsToIdentifiers(aliases));
}

shared_ptr<Relation> Relation::Filter(const string &expression) {
	auto expression_list = Parser(*context->GetContext()).ParseExpressionList(expression);
	if (expression_list.size() != 1) {
		throw ParserException("Expected a single expression as filter condition");
	}
	return Filter(std::move(expression_list[0]));
}

shared_ptr<Relation> Relation::Filter(unique_ptr<ParsedExpression> expression) {
	return make_shared_ptr<FilterRelation>(shared_from_this(), std::move(expression));
}

shared_ptr<Relation> Relation::Filter(const vector<string> &expressions) {
	// if there are multiple expressions, we AND them together
	auto expression_list = StringListToExpressionList(*context->GetContext(), expressions);
	D_ASSERT(!expression_list.empty());

	auto expr = std::move(expression_list[0]);
	for (idx_t i = 1; i < expression_list.size(); i++) {
		expr = make_uniq<ConjunctionExpression>(ExpressionType::CONJUNCTION_AND, std::move(expr),
		                                        std::move(expression_list[i]));
	}
	return make_shared_ptr<FilterRelation>(shared_from_this(), std::move(expr));
}

shared_ptr<Relation> Relation::Limit(int64_t limit, int64_t offset) {
	return make_shared_ptr<LimitRelation>(shared_from_this(), limit, offset);
}

shared_ptr<Relation> Relation::Order(const string &expression) {
	auto order_list = Parser(*context->GetContext()).ParseOrderList(expression);
	return Order(std::move(order_list));
}

shared_ptr<Relation> Relation::Order(vector<OrderByNode> order_list) {
	return make_shared_ptr<OrderRelation>(shared_from_this(), std::move(order_list));
}

shared_ptr<Relation> Relation::Order(const vector<string> &expressions) {
	if (expressions.empty()) {
		throw ParserException("Zero ORDER BY expressions provided");
	}
	vector<OrderByNode> order_list;
	for (auto &expression : expressions) {
		auto inner_list = Parser(*context->GetContext()).ParseOrderList(expression);
		if (inner_list.size() != 1) {
			throw ParserException("Expected a single ORDER BY expression in the expression list");
		}
		order_list.push_back(std::move(inner_list[0]));
	}
	return Order(std::move(order_list));
}

shared_ptr<Relation> Relation::Join(const shared_ptr<Relation> &other, const string &condition, JoinType type,
                                    JoinRefType ref_type) {
	auto expression_list = Parser(*context->GetContext()).ParseExpressionList(condition);
	D_ASSERT(!expression_list.empty());
	return Join(other, std::move(expression_list), type, ref_type);
}

shared_ptr<Relation> Relation::Join(const shared_ptr<Relation> &other,
                                    vector<unique_ptr<ParsedExpression>> expression_list, JoinType type,
                                    JoinRefType ref_type) {
	if (expression_list.size() > 1 || expression_list[0]->GetExpressionType() == ExpressionType::COLUMN_REF) {
		// multiple columns or single column ref: the condition is a USING list
		vector<Identifier> using_columns;
		for (auto &expr : expression_list) {
			if (expr->GetExpressionType() != ExpressionType::COLUMN_REF) {
				throw ParserException("Expected a single expression as join condition");
			}
			auto &colref = expr->Cast<ColumnRefExpression>();
			if (colref.IsQualified()) {
				throw ParserException("Expected unqualified column for column in USING clause");
			}
			using_columns.push_back(colref.ColumnNames()[0]);
		}
		return make_shared_ptr<JoinRelation>(shared_from_this(), other, std::move(using_columns), type, ref_type);
	} else {
		// single expression that is not a column reference: use the expression as a join condition
		return make_shared_ptr<JoinRelation>(shared_from_this(), other, std::move(expression_list[0]), type, ref_type);
	}
}

shared_ptr<Relation> Relation::CrossProduct(const shared_ptr<Relation> &other, JoinRefType join_ref_type) {
	return make_shared_ptr<CrossProductRelation>(shared_from_this(), other, join_ref_type);
}

shared_ptr<Relation> Relation::Union(const shared_ptr<Relation> &other) {
	return make_shared_ptr<SetOpRelation>(shared_from_this(), other, SetOperationType::UNION, true);
}

shared_ptr<Relation> Relation::Except(const shared_ptr<Relation> &other) {
	return make_shared_ptr<SetOpRelation>(shared_from_this(), other, SetOperationType::EXCEPT, true);
}

shared_ptr<Relation> Relation::Intersect(const shared_ptr<Relation> &other) {
	return make_shared_ptr<SetOpRelation>(shared_from_this(), other, SetOperationType::INTERSECT, true);
}

shared_ptr<Relation> Relation::Distinct() {
	return make_shared_ptr<DistinctRelation>(shared_from_this());
}

shared_ptr<Relation> Relation::Alias(const string &alias) {
	return make_shared_ptr<SubqueryRelation>(shared_from_this(), alias);
}

shared_ptr<Relation> Relation::Aggregate(const string &aggregate_list) {
	auto expression_list = Parser(*context->GetContext()).ParseExpressionList(aggregate_list);
	return make_shared_ptr<AggregateRelation>(shared_from_this(), std::move(expression_list));
}

shared_ptr<Relation> Relation::Aggregate(vector<unique_ptr<ParsedExpression>> expressions) {
	return make_shared_ptr<AggregateRelation>(shared_from_this(), std::move(expressions));
}

shared_ptr<Relation> Relation::Aggregate(const string &aggregate_list, const string &group_list) {
	auto expression_list = Parser(*context->GetContext()).ParseExpressionList(aggregate_list);
	auto groups = Parser(*context->GetContext()).ParseGroupByList(group_list);
	return make_shared_ptr<AggregateRelation>(shared_from_this(), std::move(expression_list), std::move(groups));
}

shared_ptr<Relation> Relation::Aggregate(const vector<string> &aggregates) {
	auto aggregate_list = StringListToExpressionList(*context->GetContext(), aggregates);
	return make_shared_ptr<AggregateRelation>(shared_from_this(), std::move(aggregate_list));
}

shared_ptr<Relation> Relation::Aggregate(const vector<string> &aggregates, const vector<string> &groups) {
	auto aggregate_list = StringUtil::Join(aggregates, ", ");
	auto group_list = StringUtil::Join(groups, ", ");
	return this->Aggregate(aggregate_list, group_list);
}

shared_ptr<Relation> Relation::Aggregate(vector<unique_ptr<ParsedExpression>> expressions, const string &group_list) {
	auto groups = Parser(*context->GetContext()).ParseGroupByList(group_list);
	return make_shared_ptr<AggregateRelation>(shared_from_this(), std::move(expressions), std::move(groups));
}

Identifier Relation::GetAlias() {
	return Identifier(alias);
}

unique_ptr<TableRef> Relation::GetTableRef() {
	auto select = make_uniq<SelectStatement>();
	select->node = GetQueryNode();
	return make_uniq<SubqueryRef>(std::move(select), GetAlias());
}

unique_ptr<QueryResult> Relation::Execute() {
	return context->GetContext()->Execute(shared_from_this());
}

unique_ptr<QueryResult> Relation::ExecuteOrThrow() {
	auto res = Execute();
	D_ASSERT(res);
	res->ThrowIfError();
	return res;
}

BoundStatement Relation::Bind(Binder &binder) {
	SelectStatement stmt;
	stmt.node = GetQueryNode();
	return binder.Bind(stmt.Cast<SQLStatement>());
}

shared_ptr<Relation> Relation::InsertRel(const Identifier &schema_name, const Identifier &table_name) {
	return InsertRel(Identifier::InvalidCatalog(), schema_name, table_name);
}

shared_ptr<Relation> Relation::InsertRel(const Identifier &catalog_name, const Identifier &schema_name,
                                         const Identifier &table_name) {
	return make_shared_ptr<InsertRelation>(shared_from_this(), catalog_name, schema_name, table_name);
}

void Relation::Insert(const Identifier &table_name) {
	Insert(Identifier::InvalidSchema(), table_name);
}

void Relation::Insert(const Identifier &schema_name, const Identifier &table_name) {
	Insert(Identifier::InvalidCatalog(), schema_name, table_name);
}

void Relation::Insert(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &table_name) {
	auto insert = InsertRel(catalog_name, schema_name, table_name);
	auto res = insert->Execute();
	res->ThrowIfError("Failed to insert into table '" + table_name + "': ");
}

void Relation::Insert(const vector<vector<Value>> &values) {
	throw InvalidInputException("INSERT with values can only be used on base tables!");
}

void Relation::Insert(vector<vector<unique_ptr<ParsedExpression>>> &&expressions) {
	(void)std::move(expressions);
	throw InvalidInputException("INSERT with expressions can only be used on base tables!");
}

shared_ptr<Relation> Relation::CreateRel(const Identifier &schema_name, const Identifier &table_name, bool temporary,
                                         OnCreateConflict on_conflict) {
	return CreateRel(Identifier::InvalidCatalog(), schema_name, table_name, temporary, on_conflict);
}

shared_ptr<Relation> Relation::CreateRel(const Identifier &catalog_name, const Identifier &schema_name,
                                         const Identifier &table_name, bool temporary, OnCreateConflict on_conflict) {
	return make_shared_ptr<CreateTableRelation>(shared_from_this(), catalog_name, schema_name, table_name, temporary,
	                                            on_conflict);
}

void Relation::Create(const Identifier &table_name, bool temporary, OnCreateConflict on_conflict) {
	Create(Identifier::InvalidCatalog(), Identifier::InvalidSchema(), table_name, temporary, on_conflict);
}

void Relation::Create(const Identifier &schema_name, const Identifier &table_name, bool temporary,
                      OnCreateConflict on_conflict) {
	Create(Identifier::InvalidCatalog(), schema_name, table_name, temporary, on_conflict);
}

void Relation::Create(const Identifier &catalog_name, const Identifier &schema_name, const Identifier &table_name,
                      bool temporary, OnCreateConflict on_conflict) {
	if (table_name.empty()) {
		throw ParserException("Empty table name not supported");
	}
	auto create = CreateRel(catalog_name, schema_name, table_name, temporary, on_conflict);
	auto res = create->Execute();
	res->ThrowIfError("Failed to create table '" + table_name + "': ");
}

shared_ptr<Relation> Relation::WriteCSVRel(const string &csv_file, identifier_map_t<vector<Value>> options) {
	return make_shared_ptr<duckdb::WriteCSVRelation>(shared_from_this(), csv_file, std::move(options));
}

void Relation::WriteCSV(const string &csv_file, identifier_map_t<vector<Value>> options) {
	auto write_csv = WriteCSVRel(csv_file, std::move(options));
	auto res = write_csv->Execute();
	res->ThrowIfError("Failed to write '" + csv_file + "': ");
}

shared_ptr<Relation> Relation::WriteParquetRel(const string &parquet_file, identifier_map_t<vector<Value>> options) {
	auto write_parquet =
	    make_shared_ptr<duckdb::WriteParquetRelation>(shared_from_this(), parquet_file, std::move(options));
	return std::move(write_parquet);
}

void Relation::WriteParquet(const string &parquet_file, identifier_map_t<vector<Value>> options) {
	auto write_parquet = WriteParquetRel(parquet_file, std::move(options));
	auto res = write_parquet->Execute();
	res->ThrowIfError("Failed to write '" + parquet_file + "': ");
}

shared_ptr<Relation> Relation::CreateView(const Identifier &name, bool replace, bool temporary) {
	return CreateView(Identifier::InvalidSchema(), name, replace, temporary);
}

shared_ptr<Relation> Relation::CreateView(const Identifier &schema_name, const Identifier &name, bool replace,
                                          bool temporary) {
	auto view = make_shared_ptr<CreateViewRelation>(shared_from_this(), schema_name, name, replace, temporary);
	auto res = view->Execute();
	res->ThrowIfError("Failed to create view '" + name + "': ");
	return shared_from_this();
}

unique_ptr<QueryResult> Relation::Query(const string &sql) const {
	return context->GetContext()->Query(sql, QueryParameters());
}

unique_ptr<QueryResult> Relation::Query(const Identifier &name, const string &sql) {
	bool replace = true;
	bool temp = IsReadOnly();
	CreateView(name, replace, temp);
	return Query(sql);
}

unique_ptr<QueryResult> Relation::Explain(ExplainType type, const ProfilerPrintFormat &format) {
	auto explain = make_shared_ptr<ExplainRelation>(shared_from_this(), type, format);
	return explain->Execute();
}

void Relation::TryBindRelation(vector<ColumnDefinition> &columns) {
	context->TryBindRelation(*this, columns);
}

void Relation::Update(const string &update, const string &condition) {
	throw InvalidInputException("UPDATE can only be used on base tables!");
}

void Relation::Update(vector<string>, // NOLINT: unused variable / copied on every invocation ...
                      vector<unique_ptr<ParsedExpression>> &&update, // NOLINT: unused variable
                      unique_ptr<ParsedExpression> condition) {      // NOLINT: unused variable
	(void)std::move(update);
	(void)std::move(condition);
	throw InvalidInputException("UPDATE can only be used on base tables!");
}

void Relation::Delete(const string &condition) {
	throw InvalidInputException("DELETE can only be used on base tables!");
}

shared_ptr<Relation> Relation::TableFunction(const std::string &fname, const vector<Value> &values,
                                             const named_parameter_map_t &named_parameters) {
	return make_shared_ptr<TableFunctionRelation>(context->GetContext(), fname, values, named_parameters,
	                                              shared_from_this());
}

shared_ptr<Relation> Relation::TableFunction(const std::string &fname, const vector<Value> &values) {
	return make_shared_ptr<TableFunctionRelation>(context->GetContext(), fname, values, shared_from_this());
}

string Relation::ToString() {
	string str;
	str += "---------------------\n";
	str += "--- Relation Tree ---\n";
	str += "---------------------\n";
	str += ToString(0);
	str += "\n\n";
	str += "---------------------\n";
	str += "-- Result Columns  --\n";
	str += "---------------------\n";
	auto &cols = Columns();
	for (idx_t i = 0; i < cols.size(); i++) {
		str += "- " + cols[i].Name() + " (" + cols[i].Type().ToString() + ")\n";
	}
	return str;
}

// LCOV_EXCL_START
string Relation::GetQuery() {
	return GetQueryNode()->ToString();
}

void Relation::Head(idx_t limit) {
	auto limit_node = Limit(NumericCast<int64_t>(limit));
	limit_node->Execute()->Print();
}
// LCOV_EXCL_STOP

void Relation::Print() {
	Printer::Print(ToString());
}

string Relation::RenderWhitespace(idx_t depth) {
	return string(depth * 2, ' ');
}

void Relation::AddExternalDependency(shared_ptr<ExternalDependency> dependency) {
	external_dependencies.push_back(std::move(dependency));
}

vector<shared_ptr<ExternalDependency>> Relation::GetAllDependencies() {
	vector<shared_ptr<ExternalDependency>> all_dependencies;
	Relation *cur = this;
	while (cur) {
		for (auto &dep : cur->external_dependencies) {
			all_dependencies.push_back(dep);
		}
		cur = cur->ChildRelation();
	}
	return all_dependencies;
}

} // namespace duckdb
