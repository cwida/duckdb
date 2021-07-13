//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/catalog_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/default/default_generator.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/mutex.hpp"

#include <functional>
#include <memory>

namespace duckdb {
struct AlterInfo;

class ClientContext;

typedef unordered_map<CatalogSet *, unique_lock<mutex>> set_lock_map_t;

struct MappingValue {
	explicit MappingValue(idx_t index_) : index(index_), timestamp(0), deleted(false), parent(nullptr) {
	}

	idx_t index;
	transaction_t timestamp;
	bool deleted;
	unique_ptr<MappingValue> child;
	MappingValue *parent;
};

//! The Catalog Set stores (key, value) map of a set of CatalogEntries
class CatalogSet {
	friend class DependencyManager;

public:
	explicit CatalogSet(Catalog &catalog, unique_ptr<DefaultGenerator> defaults = nullptr);

	//! Create an entry in the catalog set. Returns whether or not it was
	//! successful.
	bool CreateEntry(ClientContext &context, const string &name, unique_ptr<CatalogEntry> value,
	                 unordered_set<CatalogEntry *> &dependencies);

	bool AlterEntry(ClientContext &context, const string &name, AlterInfo *alter_info);

	bool DropEntry(ClientContext &context, const string &name, bool cascade);

	void CleanupEntry(CatalogEntry *catalog_entry);

	//! Returns the entry with the specified name
	CatalogEntry *GetEntry(ClientContext &context, const string &name);

	//! Gets the entry that is most similar to the given name (i.e. smallest levenshtein distance), or empty string if
	//! none is found
	string SimilarEntry(ClientContext &context, const string &name);

	//! Rollback <entry> to be the currently valid entry for a certain catalog
	//! entry
	void Undo(CatalogEntry *entry);

	//! Scan the catalog set, invoking the callback method for every committed entry
	void Scan(const std::function<void(CatalogEntry *)> &callback);
	//! Scan the catalog set, invoking the callback method for every entry
	void Scan(ClientContext &context, const std::function<void(CatalogEntry *)> &callback);

	template <class T>
	vector<T *> GetEntries(ClientContext &context) {
		vector<T *> result;
		Scan(context, [&](CatalogEntry *entry) { result.push_back((T *)entry); });
		return result;
	}

	static bool HasConflict(ClientContext &context, transaction_t timestamp);
	static bool UseTimestamp(ClientContext &context, transaction_t timestamp);

	CatalogEntry *GetEntryFromIndex(idx_t index);
	void UpdateTimestamp(CatalogEntry *entry, transaction_t timestamp);

private:
	//! Given a root entry, gets the entry valid for this transaction
	CatalogEntry *GetEntryForTransaction(ClientContext &context, CatalogEntry *current);
	CatalogEntry *GetCommittedEntry(CatalogEntry *current);
	bool GetEntryInternal(ClientContext &context, const string &name, idx_t &entry_index, CatalogEntry *&entry);
	bool GetEntryInternal(ClientContext &context, idx_t entry_index, CatalogEntry *&entry);
	//! Drops an entry from the catalog set; must hold the catalog_lock to safely call this
	void DropEntryInternal(ClientContext &context, idx_t entry_index, CatalogEntry &entry, bool cascade,
	                       set_lock_map_t &lock_set);
	CatalogEntry *CreateEntryInternal(ClientContext &context, unique_ptr<CatalogEntry> entry);
	MappingValue *GetMapping(ClientContext &context, const string &name, bool allow_lowercase_alias,
	                         bool get_latest = false);
	void PutMapping(ClientContext &context, const string &name, idx_t entry_index);
	void DeleteMapping(ClientContext &context, const string &name);

private:
	Catalog &catalog;
	//! The catalog lock is used to make changes to the data
	mutex catalog_lock;
	//! Mapping of string to catalog entry
	unordered_map<string, unique_ptr<MappingValue>> mapping;
	//! The set of catalog entries
	unordered_map<idx_t, unique_ptr<CatalogEntry>> entries;
	//! The current catalog entry index
	idx_t current_entry = 0;
	//! The generator used to generate default internal entries
	unique_ptr<DefaultGenerator> defaults;
};

} // namespace duckdb
