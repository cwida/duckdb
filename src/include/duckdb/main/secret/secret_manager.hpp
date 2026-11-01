//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/secret/duck_secret_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "duckdb/parser/parsed_data/create_secret_info.hpp"

namespace duckdb {

struct BoundStatement;
struct DBConfig;
class SecretManager;
class SchemaCatalogEntry;

//! A Secret Entry in the secret manager
struct SecretEntry {
public:
	explicit SecretEntry(unique_ptr<const BaseSecret> secret) : secret(secret != nullptr ? secret->Clone() : nullptr) {
	}
	SecretEntry(const SecretEntry &other)
	    : persist_type(other.persist_type), storage_mode(other.storage_mode),
	      secret((other.secret != nullptr) ? other.secret->Clone() : nullptr) {
	}

	//! Lifetime of the secret
	SecretPersistType persist_type;
	//! The storage backend of the secret
	string storage_mode;
	//! The secret pointer
	unique_ptr<const BaseSecret> secret;
};

//! Return value of a Secret Lookup
struct SecretMatch {
public:
	SecretMatch() : secret_entry(nullptr), score(NumericLimits<int64_t>::Minimum()) {
	}

	SecretMatch(const SecretMatch &other)
	    : secret_entry((other.secret_entry != nullptr) ? make_uniq<SecretEntry>(*other.secret_entry) : nullptr),
	      score(other.score) {
	}

	SecretMatch(SecretEntry &secret_entry, int64_t score)
	    : secret_entry(make_uniq<SecretEntry>(secret_entry)), score(score) {
	}

	SecretMatch &operator=(const SecretMatch &other) {
		this->secret_entry = (other.secret_entry != nullptr) ? make_uniq<SecretEntry>(*other.secret_entry) : nullptr;
		this->score = other.score;
		return *this;
	};

	//! Get the secret
	const BaseSecret &GetSecret() const;

	bool HasMatch() {
		return secret_entry != nullptr;
	}

	unique_ptr<SecretEntry> secret_entry;
	int64_t score;
};

struct SecretManagerConfig {
	static constexpr const bool DEFAULT_ALLOW_PERSISTENT_SECRETS = true;
	//! The default persistence type for secrets
	SecretPersistType default_persist_type = SecretPersistType::TEMPORARY;
	//! Secret Path can be changed by until the secret manager is initialized, after that it will be set automatically
	string secret_path = "";
	//! The default secret path is determined on startup and can be used to reset the secret path
	string default_secret_path = "";
	//! The storage type for persistent secrets when none is specified;
	string default_persistent_storage = "";
	//! Persistent secrets are enabled by default
	bool allow_persistent_secrets = DEFAULT_ALLOW_PERSISTENT_SECRETS;
};

//! The Secret Manager for DuckDB. Handles transaction, temporary and persistent secrets
class SecretManager {
	friend struct SecretEntry;

public:
	explicit SecretManager() = default;
	virtual ~SecretManager() = default;

	//! The default storage backends
	static constexpr const char *TEMPORARY_STORAGE_NAME = "memory";
	static constexpr const char *LOCAL_FILE_STORAGE_NAME = "local_file";
	static constexpr const char *TRANSACTION_STORAGE_NAME = "transaction";
	//! Connection-scoped storage: secrets live only for the creating connection and are reaped when it closes.
	static constexpr const char *CONNECTION_STORAGE_NAME = "connection";

	//! Static Helper Functions
	DUCKDB_API static SecretManager &Get(ClientContext &context);
	DUCKDB_API static SecretManager &Get(DatabaseInstance &db);

	// Initialize the secret manager with the DB instance
	DUCKDB_API void Initialize(DatabaseInstance &db);
	//! Load a secret storage
	DUCKDB_API void LoadSecretStorage(unique_ptr<SecretStorage> storage);

	//! Deserialize a secret by automatically selecting the correct deserializer, secret_path can be set to improve
	//! error hints
	DUCKDB_API unique_ptr<BaseSecret> DeserializeSecret(Deserializer &deserializer, const string &secret_path = "");
	//! Register a new SecretType
	DUCKDB_API void RegisterSecretType(SecretType &type);
	//! Lookup a SecretType
	DUCKDB_API SecretType LookupType(const string &type);
	//! Register a Secret Function i.e. a secret provider for a secret type
	DUCKDB_API void RegisterSecretFunction(CreateSecretFunction function, OnCreateConflict on_conflict);
	//! Register a secret by providing a secret manually
	DUCKDB_API unique_ptr<SecretEntry> RegisterSecret(CatalogTransaction transaction,
	                                                  unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
	                                                  SecretPersistType persist_type, const string &storage = "");
	//! Create a secret from a CreateSecretInfo
	DUCKDB_API unique_ptr<SecretEntry> CreateSecret(ClientContext &context, const CreateSecretInput &info);
	//! The Bind for create secret is done by the secret manager
	DUCKDB_API BoundStatement BindCreateSecret(CatalogTransaction transaction, CreateSecretInput &info);
	//! Lookup the best matching secret by matching the secret scopes to the path
	DUCKDB_API SecretMatch LookupSecret(CatalogTransaction transaction, const string &path, const string &type);
	//! Get a secret by name, optionally from a specific storage
	DUCKDB_API unique_ptr<SecretEntry> GetSecretByName(CatalogTransaction transaction, const string &name,
	                                                   const string &storage = "");
	//! Delete a secret by name, optionally by providing the storage to drop from
	DUCKDB_API void DropSecretByName(CatalogTransaction transaction, const Identifier &name,
	                                 OnEntryNotFound on_entry_not_found,
	                                 SecretPersistType persist_type = SecretPersistType::DEFAULT,
	                                 const Identifier &storage = "");
	//! List all secrets from all secret storages
	DUCKDB_API vector<SecretEntry> AllSecrets(CatalogTransaction transaction);

