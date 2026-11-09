//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/table_filter_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/table_filter.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/reference_map.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/map.hpp"

namespace duckdb {

class TableFilterSet {
public:
	void PushFilter(ProjectionIndex col_idx, unique_ptr<TableFilter> filter);
	void PushMultiColumnFilter(unique_ptr<TableFilter> filter);
	bool HasFilters() const;
	idx_t FilterCount() const;
	bool HasMultiColumnFilters() const;
	bool HasFilter(ProjectionIndex col_idx) const;
	TableFilter &GetFilterByColumnIndexMutable(ProjectionIndex col_idx);
	optional_ptr<TableFilter> TryGetFilterByColumnIndexMutable(ProjectionIndex col_idx);
	const TableFilter &GetFilterByColumnIndex(ProjectionIndex col_idx) const;
	optional_ptr<const TableFilter> TryGetFilterByColumnIndex(ProjectionIndex col_idx) const;
	void SetFilterByColumnIndex(ProjectionIndex col_idx, unique_ptr<TableFilter> filter);
	void RemoveFilterByColumnIndex(ProjectionIndex col_idx);
	void ClearFilters();
	const vector<unique_ptr<TableFilter>> &GetMultiColumnFilters() const;

	bool Equals(TableFilterSet &other);
	static bool Equals(TableFilterSet *left, TableFilterSet *right);

	unique_ptr<TableFilterSet> Copy() const;

	void Serialize(Serializer &serializer) const;
	static TableFilterSet Deserialize(Deserializer &deserializer);

	map<ProjectionIndex, unique_ptr<TableFilter>> GetTableFiltersForSerialization(Serializer &serializer) const;
	map<ProjectionIndex, unique_ptr<TableFilter>> &GetTableFiltersForDeserialization(Deserializer &deserializer);

public:
	class TableFilterIteratorEntry {
	public:
		explicit TableFilterIteratorEntry(map<ProjectionIndex, unique_ptr<TableFilter>>::iterator);

		ProjectionIndex GetIndex() const;
		TableFilter &Filter();
		const TableFilter &Filter() const;
		unique_ptr<TableFilter> TakeFilter();

	public:
		map<ProjectionIndex, unique_ptr<TableFilter>>::iterator iterator;
	};

	class ConstTableFilterIteratorEntry {
	public:
		explicit ConstTableFilterIteratorEntry(map<ProjectionIndex, unique_ptr<TableFilter>>::const_iterator);

		ProjectionIndex GetIndex() const;
		const TableFilter &Filter() const;

	public:
		map<ProjectionIndex, unique_ptr<TableFilter>>::const_iterator iterator;
	};

	// iterator
	template <class T>
	class TableFilterIterator {
	public:
		explicit TableFilterIterator(T entry_p) : entry(std::move(entry_p)) {
		}

	public:
		TableFilterIterator &operator++() {
			++entry.iterator;
			return *this;
		}
		bool operator!=(const TableFilterIterator &other) const {
			return entry.iterator != other.entry.iterator;
		}
		T &operator*() {
			return entry;
		}
		const T &operator*() const {
			return entry;
		}

	private:
		T entry;
	};

	TableFilterIterator<TableFilterIteratorEntry> begin() { // NOLINT: match stl API
		return TableFilterIterator<TableFilterIteratorEntry>(TableFilterIteratorEntry(filters.begin()));
	}
	TableFilterIterator<TableFilterIteratorEntry> end() { // NOLINT: match stl API
		return TableFilterIterator<TableFilterIteratorEntry>(TableFilterIteratorEntry(filters.end()));
	}
	TableFilterIterator<ConstTableFilterIteratorEntry> begin() const { // NOLINT: match stl API
		return TableFilterIterator<ConstTableFilterIteratorEntry>(ConstTableFilterIteratorEntry(filters.begin()));
	}
	TableFilterIterator<ConstTableFilterIteratorEntry> end() const { // NOLINT: match stl API
		return TableFilterIterator<ConstTableFilterIteratorEntry>(ConstTableFilterIteratorEntry(filters.end()));
	}

private:
	map<ProjectionIndex, unique_ptr<TableFilter>> filters;
	vector<unique_ptr<TableFilter>> multi_column_filters;
};

class DynamicTableFilterSet {
public:
	void ClearFilters(const PhysicalOperator &op);
	void PushFilter(const PhysicalOperator &op, ProjectionIndex column_index, unique_ptr<TableFilter> filter);

	vector<shared_ptr<DynamicTableFilterSet>> AddDisjunction(ProjectionIndex column, idx_t alternative_count);
	vector<reference<DynamicTableFilterSet>> GetDisjunctionFilters() const;

	bool HasFilters() const;
	unique_ptr<TableFilterSet> GetFinalTableFilters(const PhysicalTableScan &scan,
	                                                optional_ptr<TableFilterSet> existing_filters) const;

private:
	unique_ptr<Expression> CopyColumnFilter(ProjectionIndex column) const;

	struct FilterDisjunction {
		ProjectionIndex column;
		vector<shared_ptr<DynamicTableFilterSet>> alternatives;
	};

	mutable mutex lock;
	reference_map_t<const PhysicalOperator, unique_ptr<TableFilterSet>> filters;
	vector<FilterDisjunction> disjunctions;
};

} // namespace duckdb
