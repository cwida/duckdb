#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/context.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<Context> services_p) : services(services_p) {
	auto context = Context::TryGetClientContext(services);
	if (context) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStart(*context); });
	}
}

TaskNotifier::~TaskNotifier() {
	auto context = Context::TryGetClientContext(services);
	if (context) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStop(*context); });
	}
}

} // namespace duckdb
