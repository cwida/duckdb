//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/result_unit.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/winapi.hpp"

namespace duckdb {

//! A piece of query output: what the buffer holds and what the consumer pops. Its concrete type is
//! fixed by the format that produced it, which the consumer checks by name before casting
class ResultUnit {
public:
	DUCKDB_API ResultUnit(idx_t row_count, idx_t byte_size);
	DUCKDB_API virtual ~ResultUnit();

public:
	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}

	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}

public:
	//! The rows in this unit
	const idx_t row_count;
	//! The bytes this unit counts against the buffer's cap
	const idx_t byte_size;

private:
	ResultUnit(const ResultUnit &) = delete;
	ResultUnit &operator=(const ResultUnit &) = delete;
};

//! A unit holding a single buffered chunk.
class ChunkUnit : public ResultUnit {
public:
	DUCKDB_API explicit ChunkUnit(unique_ptr<DataChunk> chunk);

public:
	unique_ptr<DataChunk> chunk;
};

} // namespace duckdb
