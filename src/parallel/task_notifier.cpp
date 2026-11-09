#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/context.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<Context> context_p) : context(context_p) {
	if (context && context->IsClient()) {
		context->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStart(*context); });
	}
}

TaskNotifier::~TaskNotifier() {
	if (context && context->IsClient()) {
		context->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStop(*context); });
	}
}

} // namespace duckdb
