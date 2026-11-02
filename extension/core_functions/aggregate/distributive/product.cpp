#include "core_functions/aggregate/distributive_functions.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/operator/multiply.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/function/aggregate/distributive_function_utils.hpp"
#include "duckdb/function/aggregate_state_layout.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

namespace {

struct ProductState {
	using STATE_TYPE = OptionalStateType<double>;

	double val;
	bool is_set;
};

struct ProductReduce {
	template <class T>
	static T Operation(T left, T right) {
		return MultiplyOperator::template Operation<T, T, T>(left, right);
	}
};

struct ProductFunction : public EmptyValAggregate<ProductReduce, ConstantInit<1>> {
	using EmptyValAggregate<ProductReduce, ConstantInit<1>>::UpdateClusteredLocal;

	template <class INPUT_TYPE, class STATE>
	static void UpdateClusteredLocal(STATE &local, const INPUT_TYPE &input, idx_t count) {
		for (idx_t i = 0; i < count; i++) {
			EmptyValAggregate<ProductReduce, ConstantInit<1>>::template UpdateClusteredLocal<INPUT_TYPE>(local, input);
		}
	}
};

} // namespace

AggregateFunction ProductFun::GetFunction() {
	auto fun = AggregateFunction::UnaryAggregate<ProductState, double, double, ProductFunction>(
	    LogicalType(LogicalTypeId::DOUBLE), LogicalType::DOUBLE);
	fun.GetSignature().GetParameter(0).SetName("arg");
	return fun;
}

} // namespace duckdb
