#include "duckdb/catalog/catalog_entry/column_dependency_manager.hpp"
#include "duckdb/parser/column_definition.hpp"
#include "duckdb/common/set.hpp"
#include "duckdb/common/queue.hpp"
#include "duckdb/common/exception/binder_exception.hpp"

namespace duckdb {

ColumnDependencyManager::ColumnDependencyManager() {
}

ColumnDependencyManager::~ColumnDependencyManager() {
}

void ColumnDependencyManager::AddGeneratedColumn(const ColumnDefinition &column, const ColumnList &list) {
	D_ASSERT(column.Generated());
	vector<string> referenced_columns;
	column.GetListOfDependencies(referenced_columns);
	vector<LogicalIndex> indices;
	for (auto &col : referenced_columns) {
		if (!list.ColumnExists(Identifier(col))) {
			throw BinderException("Column \"%s\" referenced by generated column does not exist", col);
		}
		auto &entry = list.GetColumn(Identifier(col));
		indices.push_back(entry.Logical());
	}
	return AddGeneratedColumn(column.Logical(), indices);
}

void ColumnDependencyManager::AddGeneratedColumn(LogicalIndex index, const vector<LogicalIndex> &indices, bool root) {
	if (indices.empty()) {
		return;
	}
	auto &list = dependents_map[index];
	// Create a link between the dependencies
	for (auto &dep : indices) {
		// Add this column as a dependency of the new column
		list.insert(dep);
		// Add the new column as a dependent of the column
		dependencies_map[dep].insert(index);
		// Inherit the dependencies
		if (HasDependencies(dep)) {
			auto &inherited_deps = dependents_map[dep];
			D_ASSERT(!inherited_deps.empty());
			for (auto &inherited_dep : inherited_deps) {
				list.insert(inherited_dep);
				dependencies_map[inherited_dep].insert(index);
			}
		}
		if (!root) {
			continue;
		}
		direct_dependencies[index].insert(dep);
	}
	if (!HasDependents(index)) {
		return;
	}
	auto &dependents = dependencies_map[index];
	if (dependents.count(index)) {
		throw InvalidInputException("Circular dependency encountered when resolving generated column expressions");
	}
	// Also let the dependents of this generated column inherit the dependencies
	for (auto &dependent : dependents) {
		AddGeneratedColumn(dependent, indices, false);
	}
}

ColumnDependencyManager ColumnDependencyManager::Copy() const {
	ColumnDependencyManager result;
	result.dependencies_map = dependencies_map;
	result.dependents_map = dependents_map;
	result.direct_dependencies = direct_dependencies;
	result.deleted_columns = deleted_columns;
	return result;
}

vector<LogicalIndex> ColumnDependencyManager::RemoveColumn(LogicalIndex index, idx_t column_amount) {
	// Always add the initial column
	deleted_columns.insert(index);

	RemoveGeneratedColumn(index);
	RemoveStandardColumn(index);

	// Clean up the internal list
	vector<LogicalIndex> new_indices = CleanupInternals(column_amount);
	D_ASSERT(deleted_columns.empty());
	return new_indices;
}

bool ColumnDependencyManager::IsDependencyOf(LogicalIndex gcol, LogicalIndex col) const {
	auto entry = dependents_map.find(gcol);
	if (entry == dependents_map.end()) {
		return false;
	}
	auto &list = entry->second;
	return list.count(col);
}

bool ColumnDependencyManager::HasDependencies(LogicalIndex index) const {
	auto entry = dependents_map.find(index);
	if (entry == dependents_map.end()) {
		return false;
	}
	return true;
}

const logical_index_set_t &ColumnDependencyManager::GetDependencies(LogicalIndex index) const {
	auto entry = dependents_map.find(index);
	D_ASSERT(entry != dependents_map.end());
	return entry->second;
}

bool ColumnDependencyManager::HasDependents(LogicalIndex index) const {
	auto entry = dependencies_map.find(index);
	if (entry == dependencies_map.end()) {
		return false;
	}
	return true;
}

const logical_index_set_t &ColumnDependencyManager::GetDependents(LogicalIndex index) const {
	auto entry = dependencies_map.find(index);
	D_ASSERT(entry != dependencies_map.end());
	return entry->second;
}

void ColumnDependencyManager::RemoveStandardColumn(LogicalIndex index) {
	if (!HasDependents(index)) {
		return;
	}
	auto dependents = dependencies_map[index];
	for (auto &gcol : dependents) {
		// If index is a direct dependency of gcol, remove it from the list
		if (direct_dependencies.find(gcol) != direct_dependencies.end()) {
			direct_dependencies[gcol].erase(index);
		}
		RemoveGeneratedColumn(gcol);
	}
	// Remove this column from the dependencies map
	dependencies_map.erase(index);
}

void ColumnDependencyManager::RemoveGeneratedColumn(LogicalIndex index) {
	deleted_columns.insert(index);
	if (!HasDependencies(index)) {
		return;
	}
	auto &dependencies = dependents_map[index];
	for (auto &col : dependencies) {
		// Remove this generated column from the list of this column
		auto &col_dependents = dependencies_map[col];
		D_ASSERT(col_dependents.count(index));
		col_dependents.erase(index);
		// If the resulting list is empty, remove the column from the dependencies map altogether
		if (col_dependents.empty()) {
			dependencies_map.erase(col);
		}
	}
	// Remove this column from the dependents_map map
	dependents_map.erase(index);
}

void ColumnDependencyManager::AdjustSingle(LogicalIndex idx, idx_t offset) {
	D_ASSERT(idx.index >= offset);
	LogicalIndex new_idx = LogicalIndex(idx.index - offset);
	// Adjust this index in the dependents of this column
	bool has_dependents = HasDependents(idx);
	bool has_dependencies = HasDependencies(idx);

	if (has_dependents) {
		auto &dependents = GetDependents(idx);
		for (auto &dep : dependents) {
			auto &dep_dependencies = dependents_map[dep];
			dep_dependencies.erase(idx);
			D_ASSERT(!dep_dependencies.count(new_idx));
			dep_dependencies.insert(new_idx);
		}
	}
	if (has_dependencies) {
		auto &dependencies = GetDependencies(idx);
		for (auto &dep : dependencies) {
			auto &dep_dependents = dependencies_map[dep];
			dep_dependents.erase(idx);
			D_ASSERT(!dep_dependents.count(new_idx));
			dep_dependents.insert(new_idx);
		}
	}
	if (has_dependents) {
		D_ASSERT(!dependencies_map.count(new_idx));
		dependencies_map[new_idx] = std::move(dependencies_map[idx]);
		dependencies_map.erase(idx);
	}
	if (has_dependencies) {
		D_ASSERT(!dependents_map.count(new_idx));
		dependents_map[new_idx] = std::move(dependents_map[idx]);
		dependents_map.erase(idx);
	}
}

vector<LogicalIndex> ColumnDependencyManager::CleanupInternals(idx_t column_amount) {
	vector<LogicalIndex> to_adjust;
	D_ASSERT(!deleted_columns.empty());
	// Get the lowest index that was deleted
	vector<LogicalIndex> new_indices(column_amount, LogicalIndex(DConstants::INVALID_INDEX));
	idx_t threshold = deleted_columns.begin()->index;

	idx_t offset = 0;
	for (idx_t i = 0; i < column_amount; i++) {
		auto current_index = LogicalIndex(i);
		auto new_index = LogicalIndex(i - offset);
		new_indices[i] = new_index;
		if (deleted_columns.count(current_index)) {
			offset++;
			continue;
		}
		if (i > threshold && (HasDependencies(current_index) || HasDependents(current_index))) {
			to_adjust.push_back(current_index);
		}
	}

	// Adjust all indices inside the dependency managers internal mappings
	for (auto &col : to_adjust) {
		auto offset = col.index - new_indices[col.index].index;
		AdjustSingle(col, offset);
	}
	deleted_columns.clear();
	return new_indices;
}

stack<LogicalIndex> ColumnDependencyManager::GetBindOrder(const ColumnList &columns) {
	stack<LogicalIndex> bind_order;
	queue<LogicalIndex> to_visit;
	logical_index_set_t visited;

	for (auto &entry : direct_dependencies) {
		auto dependent = entry.first;
		//! Skip the dependents that are also dependencies
		if (dependencies_map.find(dependent) != dependencies_map.end()) {
			continue;
		}
		bind_order.push(dependent);
		visited.insert(dependent);
		for (auto &dependency : direct_dependencies[dependent]) {
			to_visit.push(dependency);
		}
	}

	while (!to_visit.empty()) {
		auto column = to_visit.front();
		to_visit.pop();

		//! If this column does not have dependencies, the queue stops getting filled
		if (direct_dependencies.find(column) == direct_dependencies.end()) {
			continue;
		}
		bind_order.push(column);
		visited.insert(column);

		for (auto &dependency : direct_dependencies[column]) {
			to_visit.push(dependency);
		}
	}

	// Add generated columns that have no dependencies, but still might need to have their type resolved
	for (auto &col : columns.Logical()) {
		// Not a generated column
		if (!col.Generated()) {
			continue;
		}
		// Already added to the bind_order stack
		if (visited.count(col.Logical())) {
			continue;
		}
		bind_order.push(col.Logical());
	}

	return bind_order;
}

} // namespace duckdb
