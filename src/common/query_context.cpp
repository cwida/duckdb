#include "duckdb/common/query_context.hpp"

#include "duckdb/main/client_context.hpp"

namespace duckdb {

QueryContext::QueryContext(optional_ptr<ClientContext> client_context_p) : client_context(client_context_p) {
	if (client_context) {
		context = &client_context->GetContext();
	}
}

QueryContext::QueryContext(ClientContext &client_context_p)
    : context(&client_context_p.GetContext()), client_context(&client_context_p) {
}

} // namespace duckdb
