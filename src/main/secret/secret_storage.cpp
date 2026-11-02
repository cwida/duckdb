#include "duckdb/common/common.hpp"
#include "duckdb/common/multi_file/multi_file_list.hpp"
#include "duckdb/common/file_system.hpp"
#include "duckdb/common/local_file_system.hpp"
#include "duckdb/main/database_file_opener.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/common/serializer/buffered_file_reader.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/function/function_set.hpp"
#include "duckdb/main/extension_helper.hpp"
#include "duckdb/main/secret/secret_storage.hpp"
#include "duckdb/main/secret/secret_manager.hpp"
#include "duckdb/parser/parsed_data/create_secret_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/catalog/catalog_transaction.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/client_context_state.hpp"
#include "duckdb/main/secret/secret.hpp"
#include "duckdb/transaction/meta_transaction.hpp"

namespace duckdb {

TransactionSecretStorage::TransactionSecretStorage(const string &name_p)
    : SecretStorage(name_p, TRANSACTION_STORAGE_OFFSET) {
}

TransactionSecretStorage::~TransactionSecretStorage() = default;

unique_ptr<SecretEntry> TransactionSecretStorage::StoreSecret(unique_ptr<const BaseSecret> secret,
                                                              OnCreateConflict on_conflict,
                                                              optional_ptr<CatalogTransaction> transaction) {
	lock_guard<mutex> guard(lock);
	auto entry = secrets.find(secret->GetName());
	if (entry != secrets.end()) {
		switch (on_conflict) {
		case OnCreateConflict::ERROR_ON_CONFLICT:
			throw InvalidInputException("Transaction secret with name %s already exists!", secret->GetName());
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return nullptr;
		case OnCreateConflict::REPLACE_ON_CONFLICT:
			secrets.erase(entry);
			break;
		default:
			throw InternalException("Unsupported conflict mode for transaction secret");
		}
	}

	auto result = make_uniq<SecretEntry>(std::move(secret));
	result->persist_type = SecretPersistType::TRANSACTION;
	result->storage_mode = storage_name;
	auto secret_name = result->secret->GetName();
	secrets.emplace(secret_name, make_uniq<SecretEntry>(*result));
	return result;
}

vector<SecretEntry> TransactionSecretStorage::AllSecrets(optional_ptr<CatalogTransaction> transaction) {
	lock_guard<mutex> guard(lock);
	vector<SecretEntry> result;
	for (auto &entry : secrets) {
		result.emplace_back(*entry.second);
	}
	return result;
}

void TransactionSecretStorage::DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
                                                optional_ptr<CatalogTransaction> transaction) {
	lock_guard<mutex> guard(lock);
	auto entry = secrets.find(name);
	if (entry == secrets.end()) {
		if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			throw InvalidInputException("Failed to remove non-existent transaction secret %s", name);
		}
		return;
	}
	secrets.erase(entry);
}

SecretMatch TransactionSecretStorage::LookupSecret(const string &path, const string &type,
                                                   optional_ptr<CatalogTransaction> transaction) {
	lock_guard<mutex> guard(lock);
	auto best_match = SecretMatch();
	for (auto &entry : secrets) {
		if (entry.second->secret->GetType() == type) {
			best_match = SelectBestMatch(*entry.second, path, tie_break_offset, best_match);
		}
	}
	return best_match;
}

unique_ptr<SecretEntry> TransactionSecretStorage::GetSecretByName(const string &name,
                                                                  optional_ptr<CatalogTransaction> transaction) {
	lock_guard<mutex> guard(lock);
	auto entry = secrets.find(Identifier(name));
	if (entry == secrets.end()) {
		return nullptr;
	}
	return make_uniq<SecretEntry>(*entry->second);
}

