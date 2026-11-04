//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/error_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/map.hpp"
#include "duckdb/common/types/string.hpp"

namespace duckdb {
class String;
class ClientContext;
class DatabaseInstance;
class TransactionException;

enum class ErrorType : uint16_t {
	// error message types
	UNSIGNED_EXTENSION = 0,
	INVALIDATED_TRANSACTION = 1,
	INVALIDATED_DATABASE = 2,

	// this should always be the last value
	ERROR_COUNT,
	INVALID = 65535,
};

//! The error manager class is responsible for formatting error messages
//! It allows for error messages to be overridden by extensions and clients
class ErrorManager {
public:
	template <typename... ARGS>
	string FormatException(ErrorType error_type, ARGS &&...params) {
		vector<ExceptionFormatValue> values;
		return FormatExceptionRecursive(error_type, values, std::forward<ARGS>(params)...);
	}

	DUCKDB_API string FormatExceptionRecursive(ErrorType error_type, vector<ExceptionFormatValue> &values);

	template <class T, typename... ARGS>
	string FormatExceptionRecursive(ErrorType error_type, vector<ExceptionFormatValue> &values, const T &param,
	                                ARGS &&...params) {
		values.push_back(ExceptionFormatValue::CreateFormatValue<T>(param));
		return FormatExceptionRecursive(error_type, values, std::forward<ARGS>(params)...);
	}

	template <typename... ARGS>
	static string FormatException(ClientContext &context, ErrorType error_type, ARGS &&...params) {
		return Get(context).FormatException(error_type, std::forward<ARGS>(params)...);
	}

	DUCKDB_API static InvalidInputException InvalidUnicodeError(const String &input, const string &context);

	DUCKDB_API static FatalException InvalidatedDatabase(ClientContext &context, const string &invalidated_msg);

	DUCKDB_API static TransactionException InvalidatedTransaction(ClientContext &context);

	//! Adds a custom error for a specific error type
	void AddCustomError(ErrorType type, string new_error);

	DUCKDB_API static ErrorManager &Get(ClientContext &context);

	DUCKDB_API static ErrorManager &Get(DatabaseInstance &context);

private:
	map<ErrorType, string> custom_errors;
};
} // namespace duckdb
