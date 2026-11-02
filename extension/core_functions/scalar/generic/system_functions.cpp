#include "duckdb/catalog/catalog_search_path.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "core_functions/scalar/generic_functions.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/transaction/meta_transaction.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/execution/expression_executor.hpp"

namespace duckdb {

namespace {

// current_query
void CurrentQueryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Value val(state.GetContext().GetCurrentQuery());
	result.Reference(val, count_t(input.size()));
}

// current_schema
void CurrentSchemaFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Value val(ClientData::Get(state.GetContext()).catalog_search_path->GetDefault().GetSchema());
	result.Reference(val, count_t(input.size()));
}

// current_database
void CurrentDatabaseFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	Value val(DatabaseManager::GetDefaultDatabase(state.GetContext()));
	result.Reference(val, count_t(input.size()));
}

struct CurrentSchemasBindData : public FunctionData {
	explicit CurrentSchemasBindData(Value result_value) : result(std::move(result_value)) {
	}

	Value result;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<CurrentSchemasBindData>(result);
	}
	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<CurrentSchemasBindData>();
		return Value::NotDistinctFrom(result, other.result);
	}
};

void CurrentSchemasResolveTypes(ResolveScalarFunctionTypesInput &input) {
	if (input.GetArgumentType(0).id() != LogicalTypeId::BOOLEAN) {
		throw BinderException("current_schemas requires a boolean input");
	}
}

unique_ptr<FunctionData> CurrentSchemasBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	Value schema_value = input.GetConstant(0);
	Value result_val;
	if (schema_value.IsNull()) {
		// null
		result_val = Value(LogicalType::LIST(LogicalType::VARCHAR));
	} else {
		auto implicit_schemas = BooleanValue::Get(schema_value);
		vector<Value> schema_list;
		auto &catalog_search_path = ClientData::Get(context).catalog_search_path;
		auto &search_path = implicit_schemas ? catalog_search_path->Get() : catalog_search_path->GetSetPaths();
		std::transform(search_path.begin(), search_path.end(), std::back_inserter(schema_list),
		               [](const CatalogSearchEntry &s) -> Value { return Value(s.GetSchema()); });
		result_val = Value::LIST(LogicalType::VARCHAR, schema_list);
	}
	return make_uniq<CurrentSchemasBindData>(std::move(result_val));
}

// current_schemas
void CurrentSchemasFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<CurrentSchemasBindData>();
	result.Reference(info.result, count_t(input.size()));
}

// in_search_path
void InSearchPathFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &context = state.GetContext();
	auto &search_path = ClientData::Get(context).catalog_search_path;
	BinaryExecutor::Execute<string_t, string_t, bool>(
	    input.data[0], input.data[1], result, [&](string_t db_name, string_t schema_name) {
		    return search_path->SchemaInSearchPath(context, Identifier(db_name.GetString()),
		                                           Identifier(schema_name.GetString()));
	    });
}

// txid_current
void TransactionIdCurrent(DataChunk &input, ExpressionState &state, Vector &result) {
	auto &transaction = MetaTransaction::Get(state.GetContext());
	auto val = Value::UBIGINT(transaction.global_transaction_id);
	result.Reference(val, count_t(input.size()));
}

// version
void VersionFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto val = Value(DuckDB::LibraryVersion());
	result.Reference(val, count_t(input.size()));
}

} // namespace

ScalarFunction CurrentQueryFun::GetFunction() {
	ScalarFunction current_query({}, LogicalType::VARCHAR, CurrentQueryFunction);
	current_query.SetStability(FunctionStability::VOLATILE);
	return current_query;
}

ScalarFunction CurrentSchemaFun::GetFunction() {
	ScalarFunction current_schema({}, LogicalType::VARCHAR, CurrentSchemaFunction);
	current_schema.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_schema;
}

ScalarFunction CurrentDatabaseFun::GetFunction() {
	ScalarFunction current_database({}, LogicalType::VARCHAR, CurrentDatabaseFunction);
	current_database.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_database;
}

ScalarFunction CurrentSchemasFun::GetFunction() {
	auto varchar_list_type = LogicalType::LIST(LogicalType::VARCHAR);
	ScalarFunction current_schemas({}, varchar_list_type, CurrentSchemasFunction, CurrentSchemasBind);
	current_schemas.GetSignature().AddParameter("include_implicit", LogicalType::BOOLEAN);
	current_schemas.SetResolveTypesCallback(CurrentSchemasResolveTypes);
	current_schemas.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return current_schemas;
}

ScalarFunction InSearchPathFun::GetFunction() {
	ScalarFunction in_search_path({}, LogicalType::BOOLEAN, InSearchPathFunction);
	in_search_path.GetSignature()
	    .AddParameter("database_name", LogicalType::VARCHAR)
	    .AddParameter("schema_name", LogicalType::VARCHAR);
	in_search_path.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return in_search_path;
}

ScalarFunction CurrentTransactionIdFun::GetFunction() {
	ScalarFunction txid_current({}, LogicalType::UBIGINT, TransactionIdCurrent);
	txid_current.SetStability(FunctionStability::CONSISTENT_WITHIN_QUERY);
	return txid_current;
}

ScalarFunction VersionFun::GetFunction() {
	return ScalarFunction({}, LogicalType::VARCHAR, VersionFunction);
}

} // namespace duckdb
