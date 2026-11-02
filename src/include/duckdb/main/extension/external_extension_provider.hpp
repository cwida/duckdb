//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension/external_extension_provider.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"

namespace duckdb {
class ClientContext;
class DatabaseInstance;
class FileSystem;
class ExtensionInstallInfo;
struct ExtensionInstallOptions;

//! Installs external extensions and opens their libraries. The base class is the "none" provider, which refuses both;
//! a linked loadable_extensions library replaces it for every database opened.
class ExternalExtensionProvider {
public:
	virtual ~ExternalExtensionProvider() = default;

public:
	virtual string GetName() const;
	//! Whether this provider can install and load external extensions at all
	virtual bool SupportsExternalExtensions() const;
	virtual unique_ptr<ExtensionInstallInfo> Install(DatabaseInstance &db, FileSystem &fs, const string &local_path,
	                                                 const string &extension, ExtensionInstallOptions &options,
	                                                 optional_ptr<ClientContext> context);
	//! Open the library of an external extension, throws if it cannot be opened
	virtual void *OpenLibrary(const string &filename, const string &filebase);
	//! Look up a function in an opened library, returns nullptr if it is not there
	virtual void *TryLoadFunction(void *library, const string &function_name);
	//! The error of the last failed library operation
	virtual string GetLibraryError();
};

} // namespace duckdb
