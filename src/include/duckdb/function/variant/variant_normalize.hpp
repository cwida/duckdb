#pragma once

#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/serializer/varint.hpp"

namespace duckdb {

struct VariantNormalizerState {
public:
	VariantNormalizerState(idx_t result_row, VariantVectorData &source, OrderedOwningStringMap<uint32_t> &dictionary,
	                       SelectionVector &keys_selvec);

public:
	data_ptr_t GetDestination();
	uint32_t GetOrCreateIndex(const string_t &key);

public:
	uint32_t keys_size = 0;
	uint32_t children_size = 0;
	uint32_t values_size = 0;
	uint32_t blob_size = 0;

	VariantVectorData &source;
	OrderedOwningStringMap<uint32_t> &dictionary;
	SelectionVector &keys_selvec;

	uint64_t keys_offset;
	uint64_t children_offset;
	ValidityMask &keys_index_validity;

	data_ptr_t blob_data;
	uint8_t *type_ids;
	uint32_t *byte_offsets;
	uint32_t *values_indexes;
	uint32_t *keys_indexes;
};

struct VariantNormalizer {
	using result_type = void;

	static void VisitNull(VariantNormalizerState &state);
	static void VisitBoolean(bool val, VariantNormalizerState &state);
	static void VisitMetadata(VariantLogicalType type_id, VariantNormalizerState &state);

	template <typename T>
	static void VisitInteger(T val, VariantNormalizerState &state) {
		Store<T>(val, state.GetDestination());
		state.blob_size += sizeof(T);
	}
	static void VisitFloat(float val, VariantNormalizerState &state);
	static void VisitDouble(double val, VariantNormalizerState &state);
	static void VisitUUID(hugeint_t val, VariantNormalizerState &state);
	static void VisitDate(date_t val, VariantNormalizerState &state);
	static void VisitInterval(interval_t val, VariantNormalizerState &state);
	static void VisitTime(dtime_t val, VariantNormalizerState &state);
	static void VisitTimeNanos(dtime_ns_t val, VariantNormalizerState &state);
	static void VisitTimeTZ(dtime_tz_t val, VariantNormalizerState &state);
	static void VisitTimestampSec(timestamp_sec_t val, VariantNormalizerState &state);
	static void VisitTimestampMs(timestamp_ms_t val, VariantNormalizerState &state);
	static void VisitTimestamp(timestamp_t val, VariantNormalizerState &state);
	static void VisitTimestampNanos(timestamp_ns_t val, VariantNormalizerState &state);
	static void VisitTimestampTZ(timestamp_tz_t val, VariantNormalizerState &state);
	static void VisitTimestampTZNanos(timestamp_tz_ns_t val, VariantNormalizerState &state);

	static void VisitString(const string_t &str, VariantNormalizerState &state);
	static void VisitBlob(const string_t &blob, VariantNormalizerState &state);
	static void VisitBignum(const string_t &bignum, VariantNormalizerState &state);
	static void VisitGeometry(const string_t &geom, VariantNormalizerState &state);
	static void VisitBitstring(const string_t &bits, VariantNormalizerState &state);

	template <typename T>
	static void VisitDecimal(T val, uint32_t width, uint32_t scale, VariantNormalizerState &state) {
		state.blob_size += VarintEncode(width, state.GetDestination());
		state.blob_size += VarintEncode(scale, state.GetDestination());
		Store<T>(val, state.GetDestination());
		state.blob_size += sizeof(T);
	}

	static void VisitArray(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                       idx_t depth, VariantNormalizerState &state);
	static void VisitObject(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
	                        idx_t depth, VariantNormalizerState &state);
	static void VisitDefault(VariantLogicalType type_id, const_data_ptr_t, VariantNormalizerState &state);

public:
	static void Normalize(const Vector &input, Vector &output);
};

} // namespace duckdb
