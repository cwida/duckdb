#include "duckdb/common/dl.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/external_extension_provider.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb_static_extension.h"

#ifdef WASM_LOADABLE_EXTENSIONS
#include <emscripten.h>
#endif

#if defined(DUCKDB_DISABLE_EXTENSION_LOAD)
#define DUCKDB_EXTENSION_LOAD_DEFINE_WARNING                                                                           \
	"DUCKDB_DISABLE_EXTENSION_LOAD no longer disables loading and installing extensions: leave "                       \
	"duckdb_loadable_extensions out of the link instead (ENABLE_EXTENSION_LOAD=OFF, or package_build.py with "         \
	"extension_load=False)"
#if defined(_MSC_VER)
#pragma message("warning: " DUCKDB_EXTENSION_LOAD_DEFINE_WARNING)
#else
#pragma GCC warning DUCKDB_EXTENSION_LOAD_DEFINE_WARNING
#endif
#endif

namespace duckdb {

unique_ptr<ExtensionInstallInfo> InstallExternalExtension(DatabaseInstance &db, FileSystem &fs,
                                                          const string &local_path, const string &extension,
                                                          ExtensionInstallOptions &options,
                                                          optional_ptr<ClientContext> context);

//! Installs extensions from repositories and opens their libraries with dlopen
class DynamicExtensionProvider : public ExternalExtensionProvider {
public:
	string GetName() const override {
		return "dynamic";
	}

	bool SupportsExternalExtensions() const override {
		return true;
	}

	unique_ptr<ExtensionInstallInfo> Install(DatabaseInstance &db, FileSystem &fs, const string &local_path,
	                                         const string &extension, ExtensionInstallOptions &options,
	                                         optional_ptr<ClientContext> context) override {
		return InstallExternalExtension(db, fs, local_path, extension, options, context);
	}

	void *OpenLibrary(const string &filename, const string &filebase) override {
#ifdef WASM_LOADABLE_EXTENSIONS
		EM_ASM(
		    {
			    // Next few lines should arguably in separate JavaScript-land function call
			    // TODO: move them out / have them configurable
			    const xhr = new XMLHttpRequest();
			    xhr.open("GET", UTF8ToString($0), false);
			    xhr.responseType = "arraybuffer";
			    xhr.send(null);
			    var uInt8Array = xhr.response;
			    WebAssembly.validate(uInt8Array);
			    console.log('Loading extension ', UTF8ToString($1));

			    // Here we add the uInt8Array to Emscripten's filesystem, for it to be found by dlopen
			    FS.writeFile(UTF8ToString($1), new Uint8Array(uInt8Array));
		    },
		    filename.c_str(), filebase.c_str());
		const auto &dopen_from = filebase;
#else
		const auto &dopen_from = filename;
#endif

		auto lib_hdl = dlopen(dopen_from.c_str(), RTLD_NOW | RTLD_LOCAL);
		if (!lib_hdl) {
			throw IOException("Extension \"%s\" could not be loaded: %s", filename, GetDLError());
		}
		return lib_hdl;
	}

	void *TryLoadFunction(void *library, const string &function_name) override {
		return dlsym(library, function_name.c_str());
	}

	string GetLibraryError() override {
		return GetDLError();
	}
};

static void RegisterDynamicExtensionProvider(DatabaseInstance &db) {
	db.config.SetExternalExtensionProvider(make_shared_ptr<DynamicExtensionProvider>());
}

} // namespace duckdb

//! Lets every database opened afterwards install and load external extensions, through
//! duckdb_register_static_extension. It is not an extension, so it asks for a database callback.
extern "C" int32_t duckdb_extension_loadable_extensions_describe(duckdb_extension_descriptor *descriptor) {
	if (descriptor->version < 2) {
		descriptor->set_error(descriptor, "loadable_extensions needs descriptor layout 2");
		return 1;
	}
	descriptor->version = 2;
	descriptor->name = "loadable_extensions";
	descriptor->database_callback = reinterpret_cast<void (*)(void)>(duckdb::RegisterDynamicExtensionProvider);
	return 0;
}
