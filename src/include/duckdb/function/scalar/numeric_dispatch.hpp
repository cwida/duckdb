//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/scalar/numeric_dispatch.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/scalar_function.hpp"

namespace duckdb {

//! How a NUMERIC overload sizes the representation its kernel computes in
enum class NumericDispatchKind : uint8_t {
	//! The representation that holds every possible sum of the operands
	ADD,
	//! The representation that holds every possible difference of the operands (unary: negation)
	SUBTRACT,
	//! The representation that holds every possible product of the operands
	MULTIPLY,
	//! The representation that holds every absolute value of the operand
	ABS,
	//! Rounding to whole numbers: a whole number is its own result, a decimal rounds in its representation
	ROUND,
	//! The representations of the operands as they are
	GENERIC
};

//! NUMERIC overloads of existing functions: at execution the NUMERIC arguments are taken in their run-time
//! representation, the typed overload of the function is bound for that signature (and cached) and executed, and an
//! exact result becomes a NUMERIC vector in the narrowest representation that holds its values.
//!
//! The kernel contract: an overload returning NUMERIC must bind, for every representation, a kernel that computes the
//! exact result (dispatch refuses inexact kernels), and the dispatch kind sizes the representation the kernel runs in
//! so that no result can overflow it. Unsigned representations run their kernels in the signed representation that
//! holds them.
struct NumericDispatch {
	DUCKDB_API static ScalarFunction GetFunction(const string &name, vector<LogicalType> arguments,
	                                             LogicalType return_type, NumericDispatchKind kind);
};

} // namespace duckdb
