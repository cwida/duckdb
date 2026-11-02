// Calls the generated static extension loader before main. Compile it into a program that wants its statically linked
// extensions registered without calling duckdb_register_static_extensions itself. Compile it into the program, not into
// an archive, or the linker drops it.

#include <stdint.h>

extern "C" int32_t duckdb_register_static_extensions(void);

namespace {
struct DuckDBStaticExtensionLoader {
	DuckDBStaticExtensionLoader() {
		duckdb_register_static_extensions();
	}
};
const DuckDBStaticExtensionLoader duckdb_static_extension_loader;
} // namespace
