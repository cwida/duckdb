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
#include "duckdb/common/reference_map.hpp"

namespace duckdb {
class CatalogEntry;
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

	//! The table macros that were expanded to produce this reference, used to detect recursive table macros.
	//! Not serialized and not compared in Equals
	reference_set_t<CatalogEntry> expanded_table_macros;

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
