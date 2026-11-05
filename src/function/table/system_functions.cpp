#include "duckdb/function/table/system_functions.hpp"
#include "duckdb/parser/parsed_data/create_view_info.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

void BuiltinFunctions::RegisterSQLiteFunctions() {
	PragmaVersion::RegisterFunction(*this);
	PragmaPlatform::RegisterFunction(*this);
	PragmaCollations::RegisterFunction(*this);
	PragmaTableInfo::RegisterFunction(*this);
	PragmaStorageInfo::RegisterFunction(*this);
	PragmaMetadataInfo::RegisterFunction(*this);
	PragmaDatabaseSize::RegisterFunction(*this);
	PragmaUserAgent::RegisterFunction(*this);

	DuckDBConnectionCountFun::RegisterFunction(*this);
	DuckDBApproxDatabaseCountFun::RegisterFunction(*this);
	DuckDBColumnsFun::RegisterFunction(*this);
	DuckDBConstraintsFun::RegisterFunction(*this);
	DuckDBCoordinateSystemsFun::RegisterFunction(*this);
	DuckDBDatabasesFun::RegisterFunction(*this);
	DuckDBFunctionsFun::RegisterFunction(*this);
	DuckDBKeywordsFun::RegisterFunction(*this);
	DuckDBPreparedStatementsFun::RegisterFunction(*this);
	DuckDBLogFun::RegisterFunction(*this);
	DuckDBLogContextFun::RegisterFunction(*this);
	DuckDBIndexesFun::RegisterFunction(*this);
	DuckDBSchemasFun::RegisterFunction(*this);
	DuckDBDependenciesFun::RegisterFunction(*this);
	DuckDBDialectsFun::RegisterFunction(*this);
	DuckDBGrammarExtensionsFun::RegisterFunction(*this);
	ActiveGrammarExtensionFun::RegisterFunction(*this);
	DuckDBExtensionsFun::RegisterFunction(*this);
	DuckDBExtensionRepositoriesFun::RegisterFunction(*this);
	RegisterExternalResourceTypeFun::RegisterFunction(*this);
	CreateExternalResourceFun::RegisterFunction(*this);
	DestroyExternalResourceFun::RegisterFunction(*this);
	RegisterExternalResourceFun::RegisterFunction(*this);
	DeregisterExternalResourceFun::RegisterFunction(*this);
	DuckDBExternalResourceTypesFun::RegisterFunction(*this);
	DuckDBExternalResourcesFun::RegisterFunction(*this);
	DuckDBLiveQueryMetricsFun::RegisterFunction(*this);
	DuckDBMemoryFun::RegisterFunction(*this);
	DuckDBEvictionQueuesFun::RegisterFunction(*this);
	DuckDBExternalFileCacheFun::RegisterFunction(*this);
	DuckDBMetricsFun::RegisterFunction(*this);
	DuckDBOptimizersFun::RegisterFunction(*this);
	DuckDBSecretsFun::RegisterFunction(*this);
	DuckDBWhichSecretFun::RegisterFunction(*this);
	DuckDBSecretTypeParametersFun::RegisterFunction(*this);
	DuckDBSecretTypesFun::RegisterFunction(*this);
	DuckDBSequencesFun::RegisterFunction(*this);
	DuckDBTriggersFun::RegisterFunction(*this);
	DuckDBSettingsFun::RegisterFunction(*this);
	DuckDBTablesFun::RegisterFunction(*this);
	DuckDBTableSample::RegisterFunction(*this);
	DuckDBTemporaryFilesFun::RegisterFunction(*this);
	DuckDBTypesFun::RegisterFunction(*this);
	DuckDBVariablesFun::RegisterFunction(*this);
	DuckDBViewsFun::RegisterFunction(*this);
	EnableLoggingFun::RegisterFunction(*this);
	EnableProfilingFun::RegisterFunction(*this);
	TestAllTypesFun::RegisterFunction(*this);
	TestVectorTypesFun::RegisterFunction(*this);
}

} // namespace duckdb
