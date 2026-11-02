#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/binary_deserializer.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "duckdb/main/secret/default_secrets.hpp"
#include "duckdb/parser/parsed_data/create_secret_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/planner/operator/logical_create_secret.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

SecretCatalogEntry::SecretCatalogEntry(unique_ptr<SecretEntry> secret_p, Catalog &catalog)
    : InCatalogEntry(CatalogType::SECRET_ENTRY, catalog, secret_p->secret->GetName()), secret(std::move(secret_p)) {
	internal = true;
}

SecretCatalogEntry::SecretCatalogEntry(unique_ptr<const BaseSecret> secret_p, Catalog &catalog)
    : InCatalogEntry(CatalogType::SECRET_ENTRY, catalog, secret_p->GetName()) {
	internal = true;
	secret = make_uniq<SecretEntry>(std::move(secret_p));
}

const BaseSecret &SecretMatch::GetSecret() const {
	return *secret_entry->secret;
}

constexpr const char *SecretManager::TEMPORARY_STORAGE_NAME;
constexpr const char *SecretManager::LOCAL_FILE_STORAGE_NAME;
constexpr const char *SecretManager::TRANSACTION_STORAGE_NAME;

void SecretManager::Initialize(DatabaseInstance &db) {
	lock_guard<mutex> lck(manager_lock);

	// Construct default path
	auto &fs = FileSystem::GetLocal(db);
	config.default_secret_path = fs.GetHomeDirectory();
	vector<string> path_components = {".duckdb", "stored_secrets"};
	for (auto &path_ele : path_components) {
		config.default_secret_path = fs.JoinPath(config.default_secret_path, path_ele);
	}
	// Use default path if none has been specified by the user configuration
	if (config.secret_path.empty()) {
		config.secret_path = config.default_secret_path;
	}

	// Set the defaults for persistent storage
	config.default_persistent_storage = LOCAL_FILE_STORAGE_NAME;

	// Store the current db for enabling autoloading
	this->db = &db;

	// Register default types
	for (auto &type : CreateHTTPSecretFunctions::GetDefaultSecretTypes()) {
		RegisterSecretTypeInternal(type);
	}

	// Register default functions
	for (auto &function : CreateHTTPSecretFunctions::GetDefaultSecretFunctions()) {
		RegisterSecretFunctionInternal(function, OnCreateConflict::ERROR_ON_CONFLICT);
	}
}

void SecretManager::LoadSecretStorage(unique_ptr<SecretStorage> storage) {
	lock_guard<mutex> lck(manager_lock);
	return LoadSecretStorageInternal(std::move(storage));
}

void SecretManager::LoadSecretStorageInternal(unique_ptr<SecretStorage> storage) {
	if (StringUtil::CIEquals(storage->GetName(), TRANSACTION_STORAGE_NAME)) {
		throw InvalidConfigurationException("Secret Storage name '%s' is reserved!", storage->GetName());
	}
	if (secret_storages.find(Identifier(storage->GetName())) != secret_storages.end()) {
		throw InvalidConfigurationException("Secret Storage with name '%s' already registered!", storage->GetName());
	}

	// Check for tie-break offset collisions to ensure we can always tie-break cleanly
	for (const auto &storage_ptr : secret_storages) {
		if (storage_ptr.second->tie_break_offset == storage->tie_break_offset) {
			throw InvalidConfigurationException(
			    "Failed to load secret storage '%s', tie break score collides with '%s'", storage->GetName(),
			    storage_ptr.second->GetName());
		}
	}

	secret_storages[Identifier(storage->GetName())] = std::move(storage);
}

