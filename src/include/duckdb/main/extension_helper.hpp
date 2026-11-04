//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_helper.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/extension_install_info.hpp"
#include "duckdb/main/extension_load_options.hpp"
#include "duckdb/main/settings.hpp"

#include <string>

namespace duckdb {

class DuckDB;
class ExtensionActiveLoad;

enum class ExtensionLoadResult : uint8_t { LOADED_EXTENSION = 0, EXTENSION_UNKNOWN = 1, NOT_LOADED = 2 };

struct DefaultExtension {
	const char *name;
	const char *description;
};

struct ExtensionAlias {
	const char *alias;
	const char *extension;
};

struct ExtensionInitResult {
	string filename;
	string filebase;
	ExtensionABIType abi_type = ExtensionABIType::UNKNOWN;

	// (only for ExtensionABIType::C_STRUCT) the CAPI version from the metadata footer. Its major selects which C API
	// family the extension targets, and therefore which entrypoint is called
	string duckdb_capi_version;

	// The deserialized install from the `<ext>.duckdb_extension.info` file
	unique_ptr<ExtensionInstallInfo> install_info;

	void *lib_hdl;
};

// Tags describe what happened during the updating process
enum class ExtensionUpdateResultTag : uint8_t {
	// Fallback for when installation information is missing
	UNKNOWN = 0,

	// Either a fresh file was downloaded and versions are identical
	NO_UPDATE_AVAILABLE = 1,
	// Only extensions from repositories can be updated
	NOT_A_REPOSITORY = 2,
	// Only known, currently installed extensions can be updated
	NOT_INSTALLED = 3,
	// Statically loaded extensions can not be updated; they are baked into the DuckDB executable
	STATICALLY_LOADED = 4,
	// This means the .info file written during installation was missing or malformed
	MISSING_INSTALL_INFO = 5,

	// The extension was re-downloaded from the repository, but due to a lack of version information
	// its impossible to tell if the extension is actually updated
	REDOWNLOADED = 254,
	// The version was updated to a new version
	UPDATED = 255,
};

struct ExtensionUpdateResult {
	ExtensionUpdateResultTag tag = ExtensionUpdateResultTag::UNKNOWN;

	string extension_name;
	string repository;

	string extension_version;
	string prev_version;
	string installed_version;
};

struct ExtensionInstallOptions {
	//! Install from a different repository that the default one
	optional_ptr<ExtensionRepository> repository;
	//! Install a specific version of the extension
	string version;

	//! Overwrite existing installation
	bool force_install = false;
	//! Use etags to avoid downloading unchanged extension files
	bool use_etags = false;
	//! Throw an error when installing an extension with a different origin than the one that is installed
	bool throw_on_origin_mismatch = false;
};

class ExtensionHelper {
public:
	static void LoadAllExtensions(DuckDB &db);
	static ExtensionLoadResult LoadExtension(DuckDB &db, const std::string &extension);
	//! Publishes the extensions linked into this binary, as collected by LinkedExtensionRegistry, onto the config
	static void RegisterLinkedExtensions(DBConfig &config);

	//! Install an extension
	static unique_ptr<ExtensionInstallInfo> InstallExtension(ClientContext &context, const string &extension,
	                                                         ExtensionInstallOptions &options);
	static unique_ptr<ExtensionInstallInfo> InstallExtension(DatabaseInstance &db, FileSystem &fs,
	                                                         const string &extension, ExtensionInstallOptions &options);
	//! Load an extension. `context`, where available, is lent to a V2 C API extension's entrypoint; without one the
	//! loader opens an internal connection for the duration of the load instead.
	static void LoadExternalExtension(ClientContext &context, const ExtensionLoadOptions &options);
	static void LoadExternalExtension(DatabaseInstance &db, FileSystem &fs, const ExtensionLoadOptions &options,
	                                  optional_ptr<ClientContext> context = nullptr);

	//! Autoload an extension (depending on config, potentially a nop. Throws when installation fails)
	static void AutoLoadExtension(ClientContext &context, const string &extension_name);
	static void AutoLoadExtension(DatabaseInstance &db, const string &extension_name);
	//! Autoload the extension a remote path requires
	DUCKDB_API static void AutoLoadExtensionForPath(DatabaseInstance &db, const string &path,
	                                                const string &path_kind = "File");

