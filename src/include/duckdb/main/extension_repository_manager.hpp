//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/extension_repository_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/main/extension_install_info.hpp"

namespace duckdb {
class ClientContext;
class DatabaseInstance;
class FileSystem;

//! Manages the trusted extension repositories of a DuckDB installation. Trusted repositories are stored as json files
//! in the extension repository directory (by default `~/.duckdb/extension_repositories`). Every repository has a
//! locally unique name, a url prefix from which extensions are downloaded and one or more public keys that are trusted
//! to sign the extensions that are served by the repository. The keys of a trusted repository are only trusted for the
//! extensions that are installed from that repository.
class ExtensionRepositoryManager {
public:
	//! The file extension used for the stored repository definitions
	//! Repository files carry a ".duckdb_extension." segment so that they are covered by the write protection in
	//! OpenerFileSystem::IsDuckDBExtensionName: they hold the pinned keys that decide which extensions may be loaded,
	//! so they must not be writable through anything but CREATE EXTENSION REPOSITORY
	static constexpr const char *FILE_EXTENSION = ".duckdb_extension.repo.json";
	//! The version of the repository file format
	static constexpr const idx_t FORMAT_VERSION = 1;
	//! The metadata file that a repository serves under its prefix, describing its trusted signing keys
	static constexpr const char *METADATA_FILE = ".well-known/duckdb-extension-repo.json";

public:
	//! The directory in which the trusted extension repositories are stored
	static string GetRepositoryDirectory(DatabaseInstance &db, FileSystem &fs);

	//! Store a new trusted repository on disk. If the repository has no public keys, they are fetched from the
	//! repository prefix and pinned. Returns the repository as it was stored
	static ExtensionRepository CreateRepository(DatabaseInstance &db, FileSystem &fs,
	                                            optional_ptr<ClientContext> context,
	                                            const ExtensionRepository &repository, OnCreateConflict on_conflict);
	//! Remove a trusted repository from disk
	static void DropRepository(DatabaseInstance &db, FileSystem &fs, const string &name,
	                           OnEntryNotFound on_entry_not_found);

	//! Get all trusted repositories that were added by the user
	static vector<ExtensionRepository> GetRepositories(DatabaseInstance &db, FileSystem &fs);
	//! Try to look up a trusted repository by name
	static bool TryGetRepository(DatabaseInstance &db, FileSystem &fs, const string &name, ExtensionRepository &result);

	//! Repository names are case insensitive and are used as file names - restrict them to a safe character set
	static string NormalizeRepositoryName(const string &name);
	static bool TryNormalizeRepositoryName(const string &name, string &result);

	//! Whether the user has opted in to adding external extension repositories
	static ExtensionRepositoryAccess GetAccess(DatabaseInstance &db);
	//! Whether the repositories that were already created are trusted: their extensions can be installed and loaded
	static bool TrustsExistingRepositories(DatabaseInstance &db);
	//! Parse the access value of the allow_extension_repositories setting (throws on an invalid value)
	static ExtensionRepositoryAccess ParseAccess(const string &value);
	//! The string representation of an access value, as used by the allow_extension_repositories setting
	static string AccessToString(ExtensionRepositoryAccess access);

	//! Fetch the trusted signing keys from the metadata file that a repository serves under its prefix. The file is a
	//! json object with a "signature_keys" array of PEM-encoded keys
	static vector<string> FetchPublicKeys(DatabaseInstance &db, optional_ptr<ClientContext> context,
	                                      const string &prefix);

	//! The sha256 fingerprint of a public key, used to compare a key against the fingerprint published by a repository
	static string GetPublicKeyFingerprint(const string &compact_key);

	//! Extract all public keys from a blob that may contain one or more keys in PEM or compact form
	static vector<string> ToCompactPublicKeys(const string &public_keys);
	//! Flatten a list of provided key blobs (each of which may contain multiple keys) into compact keys
	static vector<string> NormalizePublicKeys(const vector<string> &public_keys);
	//! Convert a public key in PEM or compact form into the compact single-line representation
	static string ToCompactPublicKey(const string &public_key);
	//! Convert a compact public key back into the PEM representation used for signature verification
	static string ToPEMPublicKey(const string &compact_key);

private:
	//! Throws unless adding a repository is currently allowed (opt-in and configuration lock)
	static void CheckCanCreate(DatabaseInstance &db);
	//! Throws if the configuration is locked, freezing the trust store
	static void CheckNotLocked(DatabaseInstance &db);
	static string GetRepositoryPath(DatabaseInstance &db, FileSystem &fs, const string &name);
	static ExtensionRepository ReadRepository(FileSystem &fs, const string &path);
};

} // namespace duckdb