// FIXME: use serialization scripts?
unique_ptr<BaseSecret> SecretManager::DeserializeSecret(Deserializer &deserializer, const string &secret_path) {
	auto type = deserializer.ReadProperty<Identifier>(100, "type");
	auto provider = deserializer.ReadProperty<Identifier>(101, "provider");
	auto name = deserializer.ReadProperty<Identifier>(102, "name");
	vector<string> scope;
	deserializer.ReadList(103, "scope",
	                      [&](Deserializer::List &list, idx_t i) { scope.push_back(list.ReadElement<string>()); });
	auto serialization_type = deserializer.ReadPropertyWithExplicitDefault(104, "serialization_type",
	                                                                       SecretSerializationType::KEY_VALUE_SECRET);

	switch (serialization_type) {
	// This allows us to skip looking up the secret type for deserialization altogether
	case SecretSerializationType::KEY_VALUE_SECRET: {
		auto result =
		    KeyValueSecret::Deserialize<KeyValueSecret>(deserializer, BaseSecret(scope, type, provider, name));
		if (type == "http") {
			// Preserve stored keys and apply current HTTP redaction rules to older secrets.
			const auto keys = CreateHTTPSecretFunctions::GetRedactedKeys();
			auto &secret = result->Cast<KeyValueSecret>();
			secret.redact_keys.insert(keys.begin(), keys.end());
		}
		return result;
	}
	// Continues below: we need to do a type lookup to find the secret deserialize method
	case SecretSerializationType::CUSTOM:
		break;
	default:
		throw IOException("Unrecognized secret serialization type found in secret '%s': %s", secret_path,
		                  EnumUtil::ToString(serialization_type));
	}

	SecretType deserialized_type;
	if (!TryLookupTypeInternal(type, deserialized_type)) {
		ThrowTypeNotFoundError(type, secret_path);
	}

	if (!deserialized_type.deserializer) {
		throw InvalidConfigurationException(
		    "Attempted to deserialize secret type %s which does not have a deserialization method", type);
	}

	return deserialized_type.deserializer(deserializer, BaseSecret(scope, type, provider, name));
}

void SecretManager::RegisterSecretType(SecretType &type) {
	lock_guard<mutex> lck(manager_lock);
	RegisterSecretTypeInternal(type);
}

void SecretManager::RegisterSecretFunction(CreateSecretFunction function, OnCreateConflict on_conflict) {
	unique_lock<mutex> lck(manager_lock);
	RegisterSecretFunctionInternal(std::move(function), on_conflict);
}

unique_ptr<SecretEntry> SecretManager::RegisterSecret(CatalogTransaction transaction,
                                                      unique_ptr<const BaseSecret> secret, OnCreateConflict on_conflict,
                                                      SecretPersistType persist_type, const string &storage) {
	InitializeSecrets(transaction);
	return RegisterSecretInternal(transaction, std::move(secret), on_conflict, persist_type, Identifier(storage));
}

unique_ptr<SecretEntry> SecretManager::RegisterSecretInternal(CatalogTransaction transaction,
                                                              unique_ptr<const BaseSecret> secret,
                                                              OnCreateConflict on_conflict,
                                                              SecretPersistType persist_type,
                                                              const Identifier &storage) {
	//! Ensure we only create secrets for known types;
	LookupTypeInternal(secret->GetType());
	if (persist_type == SecretPersistType::TRANSACTION) {
		if (!storage.empty()) {
			throw InvalidInputException("Transaction secrets do not support explicit storage");
		}
		auto &backend = GetOrCreateTransactionSecretStorage(transaction);
		return backend.StoreSecret(std::move(secret), on_conflict, &transaction);
	}

	//! Handle default for persist type
	if (persist_type == SecretPersistType::DEFAULT) {
		if (storage.empty()) {
			persist_type = config.default_persist_type;
		} else {
			// Derive the persist type from the named storage's own flag, so that *any* registered temporary storage
			// (not only "memory") accepts a default `CREATE SECRET ... IN <storage>` without the TEMPORARY keyword.
			auto storage_backend = GetSecretStorage(storage);
			persist_type = (storage_backend && !storage_backend->Persistent()) ? SecretPersistType::TEMPORARY
			                                                                   : SecretPersistType::PERSISTENT;
		}
	}

	//! Resolve storage
	string resolved_storage;
	if (storage.empty()) {
		resolved_storage = persist_type == SecretPersistType::PERSISTENT ? DefaultStorage() : TEMPORARY_STORAGE_NAME;
	} else {
		resolved_storage = storage.GetIdentifierName();
	}

	//! Lookup which backend to store the secret in
	auto backend = GetSecretStorage(Identifier(resolved_storage));
	if (!backend) {
		if (!config.allow_persistent_secrets &&
		    (persist_type == SecretPersistType::PERSISTENT || storage == LOCAL_FILE_STORAGE_NAME)) {
			throw InvalidInputException("Persistent secrets are disabled. Restart DuckDB and enable persistent secrets "
			                            "through 'SET allow_persistent_secrets=true'");
		}
		throw InvalidInputException("Secret storage '%s' not found!", resolved_storage);
	}

	// Validation on both allow_persistent_secrets and storage backend's own persist type
	if (persist_type == SecretPersistType::PERSISTENT) {
		if (backend->persistent) {
			if (!config.allow_persistent_secrets) {
				throw InvalidInputException(
				    "Persistent secrets are currently disabled. To enable them, restart duckdb and "
				    "run 'SET allow_persistent_secrets=true'");
			}
		} else { // backend is temp
			throw InvalidInputException("Cannot create persistent secrets in a temporary secret storage!");
		}
	} else { // SecretPersistType::TEMPORARY
		if (backend->persistent) {
			throw InvalidInputException("Cannot create temporary secrets in a persistent secret storage!");
		}
	}
	return backend->StoreSecret(std::move(secret), on_conflict, &transaction);
}

