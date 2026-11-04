//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/pragma_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function.hpp"
#include "duckdb/parser/parsed_data/pragma_info.hpp"
#include "duckdb/common/unordered_map.hpp"

namespace duckdb {
class ClientContext;

//! Return a substitute query to execute instead of this pragma statement
typedef string (*pragma_query_t)(ClientContext &context, const FunctionParameters &parameters);
//! Execute the main pragma function
typedef void (*pragma_function_t)(ClientContext &context, const FunctionParameters &parameters);

struct PragmaFunctionInfo {
	DUCKDB_API virtual ~PragmaFunctionInfo();

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

//! Pragma functions are invoked by calling PRAGMA x
//! Pragma functions come in three types:
//! * Call: function call, e.g. PRAGMA table_info('tbl')
//!   -> call statements can take multiple parameters
//! * Statement: statement without parameters, e.g. PRAGMA show_tables
//!   -> this is similar to a call pragma but without parameters
//! Pragma functions can either return a new query to execute (pragma_query_t)
//! or they can
class PragmaFunction : public SimpleFunction { // NOLINT: work-around bug in clang-tidy
public:
	static constexpr const FunctionKind KIND = FunctionKind::PRAGMA;
	FunctionKind GetKind() const override {
		return KIND;
	}
	// Call
	DUCKDB_API static PragmaFunction PragmaCall(const Identifier &name, pragma_query_t query,
	                                            vector<LogicalType> arguments,
	                                            LogicalType varargs = LogicalType::INVALID);
	DUCKDB_API static PragmaFunction PragmaCall(const Identifier &name, pragma_function_t function,
	                                            vector<LogicalType> arguments,
	                                            LogicalType varargs = LogicalType::INVALID);
	//! Overloads taking a braced list, so that "{}" does not also match FunctionSignature
	static PragmaFunction PragmaCall(const Identifier &name, pragma_query_t query,
	                                 std::initializer_list<LogicalType> arguments,
	                                 LogicalType varargs = LogicalType::INVALID) {
		return PragmaCall(name, query, vector<LogicalType>(arguments), std::move(varargs));
	}
	static PragmaFunction PragmaCall(const Identifier &name, pragma_function_t function,
	                                 std::initializer_list<LogicalType> arguments,
	                                 LogicalType varargs = LogicalType::INVALID) {
		return PragmaCall(name, function, vector<LogicalType>(arguments), std::move(varargs));
	}
	//! The same, declaring the parameters in full - so that they carry their own names and defaults instead of the
	//! synthetic "colN" the type-only overloads give them
	DUCKDB_API static PragmaFunction PragmaCall(const Identifier &name, pragma_query_t query,
	                                            FunctionSignature signature);
	DUCKDB_API static PragmaFunction PragmaCall(const Identifier &name, pragma_function_t function,
	                                            FunctionSignature signature);
	// Statement
	DUCKDB_API static PragmaFunction PragmaStatement(const Identifier &name, pragma_query_t query);
	DUCKDB_API static PragmaFunction PragmaStatement(const Identifier &name, pragma_function_t function);

	DUCKDB_API string ToString() const override;

public:
	PragmaType type;

	pragma_query_t query;
	pragma_function_t function;
	//! Additional function info, passed to the callbacks through FunctionParameters::function
	shared_ptr<PragmaFunctionInfo> function_info;

private:
	PragmaFunction(Identifier name, PragmaType pragma_type, pragma_query_t query, pragma_function_t function,
	               vector<LogicalType> arguments, LogicalType varargs);
	PragmaFunction(Identifier name, PragmaType pragma_type, pragma_query_t query, pragma_function_t function,
	               FunctionSignature signature);
};

} // namespace duckdb
