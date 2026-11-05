#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/transaction/transaction_data.hpp"

#include "duckdb/catalog/catalog_entry/duck_table_entry.hpp"
#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/storage/data_table.hpp"
#include "duckdb/transaction/local_storage.hpp"
#include "duckdb/catalog/catalog_entry/type_catalog_entry.hpp"
#include "duckdb/catalog/dependency_manager.hpp"
#include "duckdb/catalog/duck_catalog.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "duckdb/common/serializer/binary_serializer.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_entry.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/parsed_data/alter_table_info.hpp"
#include "duckdb/transaction/duck_transaction.hpp"
#include "duckdb/transaction/duck_transaction_manager.hpp"
#include "duckdb/transaction/transaction_manager.hpp"
#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/common/exception/transaction_exception.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"

namespace duckdb {

void CatalogEntryMap::AddEntry(unique_ptr<CatalogEntry> entry) {
	auto name = entry->name;

	if (entries.find(name) != entries.end()) {
		throw InternalException("Entry with name %s already exists", name);
	}
	entries.insert(make_pair(name, std::move(entry)));
}

void CatalogEntryMap::UpdateEntry(unique_ptr<CatalogEntry> catalog_entry) {
	auto name = catalog_entry->name;

	auto entry = entries.find(name);
	if (entry == entries.end()) {
		throw InternalException("Entry with name %s does not exist", name);
	}

	auto existing = std::move(entry->second);
	entry->second = std::move(catalog_entry);
	entry->second->SetChild(std::move(existing));
}

identifier_tree_t<unique_ptr<CatalogEntry>> &CatalogEntryMap::Entries() {
	return entries;
}

void CatalogEntryMap::DropEntry(CatalogEntry &entry) {
	auto &name = entry.name;
	auto chain = GetEntry(name);
	if (!chain) {
		throw InternalException("Attempting to drop entry with name %s but no chain with that name exists", name);
	}
	auto child = entry.TakeChild();
	if (!entry.HasParent()) {
		// This is the top of the chain
		D_ASSERT(chain.get() == &entry);
		auto it = entries.find(name);
		D_ASSERT(it != entries.end());

		// Remove the entry
		it->second.reset();
		if (child) {
			// Replace it with its child
			it->second = std::move(child);
		} else {
			entries.erase(it);
		}
	} else {
		// Just replace the entry with its child
		auto &parent = entry.Parent();
		parent.SetChild(std::move(child));
	}
}

optional_ptr<CatalogEntry> CatalogEntryMap::GetEntry(const Identifier &name) {
	auto entry = entries.find(name);
	if (entry == entries.end()) {
		return nullptr;
	}
	return entry->second.get();
}

CatalogSet::CatalogSet(Catalog &catalog_p, unique_ptr<DefaultGenerator> defaults)
    : catalog(catalog_p.Cast<DuckCatalog>()), defaults(std::move(defaults)) {
	D_ASSERT(catalog_p.IsDuckCatalog());
}
CatalogSet::~CatalogSet() {
}

bool CatalogSet::StartChain(CatalogTransaction transaction, const Identifier &name, unique_lock<mutex> &read_lock) {
	D_ASSERT(!map.GetEntry(name));

	// check if there is a default entry
	auto entry = CreateDefaultEntry(transaction, name, read_lock);
	if (entry) {
		return false;
	}

	// first create a dummy deleted entry
	// so other transactions will see that instead of the entry that is to be added.
	auto dummy_node = make_uniq<InCatalogEntry>(CatalogType::INVALID, catalog, name);
	dummy_node->timestamp = 0;
	dummy_node->deleted = true;
	dummy_node->set = this;

	map.AddEntry(std::move(dummy_node));
	return true;
}

bool CatalogSet::VerifyVacancy(CatalogTransaction transaction, CatalogEntry &entry) {
	if (HasConflict(transaction, entry.timestamp)) {
		// A transaction that is not visible to our snapshot has already made a change to this entry.
		// Because of Catalog limitations we can't push our change on this, even if the change was made by another
		// active transaction that might end up being aborted. So we have to cancel this transaction.
		throw TransactionException("Catalog write-write conflict on create with %s", entry.name);
	}
	// The entry is visible to our snapshot
	if (!entry.deleted) {
		return false;
	}
	return true;
}

static bool IsDependencyEntry(CatalogEntry &entry) {
	return entry.type == CatalogType::DEPENDENCY_ENTRY;
}

void CatalogSet::CheckCatalogEntryInvariants(CatalogEntry &value, const Identifier &name) {
	if (value.internal && !catalog.IsSystemCatalog() && name != DEFAULT_SCHEMA) {
		throw InternalException("Attempting to create internal entry %s in non-system catalog - internal entries "
		                        "can only be created in the system catalog",
		                        name);
	}
	if (!value.internal) {
		if (!value.temporary && catalog.IsSystemCatalog() && !IsDependencyEntry(value)) {
			throw InternalException("Attempting to create non-internal entry %s in system catalog - the system catalog "
			                        "can only contain internal entries",
			                        name);
		}
		if (value.temporary && !catalog.IsTemporaryCatalog()) {
			throw InternalException("Attempting to create temporary entry %s in non-temporary catalog", name);
		}
		if (!value.temporary && catalog.IsTemporaryCatalog() && name != DEFAULT_SCHEMA) {
			throw InvalidInputException("Cannot create non-temporary entry %s in temporary catalog", name);
		}
	}
}

optional_ptr<CatalogEntry> CatalogSet::CreateCommittedEntry(unique_ptr<CatalogEntry> entry) {
	auto existing_entry = map.GetEntry(entry->name);
	if (existing_entry) {
		// Return null if an entry by that name already exists
		return nullptr;
	}

	auto catalog_entry = entry.get();

	entry->set = this;
	// Give the entry commit id 0, so it is visible to all transactions
	entry->timestamp = 0;
	map.AddEntry(std::move(entry));

	return catalog_entry;
}

bool CatalogSet::CreateEntryInternal(CatalogTransaction transaction, const Identifier &name,
                                     unique_ptr<CatalogEntry> value, unique_lock<mutex> &read_lock,
                                     bool should_be_empty) {
	auto entry_value = map.GetEntry(name);
	if (!entry_value) {
		// Add a dummy node to start the chain
		if (!StartChain(transaction, name, read_lock)) {
			return false;
		}
	} else if (should_be_empty) {
		// Verify that the entry is deleted, not altered by another transaction
		if (!VerifyVacancy(transaction, *entry_value)) {
			return false;
		}
	}

	// Finally add the new entry to the chain
	auto value_ptr = value.get();
	map.UpdateEntry(std::move(value));
	// Push the old entry in the undo buffer for this transaction, so it can be restored in the event of failure
	if (transaction.transaction) {
		DuckTransactionManager::Get(GetCatalog().GetAttached())
		    .PushCatalogEntry(*transaction.transaction, value_ptr->Child());
	}
	return true;
}

bool CatalogSet::CreateEntry(CatalogTransaction transaction, const Identifier &name, unique_ptr<CatalogEntry> value,
                             const LogicalDependencyList &dependencies) {
	CheckCatalogEntryInvariants(*value, name);

	// Mark this entry as being created by the current active transaction
	value->timestamp = transaction.GetTransactionId();
	value->set = this;
	catalog.GetDependencyManager()->AddObject(transaction, *value, dependencies);

	// lock the catalog for writing
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	// lock this catalog set to disallow reading
	unique_lock<mutex> read_lock(catalog_lock);

	return CreateEntryInternal(transaction, name, std::move(value), read_lock);
}

bool CatalogSet::CreateEntry(ClientContext &context, const Identifier &name, unique_ptr<CatalogEntry> value,
                             const LogicalDependencyList &dependencies) {
	return CreateEntry(catalog.GetCatalogTransaction(context), name, std::move(value), dependencies);
}

//! This method is used to retrieve an entry for the purpose of making a new version, through an alter/drop/create
optional_ptr<CatalogEntry> CatalogSet::GetEntryInternal(CatalogTransaction transaction, const Identifier &name) {
	auto entry_value = map.GetEntry(name);
	if (!entry_value) {
		return nullptr;
	}
	auto &catalog_entry = *entry_value;

	// Check if this entry is visible to our snapshot
	if (HasConflict(transaction, catalog_entry.timestamp)) {
		// We intend to create a new version of the entry.
		// Another transaction has already made an edit to this catalog entry, because of limitations in the Catalog we
		// can't create an edit alongside this even if the other transaction might end up getting aborted. So we have to
		// abort the transaction.
		throw TransactionException("Catalog write-write conflict on alter with %s", catalog_entry.name);
	}
	// The entry is visible to our snapshot, check if it's deleted
	if (catalog_entry.deleted) {
		return nullptr;
	}
	return &catalog_entry;
}

bool CatalogSet::AlterOwnership(CatalogTransaction transaction, ChangeOwnershipInfo &info) {
	// lock the catalog for writing
	unique_lock<mutex> write_lock(catalog.GetWriteLock());

	auto entry = GetEntryInternal(transaction, info.GetQualifiedName().Name());
	if (!entry) {
		return false;
	}
	optional_ptr<CatalogEntry> owner_entry;
	auto schema = catalog.GetSchema(transaction, info.owner_schema, OnEntryNotFound::RETURN_NULL);
	if (schema) {
		vector<CatalogType> entry_types {CatalogType::TABLE_ENTRY, CatalogType::SEQUENCE_ENTRY};
		for (auto entry_type : entry_types) {
			owner_entry = schema->GetEntry(transaction, entry_type, info.owner_name);
			if (owner_entry) {
				break;
			}
		}
	}
	if (!owner_entry) {
		throw CatalogException("CatalogElement \"%s.%s\" does not exist!", info.owner_schema.GetIdentifierName(),
		                       info.owner_name.GetIdentifierName());
	}
	write_lock.unlock();
	catalog.GetDependencyManager()->AddOwnership(transaction, *owner_entry, *entry);
	return true;
}

bool CatalogSet::RenameEntryInternal(CatalogTransaction transaction, CatalogEntry &old, const Identifier &new_name,
                                     AlterInfo &alter_info, unique_lock<mutex> &read_lock) {
	auto &original_name = old.name;

	auto &context = *transaction.context;
	auto entry_value = map.GetEntry(new_name);
	if (entry_value) {
		auto &existing_entry = GetEntryForTransaction(transaction, *entry_value);
		if (!existing_entry.deleted) {
			// There exists an entry by this name that is not deleted
			old.UndoAlter(context, alter_info);
			throw CatalogException("Could not rename %s to %s: another entry with this name already exists!",
			                       original_name, new_name);
		}
	}

	// Add a RENAMED_ENTRY before adding a DELETED_ENTRY, this makes it so that when this is committed
	// we know that this was not a DROP statement.
	auto renamed_tombstone = make_uniq<InCatalogEntry>(CatalogType::RENAMED_ENTRY, old.ParentCatalog(), original_name);
	renamed_tombstone->timestamp = transaction.GetTransactionId();
	renamed_tombstone->deleted = false;
	renamed_tombstone->set = this;
	if (!CreateEntryInternal(transaction, original_name, std::move(renamed_tombstone), read_lock,
	                         /*should_be_empty = */ false)) {
		return false;
	}
	if (!DropEntryInternal(transaction, original_name, false)) {
		return false;
	}

	// Add the renamed entry
	// Start this off with a RENAMED_ENTRY node, for commit/cleanup/rollback purposes
	auto renamed_node = make_uniq<InCatalogEntry>(CatalogType::RENAMED_ENTRY, catalog, new_name);
	renamed_node->timestamp = transaction.GetTransactionId();
	renamed_node->deleted = false;
	renamed_node->set = this;
	return CreateEntryInternal(transaction, new_name, std::move(renamed_node), read_lock);
}

bool CatalogSet::AlterEntry(CatalogTransaction transaction, const Identifier &name, AlterInfo &alter_info) {
	// If the entry does not exist, we error
	auto entry = GetEntry(transaction, name);
	if (!entry) {
		return false;
	}
	if (!alter_info.allow_internal && entry->internal) {
		throw CatalogException("Cannot alter entry %s because it is an internal system entry", entry->name);
	}

	unique_ptr<CatalogEntry> value;
	if (alter_info.type == AlterType::SET_COMMENT) {
		// Copy the existing entry; we are only changing metadata here
		if (!transaction.context) {
			throw InternalException("Cannot AlterEntry::SET_COMMENT without client context");
		}
		value = entry->Copy(*transaction.context);
		value->comment = alter_info.Cast<SetCommentInfo>().comment_value;
	} else {
		// Use the existing entry to create the altered entry
		value = entry->AlterEntry(transaction, alter_info);
		if (!value) {
			// alter failed, but did not result in an error
			return true;
		}
	}

	// If this ALTER produced a new DuckTableEntry, refresh the LocalTableStorage's table_entry
	// pointer so that commit-time Flush pushes an AppendInfo referencing the current DuckTableEntry.
	if (transaction.context && value->type == CatalogType::TABLE_ENTRY) {
		auto new_entry = value->Cast<TableCatalogEntry>().TryGetDuckTableEntry();
		if (new_entry) {
			auto &new_storage = new_entry->GetStorage();
			auto lstorage = LocalStorage::Get(*transaction.context, new_storage.db).GetStorage(new_storage);
			if (lstorage) {
				lstorage->table_entry = new_entry;
			}
		}
	}

	// lock the catalog for writing
	unique_lock<mutex> write_lock(catalog.GetWriteLock());
	// lock this catalog set to disallow reading
	unique_lock<mutex> read_lock(catalog_lock);

	// fetch the entry again before doing the modification
	// this will catch any write-write conflicts between transactions
	entry = GetEntryInternal(transaction, name);

	// Mark this entry as being created by this transaction
	value->timestamp = transaction.GetTransactionId();
	value->set = this;
	// Preserve the oid across the alter: an altered entry is the same logical object as before
	value->oid = entry->oid;

	if (value->name != entry->name) {
		if (!RenameEntryInternal(transaction, *entry, value->name, alter_info, read_lock)) {
			return false;
		}
	}
	auto new_entry = value.get();
	map.UpdateEntry(std::move(value));

	// push the old entry in the undo buffer for this transaction
	unique_ptr<CatalogEntry> entry_to_destroy;
	if (transaction.transaction) {
		// serialize the AlterInfo into a temporary buffer
		MemoryStream stream(Allocator::Get(*transaction.db));
		BinarySerializer serializer(stream);
		serializer.Begin();
		serializer.WriteProperty(100, "column_name", alter_info.GetColumnName());
		serializer.WriteProperty(101, "alter_info", &alter_info);
		serializer.End();

		DuckTransactionManager::Get(GetCatalog().GetAttached())
		    .PushCatalogEntry(*transaction.transaction, new_entry->Child(), stream.GetData(), stream.GetPosition());
	} else {
		// if we don't have a transaction this alter is non-transactional
		// in that case we are able to just directly destroy the child (if there is any)
		entry_to_destroy = new_entry->TakeChild();
	}

	// Update shared entry state only after the alter is installed and rollbackable.
	if (new_entry->name != entry->name) {
		new_entry->SetAsRoot();
	}

	read_lock.unlock();
	write_lock.unlock();

	// Check the dependency manager to verify that there are no conflicting dependencies with this alter
	catalog.GetDependencyManager()->AlterObject(transaction, *entry, *new_entry, alter_info);
	return true;
}

bool CatalogSet::DropDependencies(CatalogTransaction transaction, const Identifier &name, bool cascade,
                                  bool allow_drop_internal) {
	auto entry = GetEntry(transaction, name);
	if (!entry) {
		return false;
	}
	if (entry->internal && !allow_drop_internal) {
		throw CatalogException("Cannot drop entry %s because it is an internal system entry", entry->name);
	}
	// check any dependencies of this object
	D_ASSERT(entry->ParentCatalog().IsDuckCatalog());
	auto &duck_catalog = entry->ParentCatalog().Cast<DuckCatalog>();
	duck_catalog.GetDependencyManager()->DropObject(transaction, *entry, cascade);
	return true;
}

bool CatalogSet::DropEntryInternal(CatalogTransaction transaction, const Identifier &name, bool allow_drop_internal) {
	// lock the catalog for writing
	// we can only delete an entry that exists
	auto entry = GetEntryInternal(transaction, name);
	if (!entry) {
		return false;
	}
	if (entry->internal && !allow_drop_internal) {
		throw CatalogException("Cannot drop entry %s because it is an internal system entry", entry->name);
	}

	// create a new tombstone entry and replace the currently stored one
	// set the timestamp to the timestamp of the current transaction
	// and point it at the tombstone node
	auto value = make_uniq<InCatalogEntry>(CatalogType::DELETED_ENTRY, entry->ParentCatalog(), entry->name);
	value->timestamp = transaction.GetTransactionId();
	value->set = this;
	value->deleted = true;
	auto value_ptr = value.get();
	map.UpdateEntry(std::move(value));

	// push the old entry in the undo buffer for this transaction
	if (transaction.transaction) {
		DuckTransactionManager::Get(GetCatalog().GetAttached())
		    .PushCatalogEntry(*transaction.transaction, value_ptr->Child());
	}
	return true;
}

bool CatalogSet::DropEntry(CatalogTransaction transaction, const Identifier &name, bool cascade,
                           bool allow_drop_internal) {
	if (!DropDependencies(transaction, name, cascade, allow_drop_internal)) {
		return false;
	}
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> read_lock(catalog_lock);
	return DropEntryInternal(transaction, name, allow_drop_internal);
}

bool CatalogSet::DropEntry(ClientContext &context, const Identifier &name, bool cascade, bool allow_drop_internal) {
	return DropEntry(catalog.GetCatalogTransaction(context), name, cascade, allow_drop_internal);
}

//! Verify that the object referenced by the dependency still exists when we commit the dependency
void CatalogSet::VerifyExistenceOfDependency(transaction_t commit_id, CatalogEntry &entry) {
	auto &duck_catalog = GetCatalog();

	// Make sure that we don't see any uncommitted changes
	auto transaction_id = MAX_TRANSACTION_ID;
	D_ASSERT(IsCommitted(commit_id));
	// This will allow us to see all committed changes made before this COMMIT happened
	CatalogTransaction commit_transaction(duck_catalog.GetDatabase(), transaction_id,
	                                      VisibilityBound::Through(commit_id));

	D_ASSERT(entry.type == CatalogType::DEPENDENCY_ENTRY);
	auto &dep = entry.Cast<DependencyEntry>();
	duck_catalog.GetDependencyManager()->VerifyExistence(commit_transaction, dep);
}

//! Verify that no dependencies creations were committed since our transaction started, that reference the entry we're
//! dropping
void CatalogSet::CommitDrop(transaction_t commit_id, VisibilityBound visibility_bound, CatalogEntry &entry) {
	auto &duck_catalog = GetCatalog();

	entry.OnDrop();
	// Make sure that we don't see any uncommitted changes
	auto transaction_id = MAX_TRANSACTION_ID;
	D_ASSERT(IsCommitted(commit_id));
	// This will allow us to see all committed changes made before this COMMIT happened
	CatalogTransaction commit_transaction(duck_catalog.GetDatabase(), transaction_id,
	                                      VisibilityBound::Before(commit_id));

	duck_catalog.GetDependencyManager()->VerifyCommitDrop(commit_transaction, visibility_bound, entry);
}

DuckCatalog &CatalogSet::GetCatalog() {
	return catalog;
}

void CatalogSet::CleanupEntry(CatalogEntry &catalog_entry) {
	// destroy the backed up entry: it is no longer required
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> lock(catalog_lock);
	auto &parent = catalog_entry.Parent();
	map.DropEntry(catalog_entry);
	if (parent.deleted && !parent.HasChild() && !parent.HasParent()) {
		// The entry's parent is a tombstone and the entry had no child
		// clean up the mapping and the tombstone entry as well
		D_ASSERT(map.GetEntry(parent.name).get() == &parent);
		map.DropEntry(parent);
	}
}

bool CatalogSet::CreatedByOtherActiveTransaction(CatalogTransaction transaction, transaction_t timestamp) {
	// True if this transaction is not committed yet and the entry was made by another active (not committed)
	// transaction
	return !IsCommitted(timestamp) && timestamp != transaction.GetTransactionId();
}

bool CatalogSet::CommittedAfterStarting(CatalogTransaction transaction, transaction_t timestamp) {
	// the entry is committed but not visible to this transaction, so it is not our source of truth
	return IsCommitted(timestamp) && !transaction.view.Sees(timestamp);
}

bool CatalogSet::HasConflict(CatalogTransaction transaction, transaction_t timestamp) {
	// a version conflicts exactly when it is not visible to the transaction: it is either another
	// active transaction's uncommitted version, or committed outside this transaction's snapshot
	return !transaction.view.Sees(timestamp);
}

bool CatalogSet::UseTimestamp(CatalogTransaction transaction, transaction_t timestamp) {
	return transaction.view.Sees(timestamp);
}

CatalogEntry &CatalogSet::GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current) {
	bool visible;
	return GetEntryForTransaction(transaction, current, visible);
}

