#include "duckdb/catalog/dependency_list.hpp"
#include "duckdb/catalog/catalog_entry.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/dependency/dependency_entry.hpp"
#include "duckdb/catalog/catalog.hpp"
#include "duckdb/catalog/dependency_manager.hpp"

namespace duckdb {

uint64_t LogicalDependencyHashFunction::operator()(const LogicalDependency &a) const {
	auto &name = a.entry.name;
	auto &type = a.entry.type;
	auto &catalog = a.catalog;

	hash_t hash = duckdb::Hash(name.c_str());
	for (auto &schema : a.entry.schema_path) {
		hash = CombineHash(hash, duckdb::Hash(schema.c_str()));
	}
	hash = CombineHash(hash, duckdb::Hash(catalog.c_str()));
	hash = CombineHash(hash, duckdb::Hash<uint8_t>(static_cast<uint8_t>(type)));
	return hash;
}

bool LogicalDependencyEquality::operator()(const LogicalDependency &a, const LogicalDependency &b) const {
	if (a.entry.type != b.entry.type) {
		return false;
	}
	if (a.entry.name != b.entry.name) {
		return false;
	}
	if (a.entry.schema_path != b.entry.schema_path) {
		return false;
	}
	if (a.catalog != b.catalog) {
		return false;
	}
	return true;
}

LogicalDependency::LogicalDependency() : entry(), catalog() {
}

LogicalDependency::LogicalDependency(CatalogEntry &entry) {
	catalog = Identifier::InvalidCatalog();
	if (entry.type == CatalogType::DEPENDENCY_ENTRY) {
		auto &dependency_entry = entry.Cast<DependencyEntry>();

		this->entry = dependency_entry.EntryInfo();
	} else {
		this->entry.schema_path = DependencyManager::GetSchemaPath(entry);
		this->entry.name = entry.name;
		this->entry.type = entry.type;
		catalog = entry.ParentCatalog().GetName();
	}
}

LogicalDependency::LogicalDependency(optional_ptr<Catalog> catalog_p, CatalogEntryInfo entry_p, Identifier catalog_str)
    : entry(std::move(entry_p)), catalog(std::move(catalog_str)) {
	if (catalog_p) {
		catalog = catalog_p->GetName();
	}
}

bool LogicalDependency::operator==(const LogicalDependency &other) const {
	return other.entry.name == entry.name && other.entry.schema_path == entry.schema_path &&
	       other.entry.type == entry.type;
}

void LogicalDependencyList::AddDependency(CatalogEntry &entry) {
	AddDependency(entry, DependencyDependentFlags().SetBlocking());
}

void LogicalDependencyList::AddDependency(CatalogEntry &entry, DependencyDependentFlags flags) {
	LogicalDependency dependency(entry);
	dependency.flags = std::move(flags);
	AddDependency(dependency);
}

void LogicalDependencyList::AddDependency(const LogicalDependency &entry) {
	auto it = set.find(entry);
	if (it == set.end()) {
		set.insert(entry);
		return;
	}
	// Merge flags instead of discarding the new ones - the same subject can be depended on for multiple reasons
	auto merged = *it;
	merged.flags.Apply(entry.flags);
	set.erase(it);
	set.insert(std::move(merged));
}

bool LogicalDependencyList::Contains(CatalogEntry &entry_p) {
	LogicalDependency logical_entry(entry_p);
	return set.count(logical_entry);
}

void LogicalDependencyList::VerifyDependencies(Catalog &catalog, const Identifier &name) {
	for (auto &dep : set) {
		if (dep.catalog != catalog.GetName()) {
			throw DependencyException(
			    "Error adding dependency for object %s - dependency %s is in catalog "
			    "%s, which does not match the catalog %s.\nCross catalog dependencies are not supported.",
			    name, dep.entry.name, dep.catalog, catalog.GetName());
		}
	}
}

const LogicalDependencyList::create_info_set_t &LogicalDependencyList::Set() const {
	return set;
}

bool LogicalDependencyList::operator==(const LogicalDependencyList &other) const {
	if (set.size() != other.set.size()) {
		return false;
	}

	for (auto &entry : set) {
		if (!other.set.count(entry)) {
			return false;
		}
	}
	return true;
}

} // namespace duckdb
