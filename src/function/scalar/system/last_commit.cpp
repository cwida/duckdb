#include "duckdb/function/scalar/system_functions.hpp"

#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database_manager.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"

namespace duckdb {

namespace {

struct LastCommitData : FunctionData {
	explicit LastCommitData(Value last_commit_p) : last_commit(std::move(last_commit_p)) {
	}
	Value last_commit;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<LastCommitData>(last_commit);
	}
	bool Equals(const FunctionData &other_p) const override {
		return last_commit == other_p.Cast<LastCommitData>().last_commit;
	}
};

unique_ptr<FunctionData> LastCommitBind(BindScalarFunctionInput &input) {
	// parameter is a foldable, non-null constant - resolve the attached database and read its counter already
	auto database_name = input.GetNonNullConstant(0);
	auto &context = input.GetClientContext();
	auto &db_manager = DatabaseManager::Get(context);
	auto db = db_manager.GetDatabase(context, Identifier(database_name.ToString()));
	if (!db) {
		throw BinderException("last_commit: unknown database \"%s\"", database_name.ToString());
	}
	if (!TransactionManager::Get(*db).IsDuckTransactionManager()) {
		throw InvalidInputException("last_commit: database \"%s\" is not a DuckDB-native attached database",
		                            database_name.ToString());
	}
	auto &transaction_manager = DuckTransactionManager::Get(*db);
	return make_uniq<LastCommitData>(Value::UBIGINT(transaction_manager.GetLastCommit()));
}

void LastCommitFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<LastCommitData>();
	result.Reference(info.last_commit, count_t(args.size()));
}

} // namespace

ScalarFunction LastCommitFun::GetFunction() {
	return ScalarFunction({LogicalType::VARCHAR}, LogicalType::UBIGINT, LastCommitFunction, LastCommitBind, nullptr,
	                      nullptr, LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
}

} // namespace duckdb