optional_ptr<CreateSecretFunction> SecretManager::LookupFunctionInternal(const Identifier &type,
                                                                         const Identifier &provider) {
	unique_lock<mutex> lck(manager_lock);
	auto lookup = secret_functions.find(type);

	if (lookup != secret_functions.end()) {
		if (lookup->second.ProviderExists(provider)) {
			return &lookup->second.GetFunction(provider.GetIdentifierName());
		}
	}

	// Try autoloading
	lck.unlock();
	AutoloadExtensionForFunction(type, provider);
	lck.lock();

	lookup = secret_functions.find(type);

	if (lookup != secret_functions.end()) {
		if (lookup->second.ProviderExists(provider)) {
			return &lookup->second.GetFunction(provider.GetIdentifierName());
		}
	}

	return nullptr;
}

unique_ptr<SecretEntry> SecretManager::CreateSecret(ClientContext &context, const CreateSecretInput &input) {
	// Note that a context is required for CreateSecret, as the CreateSecretFunction expects one
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	InitializeSecrets(transaction);

	// Make a copy to set the provider to default if necessary
	auto function_input = input;
	if (function_input.provider.empty()) {
		auto secret_type = LookupTypeInternal(function_input.type);
		function_input.provider = Identifier(secret_type.default_provider);
	}

	// Lookup function
	auto function_lookup = LookupFunctionInternal(function_input.type, function_input.provider);
	if (!function_lookup) {
		ThrowProviderNotFoundError(input.type, input.provider);
	}

	// Call the function
	auto secret = function_lookup->function(context, function_input);

	if (!secret) {
		throw InternalException("CreateSecretFunction for type: %s and provider: %s did not return a secret!",
		                        input.type, input.provider);
	}

	// Register the secret at the secret_manager
	return RegisterSecretInternal(transaction, std::move(secret), input.on_conflict, input.persist_type,
	                              input.storage_type);
}

