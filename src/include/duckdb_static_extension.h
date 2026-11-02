//===----------------------------------------------------------------------===//
//
//                         DuckDB
//
// duckdb_static_extension.h
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>

#ifndef DUCKDB_C_API
#ifdef _WIN32
#ifdef DUCKDB_STATIC_BUILD
#define DUCKDB_C_API
#elif defined(DUCKDB_BUILD_LIBRARY) && !defined(DUCKDB_BUILD_LOADABLE_EXTENSION)
#define DUCKDB_C_API __declspec(dllexport)
#else
#define DUCKDB_C_API __declspec(dllimport)
#endif
#else
#if defined(__GNUC__) || defined(__clang__)
#define DUCKDB_C_API __attribute__((visibility("default")))
#else
#define DUCKDB_C_API
#endif
#endif
#endif

#ifdef __cplusplus
extern "C" {
#endif

//! Descriptor layout this header defines.
#define DUCKDB_EXTENSION_DESCRIPTOR_VERSION 2

typedef struct duckdb_extension_descriptor duckdb_extension_descriptor;

//! Describes a statically linked extension, or something linked in the same way that is not an extension. DuckDB
//! allocates and zero-fills it, the describe function fills it in.
struct duckdb_extension_descriptor {
	//! In: the layout DuckDB offers. Out: the layout the describe function filled, never higher than the offer.
	uint32_t version;

	// Layout 1, set by DuckDB
	//! Reports why the describe function refused. DuckDB copies the message.
	void (*set_error)(duckdb_extension_descriptor *descriptor, const char *message);
	//! DuckDB's state for set_error, opaque to the extension.
	void *internal;

	// Layout 1, set by the extension; DuckDB copies the strings before the describe function's caller returns
	const char *name;
	const char *extension_version;
	//! What the entry point was built against: the DuckDB version for entry_cpp, the C API version the extension
	//! targets for entry_capi_v1 and entry_capi_v2. The same values the loadable extension metadata carries. DuckDB
	//! records it and reports it; it does not refuse a mismatch on it yet.
	const char *api_version;
	//! void (duckdb::ExtensionLoader &)
	void (*entry_cpp)(void);
	//! bool (duckdb_extension_info, struct duckdb_extension_access *)
	void (*entry_capi_v1)(void);
	//! void (struct duckdb_v2_extension_input *)
	void (*entry_capi_v2)(void);

	// Layout 2, set by the describe function
	//! Set instead of an entry point by a capability, something linked like an extension that is not one (such as
	//! httplib): DuckDB calls it for every database it opens, before loading extensions, and neither lists nor loads
	//! it as an extension.
	//! void (duckdb::DatabaseInstance &)
	void (*database_callback)(void);
};

//! Every statically linkable extension provides duckdb_extension_<name>_describe with this signature. Returns 0 on
//! success.
typedef int32_t (*duckdb_extension_describe_t)(duckdb_extension_descriptor *descriptor);

//! Calls describe and registers what it describes for every database opened afterwards. Returns 0 on
//! success. Registering the same describe function again is a no-op; a different one under a registered name is an
//! error. A failed registration also makes opening a database fail with the reason.
DUCKDB_C_API int32_t duckdb_register_static_extension(duckdb_extension_describe_t describe);

#ifdef __cplusplus
}
#endif
