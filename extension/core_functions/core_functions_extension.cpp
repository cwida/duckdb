#include "core_functions_extension.hpp"
#include "core_functions/function_list.hpp"

namespace duckdb {

static void LoadInternal(ExtensionLoader &loader) {
	FunctionList::RegisterExtensionFunctions(loader, CoreFunctionList::GetFunctionList());
}

// LCOV_EXCL_START
void CoreFunctionsExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string CoreFunctionsExtension::Name() {
	return "core_functions";
}

std::string CoreFunctionsExtension::Version() const {
#ifdef EXT_VERSION_CORE_FUNCTIONS
	return EXT_VERSION_CORE_FUNCTIONS;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(core_functions, loader) {
	duckdb::LoadInternal(loader);
}
}
