//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/tableref/table_function_ref.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tableref.hpp"
#include "duckdb/parser/statement/select_statement.hpp"
#include "duckdb/common/enums/ordinality_request_type.hpp"

namespace duckdb {
struct TableFunctionInfo;

//! Represents a Table producing function
class TableFunctionRef : public TableRef {
public:
	static constexpr const TableReferenceType TYPE = TableReferenceType::TABLE_FUNCTION;

public:
	DUCKDB_API TableFunctionRef();

	unique_ptr<ParsedExpression> function;

	//! Process-local input for this invocation, retained when copying the reference
	shared_ptr<TableFunctionInfo> bind_info;

	// if the function takes a subquery as argument its in here
	unique_ptr<SelectStatement> subquery;

	//! Whether or not WITH ORDINALITY has been invoked
	OrdinalityType with_ordinality = OrdinalityType::WITHOUT_ORDINALITY;

public:
	string ToString() const override;

	bool Equals(const TableRef &other_p) const override;

	unique_ptr<TableRef> Copy() override;

	//! Deserializes a blob back into a BaseTableRef
	const unique_ptr<ParsedExpression> &SerializableFunction() const;
	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableRef> Deserialize(Deserializer &source);
};
} // namespace duckdb
