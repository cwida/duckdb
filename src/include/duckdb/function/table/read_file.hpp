//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/table/read_file.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/multi_file/multi_file_function.hpp"
#include "duckdb/common/serializer/memory_stream.hpp"
#include "utf8proc_wrapper.hpp"

namespace duckdb {

struct ReadFileBindData : public TableFunctionData {
	unique_ptr<BaseFileReaderOptions> options;

	static constexpr const idx_t FILE_NAME_COLUMN = 0;
	static constexpr const idx_t FILE_CONTENT_COLUMN = 1;
	static constexpr const idx_t FILE_SIZE_COLUMN = 2;
	static constexpr const idx_t FILE_LAST_MODIFIED_COLUMN = 3;
};

struct ReadFileGlobalState : public GlobalTableFunctionState {
	ReadFileGlobalState() {
	}

	shared_ptr<MultiFileList> file_list;
	vector<idx_t> column_ids;
	bool requires_file_open = false;
	bool requires_file_metadata = false;

	unique_ptr<MemoryStream> stream;
};

} // namespace duckdb