	//! List all secret types
	DUCKDB_API vector<SecretType> AllSecretTypes();
	//! List all secret functions
	DUCKDB_API vector<CreateSecretFunction> AllSecretFunctions();

	//! Secret Manager settings
	DUCKDB_API virtual void SetEnablePersistentSecrets(bool enabled);
	DUCKDB_API virtual void ResetEnablePersistentSecrets();
	DUCKDB_API virtual bool PersistentSecretsEnabled();

	DUCKDB_API virtual void SetDefaultStorage(const string &storage);
	DUCKDB_API virtual void ResetDefaultStorage();
	DUCKDB_API virtual string DefaultStorage();

	DUCKDB_API virtual void SetPersistentSecretPath(const string &path);
	DUCKDB_API virtual void ResetPersistentSecretPath();
	DUCKDB_API virtual string PersistentSecretPath();

	//! Utility functions
	DUCKDB_API void DropSecretByName(ClientContext &context, const Identifier &name, OnEntryNotFound on_entry_not_found,
	                                 SecretPersistType persist_type = SecretPersistType::DEFAULT,
	                                 const Identifier &storage = "");

private:
	//! Register a secret type
	void RegisterSecretTypeInternal(SecretType &type);
	//! Lookup a SecretType, throws if not found
	SecretType LookupTypeInternal(const Identifier &type);
	//! Try to lookup a SecretType
	bool TryLookupTypeInternal(const Identifier &type, SecretType &type_out);
	//! Register a secret provider
	void RegisterSecretFunctionInternal(CreateSecretFunction function, OnCreateConflict on_conflict);
	//! Lookup a CreateSecretFunction
	optional_ptr<CreateSecretFunction> LookupFunctionInternal(const Identifier &type, const Identifier &provider);
	//! Register a new Secret
	unique_ptr<SecretEntry> RegisterSecretInternal(CatalogTransaction transaction, unique_ptr<const BaseSecret> secret,
	                                               OnCreateConflict on_conflict, SecretPersistType persist_type,
	                                               const Identifier &storage = Identifier());
	//! Initialize the secret catalog_set and persistent secrets (lazily)
	void InitializeSecrets(CatalogTransaction transaction);
	//! Load a secret storage
	void LoadSecretStorageInternal(unique_ptr<SecretStorage> storage);

	//! Autoload extension for specific secret type
	void AutoloadExtensionForType(const Identifier &type);
	//! Autoload extension for specific secret function
	void AutoloadExtensionForFunction(const Identifier &type, const Identifier &provider);

	//! Will throw appropriate error message when type not found
	[[noreturn]] void ThrowTypeNotFoundError(const Identifier &type, const string &secret_path = "");
	[[noreturn]] void ThrowProviderNotFoundError(const Identifier &type, const Identifier &provider,
	                                             bool was_default = false);

	//! Thread-safe accessors for secret_storages
	vector<reference<SecretStorage>> GetSecretStorages();
	optional_ptr<SecretStorage> GetSecretStorage(const Identifier &name);
	optional_ptr<SecretStorage> GetTransactionSecretStorage(CatalogTransaction transaction);
	SecretStorage &GetOrCreateTransactionSecretStorage(CatalogTransaction transaction);

	//! Throw an exception if the secret manager is initialized
	void ThrowOnSettingChangeIfInitialized();

	//! Lock for types, functions, settings and storages
	mutex manager_lock;
	//! Secret functions;
	identifier_map_t<CreateSecretFunctionSet> secret_functions;
	//! Secret types;
	identifier_map_t<SecretType> secret_types;
	//! Map of all registered SecretStorages
	identifier_map_t<unique_ptr<SecretStorage>> secret_storages;
	//! While false, secret manager settings can still be changed
	atomic<bool> initialized {false};
	//! Configuration for secret manager
	SecretManagerConfig config;
	//! Pointer to current db instance
	optional_ptr<DatabaseInstance> db;
};

//! The DefaultGenerator for persistent secrets. This is used to store lazy loaded secrets in the catalog
class DefaultSecretGenerator : public DefaultGenerator {
public:
	DefaultSecretGenerator(Catalog &catalog, SecretManager &secret_manager, identifier_set_t &persistent_secrets);

public:
	unique_ptr<CatalogEntry> CreateDefaultEntry(CatalogTransaction transaction, const Identifier &entry_name) override;
	unique_ptr<CatalogEntry> CreateDefaultEntry(ClientContext &context, const Identifier &entry_name) override;
	vector<Identifier> GetDefaultEntries() override;
	bool LockDuringCreate() const override {
		return true;
	}

protected:
	unique_ptr<CatalogEntry> CreateDefaultEntryInternal(const Identifier &entry_name);

	SecretManager &secret_manager;
	mutex lock;
	identifier_set_t persistent_secrets;
};

} // namespace duckdb
