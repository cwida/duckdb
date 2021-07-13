//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor_state.hpp"
#include "duckdb/function/function.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"

namespace duckdb {
class BoundFunctionExpression;
class ScalarFunctionCatalogEntry;

//! The type used for scalar functions
typedef std::function<void(DataChunk &, ExpressionState &, Vector &)> scalar_function_t;
//! Binds the scalar function and creates the function data
typedef unique_ptr<FunctionData> (*bind_scalar_function_t)(ClientContext &context, ScalarFunction &bound_function,
                                                           vector<unique_ptr<Expression>> &arguments);
typedef unique_ptr<BaseStatistics> (*function_statistics_t)(ClientContext &context, BoundFunctionExpression &expr,
                                                            FunctionData *bind_data,
                                                            vector<unique_ptr<BaseStatistics>> &child_stats);
//! Adds the dependencies of this BoundFunctionExpression to the set of dependencies
typedef void (*dependency_function_t)(BoundFunctionExpression &expr, unordered_set<CatalogEntry *> &dependencies);

class ScalarFunction : public BaseScalarFunction {
public:
	ScalarFunction(string name, vector<LogicalType> arguments, LogicalType return_type, scalar_function_t function,
	               bool has_side_effects = false, bind_scalar_function_t bind = nullptr,
	               dependency_function_t dependency = nullptr, function_statistics_t statistics = nullptr,
	               LogicalType varargs = LogicalType(LogicalTypeId::INVALID))
	    : BaseScalarFunction(name, arguments, return_type, has_side_effects, varargs), function(function), bind(bind),
	      dependency(dependency), statistics(statistics) {
	}

	ScalarFunction(vector<LogicalType> arguments, LogicalType return_type, scalar_function_t function,
	               bool has_side_effects = false, bind_scalar_function_t bind = nullptr,
	               dependency_function_t dependency = nullptr, function_statistics_t statistics = nullptr,
	               LogicalType varargs = LogicalType(LogicalTypeId::INVALID))
	    : ScalarFunction(string(), arguments, return_type, function, has_side_effects, bind, dependency, statistics,
	                     varargs) {
	}

	//! The main scalar function to execute
	scalar_function_t function;
	//! The bind function (if any)
	bind_scalar_function_t bind;
	// The dependency function (if any)
	dependency_function_t dependency;
	//! The statistics propagation function (if any)
	function_statistics_t statistics;

	static unique_ptr<BoundFunctionExpression> BindScalarFunction(ClientContext &context, const string &schema,
	                                                              const string &name,
	                                                              vector<unique_ptr<Expression>> children,
	                                                              string &error, bool is_operator = false);
	static unique_ptr<BoundFunctionExpression> BindScalarFunction(ClientContext &context,
	                                                              ScalarFunctionCatalogEntry &function,
	                                                              vector<unique_ptr<Expression>> children,
	                                                              string &error, bool is_operator = false);

	static unique_ptr<BoundFunctionExpression> BindScalarFunction(ClientContext &context, ScalarFunction bound_function,
	                                                              vector<unique_ptr<Expression>> children,
	                                                              bool is_operator = false);

	bool operator==(const ScalarFunction &rhs) const {
		return CompareScalarFunctionT(rhs.function) && bind == rhs.bind && dependency == rhs.dependency &&
		       statistics == rhs.statistics;
	}
	bool operator!=(const ScalarFunction &rhs) const {
		return !(*this == rhs);
	}

	bool Equal(const ScalarFunction &rhs) const {
		// number of types
		if (this->arguments.size() != rhs.arguments.size()) {
			return false;
		}
		// argument types
		for (idx_t i = 0; i < this->arguments.size(); ++i) {
			if (this->arguments[i] != rhs.arguments[i]) {
				return false;
			}
		}
		// return type
		if (this->return_type != rhs.return_type) {
			return false;
		}
		// varargs
		if (this->varargs != rhs.varargs) {
			return false;
		}

		return true; // they are equal
	}

private:
	bool CompareScalarFunctionT(const scalar_function_t other) const {
		typedef void(scalar_function_ptr_t)(DataChunk &, ExpressionState &, Vector &);

		auto func_ptr = (scalar_function_ptr_t **)function.template target<scalar_function_ptr_t *>();
		auto other_ptr = (scalar_function_ptr_t **)other.template target<scalar_function_ptr_t *>();

		// Case the functions were created from lambdas the target will return a nullptr
		if (!func_ptr && !other_ptr) {
			return true;
		}
		if (func_ptr == nullptr || other_ptr == nullptr) {
			// scalar_function_t (std::functions) from lambdas cannot be compared
			return false;
		}
		return ((size_t)*func_ptr == (size_t)*other_ptr);
	}

public:
	static void NopFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() >= 1);
		result.Reference(input.data[0]);
	}

	template <class TA, class TR, class OP>
	static void UnaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() >= 1);
		UnaryExecutor::Execute<TA, TR, OP>(input.data[0], result, input.size());
	}

	template <class TA, class TB, class TR, class OP>
	static void BinaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
		D_ASSERT(input.ColumnCount() == 2);
		BinaryExecutor::ExecuteStandard<TA, TB, TR, OP>(input.data[0], input.data[1], result, input.size());
	}

