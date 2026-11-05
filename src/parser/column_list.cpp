#include "duckdb/parser/column_list.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/to_string.hpp"
#include "duckdb/common/exception/catalog_exception.hpp"

namespace duckdb {

ColumnList::ColumnList(bool allow_duplicate_names) : allow_duplicate_names(allow_duplicate_names) {
}

ColumnList::ColumnList(vector<ColumnDefinition> columns, bool allow_duplicate_names)
    : allow_duplicate_names(allow_duplicate_names) {
	for (auto &col : columns) {
		AddColumn(std::move(col));
	}
}

void ColumnList::AddColumn(ColumnDefinition column) {
	auto oid = columns.size();
	if (!column.Generated()) {
		column.SetStorageOid(physical_columns.size());
		physical_columns.push_back(oid);
	} else {
		column.SetStorageOid(DConstants::INVALID_INDEX);
	}
	column.SetOid(columns.size());
	AddToNameMap(column);
	columns.push_back(std::move(column));
}

void ColumnList::Finalize() {
	// add the "rowid" alias, if there is no rowid column specified in the table
	if (name_map.find("rowid") == name_map.end()) {
		name_map["rowid"] = COLUMN_IDENTIFIER_ROW_ID;
	}
}

void ColumnList::AddToNameMap(ColumnDefinition &col) {
	if (allow_duplicate_names) {
		idx_t index = 1;
		string base_name = col.Name().GetIdentifierName();
		while (name_map.find(col.Name()) != name_map.end()) {
			col.SetName(Identifier(base_name + "_" + to_string(index++)));
		}
	} else {
		if (name_map.find(col.Name()) != name_map.end()) {
			throw CatalogException("Column with name %s already exists!", col.Name());
		}
	}
	name_map[col.Name()] = col.Oid();
}

ColumnDefinition &ColumnList::GetColumnMutable(LogicalIndex logical) {
	if (logical.index >= columns.size()) {
		throw InternalException("Logical column index %lld out of range", logical.index);
	}
	return columns[logical.index];
}

ColumnDefinition &ColumnList::GetColumnMutable(PhysicalIndex physical) {
	if (physical.index >= physical_columns.size()) {
		throw InternalException("Physical column index %lld out of range", physical.index);
	}
	auto logical_index = physical_columns[physical.index];
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

ColumnDefinition &ColumnList::GetColumnMutable(const Identifier &name) {
	auto entry = name_map.find(name);
	if (entry == name_map.end()) {
		throw InternalException("Column with name %s does not exist", name);
	}
	auto logical_index = entry->second;
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

const ColumnDefinition &ColumnList::GetColumn(LogicalIndex logical) const {
	if (logical.index >= columns.size()) {
		throw InternalException("Logical column index %lld out of range", logical.index);
	}
	return columns[logical.index];
}

const ColumnDefinition &ColumnList::GetColumn(PhysicalIndex physical) const {
	if (physical.index >= physical_columns.size()) {
		throw InternalException("Physical column index %lld out of range", physical.index);
	}
	auto logical_index = physical_columns[physical.index];
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

const ColumnDefinition &ColumnList::GetColumn(const Identifier &name) const {
	auto entry = name_map.find(name);
	if (entry == name_map.end()) {
		throw InternalException("Column with name %s does not exist", name);
	}
	auto logical_index = entry->second;
	D_ASSERT(logical_index < columns.size());
	return columns[logical_index];
}

vector<string> ColumnList::GetColumnNames() const {
	vector<string> names;
	names.reserve(columns.size());
	for (auto &column : columns) {
		names.emplace_back(column.Name());
	}
	return names;
}

vector<LogicalType> ColumnList::GetColumnTypes() const {
	vector<LogicalType> types;
	types.reserve(columns.size());
	for (auto &column : columns) {
		types.push_back(column.Type());
	}
	return types;
}

bool ColumnList::ColumnExists(const Identifier &name) const {
	auto entry = name_map.find(name);
	return entry != name_map.end();
}

PhysicalIndex ColumnList::LogicalToPhysical(LogicalIndex logical) const {
	auto &column = GetColumn(logical);
	if (column.Generated()) {
		throw InternalException("Column at position %d is not a physical column", logical.index);
	}
	return column.Physical();
}

LogicalIndex ColumnList::PhysicalToLogical(PhysicalIndex index) const {
	auto &column = GetColumn(index);
	return column.Logical();
}

LogicalIndex ColumnList::GetColumnIndex(Identifier &column_name) const {
	auto entry = name_map.find(column_name);
	if (entry == name_map.end()) {
		return LogicalIndex(DConstants::INVALID_INDEX);
	}
	if (entry->second == COLUMN_IDENTIFIER_ROW_ID) {
		column_name = "rowid";
		return LogicalIndex(COLUMN_IDENTIFIER_ROW_ID);
	}
	column_name = columns[entry->second].Name();
	return LogicalIndex(entry->second);
}

ColumnList ColumnList::Copy() const {
	ColumnList result(allow_duplicate_names);
	for (auto &col : columns) {
		result.AddColumn(col.Copy());
	}
	return result;
}

ColumnList::ColumnListIterator ColumnList::Logical() const {
	return ColumnListIterator(*this, false);
}

ColumnList::ColumnListIterator ColumnList::Physical() const {
	return ColumnListIterator(*this, true);
}

} // namespace duckdb
