#include "duckdb/common/query_context.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

QueryContext::QueryContext(optional_ptr<ClientContext> context) {
	if (context) {
		services = &context->Services();
	}
}

QueryContext::QueryContext(ClientContext &context) : services(&context.Services()) {
}

} // namespace duckdb