CatalogEntry &CatalogSet::GetEntryForTransaction(CatalogTransaction transaction, CatalogEntry &current, bool &visible) {
	reference<CatalogEntry> entry(current);
	while (entry.get().HasChild()) {
		if (UseTimestamp(transaction, entry.get().timestamp)) {
			visible = true;
			return entry.get();
		}
		entry = entry.get().Child();
	}
	visible = false;
	return entry.get();
}

SimilarCatalogEntry CatalogSet::SimilarEntry(CatalogTransaction transaction, const Identifier &name) {
	unique_lock<mutex> lock(catalog_lock);

	// Score the names of default entries that have not been created yet without creating them: creating a default
	// view parses and binds its SQL, which is far more work than an error suggestion needs.
	// The candidates are visited in map order, so ties resolve as they would if the defaults had been created.
	set<Identifier, IdentifierCompare> candidates;
	for (auto &kv : map.Entries()) {
		candidates.insert(kv.first);
	}
	if (defaults && !defaults->created_all_entries) {
		for (auto &default_entry : defaults->GetDefaultEntries()) {
			if (!map.GetEntry(default_entry)) {
				candidates.insert(default_entry);
			}
		}
	}

	SimilarCatalogEntry result;
	for (auto &candidate : candidates) {
		auto entry_score = StringUtil::SimilarityRating(candidate.GetIdentifierName(), name.GetIdentifierName());
		if (entry_score > result.score) {
			result.score = entry_score;
			result.name = Identifier(candidate.GetIdentifierName());
		}
	}
	return result;
}

