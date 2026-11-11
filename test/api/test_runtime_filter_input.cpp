#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"

using namespace duckdb;

namespace {

struct RuntimeFilterCastData : public BoundCastData {
	explicit RuntimeFilterCastData(int64_t offset_p) : offset(offset_p) {
	}

	int64_t offset;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<RuntimeFilterCastData>(offset);
	}
};

bool RuntimeFilterOffsetCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto offset = parameters.cast_data->Cast<RuntimeFilterCastData>().offset;
	UnaryExecutor::Execute<int32_t, int64_t>(source, result, count,
	                                         [offset](int32_t value) { return int64_t(value) + offset; });
	return true;
}

} // namespace

TEST_CASE("Runtime filter input preserves the original bound cast", "[api][runtime_filter_input]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto reference =
	    make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, ColumnBinding(TableIndex(0), ProjectionIndex(0)));
	BoundCastInfo cast(RuntimeFilterOffsetCast, make_uniq<RuntimeFilterCastData>(7));
	cast.SetStability(FunctionStability::CONSISTENT);
	auto expression = BoundCastExpression::Create(std::move(reference), LogicalType::BIGINT, std::move(cast));
	JoinFilterPushdownColumn column;
	REQUIRE(JoinFilterPushdownUtil::PushdownJoinFilterExpression(*expression, column));
	column.storage_type = LogicalType::INTEGER;
	REQUIRE(RuntimeFilterCastUtil::RequiresRuntimeFilterExpressionReconstruction(column, LogicalType::BIGINT));
	REQUIRE_FALSE(RuntimeFilterCastUtil::PreservesStorageOrdering(column));
	REQUIRE(RuntimeFilterCastUtil::RuntimeFilterUsesTryCast(column));
	bool preserves_cast_errors;
	auto input = RuntimeFilterCastUtil::CreateRuntimeFilterInputExpression(*con.context, column, preserves_cast_errors);
	REQUIRE(preserves_cast_errors);
	BoundConstantExpression value(Value::INTEGER(10));
	ExpressionFilter::ReplaceExpressionRecursive(input, value);
	REQUIRE(ExpressionExecutor::EvaluateScalar(*con.context, *input) == Value::BIGINT(17));
}
