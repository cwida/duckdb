#pragma once

#include "duckdb/function/cast/variant/to_variant_fwd.hpp"
#include "duckdb/common/types.hpp"

namespace duckdb {
namespace variant {

[[noreturn]] void ThrowEnumIndexOutOfRange(idx_t index, idx_t size);

namespace {

struct EmptyConversionPayloadToVariant {};

//! enum
struct EnumConversionPayload {
public:
	EnumConversionPayload(const string_t *values, idx_t size) : values(values), size(size) {
	}

public:
	const string_t &GetString(idx_t index) const {
		if (index >= size) {
			ThrowEnumIndexOutOfRange(index, size);
		}
		return values[index];
	}

public:
	const string_t *values;
	idx_t size;
};

//! decimal
struct DecimalConversionPayloadToVariant {
public:
	DecimalConversionPayloadToVariant(idx_t width, idx_t scale) : width(width), scale(scale) {
	}

public:
	idx_t width;
	idx_t scale;
};

} // namespace

template <typename T, VariantLogicalType TYPE_ID>
static VariantLogicalType GetTypeId(T val) {
	return TYPE_ID;
}

template <>
VariantLogicalType GetTypeId<bool, VariantLogicalType::BOOL_TRUE>(bool val) {
	return val ? VariantLogicalType::BOOL_TRUE : VariantLogicalType::BOOL_FALSE;
}

//! -------- Write the 'value' data for the Value --------

template <typename T>
static void WriteData(data_ptr_t ptr, const T &val, uint32_t lengths[3], EmptyConversionPayloadToVariant &) {
	Store(val, ptr);
}
template <>
void WriteData(data_ptr_t ptr, const bool &val, uint32_t lengths[3], EmptyConversionPayloadToVariant &) {
	return;
}
template <>
void WriteData(data_ptr_t ptr, const string_t &val, uint32_t lengths[3], EmptyConversionPayloadToVariant &) {
	auto str_length = val.GetSize();
	VarintEncode(str_length, ptr);
	memcpy(ptr + lengths[0], val.GetData(), str_length);
}

//! decimal

template <typename T>
static void WriteData(data_ptr_t ptr, const T &val, uint32_t lengths[3], DecimalConversionPayloadToVariant &payload) {
	InternalException::Throw("WriteData not implemented for this type");
}
template <>
void WriteData(data_ptr_t ptr, const int16_t &val, uint32_t lengths[3], DecimalConversionPayloadToVariant &payload) {
	VarintEncode(payload.width, ptr);
	VarintEncode(payload.scale, ptr + lengths[0]);
	Store(val, ptr + lengths[0] + lengths[1]);
}
template <>
void WriteData(data_ptr_t ptr, const int32_t &val, uint32_t lengths[3], DecimalConversionPayloadToVariant &payload) {
	VarintEncode(payload.width, ptr);
	VarintEncode(payload.scale, ptr + lengths[0]);
	Store(val, ptr + lengths[0] + lengths[1]);
}
template <>
void WriteData(data_ptr_t ptr, const int64_t &val, uint32_t lengths[3], DecimalConversionPayloadToVariant &payload) {
	VarintEncode(payload.width, ptr);
	VarintEncode(payload.scale, ptr + lengths[0]);
	Store(val, ptr + lengths[0] + lengths[1]);
}
template <>
void WriteData(data_ptr_t ptr, const hugeint_t &val, uint32_t lengths[3], DecimalConversionPayloadToVariant &payload) {
	VarintEncode(payload.width, ptr);
	VarintEncode(payload.scale, ptr + lengths[0]);
	Store(val, ptr + lengths[0] + lengths[1]);
}

//! enum

template <typename T>
static void WriteData(data_ptr_t ptr, const T &val, uint32_t lengths[3], EnumConversionPayload &payload) {
	InternalException::Throw("WriteData not implemented for this Enum physical type");
}
template <>
void WriteData(data_ptr_t ptr, const uint8_t &val, uint32_t lengths[3], EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	WriteData(ptr, str, lengths, empty_payload);
}
template <>
void WriteData(data_ptr_t ptr, const uint16_t &val, uint32_t lengths[3], EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	WriteData(ptr, str, lengths, empty_payload);
}
template <>
void WriteData(data_ptr_t ptr, const uint32_t &val, uint32_t lengths[3], EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	WriteData(ptr, str, lengths, empty_payload);
}

//! -------- Determine size of the 'value' data for the Value --------

template <typename T>
static void GetValueSize(const T &val, uint32_t lengths[3], idx_t &lengths_size, EmptyConversionPayloadToVariant &) {
	lengths[0] = sizeof(T);
	lengths_size = 1;
}
template <>
void GetValueSize(const bool &val, uint32_t lengths[3], idx_t &lengths_size, EmptyConversionPayloadToVariant &) {
}
template <>
void GetValueSize(const string_t &val, uint32_t lengths[3], idx_t &lengths_size, EmptyConversionPayloadToVariant &) {
	auto value_size = val.GetSize();
	lengths[0] = GetVarintSize(value_size);
	lengths[1] = static_cast<uint32_t>(value_size);
	lengths_size = 2;
}

//! decimal

template <typename T>
static void GetValueSize(const T &val, uint32_t lengths[3], idx_t &lengths_size,
                         DecimalConversionPayloadToVariant &payload) {
	InternalException::Throw("GetValueSize not implemented for this Decimal physical type");
}
template <>
void GetValueSize(const int16_t &val, uint32_t lengths[3], idx_t &lengths_size,
                  DecimalConversionPayloadToVariant &payload) {
	lengths[0] = GetVarintSize(payload.width);
	lengths[1] = GetVarintSize(payload.scale);
	lengths[2] = sizeof(int16_t);
	lengths_size = 3;
}
template <>
void GetValueSize(const int32_t &val, uint32_t lengths[3], idx_t &lengths_size,
                  DecimalConversionPayloadToVariant &payload) {
	lengths[0] = GetVarintSize(payload.width);
	lengths[1] = GetVarintSize(payload.scale);
	lengths[2] = sizeof(int32_t);
	lengths_size = 3;
}
template <>
void GetValueSize(const int64_t &val, uint32_t lengths[3], idx_t &lengths_size,
                  DecimalConversionPayloadToVariant &payload) {
	lengths[0] = GetVarintSize(payload.width);
	lengths[1] = GetVarintSize(payload.scale);
	lengths[2] = sizeof(int64_t);
	lengths_size = 3;
}
template <>
void GetValueSize(const hugeint_t &val, uint32_t lengths[3], idx_t &lengths_size,
                  DecimalConversionPayloadToVariant &payload) {
	lengths[0] = GetVarintSize(payload.width);
	lengths[1] = GetVarintSize(payload.scale);
	lengths[2] = sizeof(hugeint_t);
	lengths_size = 3;
}

//! enum

template <typename T>
static void GetValueSize(const T &val, uint32_t lengths[3], idx_t &lengths_size, EnumConversionPayload &payload) {
	InternalException::Throw("GetValueSize not implemented for this Enum physical type");
}
template <>
void GetValueSize(const uint8_t &val, uint32_t lengths[3], idx_t &lengths_size, EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	GetValueSize(str, lengths, lengths_size, empty_payload);
}
template <>
void GetValueSize(const uint16_t &val, uint32_t lengths[3], idx_t &lengths_size, EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	GetValueSize(str, lengths, lengths_size, empty_payload);
}
template <>
void GetValueSize(const uint32_t &val, uint32_t lengths[3], idx_t &lengths_size, EnumConversionPayload &payload) {
	EmptyConversionPayloadToVariant empty_payload;
	auto &str = payload.GetString(val);
	GetValueSize(str, lengths, lengths_size, empty_payload);
}

template <bool WRITE_DATA, bool IGNORE_NULLS, VariantLogicalType TYPE_ID, class T,
          class PAYLOAD_CLASS = EmptyConversionPayloadToVariant>
static bool ConvertPrimitiveTemplated(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                                      optional_ptr<const SelectionVector> selvec,
                                      optional_ptr<const SelectionVector> values_index_selvec, PAYLOAD_CLASS &payload,
                                      const bool is_root) {
	auto blob_offset_data = OffsetData::GetBlob(result.offsets);
	auto values_offset_data = OffsetData::GetValues(result.offsets);

	auto &source_format = source.source_format;
	auto &source_validity = source_format.validity;
	auto source_data = source_format.GetData<T>(source_format);

	auto &variant = result.variant;
	uint32_t lengths[3];
	for (idx_t i = 0; i < count; i++) {
		auto index = source[i];
		// the blob space to reserve for this row - nothing for NULL rows
		idx_t lengths_size = 0;

		auto result_index = selvec ? selvec->get_index(i) : i;
		auto &blob_offset = blob_offset_data[result_index];

		if (TYPE_ID != VariantLogicalType::VARIANT_NULL && source_validity.RowIsValid(index)) {
			//! Write the value
			auto &val = source_data[index];
			GetValueSize<T>(val, lengths, lengths_size, payload);
			WriteVariantMetadata<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, values_index_selvec,
			                                 i, GetTypeId<T, TYPE_ID>(val));
			if (WRITE_DATA) {
				auto &blob_value = variant.blob_data[result_index];
				auto blob_value_data = data_ptr_cast(blob_value.GetDataWriteable());
				WriteData<T>(blob_value_data + blob_offset, val, lengths, payload);
			}
		} else if (!IGNORE_NULLS) {
			HandleVariantNull<WRITE_DATA>(result, result_index, values_offset_data, blob_offset, values_index_selvec, i,
			                              is_root);
		}

		for (idx_t j = 0; j < lengths_size; j++) {
			OffsetData::AddBlobOffset(blob_offset, lengths[j]);
		}
	}
	return true;
}

template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertPrimitiveToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                               optional_ptr<const SelectionVector> selvec,
                               optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	auto &type = source.vec.GetType();
	auto logical_type = type.id();
	auto physical_type = type.InternalType();

