//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/row_group_segment_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/segment_tree.hpp"
#include "duckdb/storage/table/row_group.hpp"

namespace duckdb {
struct DataTableInfo;
class PersistentTableData;
class MetadataReader;

class RowGroupSegmentTree : public SegmentTree<RowGroup, true> {
public:
	RowGroupSegmentTree(RowGroupCollection &collection, idx_t base_row_id);
	~RowGroupSegmentTree() override;

	void Initialize(PersistentTableData &data, optional_ptr<vector<MetaBlockPointer>> read_pointers = nullptr);

	MetaBlockPointer GetRootPointer() const {
		return root_pointer;
	}
	//! The number of row groups, including the ones that are not loaded yet, without loading them
	idx_t GetRowGroupCountWithoutLoading() const;

protected:
	optional<LoadedSegment<RowGroup>> LoadSegment() const override;

	RowGroupCollection &collection;
	mutable idx_t current_row_group;
	mutable idx_t max_row_group;
	mutable unique_ptr<MetadataReader> reader;
	MetaBlockPointer root_pointer;
};

} // namespace duckdb
