//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/table_index.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/typed_index.hpp"
#include <functional>

namespace duckdb {

struct TableIndex : public TypedIndex<TableIndex> {
	using TypedIndex::TypedIndex;
};

} // namespace duckdb

namespace std {

template <>
struct hash<duckdb::TableIndex> {
	size_t operator()(const duckdb::TableIndex &tbl_index) const {
		return std::hash<uint64_t> {}(tbl_index.index);
	}
};
} // namespace std
