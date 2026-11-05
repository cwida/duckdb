//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/delete_state.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/storage/table/append_state.hpp"

namespace duckdb {
class TableCatalogEntry;

struct TableDeleteState {
	unique_ptr<ConstraintState> constraint_state;
	bool has_delete_constraints = false;
	DataChunk verify_chunk;
	vector<StorageIndex> col_ids;
};

} // namespace duckdb
