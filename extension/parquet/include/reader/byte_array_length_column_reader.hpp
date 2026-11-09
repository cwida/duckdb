//===----------------------------------------------------------------------===//
//                         DuckDB
//
// reader/byte_array_length_column_reader.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include <stdint.h>
#include "parquet_arena.h"

#include "column_reader.hpp"
#include "duckdb/common/constants.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "resizable_buffer.hpp"

namespace duckdb {
class Vector;
struct SelectionVector;
struct ParquetColumnSchema;
class ParquetReader;

// Column reader that writes byte length of each value into a BIGINT vector.
class ByteArrayLengthColumnReader : public ColumnReader {
public:
	static constexpr const PhysicalType TYPE = PhysicalType::INT64;

	ByteArrayLengthColumnReader(const ParquetReader &reader, const ParquetColumnSchema &schema);

	idx_t fixed_width_string_length;

	unique_ptr<BaseStatistics> Stats(idx_t, const duckdb_parquet::ColumnChunkList &) override {
		return {};
	}

protected:
	void Plain(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset,
	           Vector &result) override {
		throw NotImplementedException("ByteArrayLengthColumnReader requires shared-buffer Plain overload");
	}
	void Plain(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values, idx_t result_offset,
	           Vector &result) override;
	void PlainSkip(ByteBuffer &plain_data, uint8_t *defines, idx_t num_values) override;
	void PlainSelect(shared_ptr<ResizeableBuffer> &plain_data, uint8_t *defines, idx_t num_values, Vector &result,
	                 const SelectionVector &sel, idx_t count) override;

	bool SupportsDirectFilter() const override {
		return true;
	}
	bool SupportsDirectSelect() const override {
		return true;
	}
};

struct ByteArrayLengthValueConversion {
	template <bool CHECKED>
	static int64_t PlainRead(ByteBuffer &plain_data, ColumnReader &reader) {
		auto &r = reader.Cast<ByteArrayLengthColumnReader>();
		const uint32_t len = r.fixed_width_string_length == DConstants::INVALID_INDEX ? plain_data.read<uint32_t>()
		                                                                              : r.fixed_width_string_length;
		plain_data.available(len);
		plain_data.inc(len);
		return static_cast<int64_t>(len);
	}
	template <bool CHECKED>
	static void PlainSkip(ByteBuffer &plain_data, ColumnReader &reader) {
		auto &r = reader.Cast<ByteArrayLengthColumnReader>();
		const uint32_t len = r.fixed_width_string_length == DConstants::INVALID_INDEX ? plain_data.read<uint32_t>()
		                                                                              : r.fixed_width_string_length;
		plain_data.inc(len);
	}
	static bool PlainAvailable(const ByteBuffer &, const idx_t) {
		return false;
	}
	static idx_t PlainConstantSize() {
		return 0;
	}
};

} // namespace duckdb
