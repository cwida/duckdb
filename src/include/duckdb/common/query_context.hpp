//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/query_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/winapi.hpp"
#include "duckdb/main/context.hpp"

namespace duckdb {

class ClientContext;

//! The QueryContext wraps optional execution services.
//! It makes query-related information available to operations.
class QueryContext {
public:
	QueryContext() {
	}
	DUCKDB_API QueryContext(optional_ptr<ClientContext> client_context); // NOLINT: allow implicit construction
	DUCKDB_API QueryContext(ClientContext &client_context);              // NOLINT: allow implicit construction
	QueryContext(Context &context_p) : context(&context_p) {             // NOLINT: allow implicit construction
	}

public:
	bool Valid() const {
		return context != nullptr;
	}
	//! The context (null if the context is not valid)
	optional_ptr<Context> GetContext() const {
		return context;
	}
	//! The client the QueryContext was created from (null if it was created from a Context)
	optional_ptr<ClientContext> GetClientContext() const {
		return client_context;
	}

private:
	optional_ptr<Context> context;
	optional_ptr<ClientContext> client_context;
};

} // namespace duckdb
