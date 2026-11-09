//===----------------------------------------------------------------------===//
//                         DuckDB
//
// writer/parquet_cdc_chunker.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/common.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "writer/parquet_cdc_gear_tables.hpp"

namespace duckdb {

//! Content-defined page boundaries: instead of starting a new data page only when the previous
//! one filled up, a page also ends wherever a rolling hash over the column's values says so.
//! A page's content then no longer depends on where the page before it happened to end, so
//! inserting or deleting a row only rewrites the pages it touches. For an upstream store that
//! chunks by content that is the difference between rewriting a row group and rewriting nothing.
//!
//! This is a port of parquet::internal::ContentDefinedChunker from Apache Arrow C++ (cpp/src/
//! parquet/chunker_internal.cc), kept close on purpose: same tables, same 64-bit gear hash, same mask
//! arithmetic, same eight-table run, same minimum-size window. A column written here and the same
//! column written by arrow-cpp or arrow-rs with the same sizes therefore cut between the same
//! values. Like Arrow's, the state lives for the whole file and carries over from one row group to the next.
//! The positions this produces depend on the order values arrive in, so a reproducible file needs
//! deterministic input order - an ordered copy, which means an explicit ORDER BY when the plan has no order.
//!
//! The hash is fed the column's values in Arrow's canonical form - the value's bytes, plus the two
//! bytes of its definition level when the column is nullable - so a null is content too, exactly as
//! it is there. Only the types in ParquetCdcSupportedType get that treatment; see that function for
//! why the list stops where it does.

//! Default for the minimum page size, matching Arrow's parquet::WriterProperties defaults
static constexpr idx_t PARQUET_CDC_DEFAULT_MIN_CHUNK_SIZE = 256ULL * 1024ULL;
//! Default for the maximum page size, matching Arrow's defaults
static constexpr idx_t PARQUET_CDC_DEFAULT_MAX_CHUNK_SIZE = 1024ULL * 1024ULL;
//! Default normalisation level, matching Arrow's default. Raising it makes boundaries rarer and
//! pages more uniform; Arrow recommends leaving it alone on spinning disk, raising it to 1-3 on
//! systems that read many files concurrently.
static constexpr int32_t PARQUET_CDC_DEFAULT_NORM_LEVEL = 0;

//! The boundary half of Arrow's content defined chunker: rolling hash, mask, eight table run. The
//! other half there - remembering which value index each chunk starts at - is what the
//! ColumnChunkWriter does with the answer, which in DuckDB is what PrimitiveColumnWriter already
//! does with its cut_page flag, so it is not duplicated here.
class ParquetContentDefinedChunker {
public:
	struct Options {
		idx_t min_chunk_size = PARQUET_CDC_DEFAULT_MIN_CHUNK_SIZE;
		idx_t max_chunk_size = PARQUET_CDC_DEFAULT_MAX_CHUNK_SIZE;
		int32_t norm_level = PARQUET_CDC_DEFAULT_NORM_LEVEL;
	};

	//! floor(log2(value)), and 0 for value 0 - which is what Arrow's bit_width(target) - 1 comes out to.
	static constexpr idx_t FloorLog2(idx_t value) {
		idx_t bits = 0;
		while (value > 1) {
			value >>= 1;
			bits++;
		}
		return bits;
	}

	//! The arithmetic behind CalculateMask, constexpr so it can be asserted against Arrow's numbers. Assumes
	//! max_chunk_size > min_chunk_size, which CalculateMask checks before calling.
	static constexpr int64_t MaskBitsFor(idx_t min_chunk_size, idx_t max_chunk_size, int32_t norm_level) {
		idx_t avg_chunk_size = (min_chunk_size + max_chunk_size) / 2;
		idx_t target_chunk_size = (avg_chunk_size - min_chunk_size) / PARQUET_CDC_NUM_GEAR_TABLES;
		return static_cast<int64_t>(FloorLog2(target_chunk_size)) - norm_level;
	}

