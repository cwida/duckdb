//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/relation/insert_relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/relation.hpp"

namespace duckdb {

class InsertRelation : public Relation {
public:
	InsertRelation(shared_ptr<Relation> child, string schema_name, string table_name);

	shared_ptr<Relation> child;
	string schema_name;
	string table_name;
	vector<ColumnDefinition> columns;

public:
	BoundStatement Bind(Binder &binder) override;
	const vector<ColumnDefinition> &Columns() override;
	string ToString(idx_t depth) override;
	bool IsReadOnly() override {
		return false;
	}
};

} // namespace duckdb
