//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/catalog/dependency_manager.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/catalog/catalog_set.hpp"
#include "duckdb/catalog/dependency.hpp"

#include <functional>

namespace duckdb {
class Catalog;
class ClientContext;

//! The DependencyManager is in charge of managing dependencies between catalog entries
class DependencyManager {
	friend class CatalogSet;

public:
	explicit DependencyManager(Catalog &catalog);

	//! Erase the object from the DependencyManager; this should only happen when the object itself is destroyed
	void EraseObject(CatalogEntry *object);

	//! Scans all dependencies, returning pairs of (object, dependent)
	void Scan(const std::function<void(CatalogEntry *, CatalogEntry *, DependencyType)> &callback);

private:
	Catalog &catalog;
	//! Map of objects that DEPEND on [object], i.e. [object] can only be deleted when all entries in the dependency map
	//! are deleted.
	unordered_map<CatalogEntry *, dependency_set_t> dependents_map;
	//! Map of objects that the source object DEPENDS on, i.e. when any of the entries in the vector perform a CASCADE
	//! drop then [object] is deleted as wel
	unordered_map<CatalogEntry *, unordered_set<CatalogEntry *>> dependencies_map;

private:
	void AddObject(ClientContext &context, CatalogEntry *object, unordered_set<CatalogEntry *> &dependencies);
	void DropObject(ClientContext &context, CatalogEntry *object, bool cascade, set_lock_map_t &lock_set);
	void AlterObject(ClientContext &context, CatalogEntry *old_obj, CatalogEntry *new_obj);
	void EraseObjectInternal(CatalogEntry *object);
};
} // namespace duckdb
