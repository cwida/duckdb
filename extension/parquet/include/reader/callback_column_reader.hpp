//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/callback_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "column_reader.hpp"
#include "reader/templated_column_reader.hpp"
#include "parquet_reader.hpp"

namespace duckdb {

template <class PARQUET_PHYSICAL_TYPE, class DUCKDB_PHYSICAL_TYPE,
          DUCKDB_PHYSICAL_TYPE (*FUNC)(const PARQUET_PHYSICAL_TYPE &input)>
class CallbackColumnReader
    : public TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE,
                                   CallbackParquetValueConversion<PARQUET_PHYSICAL_TYPE, DUCKDB_PHYSICAL_TYPE, FUNC>> {
	using BaseType =
	    TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE,
	                          CallbackParquetValueConversion<PARQUET_PHYSICAL_TYPE, DUCKDB_PHYSICAL_TYPE, FUNC>>;

public:
	static constexpr const PhysicalType TYPE = PhysicalType::INVALID;

public:
	CallbackColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema)
	    : TemplatedColumnReader<DUCKDB_PHYSICAL_TYPE,
	                            CallbackParquetValueConversion<PARQUET_PHYSICAL_TYPE, DUCKDB_PHYSICAL_TYPE, FUNC>>(
	          reader, schema) {
	}

protected:
	void Dictionary(shared_ptr<ResizeableBuffer> dictionary_data, idx_t num_entries) {
		BaseType::AllocateDict(num_entries * sizeof(DUCKDB_PHYSICAL_TYPE));
		auto dict_ptr = (DUCKDB_PHYSICAL_TYPE *)this->dict->GetCurrentLoc();
		for (idx_t i = 0; i < num_entries; i++) {
			dict_ptr[i] = FUNC(dictionary_data->Read<PARQUET_PHYSICAL_TYPE>());
		}
	}
};

} // namespace duckdb