	//! Arrow's CalculateMask: aim the predicate at ((min + max) / 2 - min) / 8 bytes, then keep that many
	//! low bits off the mask. With the defaults this leaves the top 15 bits of 64, i.e. a hit every 32768
	//! hashed bytes per table. Throws if the numbers cannot be turned into a usable mask; Arrow throws the
	//! same way.
	static uint64_t CalculateMask(idx_t min_chunk_size, idx_t max_chunk_size, int32_t norm_level) {
		if (max_chunk_size <= min_chunk_size) {
			throw BinderException("content_defined_chunking: max_chunk_size (%llu) must be greater than "
			                      "min_chunk_size (%llu)",
			                      max_chunk_size, min_chunk_size);
		}
		int64_t effective_bits = MaskBitsFor(min_chunk_size, max_chunk_size, norm_level);
		if (effective_bits < 1 || effective_bits > 63) {
			throw BinderException("content_defined_chunking: the number of bits in the CDC mask must be between 1 and "
			                      "63, got %d - adjust min_chunk_size, max_chunk_size or norm_level",
			                      effective_bits);
		}
		return ~uint64_t(0) << (64 - uint64_t(effective_bits));
	}

	//! Start a fresh run at the given sizes. Called once per column per file.
	void Reset(const Options &options_p) {
		options = options_p;
		rolling_hash_mask = CalculateMask(options.min_chunk_size, options.max_chunk_size, options.norm_level);
		rolling_hash = 0;
		chunk_size = 0;
		nth_run = 0;
		has_matched = false;
	}

	//! Arrow's ContentDefinedChunker::Impl::Roll, byte for byte: the bytes of a value count before
	//! the minimum-size gate is consulted, so a value that straddles the gate is hashed whole or
	//! not at all, exactly as it is there.
	void RollBytes(const_data_ptr_t data, idx_t len) {
		chunk_size += len;
		if (chunk_size < options.min_chunk_size) {
			return;
		}
		auto ptr = data;
		for (idx_t i = 0; i < len; i++) {
			rolling_hash = (rolling_hash << 1) + PARQUET_CDC_GEAR_TABLES[nth_run][ptr[i]];
			has_matched = has_matched || ((rolling_hash & rolling_hash_mask) == 0);
		}
	}

	//! Arrow's Roll<T>: the value's own bytes, in the machine's byte order, as they are there too.
	//! Parity therefore assumes both writers run on the same endianness, which Arrow's own
	//! implementation also does.
	template <typename T>
	void RollFixedWidth(const T &value) {
		RollBytes(const_data_ptr_cast(&value), sizeof(T));
	}

	//! Arrow's Roll(&def_level): a nullable column hashes its definition level before its value, so
	//! nulls move the hash and null density shifts boundaries.
	void RollLevel(uint16_t level) {
		RollFixedWidth(level);
	}

	//! Arrow's NeedNewChunk: a mask hit only advances the run, and the run has to survive
	//! PARQUET_CDC_NUM_GEAR_TABLES consecutive hits before a boundary is real - the part that makes
	//! a hit a boundary instead of a coincidence. After that many hits the run is reset, which is
	//! what makes pages more even than a single-mask scheme. The maximum is a hard cut, with the
	//! hash kept, and the minimum is honoured by the gate inside RollBytes.
	bool NeedNewChunk() {
		if (has_matched) {
			has_matched = false;
			nth_run++;
			if (nth_run >= PARQUET_CDC_NUM_GEAR_TABLES) {
				nth_run = 0;
				chunk_size = 0;
				return true;
			}
		}
		if (chunk_size >= options.max_chunk_size) {
			chunk_size = 0;
			return true;
		}
		return false;
	}

private:
	Options options;
	//! The top N bits of 64 that a hit is made of, see CalculateMask
	uint64_t rolling_hash_mask = 0;
	//! The rolling hash over the bytes fed so far, see RollBytes
	uint64_t rolling_hash = 0;
	//! Uncompressed value bytes fed since the last cut
	idx_t chunk_size = 0;
	//! How many consecutive tables' hits are still owed before the next boundary lands
	uint8_t nth_run = 0;
	//! Whether the previous value hit the mask
	bool has_matched = false;
};

//! Arrow's own unit test pins the mask implied by its default sizes to the top 15 bits of the 64 bit hash. A
//! port that does not reproduce that number is not running the same algorithm, so this is the cheapest piece of
//! cross-implementation conformance there is - and it is the reason MaskBitsFor is constexpr.
static_assert(ParquetContentDefinedChunker::MaskBitsFor(PARQUET_CDC_DEFAULT_MIN_CHUNK_SIZE,
                                                        PARQUET_CDC_DEFAULT_MAX_CHUNK_SIZE,
                                                        PARQUET_CDC_DEFAULT_NORM_LEVEL) == 15,
              "Arrow's default chunk sizes must imply a 15 bit mask");

//! The types whose bytes DuckDB and an Arrow writer are guaranteed to agree on, which is what makes
//! boundaries comparable across clients: the fixed width integers, floats, dates and timestamps of
//! the same unit, and the bytes of strings and blobs with no length prefix.
//!
//! Everything else is left out because its canonical bytes are DuckDB's own, and cutting at content
//! with bytes no one else hashes would produce boundaries that look comparable and are not: DECIMAL
//! (Arrow's chunker reads decimal128 arrays, sixteen bytes, where DuckDB stores a DECIMAL(9,2) in
//! four), ENUM (the dictionary codes, whose numbering is DuckDB's), HUGEINT, UUID (Arrow's is a
//! sixteen byte fixed size binary), INTERVAL and BIT (no Arrow equivalent), TIME (Arrow's is 32 or
//! 64 bits depending on unit while DuckDB's is always 64), and everything nested, which Arrow cuts
//! at repetition level boundaries rather than per value and which is not handled here at all.
inline bool ParquetCdcSupportedType(const LogicalType &type) {
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
		return true;
	default:
		return false;
	}
}

