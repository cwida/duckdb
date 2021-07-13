//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/relation/explain_relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/main/relation.hpp"

namespace duckdb {

class ExplainRelation : public Relation {
public:
	explicit ExplainRelation(shared_ptr<Relation> child);

	shared_ptr<Relation> child;
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
