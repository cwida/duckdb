//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/relation/delete_relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/relation.hpp"
#include "duckdb/parser/parsed_expression.hpp"

namespace duckdb {

class DeleteRelation : public Relation {
public:
	DeleteRelation(ClientContext &context, unique_ptr<ParsedExpression> condition, string schema_name,
	               string table_name);

	vector<ColumnDefinition> columns;
	unique_ptr<ParsedExpression> condition;
	string schema_name;
	string table_name;

public:
	BoundStatement Bind(Binder &binder) override;
	const vector<ColumnDefinition> &Columns() override;
	string ToString(idx_t depth) override;
	bool IsReadOnly() override {
		return false;
	}
};

} // namespace duckdb
