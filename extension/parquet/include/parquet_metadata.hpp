//===----------------------------------------------------------------------===//
//                         DuckDB
//
// parquet_metadata.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "parquet_reader.hpp"
#include "duckdb/function/function_set.hpp"

namespace duckdb {

class ParquetMetaDataFunction : public TableFunction {
public:
	ParquetMetaDataFunction();
};

class ParquetSchemaFunction : public TableFunction {
public:
	ParquetSchemaFunction();
};

class ParquetKeyValueMetadataFunction : public TableFunction {
public:
	ParquetKeyValueMetadataFunction();
};

class ParquetFileMetadataFunction : public TableFunction {
public:
	ParquetFileMetadataFunction();
};

class ParquetBloomProbeFunction : public TableFunction {
public:
	ParquetBloomProbeFunction();
};

class ParquetFullMetadataFunction : public TableFunction {
public:
	ParquetFullMetadataFunction();
};

class ParquetPageIndexFunction : public TableFunction {
public:
	ParquetPageIndexFunction();
};

} // namespace duckdb