	//! Autoload an extension (depending on config, potentially a nop. Returns false on failure)
	DUCKDB_API static bool TryAutoLoadExtension(DatabaseInstance &db, const string &extension_name) noexcept;
	DUCKDB_API static bool TryAutoLoadExtension(ClientContext &context, const string &extension_name) noexcept;
	//! Where automatic installs go: autoinstall_extension_repository, else custom_extension_repository, else
	//! DBConfigOptions::default_autoinstall_repository, else the core repository
	DUCKDB_API static ExtensionRepository GetAutoinstallRepository(DatabaseInstance &db);

	//! Autoload an extension, only if available locally
	DUCKDB_API static bool TryAutoLoadAvailableExtension(DatabaseInstance &instance,
	                                                     const string &extension_name) noexcept;

	//! Update all extensions, return a vector of extension names that were updated;
	static vector<ExtensionUpdateResult> UpdateExtensions(ClientContext &context);
	//! Update a specific extension
	static ExtensionUpdateResult UpdateExtension(ClientContext &context, const string &extension_name);

	//! Get the extension directory base on the current config
	static string ExtensionDirectory(ClientContext &context);
	static string ExtensionDirectory(DatabaseInstance &db, FileSystem &fs);

	// Get all extension directory paths
	static vector<string> GetExtensionDirectoryPath(ClientContext &context);
	static vector<string> GetExtensionDirectoryPath(DatabaseInstance &db, FileSystem &fs);
	//! Whether a file in an extension directory is an installed extension, and not the temporary file of an install
	static bool IsInstalledExtensionFile(const string &path);

	// Check signature of an Extension stored as FileHandle
	static bool CheckExtensionSignature(DatabaseInstance &db, FileHandle &handle,
	                                    ParsedExtensionMetaData &parsed_metadata,
	                                    ExtensionRepositoryType repository_type, const string &repository_name);
	// Check signature of an Extension, represented by a buffer and total_buffer_length, and a signature to be added.
	// When a key matches, its fingerprint is written to signature_key_fingerprint (if provided)
	static bool CheckExtensionBufferSignature(DatabaseInstance &db, const char *buffer, idx_t buffer_length,
	                                          const string &signature, ExtensionRepositoryType repository_type,
	                                          const string &repository_name,
	                                          optional_ptr<string> signature_key_fingerprint = nullptr);
	// Check signature of an Extension, represented by a buffer and total_buffer_length
	static bool CheckExtensionBufferSignature(DatabaseInstance &db, const char *buffer, idx_t total_buffer_length,
	                                          ExtensionRepositoryType repository_type, const string &repository_name,
	                                          optional_ptr<string> signature_key_fingerprint = nullptr);
	static ParsedExtensionMetaData ParseExtensionMetaData(const char *metadata) noexcept;
	static ParsedExtensionMetaData ParseExtensionMetaData(FileHandle &handle);

	//! Get the extension url template, containing placeholders for version, platform and extension name
	static string ExtensionUrlTemplate(optional_ptr<const DatabaseInstance> db, const ExtensionRepository &repository,
	                                   const string &version);
	//! Return the extension url template with the variables replaced
	static string ExtensionFinalizeUrlTemplate(const string &url, const string &name);

	//! Default extensions are all extensions that DuckDB knows and expect to be available (both in-tree and
	//! out-of-tree)
	static idx_t DefaultExtensionCount();
	static DefaultExtension GetDefaultExtension(idx_t index);

	//! Extension can have aliases
	static idx_t ExtensionAliasCount();
	static ExtensionAlias GetInternalExtensionAlias(idx_t index);

	//! Get the built-in public signing keys for extension signing
	static const vector<string> GetPublicKeys(bool allow_community_extension = false);
	//! Get the public keys that are trusted to sign extensions that originate from the given repository. Only the keys
	//! of that repository are returned: the core keys, the community keys and the key of every user provided
	//! repository are managed separately, so a leak of any of them only affects that single repository
	static vector<string> GetTrustedPublicKeys(DatabaseInstance &db, ExtensionRepositoryType repository_type,
	                                           const string &repository_name);

	//! The origin whose signing keys a load trusts: an explicit FROM trusts the named origin, an autoload (core_only)
	//! trusts the core keys only, and a plain bare LOAD trusts the core keys plus the community keys for a community
	//! extension - never a user-provided repository's own keys
	static ExtensionRepositoryType ResolveTrustedSignatureOrigin(bool has_from_clause, bool core_only,
	                                                             ExtensionRepositoryType recorded_origin);

	// Returns extension name, or empty string if not a replacement open path
	static string ExtractExtensionPrefixFromPath(const string &path);