optional_ptr<CatalogEntry> CatalogSet::CreateDefaultEntry(CatalogTransaction transaction, const Identifier &name,
                                                          unique_lock<mutex> &read_lock) {
	// no entry found with this name, check for defaults
	if (!defaults || defaults->created_all_entries) {
		// no defaults either: return null
		return nullptr;
	}
	auto unlock = !defaults->LockDuringCreate();
	if (unlock) {
		read_lock.unlock();
	}
	// this catalog set has a default map defined
	// check if there is a default entry that we can create with this name
	auto entry = defaults->CreateDefaultEntry(transaction, name);

	if (unlock) {
		read_lock.lock();
	}
	if (!entry) {
		// no default entry
		return nullptr;
	}
	// there is a default entry! create it
	auto result = CreateCommittedEntry(std::move(entry));
	if (result) {
		return result;
	}
	// we found a default entry, but failed
	// this means somebody else created the entry first
	// just retry?
	if (unlock) {
		read_lock.unlock();
	}
	return GetEntry(transaction, name);
}

CatalogSet::EntryLookup CatalogSet::GetEntryDetailed(CatalogTransaction transaction, const Identifier &name) {
	unique_lock<mutex> read_lock(catalog_lock);
	auto entry_value = map.GetEntry(name);
	if (entry_value) {
		// we found an entry for this name
		// check the version numbers

		auto &catalog_entry = *entry_value;
		bool visible;
		auto &current = GetEntryForTransaction(transaction, catalog_entry, visible);
		if (current.deleted) {
			if (!visible) {
				return EntryLookup {nullptr, EntryLookup::FailureReason::INVISIBLE};
			} else {
				auto deleted_entry = current.HasChild() ? &current.Child() : nullptr;
				return EntryLookup {nullptr, EntryLookup::FailureReason::DELETED, deleted_entry};
			}
		}
		D_ASSERT(current.name == name);
		return EntryLookup {&current, EntryLookup::FailureReason::SUCCESS};
	}
	auto default_entry = CreateDefaultEntry(transaction, name, read_lock);
	if (!default_entry) {
		return EntryLookup {default_entry, EntryLookup::FailureReason::NOT_PRESENT};
	}
	return EntryLookup {default_entry, EntryLookup::FailureReason::SUCCESS};
}

