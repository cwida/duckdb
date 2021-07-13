#include "duckdb/catalog/dependency_manager.hpp"

#include "duckdb/catalog/catalog.hpp"

namespace duckdb {

DependencyManager::DependencyManager(Catalog &catalog) : catalog(catalog) {
}

void DependencyManager::AddObject(ClientContext &context, CatalogEntry *object,
                                  unordered_set<CatalogEntry *> &dependencies) {
	// check for each object in the sources if they were not deleted yet
	for (auto &dependency : dependencies) {
		idx_t entry_index;
		CatalogEntry *catalog_entry;
		if (!dependency->set->GetEntryInternal(context, dependency->name, entry_index, catalog_entry)) {
			throw InternalException("Dependency has already been deleted?");
		}
	}
	// indexes do not require CASCADE to be dropped, they are simply always dropped along with the table
	auto dependency_type = object->type == CatalogType::INDEX_ENTRY ? DependencyType::DEPENDENCY_AUTOMATIC
	                                                                : DependencyType::DEPENDENCY_REGULAR;
	// add the object to the dependents_map of each object that it depends on
	for (auto &dependency : dependencies) {
		dependents_map[dependency].insert(Dependency(object, dependency_type));
	}
	// create the dependents map for this object: it starts out empty
	dependents_map[object] = dependency_set_t();
	dependencies_map[object] = dependencies;
}

void DependencyManager::DropObject(ClientContext &context, CatalogEntry *object, bool cascade,
                                   set_lock_map_t &lock_set) {
	D_ASSERT(dependents_map.find(object) != dependents_map.end());

	// first check the objects that depend on this object
	auto &dependent_objects = dependents_map[object];
	for (auto &dep : dependent_objects) {
		// look up the entry in the catalog set
		auto &catalog_set = *dep.entry->set;
		auto mapping_value = catalog_set.GetMapping(context, dep.entry->name, true /* get_latest */);
		if (mapping_value == nullptr) {
			continue;
		}
		idx_t entry_index = mapping_value->index;
		CatalogEntry *dependency_entry;

		if (!catalog_set.GetEntryInternal(context, entry_index, dependency_entry)) {
			// the dependent object was already deleted, no conflict
			continue;
		}
		// conflict: attempting to delete this object but the dependent object still exists
		if (cascade || dep.dependency_type == DependencyType::DEPENDENCY_AUTOMATIC) {
			// cascade: drop the dependent object
			catalog_set.DropEntryInternal(context, entry_index, *dependency_entry, cascade, lock_set);
		} else {
			// no cascade and there are objects that depend on this object: throw error
			throw CatalogException("Cannot drop entry \"%s\" because there are entries that "
			                       "depend on it. Use DROP...CASCADE to drop all dependents.",
			                       object->name);
		}
	}
}

void DependencyManager::AlterObject(ClientContext &context, CatalogEntry *old_obj, CatalogEntry *new_obj) {
	D_ASSERT(dependents_map.find(old_obj) != dependents_map.end());
	D_ASSERT(dependencies_map.find(old_obj) != dependencies_map.end());

	// first check the objects that depend on this object
	auto &dependent_objects = dependents_map[old_obj];
	for (auto &dep : dependent_objects) {
		// look up the entry in the catalog set
		auto &catalog_set = *dep.entry->set;
		idx_t entry_index;
		CatalogEntry *dependency_entry;
		if (!catalog_set.GetEntryInternal(context, dep.entry->name, entry_index, dependency_entry)) {
			// the dependent object was already deleted, no conflict
			continue;
		}
		// conflict: attempting to alter this object but the dependent object still exists
		// no cascade and there are objects that depend on this object: throw error
		throw CatalogException("Cannot alter entry \"%s\" because there are entries that "
		                       "depend on it.",
		                       old_obj->name);
	}
	// add the new object to the dependents_map of each object that it depents on
	auto &old_dependencies = dependencies_map[old_obj];
	for (auto &dependency : old_dependencies) {
		dependents_map[dependency].insert(new_obj);
	}
	// add the new object to the dependency manager
	dependents_map[new_obj] = dependency_set_t();
	dependencies_map[new_obj] = old_dependencies;
}

void DependencyManager::EraseObject(CatalogEntry *object) {
	// obtain the writing lock
	EraseObjectInternal(object);
}

void DependencyManager::EraseObjectInternal(CatalogEntry *object) {
	if (dependents_map.find(object) == dependents_map.end()) {
		// dependencies already removed
		return;
	}
	D_ASSERT(dependents_map.find(object) != dependents_map.end());
	D_ASSERT(dependencies_map.find(object) != dependencies_map.end());
	// now for each of the dependencies, erase the entries from the dependents_map
	for (auto &dependency : dependencies_map[object]) {
		auto entry = dependents_map.find(dependency);
		if (entry != dependents_map.end()) {
			D_ASSERT(entry->second.find(object) != entry->second.end());
			entry->second.erase(object);
		}
	}
	// erase the dependents and dependencies for this object
	dependents_map.erase(object);
	dependencies_map.erase(object);
}

void DependencyManager::Scan(const std::function<void(CatalogEntry *, CatalogEntry *, DependencyType)> &callback) {
	lock_guard<mutex> write_lock(catalog.write_lock);
	for (auto &entry : dependents_map) {
		for (auto &dependent : entry.second) {
			callback(entry.first, dependent.entry, dependent.dependency_type);
		}
	}
}

} // namespace duckdb
