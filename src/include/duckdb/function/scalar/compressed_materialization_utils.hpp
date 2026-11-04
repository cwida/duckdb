//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/compressed_materialization_utils.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/built_in_functions.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

enum class CMExpressionType : uint8_t { NONE, CAST, COMPRESS, DECOMPRESS };

struct CMUtils {
	//! The types we compress integral types to
	static const vector<LogicalType> IntegralTypes();
	//! The types we compress strings to
	static const vector<LogicalType> StringTypes();

	static unique_ptr<FunctionData> Bind(BindScalarFunctionInput &input);
	static CMExpressionType GetExpressionType(const BoundFunctionExpression &expression);
	//! The input wrapped by a compressed materialization expression, or nullptr for any other expression
	static optional_ptr<const Expression> GetWrappedInput(const Expression &expression);
	static void MarkCast(BoundFunctionExpression &expression);

private:
	static CMExpressionType GetIntegralType(const BoundScalarFunction &function);
	static CMExpressionType GetStringType(const BoundScalarFunction &function);
	static CMExpressionType GetGeometryType(const BoundScalarFunction &function);
};

//! Needed for (de)serialization without binding
enum class CompressedMaterializationDirection : uint8_t { INVALID = 0, COMPRESS = 1, DECOMPRESS = 2 };

struct CMIntegralCompressFun {
	static ScalarFunction GetFunction(const LogicalType &input_type, const LogicalType &result_type);
};

struct CMIntegralDecompressFun {
	static ScalarFunction GetFunction(const LogicalType &input_type, const LogicalType &result_type);
};

struct CMStringCompressFun {
	static ScalarFunction GetFunction(const LogicalType &result_type);
};

struct CMStringDecompressFun {
	static ScalarFunction GetFunction(const LogicalType &input_type);
};

struct CMGeometryPointCompressFun {
	static ScalarFunction GetFunction();
};

struct CMGeometryPointDecompressFun {
	static ScalarFunction GetFunction();
};

} // namespace duckdb