optional_ptr<CatalogEntry> CatalogSet::GetEntry(CatalogTransaction transaction, const Identifier &name) {
	auto lookup = GetEntryDetailed(transaction, name);
	return lookup.result;
}

optional_ptr<CatalogEntry> CatalogSet::GetEntry(ClientContext &context, const Identifier &name) {
	return GetEntry(catalog.GetCatalogTransaction(context), name);
}

void CatalogSet::UpdateTimestamp(CatalogEntry &entry, transaction_t timestamp) {
	entry.timestamp = timestamp;
}

void CatalogSet::Undo(CatalogEntry &entry) {
	lock_guard<mutex> write_lock(catalog.GetWriteLock());
	lock_guard<mutex> lock(catalog_lock);

	// entry has to be restored
	// and entry->parent has to be removed ("rolled back")

	// i.e. we have to place (entry) as (entry->parent) again
	auto &to_be_removed_node = entry.Parent();
	to_be_removed_node.Rollback(entry);

	D_ASSERT(entry.name == to_be_removed_node.name);
	if (!to_be_removed_node.HasParent()) {
		to_be_removed_node.Child().SetAsRoot();
	}
	map.DropEntry(to_be_removed_node);

	if (entry.type == CatalogType::INVALID) {
		// This was the root of the entry chain
		map.DropEntry(entry);
	}
}