BoundStatement SecretManager::BindCreateSecret(CatalogTransaction transaction, CreateSecretInput &info) {
	if (info.persist_type == SecretPersistType::TRANSACTION || info.storage_type == TRANSACTION_STORAGE_NAME) {
		throw BinderException("Transaction-scoped secrets cannot be created through SQL");
	}
	InitializeSecrets(transaction);

	auto type = info.type;
	auto provider = info.provider;
	bool default_provider = false;

	if (provider.empty()) {
		default_provider = true;
		auto secret_type = LookupTypeInternal(type);
		provider = Identifier(secret_type.default_provider);
	}

	string default_string = default_provider ? "default " : "";

	auto function = LookupFunctionInternal(type, provider);

	if (!function) {
		ThrowProviderNotFoundError(info.type, info.provider, default_provider);
	}

	auto bound_info = info;
	bound_info.options.clear();

	// We cast the passed parameters
	for (const auto &param : info.options) {
		auto matched_param = function->named_parameters.find(Identifier(param.first));
		if (matched_param == function->named_parameters.end()) {
			throw BinderException("Unknown parameter %s for secret type %s with %sprovider %s", Identifier(param.first),
			                      type, default_string, provider);
		}

		// Cast the provided value to the expected type
		string error_msg;
		auto cast_value = param.second.DefaultTryCastAs(matched_param->second, &error_msg);
		if (!cast_value) {
			throw BinderException("Failed to cast option %s to type '%s': '%s'", matched_param->first,
			                      matched_param->second.ToString(), error_msg);
		}

		bound_info.options[Identifier(matched_param->first.GetIdentifierName()).GetIdentifierName()] = {*cast_value};
	}

	BoundStatement result;
	result.names = {"Success"};
	result.types = {LogicalType::BOOLEAN};
	result.plan = make_uniq<LogicalCreateSecret>(std::move(bound_info));
	return result;
}

SecretMatch SecretManager::LookupSecret(CatalogTransaction transaction, const string &path, const string &type) {
	InitializeSecrets(transaction);

	int64_t best_match_score = NumericLimits<int64_t>::Minimum();
	unique_ptr<SecretEntry> best_match = nullptr;
	if (auto transaction_storage = GetTransactionSecretStorage(transaction)) {
		auto match = transaction_storage->LookupSecret(path, StringUtil::Lower(type), &transaction);
		if (match.HasMatch()) {
			best_match = std::move(match.secret_entry);
			best_match_score = match.score;
		}
	}

	for (const auto &storage_ref : GetSecretStorages()) {
		if (!storage_ref.get().IncludeInLookups()) {
			continue;
		}
		auto match = storage_ref.get().LookupSecret(path, StringUtil::Lower(type), &transaction);
		if (match.HasMatch() && match.score > best_match_score) {
			best_match = std::move(match.secret_entry);
			best_match_score = match.score;
		}
	}

	if (best_match) {
		return SecretMatch(*best_match, best_match_score);
	}

	return SecretMatch();
}

unique_ptr<SecretEntry> SecretManager::GetSecretByName(CatalogTransaction transaction, const string &name,
                                                       const string &storage_p) {
	InitializeSecrets(transaction);
	auto storage = Identifier(storage_p);
	if (storage.empty() || storage == TRANSACTION_STORAGE_NAME) {
		if (auto transaction_storage = GetTransactionSecretStorage(transaction)) {
			auto result = transaction_storage->GetSecretByName(name, &transaction);
			if (result || storage == TRANSACTION_STORAGE_NAME) {
				return result;
			}
		} else if (storage == TRANSACTION_STORAGE_NAME) {
			return nullptr;
		}
	}

	unique_ptr<SecretEntry> result = nullptr;
	bool found = false;

	if (!storage.empty()) {
		auto storage_lookup = GetSecretStorage(storage);

		if (!storage_lookup) {
			throw InvalidInputException("Unknown secret storage found: %s", storage);
		}

		return storage_lookup->GetSecretByName(name, &transaction);
	}

	for (const auto &storage_ref : GetSecretStorages()) {
		auto lookup = storage_ref.get().GetSecretByName(name, &transaction);
		if (lookup) {
			if (found) {
				throw InvalidConfigurationException(
				    "Ambiguity detected for secret name '%s', secret occurs in multiple storage backends.", name);
			}

			result = std::move(lookup);
			found = true;
		}
	}

	return result;
}