SecretMatch SecretStorage::SelectBestMatch(SecretEntry &secret_entry, const string &path, int64_t offset,
                                           SecretMatch &current_best) {
	// Get secret match score
	auto match_score = secret_entry.secret->MatchScore(path);

	// On no match
	if (match_score == NumericLimits<int64_t>::Minimum()) {
		return current_best;
	}

	// The number of characters that match, where 0 means matching the catchall of "*"
	D_ASSERT(match_score >= 0);

	// Apply storage tie-break offset
	match_score = 100 * match_score - offset;

	// Choose the best matching score, tie-breaking on secret name when necessary
	if (match_score > current_best.score) {
		return SecretMatch(secret_entry, match_score);
	} else if (match_score == current_best.score &&
	           secret_entry.secret->GetName() < current_best.GetSecret().GetName()) {
		return SecretMatch(secret_entry, match_score);
	} else {
		return current_best;
	}
}

unique_ptr<SecretEntry> CatalogSetSecretStorage::StoreSecret(unique_ptr<const BaseSecret> secret,
                                                             OnCreateConflict on_conflict,
                                                             optional_ptr<CatalogTransaction> transaction) {
	if (secrets->GetEntry(GetTransactionOrDefault(transaction), secret->GetName())) {
		if (on_conflict == OnCreateConflict::ERROR_ON_CONFLICT) {
			string persist_string = persistent ? "Persistent" : "Temporary";
			string storage_string = persistent ? " in secret storage '" + storage_name + "'" : "";
			throw InvalidInputException("%s secret with name %s already exists%s!", persist_string, secret->GetName(),
			                            storage_string);
		} else if (on_conflict == OnCreateConflict::IGNORE_ON_CONFLICT) {
			return nullptr;
		} else if (on_conflict == OnCreateConflict::ALTER_ON_CONFLICT) {
			throw InternalException("unknown OnCreateConflict found while registering secret");
		} else if (on_conflict == OnCreateConflict::REPLACE_ON_CONFLICT) {
			secrets->DropEntry(GetTransactionOrDefault(transaction), secret->GetName(), true, true);
		}
	}

	// Call write function
	WriteSecret(*secret, on_conflict);

	auto secret_name = secret->GetName();
	auto secret_entry = make_uniq<SecretCatalogEntry>(std::move(secret), Catalog::GetSystemCatalog(db));
	secret_entry->temporary = !persistent;
	secret_entry->secret->storage_mode = storage_name;
	secret_entry->secret->persist_type = persistent ? SecretPersistType::PERSISTENT : SecretPersistType::TEMPORARY;
	LogicalDependencyList l;
	secrets->CreateEntry(GetTransactionOrDefault(transaction), secret_name, std::move(secret_entry), l);

	auto secret_catalog_entry =
	    &secrets->GetEntry(GetTransactionOrDefault(transaction), secret_name)->Cast<SecretCatalogEntry>();
	return make_uniq<SecretEntry>(*secret_catalog_entry->secret);
}

vector<SecretEntry> CatalogSetSecretStorage::AllSecrets(optional_ptr<CatalogTransaction> transaction) {
	vector<SecretEntry> ret_value;
	const std::function<void(CatalogEntry &)> callback = [&](CatalogEntry &entry) {
		auto &cast_entry = entry.Cast<SecretCatalogEntry>();
		ret_value.push_back(*cast_entry.secret);
	};
	secrets->Scan(GetTransactionOrDefault(transaction), callback);
	return ret_value;
}

void CatalogSetSecretStorage::DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
                                               optional_ptr<CatalogTransaction> transaction) {
	auto entry = secrets->GetEntry(GetTransactionOrDefault(transaction), Identifier(name));
	if (!entry) {
		if (on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
			string persist_string = persistent ? "persistent" : "temporary";
			string storage_string = persistent ? " in secret storage '" + storage_name + "'" : "";
			throw InvalidInputException("Failed to remove non-existent %s secret %s%s", persist_string, name,
			                            storage_string);
		}
		return;
	}

	secrets->DropEntry(GetTransactionOrDefault(transaction), Identifier(name), true, true);
	RemoveSecret(name.GetIdentifierName(), on_entry_not_found);
}

SecretMatch CatalogSetSecretStorage::LookupSecret(const string &path, const string &type,
                                                  optional_ptr<CatalogTransaction> transaction) {
	auto best_match = SecretMatch();

	const std::function<void(CatalogEntry &)> callback = [&](CatalogEntry &entry) {
		auto &cast_entry = entry.Cast<SecretCatalogEntry>();
		if (cast_entry.secret->secret->GetType() == type) {
			best_match = SelectBestMatch(*cast_entry.secret, path, tie_break_offset, best_match);
		}
	};
	secrets->Scan(GetTransactionOrDefault(transaction), callback);

	if (best_match.HasMatch()) {
		return best_match;
	}

	return SecretMatch();
}

