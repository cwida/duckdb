//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_install_info.hpp
//
//
//===----------------------------------------------------------------------===//

#include "duckdb/common/types.hpp"
#include "duckdb/main/config.hpp"

#pragma once

namespace duckdb {
class FileSystem;

enum class ExtensionInstallMode : uint8_t {
	// Fallback for when install info is missing
	UNKNOWN = 0,
	//! Extension was installed using a url deduced from a repository base url
	REPOSITORY = 1,
	//! Extension was install from a custom path, this could be either local or remote
	CUSTOM_PATH = 2,
	//! Extension was statically linked
	STATICALLY_LINKED = 3,
	//! Extension is not installed, for example the extension might be directly loaded without installing
	NOT_INSTALLED = 4
};

//! Whether the user has opted in to adding external extension repositories. This is a one-way ratchet: once FORBIDDEN
//! it cannot be moved back within a session. UNDECIDED (the default) blocks adding new repositories, but extensions
//! from repositories that were already created can still be installed and loaded
enum class ExtensionRepositoryAccess : uint8_t {
	//! No decision has been made yet - adding repositories fails until the user opts in explicitly
	UNDECIDED = 0,
	//! Adding external repositories is allowed
	ALLOWED = 1,
	//! Adding external repositories is forbidden - this cannot be undone in the same session
	FORBIDDEN = 2
};

//! The type of a repository determines which public keys are trusted to sign the extensions that it serves. Keys of
//! different types are managed separately, so a leak of one of them must not affect the other types
enum class ExtensionRepositoryType : uint8_t {
	//! The repositories that are maintained by DuckDB itself - this is also the fallback for unknown repositories,
	//! custom paths and urls
	CORE = 0,
	//! The community extension repository
	COMMUNITY = 1,
	//! A trusted repository that was added by the user
	USER_PROVIDED = 2
};

struct ExtensionLoadedInfo {
	string description;
};

class ExtensionInstallInfo {
public:
	//! How the extension was installed
	ExtensionInstallMode mode = ExtensionInstallMode::UNKNOWN;
	//! (optional) Full path where the extension came from
	string full_path;
	//! (optional) Repository url where the extension came from
	string repository_url;
	//! (optional) Version of the extension
	string version;
	//! (optional) ETag of last fetched resource
	string etag;
	//! The type of the repository the extension was installed from, together with the repository name for user
	//! provided repositories. This identifies the repository the extension came from, and with that the public keys
	//! that are trusted to sign this extension when it is loaded
	ExtensionRepositoryType repository_type = ExtensionRepositoryType::CORE;
	//! (optional) Name of the repository the extension came from
	string repository_name;
	//! (optional) Fingerprint of the public key that signed this extension, as verified at install time. Empty when
	//! the extension is unsigned. Matches the key_fingerprints reported by CREATE EXTENSION REPOSITORY
	string signature_key_fingerprint;

	void Serialize(Serializer &serializer) const;

	//! Try to read install info. returns ExtensionInstallMode::UNKNOWN on missing file, and throws on corrupt file
	static unique_ptr<ExtensionInstallInfo> TryReadInfoFile(FileSystem &fs, const string &info_file_path,
	                                                        const string &extension_name);

	static unique_ptr<ExtensionInstallInfo> Deserialize(Deserializer &deserializer);
};

struct ExtensionRepository {
	//! All currently available repositories
	static constexpr const char *CORE_REPOSITORY_URL = "http://extensions.duckdb.org";
	static constexpr const char *CORE_NIGHTLY_REPOSITORY_URL = "http://nightly-extensions.duckdb.org";
	static constexpr const char *COMMUNITY_REPOSITORY_URL = "http://community-extensions.duckdb.org";

	//! Backup servers for the core and community repositories: they serve identical binaries and are used when the
	//! primary server is unavailable
	static constexpr const char *CORE_BACKUP_REPOSITORY_URL = "http://extensions.duckdb-backup.org";
	static constexpr const char *COMMUNITY_BACKUP_REPOSITORY_URL = "http://community-extensions.duckdb-backup.org";

	//! Debugging repositories (target local, relative paths that are produced by DuckDB's build system)
	static constexpr const char *BUILD_DEBUG_REPOSITORY_PATH = "./build/debug/repository";
	static constexpr const char *BUILD_RELEASE_REPOSITORY_PATH = "./build/release/repository";

	//! The default is CORE
	static constexpr const char *DEFAULT_REPOSITORY_URL = CORE_REPOSITORY_URL;

	//! Try to look up one of the built-in repositories by name
	static bool TryGetKnownRepository(const string &repository, ExtensionRepository &result);
	//! The names of all built-in repositories
	static vector<string> GetKnownRepositoryNames();

	//! Returns the repository name is this is a known repository, or the full url if it is not
	static string GetRepository(const string &repository_url);
	//! Try to convert a repository to a url, will return empty string if the repository is unknown
	static string TryGetRepositoryUrl(const string &repository);
	//! Try to convert a url to a known repository name, will return empty string if the repository is unknown
	static string TryConvertUrlToKnownRepository(const string &url);
	//! Try to get the backup server url of a repository url, will return empty string if the repository has no backup
	static string TryGetBackupRepositoryUrl(const string &url);
	//! Whether a url points into one of the official DuckDB repositories (core, nightly, community or their backups)
	static bool IsOfficialRepositoryUrl(const string &url);

	//! Get the default repository, optionally passing a config to allow
	static ExtensionRepository GetDefaultRepository(optional_ptr<DBConfig> config);
	static ExtensionRepository GetDefaultRepository(ClientContext &context);

	static ExtensionRepository GetCoreRepository();
	static ExtensionRepository GetRepositoryByUrl(const string &url);

	ExtensionRepository();
	ExtensionRepository(const string &name, const string &url, vector<string> public_keys = {});

	//! Print the name if it has one, or the full path if not
	string ToReadableString();

	//! Repository name
	string name;
	//! Repository path/url
	string path;
	//! (optional) Compact public keys that are trusted to sign the extensions in this repository (user provided
	//! repositories only). A repository can publish more than one key, e.g. to allow key rotation
	vector<string> public_keys;
	//! Which public keys are trusted to sign the extensions of this repository
	ExtensionRepositoryType type = ExtensionRepositoryType::CORE;
};

} // namespace duckdb
