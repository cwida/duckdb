//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/arrow/arrow_appender.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/arrow/arrow_converter.hpp"
#include "duckdb/common/arrow/arrow.hpp"

namespace duckdb {

struct ArrowAppendData;

class ArrowTypeExtensionData;

//! The ArrowAppender class can be used to incrementally construct an arrow array by appending data chunks into it
class ArrowAppender {
public:
	DUCKDB_API ArrowAppender(vector<LogicalType> types_p, const idx_t initial_capacity, ClientProperties options,
	                         unordered_map<idx_t, const shared_ptr<ArrowTypeExtensionData>> extension_type_cast);
	DUCKDB_API ~ArrowAppender();

public:
	//! Append a data chunk to the underlying arrow array
	DUCKDB_API void Append(DataChunk &input, idx_t from, idx_t to, idx_t input_size);
	//! Returns the underlying arrow array
	DUCKDB_API ArrowArray Finalize();
	idx_t RowCount() const;
	//! The bytes the appended rows occupy. Valid until Finalize hands the buffers to the array
	DUCKDB_API idx_t ByteSize() const;
	static void ReleaseArray(ArrowArray *array);
	static ArrowArray *FinalizeChild(const LogicalType &type, unique_ptr<ArrowAppendData> append_data_p);
	static unique_ptr<ArrowAppendData>
	InitializeChild(const LogicalType &type, const idx_t capacity, ClientProperties &options,
	                const shared_ptr<ArrowTypeExtensionData> &extension_type = nullptr);
	static void AddChildren(ArrowAppendData &data, const idx_t count);

private:
	//! The types of the chunks that will be appended in
	vector<LogicalType> types;
	//! The root arrow append data
	vector<unique_ptr<ArrowAppendData>> root_data;
	//! The total row count that has been appended
	idx_t row_count = 0;

	ClientProperties options;
};

} // namespace duckdb
