#include "duckdb/parallel/task_notifier.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/execution_services.hpp"

namespace duckdb {

TaskNotifier::TaskNotifier(optional_ptr<ExecutionServices> services_p) : services(services_p) {
	auto context = ExecutionServices::TryGetClientContext(services);
	if (context) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStart(*context); });
	}
}

TaskNotifier::~TaskNotifier() {
	auto context = ExecutionServices::TryGetClientContext(services);
	if (context) {
		services->GetRegisteredState().ForEachState([&](ClientContextState &state) { state.OnTaskStop(*context); });
	}
}

} // namespace duckdb