	// Returns the user-readable name of a repository URL
	static string GetRepositoryName(const string &repository_base_url);

	//! Apply any known extension aliases, return the lowercase name
	static string ApplyExtensionAlias(const string &extension_name);

	static string GetExtensionName(const string &extension);
	static bool IsFullPath(const string &extension);

	//! Lookup a name + type in an ExtensionFunctionEntry list
	template <size_t N>
	static vector<pair<string, CatalogType>>
	FindExtensionInFunctionEntries(const Identifier &name, const ExtensionFunctionEntry (&entries)[N]) {
		vector<pair<string, CatalogType>> result;
		for (idx_t i = 0; i < N; i++) {
			auto &element = entries[i];
			if (element.name == name) {
				result.push_back(make_pair(element.extension, element.type));
			}
		}
		return result;
	}

	template <idx_t N>
	static idx_t ArraySize(const ExtensionEntry (&entries)[N]) {
		return N;
	}

	template <idx_t N>
	static const ExtensionEntry *GetArrayEntry(const ExtensionEntry (&entries)[N], idx_t entry) {
		if (entry >= N) {
			return nullptr;
		}
		return entries + entry;
	}

	//! Lookup a name in an ExtensionEntry list
	template <idx_t N>
	static string FindExtensionInEntries(const Identifier &name, const ExtensionEntry (&entries)[N]) {
		auto it =
		    std::find_if(entries, entries + N, [&](const ExtensionEntry &element) { return element.name == name; });

		if (it != entries + N) {
			return it->extension;
		}
		return "";
	}

	//! Lookup a name in an extension entry and try to autoload it
	template <idx_t N>
	static void TryAutoloadFromEntry(DatabaseInstance &db, const Identifier &entry,
	                                 const ExtensionEntry (&entries)[N]) {
		if (Settings::Get<AutoloadKnownExtensionsSetting>(db)) {
			auto extension_name = ExtensionHelper::FindExtensionInEntries(entry, entries);
			if (ExtensionHelper::CanAutoloadExtension(db, extension_name)) {
				ExtensionHelper::AutoLoadExtension(db, extension_name);
			}
		}
	}

	//! Whether an extension can be autoloaded (i.e. it's registered as an autoloadable extension in
	//! extension_entries.hpp)
	static bool CanAutoloadExtension(DatabaseInstance &db, const string &ext_name);

	//! Utility functions for creating meaningful error messages regarding missing extensions
	static string WrapAutoLoadExtensionErrorMsg(ClientContext &context, const string &base_error,
	                                            const string &extension_name);
	static string AddExtensionInstallHintToErrorMsg(ClientContext &context, const string &base_error,
	                                                const string &extension_name);
	static string AddExtensionInstallHintToErrorMsg(DatabaseInstance &db, const string &base_error,
	                                                const string &extension_name);

	//! For tagged releases we use the tag, else we use the git commit hash
	static const string GetVersionDirectoryName();

	static bool IsRelease(const string &version_tag);
	static bool CreateSuggestions(const string &extension_name, string &message);
	static string ExtensionInstallDocumentationLink(const string &extension_name);

private:
	static unique_ptr<ExtensionInstallInfo> InstallExtensionInternal(DatabaseInstance &db, FileSystem &fs,
	                                                                 const string &local_path, const string &extension,
	                                                                 ExtensionInstallOptions &options,
	                                                                 optional_ptr<ClientContext> context = nullptr);
	static const vector<string> PathComponents();
	static vector<string> DefaultExtensionFolders(FileSystem &fs);
	static bool AllowAutoInstall(const string &extension);
	static ExtensionInitResult InitialLoad(DatabaseInstance &db, FileSystem &fs, const string &extension,
	                                       const string &repository_name = string(), bool core_only = false);
	static bool TryInitialLoad(DatabaseInstance &db, FileSystem &fs, const string &extension,
	                           const string &repository_name, bool core_only, ExtensionInitResult &result,
	                           string &error);
	//! Version tags occur with and without 'v', tag in extension path is always with 'v'
	static const string NormalizeVersionTag(const string &version_tag);
	static void LoadExternalExtensionInternal(DatabaseInstance &db, FileSystem &fs, const string &extension,
	                                          const string &repository_name, bool core_only, ExtensionActiveLoad &info,
	                                          optional_ptr<ClientContext> context);

private:
	static ExtensionLoadResult LoadExtensionInternal(DuckDB &db, const std::string &extension, bool initial_load);
};

} // namespace duckdb