void SecretManager::DropSecretByName(CatalogTransaction transaction, const Identifier &name,
                                     OnEntryNotFound on_entry_not_found, SecretPersistType persist_type,
                                     const Identifier &storage) {
	InitializeSecrets(transaction);
	auto transaction_storage = GetTransactionSecretStorage(transaction);
	if (storage == TRANSACTION_STORAGE_NAME || persist_type == SecretPersistType::TRANSACTION) {
		if (transaction_storage) {
			return transaction_storage->DropSecretByName(name, on_entry_not_found, &transaction);
		}
		if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw InvalidInputException("Failed to remove non-existent transaction secret with name %s", name);
		}
		return;
	}
	if (storage.empty() && persist_type == SecretPersistType::DEFAULT && transaction_storage &&
	    transaction_storage->GetSecretByName(name.GetIdentifierName(), &transaction)) {
		return transaction_storage->DropSecretByName(name, on_entry_not_found, &transaction);
	}

	vector<reference<SecretStorage>> matches;

	// storage to drop from was specified directly
	if (!storage.empty()) {
		auto storage_lookup = GetSecretStorage(Identifier(storage));
		if (!storage_lookup) {
			throw InvalidInputException("Unknown storage type found for drop secret: %s", storage);
		}
		matches.push_back(*storage_lookup.get());
	} else {
		for (const auto &storage_ref : GetSecretStorages()) {
			if (persist_type == SecretPersistType::PERSISTENT && !storage_ref.get().Persistent()) {
				continue;
			}
			if (persist_type == SecretPersistType::TEMPORARY && storage_ref.get().Persistent()) {
				continue;
			}

			auto lookup = storage_ref.get().GetSecretByName(name.GetIdentifierName(), &transaction);
			if (lookup) {
				matches.push_back(storage_ref.get());
			}
		}
	}

	if (matches.size() > 1) {
		string list_of_matches;
		for (const auto &match : matches) {
			list_of_matches += match.get().GetName() + ",";
		}
		list_of_matches.pop_back(); // trailing comma

		throw InvalidInputException(
		    "Ambiguity found for secret name %s, secret occurs in multiple storages: [%s] Please specify which "
		    "secret to drop using: 'DROP <PERSISTENT|TEMPORARY> SECRET [FROM <storage>]'.",
		    name, list_of_matches);
	}

	if (matches.empty()) {
		if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			string storage_str;
			if (!storage.empty()) {
				storage_str = " for storage '" + storage + "'";
			}
			throw InvalidInputException("Failed to remove non-existent secret with name %s%s", name, storage_str);
		}
		// Do nothing on OnEntryNotFound::RETURN_NULL...
	} else {
		matches[0].get().DropSecretByName(Identifier(name), on_entry_not_found, &transaction);
	}
}

SecretType SecretManager::LookupType(const string &type) {
	return LookupTypeInternal(Identifier(type));
}

void SecretManager::RegisterSecretTypeInternal(SecretType &type) {
	auto lookup = secret_types.find(type.name);
	if (lookup != secret_types.end()) {
		throw InternalException("Attempted to register an already registered secret type: %s", type.name);
	}
	secret_types[type.name] = type;
}

bool SecretManager::TryLookupTypeInternal(const Identifier &type, SecretType &type_out) {
	unique_lock<mutex> lck(manager_lock);
	auto lookup = secret_types.find(type);
	if (lookup != secret_types.end()) {
		type_out = lookup->second;
		return true;
	}

	// Try autoloading
	lck.unlock();
	AutoloadExtensionForType(type);
	lck.lock();

	lookup = secret_types.find(type);
	if (lookup != secret_types.end()) {
		type_out = lookup->second;
		return true;
	}

	return false;
}

SecretType SecretManager::LookupTypeInternal(const Identifier &type) {
	SecretType return_value;
	if (!TryLookupTypeInternal(type, return_value)) {
		ThrowTypeNotFoundError(type);
	}
	return return_value;
}

void SecretManager::RegisterSecretFunctionInternal(CreateSecretFunction function, OnCreateConflict on_conflict) {
	auto lookup = secret_functions.find(Identifier(function.secret_type));
	if (lookup != secret_functions.end()) {
		lookup->second.AddFunction(function, on_conflict);
		return;
	}
	CreateSecretFunctionSet new_set(function.secret_type);
	new_set.AddFunction(function, OnCreateConflict::ERROR_ON_CONFLICT);
	secret_functions.insert(make_pair(Identifier(function.secret_type), new_set));
}

