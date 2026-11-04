//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/relation/table_function_relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/relation.hpp"

namespace duckdb {
struct TableFunctionInfo;

class TableFunctionRelation : public Relation {
public:
	TableFunctionRelation(const shared_ptr<ClientContext> &context, string name, vector<Value> parameters,
	                      named_parameter_map_t named_parameters, shared_ptr<Relation> input_relation_p = nullptr,
	                      bool auto_init = true, shared_ptr<TableFunctionInfo> bind_info = nullptr);
	TableFunctionRelation(const shared_ptr<RelationContextWrapper> &context, string name, vector<Value> parameters,
	                      named_parameter_map_t named_parameters, shared_ptr<Relation> input_relation_p = nullptr,
	                      bool auto_init = true, shared_ptr<TableFunctionInfo> bind_info = nullptr);
	TableFunctionRelation(const shared_ptr<ClientContext> &context, string name, vector<Value> parameters,
	                      shared_ptr<Relation> input_relation_p = nullptr, bool auto_init = true,
	                      shared_ptr<TableFunctionInfo> bind_info = nullptr);
	~TableFunctionRelation() override {
	}

	Identifier name;
	vector<Value> parameters;
	named_parameter_map_t named_parameters;
	vector<ColumnDefinition> columns;
	shared_ptr<Relation> input_relation;
	shared_ptr<TableFunctionInfo> bind_info;

public:
	unique_ptr<QueryNode> GetQueryNode() override;
	unique_ptr<TableRef> GetTableRef() override;

	const vector<ColumnDefinition> &Columns() override;
	string ToString(idx_t depth) override;
	Identifier GetAlias() override;
	void AddNamedParameter(const string &name, Value argument);
	void RemoveNamedParameterIfExists(const string &name);
	void SetNamedParameters(named_parameter_map_t &&named_parameters);

private:
	void InitializeColumns();

private:
	//! Whether or not to auto initialize the columns on construction
	bool auto_initialize;
};

} // namespace duckdb
