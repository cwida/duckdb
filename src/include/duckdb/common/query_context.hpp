//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/query_context.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/main/execution_services.hpp"

namespace duckdb {

class ClientContext;

//! The QueryContext wraps optional execution services.
//! It makes query-related information available to operations.
class QueryContext {
public:
	QueryContext() {
	}
	QueryContext(optional_ptr<ClientContext> context);                    // NOLINT: allow implicit construction
	QueryContext(ClientContext &context);                                 // NOLINT: allow implicit construction
	QueryContext(ExecutionServices &services_p) : services(&services_p) { // NOLINT: allow implicit construction
	}

public:
	bool Valid() const {
		return services != nullptr;
	}
	//! The execution services (null if the context is not valid)
	optional_ptr<ExecutionServices> GetServices() const {
		return services;
	}

private:
	optional_ptr<ExecutionServices> services;
};

} // namespace duckdb