void CatalogSet::CreateDefaultEntries(CatalogTransaction transaction, unique_lock<mutex> &read_lock) {
	if (!defaults || defaults->created_all_entries) {
		return;
	}
	auto unlock = !defaults->LockDuringCreate();
	// this catalog set has a default set defined:
	auto default_entries = defaults->GetDefaultEntries();
	for (auto &default_entry : default_entries) {
		auto entry_value = map.GetEntry(default_entry);
		if (!entry_value) {
			// we unlock during the CreateEntry, since it might reference other catalog sets...
			// specifically for views this can happen since the view will be bound
			if (unlock) {
				read_lock.unlock();
			}
			auto entry = defaults->CreateDefaultEntry(transaction, default_entry);
			if (!entry) {
				throw InternalException("Failed to create default entry for %s", default_entry);
			}

			if (unlock) {
				read_lock.lock();
			}
			CreateCommittedEntry(std::move(entry));
		}
	}
	defaults->created_all_entries = true;
}

void CatalogSet::Scan(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback) {
	// Lock the catalog set.
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	for (auto &kv : map.Entries()) {
		auto &entry = *kv.second;
		auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
		if (!entry_for_transaction.deleted) {
			callback(entry_for_transaction);
		}
	}
}

void CatalogSet::ScanWithReturn(CatalogTransaction transaction, const std::function<bool(CatalogEntry &)> &callback) {
	// Lock the catalog set.
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	for (auto &kv : map.Entries()) {
		auto &entry = *kv.second;
		auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
		if (!entry_for_transaction.deleted) {
			if (!callback(entry_for_transaction)) {
				return;
			}
		}
	}
}