unique_ptr<SecretEntry> CatalogSetSecretStorage::GetSecretByName(const string &name,
                                                                 optional_ptr<CatalogTransaction> transaction) {
	auto res = secrets->GetEntry(GetTransactionOrDefault(transaction), Identifier(name));

	if (res) {
		auto &cast_entry = res->Cast<SecretCatalogEntry>();
		return make_uniq<SecretEntry>(*cast_entry.secret);
	}

	return nullptr;
}

LocalFileSecretStorage::LocalFileSecretStorage(SecretManager &manager, DatabaseInstance &db_p, const string &name_p,
                                               const string &secret_path_p)
    : CatalogSetSecretStorage(db_p, name_p, LOCAL_FILE_STORAGE_OFFSET),
      secret_path(FileSystem::ExpandPath(secret_path_p, nullptr)) {
	persistent = true;

	// Check existence of persistent secret dir
	try {
		auto &fs = FileSystem::GetLocal(db);
		if (fs.DirectoryExists(secret_path)) {
			fs.ListFiles(secret_path, [&](const string &fname, bool is_dir) {
				string full_path = fs.JoinPath(secret_path, fname);

				if (StringUtil::EndsWith(full_path, ".duckdb_secret")) {
					string secret_name = fname.substr(0, fname.size() - 14); // size of file ext
					persistent_secrets.insert(Identifier(secret_name));
				}
			});
		}
	} catch (PermissionException &ex) {
		// If LocalFileSystem is specifically disabled (not all external access), skip loading persistent secrets
		if (!DBConfig::GetConfig(db).file_system->SubSystemIsDisabled("LocalFileSystem")) {
			throw;
		}
	}

	auto &catalog = Catalog::GetSystemCatalog(db);
	secrets = make_uniq<CatalogSet>(Catalog::GetSystemCatalog(db),
	                                make_uniq<DefaultSecretGenerator>(catalog, manager, persistent_secrets));
}

void CatalogSetSecretStorage::WriteSecret(const BaseSecret &secret, OnCreateConflict on_conflict) {
	// By default, this writes nothing
}
void CatalogSetSecretStorage::RemoveSecret(const string &name, OnEntryNotFound on_entry_not_found) {
	// By default, this writes nothing
}

CatalogTransaction CatalogSetSecretStorage::GetTransactionOrDefault(optional_ptr<CatalogTransaction> transaction) {
	if (transaction) {
		return *transaction;
	}
	return CatalogTransaction::GetSystemTransaction(db);
}

static void WriteSecretFileToDisk(FileSystem &fs, const string &path, const BaseSecret &secret) {
	auto open_flags = FileFlags::FILE_FLAGS_WRITE;
	// Ensure we are writing to a private file with 600 permission
	open_flags |= FileFlags::FILE_FLAGS_PRIVATE;
	// Ensure we overwrite anything that may have been placed there since our delete above
	open_flags |= FileFlags::FILE_FLAGS_FILE_CREATE_NEW;

	auto file_writer = BufferedFileWriter(fs, path, open_flags);

	auto serializer = BinarySerializer(file_writer);
	serializer.Begin();
	secret.Serialize(serializer);
	serializer.End();

	file_writer.Flush();
}

void LocalFileSecretStorage::WriteSecret(const BaseSecret &secret, OnCreateConflict on_conflict) {
	auto &fs = FileSystem::GetLocal(db);

	// We may need to create the secret dir here if the directory was not present during LocalFileSecretStorage
	// construction
	if (!fs.DirectoryExists(secret_path)) {
		try {
			fs.CreateDirectoriesRecursive(secret_path);
		} catch (std::exception &ex) {
			ErrorData error(ex);
			if (error.Type() == ExceptionType::IO) {
				throw IOException("Failed to initialize persistent storage directory. (original error: '%s')",
				                  error.RawMessage());
			}
			throw;
		}
	}

	string file_path = fs.JoinPath(secret_path, secret.GetName() + ".duckdb_secret");
	string temp_path = file_path + ".tmp-" + UUID::ToString(UUID::GenerateRandomUUID());

	// If persistent file already exists remove
	fs.TryRemoveFile(file_path);
	// If temporary file already exists remove
	fs.TryRemoveFile(temp_path);

	WriteSecretFileToDisk(fs, temp_path, secret);

	fs.MoveFile(temp_path, file_path);
}