vector<SecretEntry> SecretManager::AllSecrets(CatalogTransaction transaction) {
	InitializeSecrets(transaction);

	vector<SecretEntry> result;
	if (auto transaction_storage = GetTransactionSecretStorage(transaction)) {
		auto transaction_secrets = transaction_storage->AllSecrets(&transaction);
		for (auto &secret : transaction_secrets) {
			result.push_back(std::move(secret));
		}
	}

	// Add results from all backends to the result set
	for (const auto &backend : GetSecretStorages()) {
		auto backend_result = backend.get().AllSecrets(&transaction);
		for (const auto &it : backend_result) {
			result.push_back(it);
		}
	}

	return result;
}

vector<SecretType> SecretManager::AllSecretTypes() {
	unique_lock<mutex> lck(manager_lock);
	vector<SecretType> result;

	for (const auto &secret : secret_types) {
		result.push_back(secret.second);
	}

	return result;
}

vector<CreateSecretFunction> SecretManager::AllSecretFunctions() {
	unique_lock<mutex> lck(manager_lock);
	vector<CreateSecretFunction> result;

	for (auto &secret_set : secret_functions) {
		auto &inner_functions = secret_set.second.GetFunctions();
		for (auto &func_pair : inner_functions) {
			result.push_back(func_pair.second);
		}
	}

	return result;
}

void SecretManager::ThrowOnSettingChangeIfInitialized() {
	if (initialized) {
		throw InvalidInputException(
		    "Changing Secret Manager settings after the secret manager is used is not allowed!");
	}
}

void SecretManager::SetEnablePersistentSecrets(bool enabled) {
	ThrowOnSettingChangeIfInitialized();
	config.allow_persistent_secrets = enabled;
}

void SecretManager::ResetEnablePersistentSecrets() {
	ThrowOnSettingChangeIfInitialized();
	config.allow_persistent_secrets = SecretManagerConfig::DEFAULT_ALLOW_PERSISTENT_SECRETS;
}

bool SecretManager::PersistentSecretsEnabled() {
	return config.allow_persistent_secrets;
}

void SecretManager::SetDefaultStorage(const string &storage) {
	lock_guard<mutex> lck(manager_lock);
	config.default_persistent_storage = storage;
}

void SecretManager::ResetDefaultStorage() {
	lock_guard<mutex> lck(manager_lock);
	config.default_persistent_storage = SecretManager::LOCAL_FILE_STORAGE_NAME;
}

string SecretManager::DefaultStorage() {
	lock_guard<mutex> lck(manager_lock);
	return config.default_persistent_storage;
}

void SecretManager::SetPersistentSecretPath(const string &path) {
	ThrowOnSettingChangeIfInitialized();
	config.secret_path = path;
}

void SecretManager::ResetPersistentSecretPath() {
	ThrowOnSettingChangeIfInitialized();
	config.secret_path = config.default_secret_path;
}

string SecretManager::PersistentSecretPath() {
	return config.secret_path;
}

void SecretManager::InitializeSecrets(CatalogTransaction transaction) {
	if (!initialized) {
		lock_guard<mutex> lck(manager_lock);
		if (initialized) {
			// some sneaky other thread beat us to it
			return;
		}

		// load the tmp storage
		LoadSecretStorageInternal(make_uniq<TemporarySecretStorage>(TEMPORARY_STORAGE_NAME, *transaction.db));

		// load the connection-scoped storage: secrets created `IN connection_storage` are visible only to the
		// connection that created them, and are dropped automatically when that connection closes.
		LoadSecretStorageInternal(make_uniq<ConnectionSecretStorage>(CONNECTION_STORAGE_NAME));

		if (config.allow_persistent_secrets) {
			// load the persistent storage if enabled
			LoadSecretStorageInternal(
			    make_uniq<LocalFileSecretStorage>(*this, *transaction.db, LOCAL_FILE_STORAGE_NAME, config.secret_path));
		}

		initialized = true;
	}
}

