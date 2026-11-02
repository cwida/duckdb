#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<ClientContext> context_p) : context(context_p) {
	if (context) {
		context->registered_state->ForEachState([&](ClientContextState &state) { state.OnTaskStart(*context); });
	}
}

TaskNotifier::~TaskNotifier() {
	if (context) {
		context->registered_state->ForEachState([&](ClientContextState &state) { state.OnTaskStop(*context); });
	}
}

} // namespace duckdb