void LocalFileSecretStorage::RemoveSecret(const string &secret, OnEntryNotFound on_entry_not_found) {
	auto &fs = FileSystem::GetLocal(db);
	string file = fs.JoinPath(secret_path, secret + ".duckdb_secret");
	persistent_secrets.erase(Identifier(secret));
	try {
		fs.RemoveFile(file);
	} catch (std::exception &ex) {
		ErrorData error(ex);
		if (error.Type() == ExceptionType::IO) {
			throw IOException("Failed to remove secret file '%s', the file may have been removed by another duckdb "
			                  "instance. (original error: '%s')",
			                  file, error.RawMessage());
		}
		throw;
	}
}

//===--------------------------------------------------------------------===//
// ConnectionSecretStorage
//===--------------------------------------------------------------------===//
namespace {

constexpr const char *CONNECTION_SECRET_STATE_KEY = "connection_secret_storage";

//! Per-connection secret container. Lives on the ClientContext's RegisteredStateManager, so it is destroyed exactly
//! when the connection (its ClientContext) goes away - giving automatic, crash-robust cleanup with no cooperation.
//! Rollback is implemented here rather than by storing the secrets in a CatalogSet: the container is private to a
//! single connection, and a connection runs one transaction at a time, so there is never a second snapshot to serve.
//! All that is needed is undo - the pre-image of every secret the active transaction touched, replayed if it aborts.
struct ConnectionSecretState : public ClientContextState {
	mutex lock;
	//! The secrets of this connection: the last committed state, plus the changes of the active transaction
	identifier_map_t<unique_ptr<SecretEntry>> secrets;

	//! Records the pre-image of `name` before the active transaction modifies it, so that a rollback can restore it.
	//! Only the first change per name is recorded, making the undo replay order-independent.
	void StageChange(MetaTransaction &transaction, const Identifier &name) {
		if (undo_transaction_id != transaction.global_transaction_id) {
			// First change of a new transaction: any leftover pre-images belong to a transaction that already ended
			undo_log.clear();
			undo_transaction_id = transaction.global_transaction_id;
		}
		if (undo_log.find(name) != undo_log.end()) {
			return;
		}
		auto entry = secrets.find(name);
		auto pre_image = entry == secrets.end() ? nullptr : make_uniq<SecretEntry>(*entry->second);
		undo_log.emplace(name, std::move(pre_image));
	}

	void TransactionCommit(MetaTransaction &transaction, ClientContext &context) override {
		lock_guard<mutex> guard(lock);
		undo_log.clear();
	}

	void TransactionRollback(MetaTransaction &transaction, ClientContext &context) override {
		lock_guard<mutex> guard(lock);
		for (auto &entry : undo_log) {
			if (entry.second) {
				secrets[entry.first] = std::move(entry.second);
			} else {
				// The secret did not exist before the transaction
				secrets.erase(entry.first);
			}
		}
		undo_log.clear();
	}

private:
	//! Pre-images of the secrets modified by the active transaction, a null entry meaning "did not exist"
	identifier_map_t<unique_ptr<SecretEntry>> undo_log;
	//! The transaction the pre-images belong to
	transaction_t undo_transaction_id = 0;
};

//! Fetch the calling connection's secret container. With create=false returns nullptr when there is no context or no
//! container yet; with create=true it allocates the container on the context (used by StoreSecret).
optional_ptr<ConnectionSecretState> GetConnectionState(optional_ptr<CatalogTransaction> transaction, bool create) {
	if (!transaction || !transaction->HasContext()) {
		return nullptr;
	}
	auto &context = transaction->GetContext();
	if (create) {
		return context.registered_state->GetOrCreate<ConnectionSecretState>(CONNECTION_SECRET_STATE_KEY).get();
	}
	return context.registered_state->Get<ConnectionSecretState>(CONNECTION_SECRET_STATE_KEY).get();
}

} // namespace