//! EXTENSION_SECRET_PROVIDERS is keyed on the composite "type/provider" name
static Identifier SecretProviderKey(const Identifier &type, const Identifier &provider) {
	return Identifier(type + "/" + provider);
}

void SecretManager::AutoloadExtensionForType(const Identifier &type) {
	ExtensionHelper::TryAutoloadFromEntry(*db, type, EXTENSION_SECRET_TYPES);
}

void SecretManager::ThrowTypeNotFoundError(const Identifier &type, const string &secret_path) {
	auto entry = ExtensionHelper::FindExtensionInEntries(type, EXTENSION_SECRET_TYPES);
	string error_message;

	if (!entry.empty() && db) {
		error_message = "Secret type '" + type + "' does not exist, but it exists in the " + entry + " extension.";
		error_message = ExtensionHelper::AddExtensionInstallHintToErrorMsg(*db, error_message, entry);

		if (!secret_path.empty()) {
			error_message += "\n\nAlternatively, ";
		}
	} else {
		error_message = StringUtil::Format("Secret type %s not found", type);

		if (!secret_path.empty()) {
			error_message += ", ";
		}
	}

	if (!secret_path.empty()) {
		error_message += StringUtil::Format("try removing the secret at path '%s'.", secret_path);
	}

	throw InvalidInputException(error_message);
}

void SecretManager::AutoloadExtensionForFunction(const Identifier &type, const Identifier &provider) {
	ExtensionHelper::TryAutoloadFromEntry(*db, SecretProviderKey(type, provider), EXTENSION_SECRET_PROVIDERS);
}

void SecretManager::ThrowProviderNotFoundError(const Identifier &type, const Identifier &provider, bool was_default) {
	auto entry = ExtensionHelper::FindExtensionInEntries(SecretProviderKey(type, provider), EXTENSION_SECRET_PROVIDERS);
	if (!entry.empty() && db) {
		string error_message = was_default ? "Default secret provider" : "Secret provider";
		error_message += StringUtil::Format(" %s for type %s does not exist, but it exists in the %s extension.",
		                                    provider, type, entry);
		error_message = ExtensionHelper::AddExtensionInstallHintToErrorMsg(*db, error_message, entry);

		throw InvalidInputException(error_message);
	}
	throw InvalidInputException("Secret provider %s not found for type %s", provider, type);
}

optional_ptr<SecretStorage> SecretManager::GetSecretStorage(const Identifier &name) {
	lock_guard<mutex> lock(manager_lock);

	auto lookup = secret_storages.find(name);
	if (lookup != secret_storages.end()) {
		return lookup->second.get();
	}

	return nullptr;
}

optional_ptr<SecretStorage> SecretManager::GetTransactionSecretStorage(CatalogTransaction transaction) {
	if (!transaction.HasContext()) {
		return nullptr;
	}
	auto &meta_transaction = MetaTransaction::Get(transaction.GetContext());
	lock_guard<mutex> guard(meta_transaction.lock);
	return meta_transaction.transaction_secret_storage.get();
}

SecretStorage &SecretManager::GetOrCreateTransactionSecretStorage(CatalogTransaction transaction) {
	if (!transaction.HasContext()) {
		throw InvalidInputException("Transaction secrets require an active client transaction");
	}
	auto &meta_transaction = MetaTransaction::Get(transaction.GetContext());
	lock_guard<mutex> guard(meta_transaction.lock);
	if (!meta_transaction.transaction_secret_storage) {
		meta_transaction.transaction_secret_storage = make_uniq<TransactionSecretStorage>(TRANSACTION_STORAGE_NAME);
	}
	return *meta_transaction.transaction_secret_storage;
}

vector<reference<SecretStorage>> SecretManager::GetSecretStorages() {
	lock_guard<mutex> lock(manager_lock);

	vector<reference<SecretStorage>> result;

	for (const auto &storage : secret_storages) {
		result.push_back(*storage.second);
	}

	return result;
}

