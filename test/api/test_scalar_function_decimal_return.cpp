#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/parser/parsed_data/create_scalar_function_info.hpp"

using namespace duckdb;

// f(NULL) folds to a constant before execution, so this body never runs.
static void DecimalReturnExec(DataChunk &args, ExpressionState &state, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::SetNull(result, true);
}

// A NULL argument folds to a constant typed via IsComplete; a concrete DECIMAL return type must survive.
TEST_CASE("A decimal-returning function called with NULL keeps its DECIMAL type", "[api][scalar_function]") {
	DuckDB db(nullptr);
	Connection con(db);

	ScalarFunction fn("decimal_ret", {LogicalType::INTEGER}, LogicalType::DECIMAL(18, 3), DecimalReturnExec);
	CreateScalarFunctionInfo info(fn);
	con.context->RunFunctionInTransaction(
	    [&]() { Catalog::GetSystemCatalog(*con.context).CreateFunction(*con.context, info); });

	auto result = con.Query("SELECT typeof(decimal_ret(NULL))");
	REQUIRE_NO_FAIL(*result);
	REQUIRE(result->Collection().GetValue(0, 0).ToString() == "DECIMAL(18,3)");
}