unique_ptr<SecretEntry> ConnectionSecretStorage::StoreSecret(unique_ptr<const BaseSecret> secret,
                                                             OnCreateConflict on_conflict,
                                                             optional_ptr<CatalogTransaction> transaction) {
	auto state = GetConnectionState(transaction, true);
	if (!state) {
		throw InvalidInputException("Cannot create a connection-scoped secret without an active client context");
	}
	lock_guard<mutex> guard(state->lock);
	// Copy the name: we std::move(secret) below, after which a reference into it would dangle and the entry would be
	// inserted under a garbage key (breaking later name-keyed lookups like DROP / GetSecretByName).
	auto name = secret->GetName();

	auto existing = state->secrets.find(name);
	if (existing != state->secrets.end()) {
		switch (on_conflict) {
		case OnCreateConflict::ERROR_ON_CONFLICT:
			throw InvalidInputException("Connection secret with name '%s' already exists", name.GetIdentifierName());
		case OnCreateConflict::IGNORE_ON_CONFLICT:
			return nullptr;
		default: // REPLACE_ON_CONFLICT
			break;
		}
	}

	auto entry = make_uniq<SecretEntry>(std::move(secret));
	entry->persist_type = SecretPersistType::TEMPORARY;
	entry->storage_mode = storage_name;
	auto result = make_uniq<SecretEntry>(*entry);
	state->StageChange(MetaTransaction::Get(transaction->GetContext()), name);
	state->secrets[name] = std::move(entry);
	return result;
}

SecretMatch ConnectionSecretStorage::LookupSecret(const string &path, const string &type,
                                                  optional_ptr<CatalogTransaction> transaction) {
	auto state = GetConnectionState(transaction, false);
	if (!state) {
		// No connection context (or nothing stored yet) - decline, so the global storages serve this lookup.
		return SecretMatch();
	}
	lock_guard<mutex> guard(state->lock);
	auto best_match = SecretMatch();
	for (auto &entry : state->secrets) {
		if (entry.second->secret->GetType() == type) {
			best_match = SelectBestMatch(*entry.second, path, tie_break_offset, best_match);
		}
	}
	return best_match;
}

unique_ptr<SecretEntry> ConnectionSecretStorage::GetSecretByName(const string &name,
                                                                 optional_ptr<CatalogTransaction> transaction) {
	auto state = GetConnectionState(transaction, false);
	if (!state) {
		return nullptr;
	}
	lock_guard<mutex> guard(state->lock);
	auto entry = state->secrets.find(Identifier(name));
	if (entry == state->secrets.end()) {
		return nullptr;
	}
	return make_uniq<SecretEntry>(*entry->second);
}

void ConnectionSecretStorage::DropSecretByName(const Identifier &name, OnEntryNotFound on_entry_not_found,
                                               optional_ptr<CatalogTransaction> transaction) {
	auto state = GetConnectionState(transaction, false);
	idx_t erased = 0;
	if (state) {
		lock_guard<mutex> guard(state->lock);
		if (state->secrets.find(name) != state->secrets.end()) {
			state->StageChange(MetaTransaction::Get(transaction->GetContext()), name);
			erased = state->secrets.erase(name);
		}
	}
	if (erased == 0 && on_entry_not_found == OnEntryNotFound::THROW_EXCEPTION) {
		throw InvalidInputException("Connection secret with name '%s' not found", name.GetIdentifierName());
	}
}

vector<SecretEntry> ConnectionSecretStorage::AllSecrets(optional_ptr<CatalogTransaction> transaction) {
	vector<SecretEntry> result;
	auto state = GetConnectionState(transaction, false);
	if (!state) {
		return result;
	}
	lock_guard<mutex> guard(state->lock);
	for (auto &entry : state->secrets) {
		result.push_back(*entry.second);
	}
	return result;
}

} // namespace duckdb
