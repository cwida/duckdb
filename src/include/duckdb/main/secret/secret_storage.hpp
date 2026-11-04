//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/secret/secret_storage.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/on_create_conflict.hpp"
#include "duckdb/common/enums/on_entry_not_found.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/case_insensitive_map.hpp"

namespace duckdb {

class BaseSecret;
class Catalog;
class CatalogSet;
struct CatalogTransaction;
class DatabaseInstance;
class FileSystem;
struct SecretMatch;
struct SecretEntry;
class SecretManager;

//! Base class for SecretStorage API
class SecretStorage {
	friend class SecretManager;

public:
	explicit SecretStorage(const string &name, const int64_t tie_break_offset)
	    : storage_name(name), tie_break_offset(tie_break_offset), persistent(false) {};
	virtual ~SecretStorage() = default;

	//! Default storage backend offsets. Lower offsets win tie-breaks (equal path-match score), so the connection-scoped
	//! storage is preferred over the global ones when a connection secret and a global secret match equally well.
	static const int64_t TRANSACTION_STORAGE_OFFSET = 0;
	static const int64_t CONNECTION_STORAGE_OFFSET = 5;
	static const int64_t TEMPORARY_STORAGE_OFFSET = 10;
	static const int64_t LOCAL_FILE_STORAGE_OFFSET = 20;

public:
	//! SecretStorage API

	//! Get the storage name (e.g. local_file, :memory:)
	virtual string &GetName() {
		return storage_name;
	};

	//! Store a secret
	virtual unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                            optional_ptr<CatalogTransaction> transaction = nullptr) = 0;
	//! Get all secrets
	virtual vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction> transaction = nullptr) = 0;
	//! Drop secret by name
	virtual void DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
	                              optional_ptr<CatalogTransaction> transaction = nullptr) = 0;
	//! Get best match
	virtual SecretMatch LookupSecret(const string &path, const string &type,
	                                 optional_ptr<CatalogTransaction> transaction = nullptr) = 0;
	//! Get a secret by name
	virtual unique_ptr<SecretEntry> GetSecretByName(const string &name,
	                                                optional_ptr<CatalogTransaction> transaction = nullptr) = 0;

	//! Returns include_in_lookups, used to create secret storage
	virtual bool IncludeInLookups() {
		return true;
	}

	virtual bool Persistent() const {
		return persistent;
	}

protected:
	//! Helper function to select the best matching secret within a storage. Tie-breaks within a storage are broken
	//! by secret name by default.
	static SecretMatch SelectBestMatch(SecretEntry &secret_entry, const string &path, int64_t offset,
	                                   SecretMatch &current_best);

	//! Name of the storage backend (e.g. temporary, file, etc)
	string storage_name;
	//! Offset associated to this storage for tie-breaking secrets between storages
	const int64_t tie_break_offset;
	//! Whether entries in this storage will survive duckdb reboots
	bool persistent;
};

//! In-memory secret storage owned by a single transaction
class TransactionSecretStorage : public SecretStorage {
public:
	explicit TransactionSecretStorage(const string &name_p);
	~TransactionSecretStorage() override;

public:
	unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                    optional_ptr<CatalogTransaction> transaction = nullptr) override;
	vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction> transaction = nullptr) override;
	void DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
	                      optional_ptr<CatalogTransaction> transaction = nullptr) override;
	SecretMatch LookupSecret(const string &path, const string &type,
	                         optional_ptr<CatalogTransaction> transaction = nullptr) override;
	unique_ptr<SecretEntry> GetSecretByName(const string &name,
	                                        optional_ptr<CatalogTransaction> transaction = nullptr) override;

private:
	mutex lock;
	identifier_map_t<unique_ptr<SecretEntry>> secrets;
};

//! Wrapper struct around a SecretEntry to allow storing it
struct SecretCatalogEntry : public InCatalogEntry {
public:
	SecretCatalogEntry(unique_ptr<SecretEntry> secret_p, Catalog &catalog);
	SecretCatalogEntry(unique_ptr<const BaseSecret> secret_p, Catalog &catalog);

	//! The secret to store in a catalog
	unique_ptr<SecretEntry> secret;
};

//! Base Implementation for catalog set based secret storage
class CatalogSetSecretStorage : public SecretStorage {
public:
	CatalogSetSecretStorage(DatabaseInstance &db_instance, const string &name_p, const int64_t offset)
	    : SecretStorage(name_p, offset), db(db_instance) {};

public:
	//! SecretStorage API
	string &GetName() override {
		return storage_name;
	};

	unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                    optional_ptr<CatalogTransaction> transaction = nullptr) override;
	vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction> transaction = nullptr) override;
	void DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
	                      optional_ptr<CatalogTransaction> transaction = nullptr) override;
	SecretMatch LookupSecret(const string &path, const string &type,
	                         optional_ptr<CatalogTransaction> transaction = nullptr) override;
	unique_ptr<SecretEntry> GetSecretByName(const string &name,
	                                        optional_ptr<CatalogTransaction> transaction = nullptr) override;

protected:
	//! Callback called on Store to allow child classes to implement persistence.
	virtual void WriteSecret(const BaseSecret &secret, OnCreateConflict on_conflict);
	virtual void RemoveSecret(const string &name, OnEntryNotFound on_entry_not_found);
	//! Returns the CatalogTransaction in `transaction` if not set, return the System transaction
	CatalogTransaction GetTransactionOrDefault(optional_ptr<CatalogTransaction> transaction);

	//! CatalogSet containing the secrets
	unique_ptr<CatalogSet> secrets;
	//! DB instance for accessing the system catalog transaction
	DatabaseInstance &db;
};

class TemporarySecretStorage : public CatalogSetSecretStorage {
public:
	TemporarySecretStorage(const string &name_p, DatabaseInstance &db_p)
	    : CatalogSetSecretStorage(db_p, name_p, TEMPORARY_STORAGE_OFFSET) {
		secrets = make_uniq<CatalogSet>(Catalog::GetSystemCatalog(db));
		persistent = false;
	}

protected:
};

//! The file of each persistent secret. The file name alone does not identify the secret: names are escaped in file
//! names, and files written before that are named after the plain secret name, so one file name can belong to two
//! different secrets (the escaped `a/b` and the plain `a%2Fb`).
class PersistentSecretFiles {
public:
	//! Registers the file that holds the secret
	void Set(const Identifier &secret_name, const string &path);
	//! Unregisters the secret
	void Remove(const Identifier &secret_name);
	//! The file that holds the secret, or an empty string if the secret has no file
	string Get(const Identifier &secret_name);
	//! Whether the file at the path holds a secret other than the given one
	bool TryGetOtherSecretAt(const string &path, const Identifier &secret_name, string &other_secret_name);

private:
	mutex lock;
	identifier_map_t<string> paths;
};

class LocalFileSecretStorage : public CatalogSetSecretStorage {
public:
	LocalFileSecretStorage(SecretManager &manager, DatabaseInstance &db, const string &name_p,
	                       const string &secret_path);

public:
	//! The file name of a persistent secret: path separators, ':', NUL, '%' and a leading '.' in the name are
	//! percent-encoded
	static string SecretFileName(const string &secret_name);
	//! The secret name for a persistent secret file name (reverses SecretFileName)
	static string SecretNameFromFileName(const string &file_name);

protected:
	//! Implements the writes to disk
	void WriteSecret(const BaseSecret &secret, OnCreateConflict on_conflict) override;
	//! Implements the deletes from disk
	void RemoveSecret(const string &secret, OnEntryNotFound on_entry_not_found) override;

	//! Set of persistent secrets that are lazily loaded
	identifier_set_t persistent_secrets;
	//! The files of the persistent secrets, shared with the generator that loads them
	shared_ptr<PersistentSecretFiles> files;
	//! Path that is searched for secrets;
	string secret_path;
};

//! A secret storage whose secrets are scoped to the calling connection. It is registered once on the SecretManager but
//! presents a per-ClientContext view: every operation reads the connection out of the passed CatalogTransaction and
//! works only on that connection's secrets, which live in a state object on the connection's RegisteredStateManager.
//! That gives automatic, crash-robust cleanup (the secrets die with the ClientContext) and full isolation between
//! connections. A context-less lookup (e.g. a system/background call) simply returns no match, so it never leaks
//! across connections and never resolves where there is no connection to attribute the request to.
//! The container keeps the pre-image of every secret the active transaction touched, so a CREATE or DROP inside an
//! aborted transaction is undone, like in the global storages.
class ConnectionSecretStorage : public SecretStorage {
public:
	explicit ConnectionSecretStorage(const string &name_p) : SecretStorage(name_p, CONNECTION_STORAGE_OFFSET) {
		// persistent stays false (base default): these secrets are connection-lifetime only.
	}

public:
	unique_ptr<SecretEntry> StoreSecret(unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                    optional_ptr<CatalogTransaction> transaction = nullptr) override;
	vector<SecretEntry> AllSecrets(optional_ptr<CatalogTransaction> transaction = nullptr) override;
	void DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
	                      optional_ptr<CatalogTransaction> transaction = nullptr) override;
	SecretMatch LookupSecret(const string &path, const string &type,
	                         optional_ptr<CatalogTransaction> transaction = nullptr) override;
	unique_ptr<SecretEntry> GetSecretByName(const string &name,
	                                        optional_ptr<CatalogTransaction> transaction = nullptr) override;
};

} // namespace duckdb
