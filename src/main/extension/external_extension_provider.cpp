#include "duckdb/main/extension/external_extension_provider.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/main/extension_install_info.hpp"

namespace duckdb {

static constexpr const char *NO_LOADER_HINT = "this build does not link the loadable_extensions library";

string ExternalExtensionProvider::GetName() const {
	return "none";
}

bool ExternalExtensionProvider::SupportsExternalExtensions() const {
	return false;
}

unique_ptr<ExtensionInstallInfo> ExternalExtensionProvider::Install(DatabaseInstance &db, FileSystem &fs,
                                                                    const string &local_path, const string &extension,
                                                                    ExtensionInstallOptions &options,
                                                                    optional_ptr<ClientContext> context) {
	throw PermissionException("Installing external extensions is not supported: %s", NO_LOADER_HINT);
}

void *ExternalExtensionProvider::OpenLibrary(const string &filename, const string &filebase) {
	throw PermissionException("Loading external extensions is not supported: %s", NO_LOADER_HINT);
}

void *ExternalExtensionProvider::TryLoadFunction(void *library, const string &function_name) {
	throw PermissionException("Loading external extensions is not supported: %s", NO_LOADER_HINT);
}

string ExternalExtensionProvider::GetLibraryError() {
	return string();
}

} // namespace duckdb
