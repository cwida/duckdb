//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/basetableref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/tableref.hpp"
#include "duckdb/common/vector.hpp"

namespace duckdb {
//! Represents a TableReference to a base table in the schema
class BaseTableRef : public TableRef {
public:
	BaseTableRef() : TableRef(TableReferenceType::BASE_TABLE), schema_name(INVALID_SCHEMA) {
	}

	//! Schema name
	string schema_name;
	//! Table name
	string table_name;
	//! Alises for the column names
	vector<string> column_name_alias;

public:
	string ToString() const override;
	bool Equals(const TableRef *other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Serializes a blob into a BaseTableRef
	void Serialize(Serializer &serializer) override;
	//! Deserializes a blob back into a BaseTableRef
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
