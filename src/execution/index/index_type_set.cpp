#include "duckdb/common/exception/catalog_exception.hpp"
#include "duckdb/execution/index/index_type.hpp"
#include "duckdb/execution/index/index_type_set.hpp"
#include "duckdb/execution/index/art/art.hpp"

namespace duckdb {

IndexTypeSet::IndexTypeSet() {
	// Register the ART index type by default
	RegisterIndexType(ART::GetARTIndexType());
}

optional_ptr<IndexType> IndexTypeSet::FindByName(const string &name) {
	lock_guard<mutex> g(lock);
	auto entry = functions.find(Identifier(name));
	if (entry == functions.end()) {
		return nullptr;
	}
	return &entry->second;
}

void IndexTypeSet::RegisterIndexType(const IndexType &index_type) {
	lock_guard<mutex> g(lock);
	if (functions.find(Identifier(index_type.name)) != functions.end()) {
		throw CatalogException("Index type with name \"%s\" already exists!", index_type.name.c_str());
	}
	functions[Identifier(index_type.name)] = index_type;
}

} // namespace duckdb