	EmptyConversionPayloadToVariant empty_payload;
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::VARIANT_NULL, int32_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::BOOLEAN:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::BOOL_TRUE, bool>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TINYINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INT8, int8_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::UTINYINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UINT8, uint8_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::SMALLINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INT16, int16_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::USMALLINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UINT16, uint16_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::INTEGER:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INT32, int32_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::UINTEGER:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UINT32, uint32_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::BIGINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INT64, int64_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::UBIGINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UINT64, uint64_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::HUGEINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INT128, hugeint_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::UHUGEINT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UINT128, uhugeint_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::DATE:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DATE, date_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIME:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIME_MICROS, dtime_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIME_NS:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIME_NANOS, dtime_ns_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_MICROS, timestamp_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP_SEC:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_SEC, timestamp_sec_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP_NS:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_NANOS, timestamp_ns_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP_MS:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_MILIS, timestamp_ms_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIME_TZ:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIME_MICROS_TZ, dtime_tz_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP_TZ:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_MICROS_TZ,
		                                 timestamp_tz_t>(source, result, count, selvec, values_index_selvec,
		                                                 empty_payload, is_root);
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::TIMESTAMP_NANOS_TZ,
		                                 timestamp_tz_ns_t>(source, result, count, selvec, values_index_selvec,
		                                                    empty_payload, is_root);
	case LogicalTypeId::UUID:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::UUID, hugeint_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::FLOAT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::FLOAT, float>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::DOUBLE:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DOUBLE, double>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::DECIMAL: {
		uint8_t width;
		uint8_t scale;
		type.GetDecimalProperties(width, scale);
		DecimalConversionPayloadToVariant payload(width, scale);

		switch (physical_type) {
		case PhysicalType::INT16:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DECIMAL, int16_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		case PhysicalType::INT32:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DECIMAL, int32_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		case PhysicalType::INT64:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DECIMAL, int64_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		case PhysicalType::INT128:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::DECIMAL, hugeint_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		default:
			NotImplementedException::Throw("Can't convert DECIMAL value of physical type: %s", physical_type);
		};
	}
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::CHAR:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::VARCHAR, string_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::GEOMETRY:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::GEOMETRY, string_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::BLOB:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::BLOB, string_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::INTERVAL:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::INTERVAL, interval_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::ENUM: {
		auto &enum_values = EnumType::GetValuesInsertOrder(type);
		auto dict_size = EnumType::GetSize(type);
		D_ASSERT(enum_values.GetVectorType() == VectorType::FLAT_VECTOR);
		EnumConversionPayload payload(FlatVector::GetData<string_t>(enum_values), dict_size);
		switch (physical_type) {
		case PhysicalType::UINT8:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::VARCHAR, uint8_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		case PhysicalType::UINT16:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::VARCHAR, uint16_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		case PhysicalType::UINT32:
			return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::VARCHAR, uint32_t>(
			    source, result, count, selvec, values_index_selvec, payload, is_root);
		default:
			NotImplementedException::Throw("ENUM conversion for PhysicalType (%s) not supported", physical_type);
		}
	}
	case LogicalTypeId::BIGNUM:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::BIGNUM, string_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	case LogicalTypeId::BIT:
		return ConvertPrimitiveTemplated<WRITE_DATA, IGNORE_NULLS, VariantLogicalType::BITSTRING, string_t>(
		    source, result, count, selvec, values_index_selvec, empty_payload, is_root);
	default:
		throw NotImplementedException("Invalid LogicalType (%s) for ConvertToVariant",
		                              EnumUtil::ToString(logical_type));
	}
}

} // namespace variant
} // namespace duckdb
