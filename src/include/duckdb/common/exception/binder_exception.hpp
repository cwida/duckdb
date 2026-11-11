//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/exception/binder_exception.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/query_location.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/parser/query_error_context.hpp"

namespace duckdb {

class BinderException : public Exception {
public:
	DUCKDB_API explicit BinderException(const string &msg);

	DUCKDB_API explicit BinderException(const unordered_map<string, string> &extra_info, const string &msg);

	template <typename... ARGS>
	explicit BinderException(const string &msg, ARGS &&...params)
	    : BinderException(ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	template <typename... ARGS>
	explicit BinderException(const TableRef &ref, const string &msg, ARGS &&...params)
	    : BinderException(Exception::InitializeExtraInfo(ref), ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}
	template <typename... ARGS>
	explicit BinderException(const ParsedExpression &expr, const string &msg, ARGS &&...params)
	    : BinderException(Exception::InitializeExtraInfo(expr), ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	template <typename... ARGS>
	explicit BinderException(const Expression &expr, const string &msg, ARGS &&...params)
	    : BinderException(Exception::InitializeExtraInfo(expr), ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	template <typename... ARGS>
	explicit BinderException(QueryErrorContext error_context, const string &msg, ARGS &&...params)
	    : BinderException(Exception::InitializeExtraInfo(error_context),
	                      ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	template <typename... ARGS>
	explicit BinderException(QueryLocation error_location, const string &msg, ARGS &&...params)
	    : BinderException(Exception::InitializeExtraInfo(error_location),
	                      ConstructMessage(msg, std::forward<ARGS>(params)...)) {
	}

	static BinderException ColumnNotFound(const Identifier &name, const vector<Identifier> &similar_bindings,
	                                      QueryErrorContext context = QueryErrorContext());
	static BinderException NoMatchingFunction(const Identifier &catalog_name, const Identifier &schema_name,
	                                          const Identifier &name, const vector<LogicalType> &arguments,
	                                          const vector<pair<Identifier, LogicalType>> &named_arguments,
	                                          const vector<string> &candidates, const string &message);
	static BinderException Unsupported(ParsedExpression &expr, const string &message);
};

} // namespace duckdb