DefaultSecretGenerator::DefaultSecretGenerator(Catalog &catalog, SecretManager &secret_manager,
                                               identifier_set_t &persistent_secrets)
    : DefaultGenerator(catalog), secret_manager(secret_manager), persistent_secrets(persistent_secrets) {
}

unique_ptr<CatalogEntry> DefaultSecretGenerator::CreateDefaultEntryInternal(const Identifier &entry_name) {
	lock_guard<mutex> guard(lock);
	auto secret_lu = persistent_secrets.find(Identifier(entry_name));
	if (secret_lu == persistent_secrets.end()) {
		return nullptr;
	}

	auto &fs = FileSystem::GetLocal(catalog.GetDatabase());

	string base_secret_path = secret_manager.PersistentSecretPath();
	string secret_path = fs.JoinPath(base_secret_path, entry_name + ".duckdb_secret");

	// Note each file should contain 1 secret
	try {
		auto file_reader = BufferedFileReader(fs, secret_path.c_str());

		if (!LocalFileSystem::IsPrivateFile(secret_path, nullptr)) {
			throw IOException(
			    "The secret file '%s' has incorrect permissions! Please set correct permissions or remove file",
			    secret_path);
		}

		if (!file_reader.Finished()) {
			BinaryDeserializer deserializer(file_reader);

			deserializer.Begin();
			auto deserialized_secret = secret_manager.DeserializeSecret(deserializer, secret_path);
			deserializer.End();

			auto entry = make_uniq<SecretCatalogEntry>(std::move(deserialized_secret), catalog);
			entry->secret->storage_mode = SecretManager::LOCAL_FILE_STORAGE_NAME;
			entry->secret->persist_type = SecretPersistType::PERSISTENT;

			// Finally: we remove the default entry from the persistent_secrets, otherwise we aren't able to drop it
			// later
			persistent_secrets.erase(secret_lu);

			return std::move(entry);
		}
	} catch (std::exception &ex) {
		ErrorData error(ex);
		switch (error.Type()) {
		case ExceptionType::SERIALIZATION:
			throw SerializationException(
			    "Failed to deserialize the persistent secret file: '%s'. The file maybe be "
			    "corrupt, please remove the file, restart and try again. (error message: '%s')",
			    secret_path, error.RawMessage());
		case ExceptionType::IO:
			throw IOException(
			    "Failed to open the persistent secret file: '%s'. Some other process may have removed it, "
			    "please restart and try again. (error message: '%s')",
			    secret_path, error.RawMessage());
		default:
			throw;
		}
	}

	throw SerializationException("Failed to deserialize secret %s from '%s': file appears empty! Please remove the "
	                             "file, restart and try again",
	                             entry_name, secret_path);
}

unique_ptr<CatalogEntry> DefaultSecretGenerator::CreateDefaultEntry(CatalogTransaction transaction,
                                                                    const Identifier &entry_name) {
	return CreateDefaultEntryInternal(entry_name);
}

unique_ptr<CatalogEntry> DefaultSecretGenerator::CreateDefaultEntry(ClientContext &context,
                                                                    const Identifier &entry_name) {
	return CreateDefaultEntryInternal(entry_name);
}

vector<Identifier> DefaultSecretGenerator::GetDefaultEntries() {
	vector<Identifier> ret;

	lock_guard<mutex> guard(lock);
	for (const auto &res : persistent_secrets) {
		ret.emplace_back(res.GetIdentifierName());
	}

	return ret;
}

SecretManager &SecretManager::Get(ClientContext &context) {
	return *DBConfig::GetConfig(context).secret_manager;
}
SecretManager &SecretManager::Get(DatabaseInstance &db) {
	return *DBConfig::GetConfig(db).secret_manager;
}

void SecretManager::DropSecretByName(ClientContext &context, const Identifier &name, OnEntryNotFound on_entry_not_found,
                                     SecretPersistType persist_type, const Identifier &storage) {
	auto transaction = CatalogTransaction::GetSystemCatalogTransaction(context);
	return DropSecretByName(transaction, name, on_entry_not_found, persist_type, storage);
}

} // namespace duckdb