public:
	template <class OP>
	static scalar_function_t GetScalarUnaryFunction(LogicalType type) {
		scalar_function_t function;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			function = &ScalarFunction::UnaryFunction<int8_t, int8_t, OP>;
			break;
		case LogicalTypeId::SMALLINT:
			function = &ScalarFunction::UnaryFunction<int16_t, int16_t, OP>;
			break;
		case LogicalTypeId::INTEGER:
			function = &ScalarFunction::UnaryFunction<int32_t, int32_t, OP>;
			break;
		case LogicalTypeId::BIGINT:
			function = &ScalarFunction::UnaryFunction<int64_t, int64_t, OP>;
			break;
		case LogicalTypeId::UTINYINT:
			function = &ScalarFunction::UnaryFunction<uint8_t, uint8_t, OP>;
			break;
		case LogicalTypeId::USMALLINT:
			function = &ScalarFunction::UnaryFunction<uint16_t, uint16_t, OP>;
			break;
		case LogicalTypeId::UINTEGER:
			function = &ScalarFunction::UnaryFunction<uint32_t, uint32_t, OP>;
			break;
		case LogicalTypeId::UBIGINT:
			function = &ScalarFunction::UnaryFunction<uint64_t, uint64_t, OP>;
			break;
		case LogicalTypeId::HUGEINT:
			function = &ScalarFunction::UnaryFunction<hugeint_t, hugeint_t, OP>;
			break;
		case LogicalTypeId::FLOAT:
			function = &ScalarFunction::UnaryFunction<float, float, OP>;
			break;
		case LogicalTypeId::DOUBLE:
			function = &ScalarFunction::UnaryFunction<double, double, OP>;
			break;
		default:
			throw InternalException("Unimplemented type for GetScalarUnaryFunction");
		}
		return function;
	}

	template <class TR, class OP>
	static scalar_function_t GetScalarUnaryFunctionFixedReturn(LogicalType type) {
		scalar_function_t function;
		switch (type.id()) {
		case LogicalTypeId::TINYINT:
			function = &ScalarFunction::UnaryFunction<int8_t, TR, OP>;
			break;
		case LogicalTypeId::SMALLINT:
			function = &ScalarFunction::UnaryFunction<int16_t, TR, OP>;
			break;
		case LogicalTypeId::INTEGER:
			function = &ScalarFunction::UnaryFunction<int32_t, TR, OP>;
			break;
		case LogicalTypeId::BIGINT:
			function = &ScalarFunction::UnaryFunction<int64_t, TR, OP>;
			break;
		case LogicalTypeId::UTINYINT:
			function = &ScalarFunction::UnaryFunction<uint8_t, TR, OP>;
			break;
		case LogicalTypeId::USMALLINT:
			function = &ScalarFunction::UnaryFunction<uint16_t, TR, OP>;
			break;
		case LogicalTypeId::UINTEGER:
			function = &ScalarFunction::UnaryFunction<uint32_t, TR, OP>;
			break;
		case LogicalTypeId::UBIGINT:
			function = &ScalarFunction::UnaryFunction<uint64_t, TR, OP>;
			break;
		case LogicalTypeId::HUGEINT:
			function = &ScalarFunction::UnaryFunction<hugeint_t, TR, OP>;
			break;
		case LogicalTypeId::FLOAT:
			function = &ScalarFunction::UnaryFunction<float, TR, OP>;
			break;
		case LogicalTypeId::DOUBLE:
			function = &ScalarFunction::UnaryFunction<double, TR, OP>;
			break;
		default:
			throw InternalException("Unimplemented type for GetScalarUnaryFunctionFixedReturn");
		}
		return function;
	}
};

} // namespace duckdb
