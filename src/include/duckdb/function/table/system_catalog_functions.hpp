//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/system_catalog_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"

namespace duckdb {

class LogicalGet;
class SchemaCatalogEntry;

struct SystemCatalogScanBindData : public TableFunctionData {
	string catalog;

	unique_ptr<FunctionData> Copy() const override;
	bool Equals(const FunctionData &other) const override;
};

struct SystemCatalogScanFunction {
	static constexpr const char *DATABASE_NAME_COLUMN = "database_name";

	static unique_ptr<FunctionData> Bind();
	static vector<reference<SchemaCatalogEntry>> GetSchemas(ClientContext &context,
	                                                        optional_ptr<const FunctionData> bind_data);
	static void PushdownComplexFilter(ClientContext &context, LogicalGet &get, FunctionData *bind_data,
	                                  vector<unique_ptr<Expression>> &filters);
	static InsertionOrderPreservingMap<string> ToString(TableFunctionToStringInput &input);
	static void Serialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
	                      const TableFunction &function);
	static unique_ptr<FunctionData> Deserialize(Deserializer &deserializer, TableFunction &function);
	static void Register(TableFunction &function);
};

} // namespace duckdb