//! Feed one value to the chunker: the value's bytes, nothing else. Nulls are handled by the caller,
//! which feeds the definition level the way Arrow does and no value bytes for them.
inline void ParquetCdcHashValue(ParquetContentDefinedChunker &chunker, Vector &vector, idx_t index) {
	switch (vector.GetType().id()) {
	case LogicalTypeId::BOOLEAN:
		chunker.RollFixedWidth(FlatVector::GetValue<bool>(vector, index));
		return;
	case LogicalTypeId::TINYINT:
		chunker.RollFixedWidth(FlatVector::GetValue<int8_t>(vector, index));
		return;
	case LogicalTypeId::SMALLINT:
		chunker.RollFixedWidth(FlatVector::GetValue<int16_t>(vector, index));
		return;
	case LogicalTypeId::INTEGER:
		chunker.RollFixedWidth(FlatVector::GetValue<int32_t>(vector, index));
		return;
	case LogicalTypeId::BIGINT:
		chunker.RollFixedWidth(FlatVector::GetValue<int64_t>(vector, index));
		return;
	case LogicalTypeId::UTINYINT:
		chunker.RollFixedWidth(FlatVector::GetValue<uint8_t>(vector, index));
		return;
	case LogicalTypeId::USMALLINT:
		chunker.RollFixedWidth(FlatVector::GetValue<uint16_t>(vector, index));
		return;
	case LogicalTypeId::UINTEGER:
		chunker.RollFixedWidth(FlatVector::GetValue<uint32_t>(vector, index));
		return;
	case LogicalTypeId::UBIGINT:
		chunker.RollFixedWidth(FlatVector::GetValue<uint64_t>(vector, index));
		return;
	case LogicalTypeId::FLOAT:
		chunker.RollFixedWidth(FlatVector::GetValue<float>(vector, index));
		return;
	case LogicalTypeId::DOUBLE:
		chunker.RollFixedWidth(FlatVector::GetValue<double>(vector, index));
		return;
	case LogicalTypeId::DATE:
		chunker.RollFixedWidth(FlatVector::GetValue<date_t>(vector, index));
		return;
	case LogicalTypeId::TIMESTAMP:
		chunker.RollFixedWidth(FlatVector::GetValue<timestamp_t>(vector, index));
		return;
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_TZ:
		// all of these are a single int64 under the hood, in their own unit, like Arrow's
		chunker.RollFixedWidth(FlatVector::GetValue<int64_t>(vector, index));
		return;
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB: {
		const auto &str = FlatVector::GetData<string_t>(vector)[index];
		chunker.RollBytes(const_data_ptr_cast(str.GetData()), str.GetSize());
		return;
	}
	default:
		throw InternalException("ParquetCdcHashValue called for unsupported type %s", vector.GetType().ToString());
	}
}

} // namespace duckdb