optional_ptr<CatalogEntry> CatalogSet::GetHeadEntry(const Identifier &name) {
	lock_guard<mutex> lock(catalog_lock);
	return map.GetEntry(name);
}

void CatalogSet::ScanWithConflictDetection(CatalogTransaction transaction,
                                           const std::function<void(CatalogEntry &)> &scan_callback,
                                           const std::function<void(CatalogEntry &)> &conflict_callback) {
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	for (auto &kv : map.Entries()) {
		auto &entry = *kv.second;
		if (HasConflict(transaction, entry.timestamp) && !entry.deleted) {
			conflict_callback(entry);
		}
		auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
		if (!entry_for_transaction.deleted) {
			scan_callback(entry_for_transaction);
		}
	}
}

void CatalogSet::Scan(ClientContext &context, const std::function<void(CatalogEntry &)> &callback) {
	Scan(catalog.GetCatalogTransaction(context), callback);
}

void CatalogSet::ScanWithReturn(ClientContext &context, const std::function<bool(CatalogEntry &)> &callback) {
	ScanWithReturn(catalog.GetCatalogTransaction(context), callback);
}

void CatalogSet::ScanWithPrefix(CatalogTransaction transaction, const std::function<void(CatalogEntry &)> &callback,
                                const Identifier &prefix) {
	// lock the catalog set
	unique_lock<mutex> lock(catalog_lock);
	CreateDefaultEntries(transaction, lock);

	auto &entries = map.Entries();
	auto it = entries.lower_bound(prefix);
	auto end = entries.upper_bound(Identifier(prefix.GetIdentifierName() + char(255)));
	for (; it != end; it++) {
		auto &entry = *it->second;
		auto &entry_for_transaction = GetEntryForTransaction(transaction, entry);
		if (!entry_for_transaction.deleted) {
			callback(entry_for_transaction);
		}
	}
}

void CatalogSet::Scan(VisibilityBound bound, const std::function<void(CatalogEntry &)> &callback) {
	// lock the catalog set
	lock_guard<mutex> lock(catalog_lock);
	CatalogTransaction transaction(catalog.GetDatabase(), MAX_TRANSACTION_ID, bound);
	for (auto &kv : map.Entries()) {
		auto &entry = GetEntryForTransaction(transaction, *kv.second);
		if (!entry.deleted) {
			callback(entry);
		}
	}
}

void CatalogSet::SetDefaultGenerator(unique_ptr<DefaultGenerator> defaults_p) {
	lock_guard<mutex> lock(catalog_lock);
	defaults = std::move(defaults_p);
}

void CatalogSet::Verify(Catalog &catalog_p) {
	D_ASSERT(&catalog_p == &catalog);
	vector<reference<CatalogEntry>> entries;
	Scan(VisibilityBound::AllCommitted(), [&](CatalogEntry &entry) { entries.push_back(entry); });
	for (auto &entry : entries) {
		entry.get().Verify(catalog_p);
	}
}

} // namespace duckdb
