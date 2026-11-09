//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/system_functions.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/function/built_in_functions.hpp"

namespace duckdb {

struct PragmaCollations {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaTableInfo {
	static void GetColumnInfo(TableCatalogEntry &table, const ColumnDefinition &column, DataChunk &output);

	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaStorageInfo {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaMetadataInfo {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaVersion {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaPlatform {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaDatabaseSize {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSchemasFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBConnectionCountFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBApproxDatabaseCountFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBColumnsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBConstraintsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBCoordinateSystemsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSecretsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBWhichSecretFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBDatabasesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBDependenciesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBDialectsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBGrammarExtensionsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct ActiveGrammarExtensionFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBExtensionsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBExtensionRepositoriesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct RegisterExternalResourceTypeFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct CreateExternalResourceFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DestroyExternalResourceFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct RegisterExternalResourceFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DeregisterExternalResourceFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBExternalResourceTypesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBExternalResourcesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

//! An external resource callback's handle/result column is part of the contract: it must be a MAP. Validate
//! and normalize to MAP(VARCHAR, VARCHAR), so a callback that returns something else gets a clear error
//! instead of crashing when the value is later read or emitted. NULL passes through unchanged.
Value RequireResourceMap(const Value &value, const string &function_name, const string &column);

struct DuckDBPreparedStatementsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBFunctionsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBKeywordsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBLogFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBLogContextFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBIndexesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBLiveQueryMetricsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBMemoryFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBEvictionQueuesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBExternalFileCacheFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBMetricsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBOptimizersFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSecretTypeParametersFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSecretTypesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSequencesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBTriggersFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBSettingsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBTablesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBTableSample {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBTemporaryFilesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBTypesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBVariablesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct DuckDBViewsFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct EnableLoggingFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct EnableProfilingFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct TestType {
	TestType(LogicalType type_p, string name_p)
	    : type(std::move(type_p)), name(std::move(name_p)), min_value(Value::MinimumValue(type)),
	      max_value(Value::MaximumValue(type)) {
	}
	TestType(LogicalType type_p, string name_p, Value min, Value max)
	    : type(std::move(type_p)), name(std::move(name_p)), min_value(std::move(min)), max_value(std::move(max)) {
	}

	LogicalType type;
	string name;
	Value min_value;
	Value max_value;
};

struct TestAllTypesFun {
	static void RegisterFunction(BuiltinFunctions &set);
	static vector<TestType> GetTestTypes(bool large_enum = false, bool large_bignum = false);
};

struct TestVectorTypesFun {
	static void RegisterFunction(BuiltinFunctions &set);
};

struct PragmaUserAgent {
	static void RegisterFunction(BuiltinFunctions &set);
};

} // namespace duckdb
