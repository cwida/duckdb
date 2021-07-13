//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/enums/relation_type.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/query_result.hpp"
#include "duckdb/parser/column_definition.hpp"

#include <memory>

namespace duckdb {
struct BoundStatement;

class ClientContext;
class Binder;
class LogicalOperator;
class QueryNode;
class TableRef;

class Relation : public std::enable_shared_from_this<Relation> {
public:
	DUCKDB_API Relation(ClientContext &context, RelationType type) : context(context), type(type) {
	}
	DUCKDB_API virtual ~Relation() {
	}

	ClientContext &context;
	RelationType type;

public:
	DUCKDB_API virtual const vector<ColumnDefinition> &Columns() = 0;
	DUCKDB_API virtual unique_ptr<QueryNode> GetQueryNode() {
		throw InternalException("Cannot create a query node from this node type");
	}
	DUCKDB_API virtual BoundStatement Bind(Binder &binder);
	DUCKDB_API virtual string GetAlias();

	DUCKDB_API unique_ptr<QueryResult> Execute();
	DUCKDB_API string ToString();
	DUCKDB_API virtual string ToString(idx_t depth) = 0;

	DUCKDB_API void Print();
	DUCKDB_API void Head(idx_t limit = 10);

	DUCKDB_API shared_ptr<Relation> CreateView(const string &name, bool replace = true, bool temporary = false);
	DUCKDB_API unique_ptr<QueryResult> Query(const string &sql);
	DUCKDB_API unique_ptr<QueryResult> Query(const string &name, const string &sql);

	//! Explain the query plan of this relation
	DUCKDB_API unique_ptr<QueryResult> Explain();

	DUCKDB_API virtual unique_ptr<TableRef> GetTableRef();
	DUCKDB_API virtual bool IsReadOnly() {
		return true;
	}

public:
	// PROJECT
	DUCKDB_API shared_ptr<Relation> Project(const string &select_list);
	DUCKDB_API shared_ptr<Relation> Project(const string &expression, const string &alias);
	DUCKDB_API shared_ptr<Relation> Project(const string &select_list, const vector<string> &aliases);
	DUCKDB_API shared_ptr<Relation> Project(const vector<string> &expressions);
	DUCKDB_API shared_ptr<Relation> Project(const vector<string> &expressions, const vector<string> &aliases);

	// FILTER
	DUCKDB_API shared_ptr<Relation> Filter(const string &expression);
	DUCKDB_API shared_ptr<Relation> Filter(const vector<string> &expressions);

	// LIMIT
	DUCKDB_API shared_ptr<Relation> Limit(int64_t n, int64_t offset = 0);

	// ORDER
	DUCKDB_API shared_ptr<Relation> Order(const string &expression);
	DUCKDB_API shared_ptr<Relation> Order(const vector<string> &expressions);

	// JOIN operation
	DUCKDB_API shared_ptr<Relation> Join(const shared_ptr<Relation> &other, const string &condition,
	                                     JoinType type = JoinType::INNER);

	// SET operations
	DUCKDB_API shared_ptr<Relation> Union(const shared_ptr<Relation> &other);
	DUCKDB_API shared_ptr<Relation> Except(const shared_ptr<Relation> &other);
	DUCKDB_API shared_ptr<Relation> Intersect(const shared_ptr<Relation> &other);

	// DISTINCT operation
	DUCKDB_API shared_ptr<Relation> Distinct();

	// AGGREGATES
	DUCKDB_API shared_ptr<Relation> Aggregate(const string &aggregate_list);
	DUCKDB_API shared_ptr<Relation> Aggregate(const vector<string> &aggregates);
	DUCKDB_API shared_ptr<Relation> Aggregate(const string &aggregate_list, const string &group_list);
	DUCKDB_API shared_ptr<Relation> Aggregate(const vector<string> &aggregates, const vector<string> &groups);

	// ALIAS
	DUCKDB_API shared_ptr<Relation> Alias(const string &alias);

	//! Insert the data from this relation into a table
	DUCKDB_API void Insert(const string &table_name);
	DUCKDB_API void Insert(const string &schema_name, const string &table_name);
	//! Insert a row (i.e.,list of values) into a table
	DUCKDB_API void Insert(const vector<vector<Value>> &values);
	//! Create a table and insert the data from this relation into that table
	DUCKDB_API void Create(const string &table_name);
	DUCKDB_API void Create(const string &schema_name, const string &table_name);

	//! Write a relation to a CSV file
	DUCKDB_API void WriteCSV(const string &csv_file);

	//! Update a table, can only be used on a TableRelation
	DUCKDB_API virtual void Update(const string &update, const string &condition = string());
	//! Delete from a table, can only be used on a TableRelation
	DUCKDB_API virtual void Delete(const string &condition = string());
	//! Create a relation from calling a table in/out function on the input relation
	DUCKDB_API shared_ptr<Relation> TableFunction(const std::string &fname, vector<Value> values);

public:
	//! Whether or not the relation inherits column bindings from its child or not, only relevant for binding
	DUCKDB_API virtual bool InheritsColumnBindings() {
		return false;
	}
	DUCKDB_API virtual Relation *ChildRelation() {
		return nullptr;
	}

protected:
	DUCKDB_API string RenderWhitespace(idx_t depth);
};

} // namespace duckdb