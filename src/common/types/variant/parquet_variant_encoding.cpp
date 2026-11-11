#include <algorithm>
#include <stdint.h>
#include <string.h>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#include "duckdb/common/types/variant/parquet_variant_iterator.hpp"
#include "duckdb/common/types/variant/variant_binary_decoder.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/variant.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/assert.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/hugeint.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/types/selection_vector.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/validity_mask.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/vector/unified_vector_format.hpp"

namespace duckdb {

static idx_t CalculateByteLength(idx_t value) {
	if (value == 0) {
		return 1;
	}
	auto value_data = reinterpret_cast<data_ptr_t>(&value);
	idx_t irrelevant_bytes = 0;
	//! Check how many of the most significant bytes are 0
	for (idx_t i = sizeof(idx_t); i > 0 && value_data[i - 1] == 0; i--) {
		irrelevant_bytes++;
	}
	return sizeof(idx_t) - irrelevant_bytes;
}

static uint8_t EncodeMetadataHeader(idx_t byte_length) {
	D_ASSERT(byte_length <= 4);

	uint8_t header_byte = 0;
	//! Set 'version' to 1
	header_byte |= static_cast<uint8_t>(1);
	//! NOTE: keep 'sorted_strings' at 0, we don't always sort the key strings
	//! Set 'offset_size_minus_one' to byte_length-1
	header_byte |= (static_cast<uint8_t>(byte_length) - 1) << 6;

#ifdef DEBUG
	auto decoded_header = VariantMetadataHeader::FromHeaderByte(header_byte);
	D_ASSERT(decoded_header.offset_size == byte_length);
#endif

	return header_byte;
}

static void CreateMetadata(UnifiedVariantVectorData &variant, Vector &metadata, idx_t count) {
	//! NOTE: the parquet variant is limited to a max dictionary size of NumericLimits<uint32_t>::Maximum()
	//! Whereas we can have NumericLimits<uint32_t>::Maximum() *per* string in DuckDB
	auto metadata_data = FlatVector::GetDataMutable<string_t>(metadata);
	for (idx_t row = 0; row < count; row++) {
		uint64_t dictionary_count = 0;
		if (variant.RowIsValid(row)) {
			dictionary_count = variant.GetKeysCount(row);
		}
		idx_t dictionary_size = 0;
		for (idx_t i = 0; i < dictionary_count; i++) {
			auto &key = variant.GetKey(row, i);
			dictionary_size += key.GetSize();
		}
		if (dictionary_size >= NumericLimits<uint32_t>::Maximum()) {
			throw InvalidInputException("The total length of the dictionary exceeds a 4 byte value (uint32_t), failed "
			                            "to export VARIANT to Parquet");
		}

		auto byte_length = CalculateByteLength(dictionary_size);
		auto total_length = 1 + (byte_length * (dictionary_count + 2)) + dictionary_size;

		metadata_data[row] = StringVector::EmptyString(metadata, total_length);
		auto &metadata_blob = metadata_data[row];
		auto metadata_blob_data = metadata_blob.GetDataWriteable();

		metadata_blob_data[0] = static_cast<char>(EncodeMetadataHeader(byte_length));
		memcpy(metadata_blob_data + 1, const_data_ptr_cast(&dictionary_count), byte_length);

		auto offset_ptr = metadata_blob_data + 1 + byte_length;
		auto string_ptr =
		    metadata_blob_data + 1 + byte_length + (NumericCast<idx_t>(dictionary_count + 1) * byte_length);
		idx_t total_offset = 0;
		for (idx_t i = 0; i < dictionary_count; i++) {
			memcpy(offset_ptr + (i * byte_length), const_data_ptr_cast(&total_offset), byte_length);
			auto &key = variant.GetKey(row, i);

			memcpy(string_ptr + total_offset, key.GetData(), key.GetSize());
			total_offset += key.GetSize();
		}
		memcpy(offset_ptr + (NumericCast<idx_t>(dictionary_count) * byte_length), const_data_ptr_cast(&total_offset),
		       byte_length);
		D_ASSERT(offset_ptr + (NumericCast<idx_t>(dictionary_count + 1) * byte_length) == string_ptr);
		D_ASSERT(string_ptr + total_offset == metadata_blob_data + total_length);
		metadata_blob.SetSizeAndFinalize(NumericCast<uint32_t>(total_length), total_length);

#ifdef DEBUG
		auto decoded_metadata = VariantMetadata(metadata_blob);
		D_ASSERT(decoded_metadata.strings.size() == dictionary_count);
		for (idx_t i = 0; i < dictionary_count; i++) {
			D_ASSERT(decoded_metadata.strings[i] == variant.GetKey(row, i).GetString());
		}
#endif
	}
}

namespace {

static unordered_set<VariantLogicalType> GetVariantType(const LogicalType &type) {
	if (type.id() == LogicalTypeId::ANY) {
		return {};
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
		return {VariantLogicalType::OBJECT};
	case LogicalTypeId::LIST:
		return {VariantLogicalType::ARRAY};
	case LogicalTypeId::BOOLEAN:
		return {VariantLogicalType::BOOL_TRUE, VariantLogicalType::BOOL_FALSE};
	case LogicalTypeId::TINYINT:
		return {VariantLogicalType::INT8};
	case LogicalTypeId::SMALLINT:
		return {VariantLogicalType::INT16};
	case LogicalTypeId::INTEGER:
		return {VariantLogicalType::INT32};
	case LogicalTypeId::BIGINT:
		return {VariantLogicalType::INT64};
	case LogicalTypeId::FLOAT:
		return {VariantLogicalType::FLOAT};
	case LogicalTypeId::DOUBLE:
		return {VariantLogicalType::DOUBLE};
	case LogicalTypeId::DECIMAL:
		return {VariantLogicalType::DECIMAL};
	case LogicalTypeId::DATE:
		return {VariantLogicalType::DATE};
	case LogicalTypeId::TIME:
		return {VariantLogicalType::TIME_MICROS};
	case LogicalTypeId::TIMESTAMP_TZ:
		return {VariantLogicalType::TIMESTAMP_MICROS_TZ};
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		return {VariantLogicalType::TIMESTAMP_NANOS_TZ};
	case LogicalTypeId::TIMESTAMP:
		return {VariantLogicalType::TIMESTAMP_MICROS};
	case LogicalTypeId::TIMESTAMP_NS:
		return {VariantLogicalType::TIMESTAMP_NANOS};
	case LogicalTypeId::BLOB:
		return {VariantLogicalType::BLOB};
	case LogicalTypeId::VARCHAR:
		return {VariantLogicalType::VARCHAR};
	case LogicalTypeId::UUID:
		return {VariantLogicalType::UUID};
	default:
		throw BinderException("Type '%s' can't be translated to a VARIANT type", type.ToString());
	}
}

struct ParquetVariantShreddingState : public VariantShreddingState {
public:
	ParquetVariantShreddingState(const LogicalType &type, idx_t total_count)
	    : VariantShreddingState(type, total_count), variant_types(GetVariantType(type)) {
	}

public:
	const unordered_set<VariantLogicalType> &GetVariantTypes() override {
		return variant_types;
	}

private:
	unordered_set<VariantLogicalType> variant_types;
};

struct ParquetVariantShredding : public VariantShredding {
	ParquetVariantShredding() {
		// for parquet untyped ("value") comes before typed ("typed_value")
		untyped_value_index = 0;
		typed_value_index = 1;
	}

	void WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result, optional_ptr<const SelectionVector> sel,
	                        optional_ptr<const SelectionVector> value_index_sel,
	                        optional_ptr<const SelectionVector> result_sel, idx_t count) override;

protected:
	void WriteMissingField(Vector &vector, idx_t index) override;
};

} // namespace

vector<idx_t> GetChildIndices(const UnifiedVariantVectorData &variant, idx_t row, const VariantNestedData &nested_data,
                              optional_ptr<ParquetVariantShreddingState> shredding_state) {
	vector<idx_t> child_indices;
	if (!shredding_state || shredding_state->type.id() != LogicalTypeId::STRUCT) {
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			child_indices.push_back(i);
		}
	} else {
		auto shredded_fields = shredding_state->ObjectFields();

		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto keys_index = variant.GetKeysIndex(row, i + nested_data.children_idx);
			auto &key = variant.GetKey(row, keys_index);

			if (shredded_fields.count(key.GetString())) {
				//! This field is shredded on, omit it from the value
				continue;
			}
			child_indices.push_back(i);
		}
	}
	// Object field IDs and offsets must follow UTF-8 field name order, even with an unsorted dictionary.
	std::sort(child_indices.begin(), child_indices.end(), [&](idx_t left, idx_t right) {
		auto left_key = variant.GetKeysIndex(row, left + nested_data.children_idx);
		auto right_key = variant.GetKeysIndex(row, right + nested_data.children_idx);
		return variant.GetKey(row, left_key) < variant.GetKey(row, right_key);
	});
	return child_indices;
}

//! The size of a DECIMAL(38, 0) encoded value: the scale byte and the 16 byte payload
static constexpr idx_t WIDE_INTEGER_DECIMAL_SIZE = 1 + sizeof(hugeint_t);

//! INT128, UINT64 and UINT128 are written as an INT64 if their value fits one, and as a DECIMAL(38, 0) otherwise - the
//! widest integer the Parquet VARIANT encoding has
static bool WideIntegerFitsInt64(hugeint_t value) {
	return value >= hugeint_t(NumericLimits<int64_t>::Minimum()) &&
	       value <= hugeint_t(NumericLimits<int64_t>::Maximum());
}

[[noreturn]] static void ThrowWideIntegerTooLarge(VariantLogicalType type_id) {
	throw InvalidInputException(
	    "Can't convert VARIANT of type '%s' to Parquet VARIANT - the value does not fit a DECIMAL(38, 0)",
	    EnumUtil::ToString(type_id));
}

//! Loads the value of a wide integer, throwing if it does not fit a DECIMAL(38, 0)
static hugeint_t GetWideIntegerValue(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index,
                                     VariantLogicalType type_id) {
	auto ptr = const_data_ptr_cast(variant.GetData(row).GetData()) + variant.GetByteOffset(row, values_index);
	hugeint_t value;
	switch (type_id) {
	case VariantLogicalType::UINT64:
		value = hugeint_t(0, Load<uint64_t>(ptr));
		break;
	case VariantLogicalType::UINT128: {
		auto unsigned_value = Load<uhugeint_t>(ptr);
		if (unsigned_value.upper > static_cast<uint64_t>(NumericLimits<int64_t>::Maximum())) {
			// the value is above 2^127 - 1, so it does not fit a hugeint_t at all
			ThrowWideIntegerTooLarge(type_id);
		}
		value = hugeint_t(static_cast<int64_t>(unsigned_value.upper), unsigned_value.lower);
		break;
	}
	default:
		D_ASSERT(type_id == VariantLogicalType::INT128);
		value = Load<hugeint_t>(ptr);
		break;
	}
	const auto &limit = Hugeint::POWERS_OF_TEN[Decimal::MAX_WIDTH_DECIMAL];
	if (value >= limit || value <= -limit) {
		ThrowWideIntegerTooLarge(type_id);
	}
	return value;
}

static idx_t AnalyzeValueData(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index,
                              vector<uint32_t> &offsets, optional_ptr<ParquetVariantShreddingState> shredding_state) {
	idx_t total_size = 0;
	//! Every value has at least a value header
	total_size++;

	idx_t offset_size = offsets.size();
	VariantLogicalType type_id = VariantLogicalType::VARIANT_NULL;
	if (variant.RowIsValid(row)) {
		type_id = variant.GetTypeId(row, values_index);
	}
	switch (type_id) {
	case VariantLogicalType::OBJECT: {
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);

		//! Calculate value and key offsets for all children
		idx_t total_offset = 0;
		uint32_t highest_keys_index = 0;

		auto child_indices = GetChildIndices(variant, row, nested_data, shredding_state);
		if (nested_data.child_count && child_indices.empty()) {
			//! All fields of the object are shredded, omit the object entirely
			return 0;
		}

		auto num_elements = child_indices.size();
		offsets.resize(offset_size + num_elements + 1);

		for (idx_t entry = 0; entry < child_indices.size(); entry++) {
			auto i = child_indices[entry];
			auto keys_index = variant.GetKeysIndex(row, i + nested_data.children_idx);
			auto values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);
			offsets[offset_size + entry] = NumericCast<uint32_t>(total_offset);

			total_offset += AnalyzeValueData(variant, row, values_index, offsets, nullptr);
			highest_keys_index = MaxValue(highest_keys_index, keys_index);
		}
		offsets[offset_size + num_elements] = NumericCast<uint32_t>(total_offset);

		//! Calculate the sizes for the objects value data
		auto field_id_size = CalculateByteLength(highest_keys_index);
		auto field_offset_size = CalculateByteLength(total_offset);
		const bool is_large = num_elements > NumericLimits<uint8_t>::Maximum();

		//! Now add the sizes for the objects value data
		if (is_large) {
			total_size += sizeof(uint32_t);
		} else {
			total_size += sizeof(uint8_t);
		}
		total_size += num_elements * field_id_size;
		total_size += (num_elements + 1) * field_offset_size;
		total_size += total_offset;
		break;
	}
	case VariantLogicalType::ARRAY: {
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);

		idx_t total_offset = 0;
		offsets.resize(offset_size + nested_data.child_count + 1);
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);
			offsets[offset_size + i] = NumericCast<uint32_t>(total_offset);

			total_offset += AnalyzeValueData(variant, row, values_index, offsets, nullptr);
		}
		offsets[offset_size + nested_data.child_count] = NumericCast<uint32_t>(total_offset);

		auto field_offset_size = CalculateByteLength(total_offset);
		auto num_elements = nested_data.child_count;
		const bool is_large = num_elements > NumericLimits<uint8_t>::Maximum();

		if (is_large) {
			total_size += sizeof(uint32_t);
		} else {
			total_size += sizeof(uint8_t);
		}
		total_size += (num_elements + 1) * field_offset_size;
		total_size += total_offset;
		break;
	}
	case VariantLogicalType::BLOB:
	case VariantLogicalType::VARCHAR: {
		auto string_value = VariantUtils::DecodeStringData(variant, row, values_index);
		total_size += string_value.GetSize();
		if (type_id == VariantLogicalType::BLOB || string_value.GetSize() >= 64) {
			//! Save as regular string value
			total_size += sizeof(uint32_t);
		}
		break;
	}
	case VariantLogicalType::VARIANT_NULL:
	case VariantLogicalType::BOOL_TRUE:
	case VariantLogicalType::BOOL_FALSE:
		break;
	case VariantLogicalType::INT8:
		total_size += sizeof(uint8_t);
		break;
	case VariantLogicalType::INT16:
		total_size += sizeof(uint16_t);
		break;
	case VariantLogicalType::INT32:
		total_size += sizeof(uint32_t);
		break;
	case VariantLogicalType::INT64:
		total_size += sizeof(uint64_t);
		break;
	case VariantLogicalType::FLOAT:
		total_size += sizeof(float);
		break;
	case VariantLogicalType::DOUBLE:
		total_size += sizeof(double);
		break;
	case VariantLogicalType::DECIMAL: {
		auto decimal_data = VariantUtils::DecodeDecimalData(variant, row, values_index);
		total_size += 1;
		if (decimal_data.width <= 9) {
			total_size += sizeof(int32_t);
		} else if (decimal_data.width <= 18) {
			total_size += sizeof(int64_t);
		} else if (decimal_data.width <= 38) {
			total_size += sizeof(uhugeint_t);
		} else {
			throw InvalidInputException("Can't convert VARIANT DECIMAL(%d, %d) to Parquet VARIANT", decimal_data.width,
			                            decimal_data.scale);
		}
		break;
	}
	case VariantLogicalType::UUID:
		total_size += sizeof(uhugeint_t);
		break;
	case VariantLogicalType::DATE:
		total_size += sizeof(uint32_t);
		break;
	case VariantLogicalType::TIME_MICROS:
	case VariantLogicalType::TIMESTAMP_MICROS:
	case VariantLogicalType::TIMESTAMP_NANOS:
	case VariantLogicalType::TIMESTAMP_MICROS_TZ:
		total_size += sizeof(uint64_t);
		break;
	case VariantLogicalType::UINT8:
		// store as int16_t
		total_size += sizeof(int16_t);
		break;
	case VariantLogicalType::UINT16:
		// store as int32_t
		total_size += sizeof(int32_t);
		break;
	case VariantLogicalType::UINT32:
		// store as int64_t
		total_size += sizeof(int64_t);
		break;
	case VariantLogicalType::UINT64:
	case VariantLogicalType::UINT128:
	case VariantLogicalType::INT128: {
		auto value = GetWideIntegerValue(variant, row, values_index, type_id);
		total_size += WideIntegerFitsInt64(value) ? sizeof(int64_t) : WIDE_INTEGER_DECIMAL_SIZE;
		break;
	}
	case VariantLogicalType::INTERVAL:
	case VariantLogicalType::BIGNUM:
	case VariantLogicalType::BITSTRING:
	case VariantLogicalType::TIMESTAMP_MILIS:
	case VariantLogicalType::TIMESTAMP_SEC:
	case VariantLogicalType::TIME_MICROS_TZ:
	case VariantLogicalType::TIME_NANOS:
	default:
		throw InvalidInputException("Can't convert VARIANT of type '%s' to Parquet VARIANT",
		                            EnumUtil::ToString(type_id));
	}

	return total_size;
}

template <VariantPrimitiveType TYPE_ID>
void WritePrimitiveTypeHeader(data_ptr_t &value_data) {
	uint8_t value_header = 0;
	value_header |= static_cast<uint8_t>(VariantBasicType::PRIMITIVE);
	value_header |= static_cast<uint8_t>(TYPE_ID) << 2;

	*value_data = value_header;
	value_data++;
}

struct VariantSimpleCopy {
	template <class T>
	static void CopyValue(const_data_ptr_t source, data_ptr_t target) {
		memcpy(target, source, sizeof(T));
	}
};

template <class SRC>
struct VariantSimpleConversion {
	template <class T>
	static void CopyValue(const_data_ptr_t source, data_ptr_t target) {
		auto src = Load<SRC>(source);
		Store(static_cast<T>(src), target);
	}
};

template <class T, class OP = VariantSimpleCopy>
void CopySimplePrimitiveData(const UnifiedVariantVectorData &variant, data_ptr_t &value_data, idx_t row,
                             uint32_t values_index) {
	auto byte_offset = variant.GetByteOffset(row, values_index);
	auto data = const_data_ptr_cast(variant.GetData(row).GetData());
	auto ptr = data + byte_offset;
	OP::template CopyValue<T>(ptr, value_data);
	value_data += sizeof(T);
}

void CopyUUIDData(const UnifiedVariantVectorData &variant, data_ptr_t &value_data, idx_t row, uint32_t values_index) {
	auto byte_offset = variant.GetByteOffset(row, values_index);
	auto data = const_data_ptr_cast(variant.GetData(row).GetData());
	auto ptr = data + byte_offset;

	auto uuid = Load<uhugeint_t>(ptr);
	BaseUUID::ToBlob(uuid, value_data);
	value_data += sizeof(uhugeint_t);
}

static void WritePrimitiveValueData(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index,
                                    data_ptr_t &value_data, const vector<uint32_t> &offsets, idx_t &offset_index) {
	VariantLogicalType type_id = VariantLogicalType::VARIANT_NULL;
	if (variant.RowIsValid(row)) {
		type_id = variant.GetTypeId(row, values_index);
	}

	D_ASSERT(type_id != VariantLogicalType::OBJECT && type_id != VariantLogicalType::ARRAY);
	switch (type_id) {
	case VariantLogicalType::BLOB:
	case VariantLogicalType::VARCHAR: {
		auto string_value = VariantUtils::DecodeStringData(variant, row, values_index);
		auto string_size = string_value.GetSize();
		if (type_id == VariantLogicalType::BLOB || string_size >= 64) {
			if (type_id == VariantLogicalType::BLOB) {
				WritePrimitiveTypeHeader<VariantPrimitiveType::BINARY>(value_data);
			} else {
				WritePrimitiveTypeHeader<VariantPrimitiveType::STRING>(value_data);
			}
			Store<uint32_t>(NumericCast<uint32_t>(string_size), value_data);
			value_data += sizeof(uint32_t);
		} else {
			uint8_t value_header = 0;
			value_header |= static_cast<uint8_t>(VariantBasicType::SHORT_STRING);
			value_header |= static_cast<uint8_t>(string_size) << 2;

			*value_data = value_header;
			value_data++;
		}
		memcpy(value_data, reinterpret_cast<const char *>(string_value.GetData()), string_size);
		value_data += string_size;
		break;
	}
	case VariantLogicalType::VARIANT_NULL:
		WritePrimitiveTypeHeader<VariantPrimitiveType::NULL_TYPE>(value_data);
		break;
	case VariantLogicalType::BOOL_TRUE:
		WritePrimitiveTypeHeader<VariantPrimitiveType::BOOLEAN_TRUE>(value_data);
		break;
	case VariantLogicalType::BOOL_FALSE:
		WritePrimitiveTypeHeader<VariantPrimitiveType::BOOLEAN_FALSE>(value_data);
		break;
	case VariantLogicalType::INT8:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT8>(value_data);
		CopySimplePrimitiveData<int8_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::INT16:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT16>(value_data);
		CopySimplePrimitiveData<int16_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::INT32:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT32>(value_data);
		CopySimplePrimitiveData<int32_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::INT64:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT64>(value_data);
		CopySimplePrimitiveData<int64_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::FLOAT:
		WritePrimitiveTypeHeader<VariantPrimitiveType::FLOAT>(value_data);
		CopySimplePrimitiveData<float>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::DOUBLE:
		WritePrimitiveTypeHeader<VariantPrimitiveType::DOUBLE>(value_data);
		CopySimplePrimitiveData<double>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::UUID:
		WritePrimitiveTypeHeader<VariantPrimitiveType::UUID>(value_data);
		CopyUUIDData(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::DATE:
		WritePrimitiveTypeHeader<VariantPrimitiveType::DATE>(value_data);
		CopySimplePrimitiveData<int32_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::TIME_MICROS:
		WritePrimitiveTypeHeader<VariantPrimitiveType::TIME_NTZ_MICROS>(value_data);
		CopySimplePrimitiveData<int64_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::TIMESTAMP_MICROS:
		WritePrimitiveTypeHeader<VariantPrimitiveType::TIMESTAMP_NTZ_MICROS>(value_data);
		CopySimplePrimitiveData<int64_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::TIMESTAMP_NANOS:
		WritePrimitiveTypeHeader<VariantPrimitiveType::TIMESTAMP_NTZ_NANOS>(value_data);
		CopySimplePrimitiveData<int64_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::TIMESTAMP_MICROS_TZ:
		WritePrimitiveTypeHeader<VariantPrimitiveType::TIMESTAMP_MICROS>(value_data);
		CopySimplePrimitiveData<int64_t>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::DECIMAL: {
		auto decimal_data = VariantUtils::DecodeDecimalData(variant, row, values_index);

		if (decimal_data.width > 38) {
			throw InvalidInputException("Can't convert VARIANT DECIMAL(%d, %d) to Parquet VARIANT", decimal_data.width,
			                            decimal_data.scale);
		} else if (decimal_data.width <= 4) {
			// DuckDB uses INT16 to store small decimals, but parquet only supports DECIMAL4 at minimum, so here we
			// promote to INT32.
			WritePrimitiveTypeHeader<VariantPrimitiveType::DECIMAL4>(value_data);
			Store<int8_t>(NumericCast<int8_t>(decimal_data.scale), value_data);
			value_data++;
			const int32_t promoted = Load<int16_t>(decimal_data.value_ptr);
			Store<int32_t>(promoted, value_data);
			value_data += sizeof(int32_t);
		} else if (decimal_data.width <= 9) {
			WritePrimitiveTypeHeader<VariantPrimitiveType::DECIMAL4>(value_data);
			Store<int8_t>(NumericCast<int8_t>(decimal_data.scale), value_data);
			value_data++;
			memcpy(value_data, decimal_data.value_ptr, sizeof(int32_t));
			value_data += sizeof(int32_t);
		} else if (decimal_data.width <= 18) {
			WritePrimitiveTypeHeader<VariantPrimitiveType::DECIMAL8>(value_data);
			Store<int8_t>(NumericCast<int8_t>(decimal_data.scale), value_data);
			value_data++;
			memcpy(value_data, decimal_data.value_ptr, sizeof(int64_t));
			value_data += sizeof(int64_t);
		} else if (decimal_data.width <= 38) {
			WritePrimitiveTypeHeader<VariantPrimitiveType::DECIMAL16>(value_data);
			Store<int8_t>(NumericCast<int8_t>(decimal_data.scale), value_data);
			value_data++;
			memcpy(value_data, decimal_data.value_ptr, sizeof(hugeint_t));
			value_data += sizeof(hugeint_t);
		} else {
			throw InternalException(
			    "Uncovered VARIANT(DECIMAL) -> Parquet VARIANT conversion for type 'DECIMAL(%d, %d)'",
			    decimal_data.width, decimal_data.scale);
		}
		break;
	}
	case VariantLogicalType::UINT8:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT16>(value_data);
		CopySimplePrimitiveData<int16_t, VariantSimpleConversion<uint8_t>>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::UINT16:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT32>(value_data);
		CopySimplePrimitiveData<int32_t, VariantSimpleConversion<uint16_t>>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::UINT32:
		WritePrimitiveTypeHeader<VariantPrimitiveType::INT64>(value_data);
		CopySimplePrimitiveData<int64_t, VariantSimpleConversion<uint32_t>>(variant, value_data, row, values_index);
		break;
	case VariantLogicalType::UINT64:
	case VariantLogicalType::UINT128:
	case VariantLogicalType::INT128: {
		auto value = GetWideIntegerValue(variant, row, values_index, type_id);
		if (WideIntegerFitsInt64(value)) {
			WritePrimitiveTypeHeader<VariantPrimitiveType::INT64>(value_data);
			Store<int64_t>(static_cast<int64_t>(value.lower), value_data);
			value_data += sizeof(int64_t);
		} else {
			WritePrimitiveTypeHeader<VariantPrimitiveType::DECIMAL16>(value_data);
			Store<int8_t>(0, value_data);
			value_data++;
			Store<hugeint_t>(value, value_data);
			value_data += sizeof(hugeint_t);
		}
		break;
	}
	case VariantLogicalType::INTERVAL:
	case VariantLogicalType::BIGNUM:
	case VariantLogicalType::BITSTRING:
	case VariantLogicalType::TIMESTAMP_MILIS:
	case VariantLogicalType::TIMESTAMP_SEC:
	case VariantLogicalType::TIME_MICROS_TZ:
	case VariantLogicalType::TIME_NANOS:
	default:
		throw InvalidInputException("Can't convert VARIANT of type '%s' to Parquet VARIANT",
		                            EnumUtil::ToString(type_id));
	}
}

static void WriteValueData(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index,
                           data_ptr_t &value_data, const vector<uint32_t> &offsets, idx_t &offset_index,
                           optional_ptr<ParquetVariantShreddingState> shredding_state) {
	VariantLogicalType type_id = VariantLogicalType::VARIANT_NULL;
	if (variant.RowIsValid(row)) {
		type_id = variant.GetTypeId(row, values_index);
	}
	if (type_id == VariantLogicalType::OBJECT) {
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);

		//! -- Object value header --

		auto child_indices = GetChildIndices(variant, row, nested_data, shredding_state);
		if (nested_data.child_count && child_indices.empty()) {
			throw InternalException(
			    "The entire should be omitted, should have been handled by the Analyze step already");
		}
		auto num_elements = child_indices.size();

		//! Determine the 'field_id_size'
		uint32_t highest_keys_index = 0;
		for (auto &i : child_indices) {
			auto keys_index = variant.GetKeysIndex(row, i + nested_data.children_idx);
			highest_keys_index = MaxValue(highest_keys_index, keys_index);
		}
		auto field_id_size = CalculateByteLength(highest_keys_index);

		uint32_t last_offset = 0;
		if (num_elements) {
			last_offset = offsets[offset_index + num_elements];
		}
		offset_index += num_elements + 1;
		auto field_offset_size = CalculateByteLength(last_offset);

		const bool is_large = num_elements > NumericLimits<uint8_t>::Maximum();

		uint8_t value_header = 0;
		value_header |= static_cast<uint8_t>(VariantBasicType::OBJECT);
		value_header |= static_cast<uint8_t>(is_large) << 6;
		value_header |= (static_cast<uint8_t>(field_id_size) - 1) << 4;
		value_header |= (static_cast<uint8_t>(field_offset_size) - 1) << 2;

#ifdef DEBUG
		auto object_value_header = VariantValueMetadata::FromHeaderByte(value_header);
		D_ASSERT(object_value_header.basic_type == VariantBasicType::OBJECT);
		D_ASSERT(object_value_header.is_large == is_large);
		D_ASSERT(object_value_header.field_offset_size == field_offset_size);
		D_ASSERT(object_value_header.field_id_size == field_id_size);
#endif

		*value_data = value_header;
		value_data++;

		//! Write the 'num_elements'
		if (is_large) {
			Store<uint32_t>(static_cast<uint32_t>(num_elements), value_data);
			value_data += sizeof(uint32_t);
		} else {
			Store<uint8_t>(static_cast<uint8_t>(num_elements), value_data);
			value_data += sizeof(uint8_t);
		}

		//! Write the 'field_id' entries
		for (auto &i : child_indices) {
			auto keys_index = variant.GetKeysIndex(row, i + nested_data.children_idx);
			memcpy(value_data, reinterpret_cast<data_ptr_t>(&keys_index), field_id_size);
			value_data += field_id_size;
		}

		//! Write the 'field_offset' entries and the child 'value's
		auto children_ptr = value_data + ((num_elements + 1) * field_offset_size);
		idx_t total_offset = 0;
		for (auto &i : child_indices) {
			auto values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);

			memcpy(value_data, reinterpret_cast<data_ptr_t>(&total_offset), field_offset_size);
			value_data += field_offset_size;
			auto start_ptr = children_ptr;
			WriteValueData(variant, row, values_index, children_ptr, offsets, offset_index, nullptr);
			total_offset += NumericCast<idx_t>(children_ptr - start_ptr);
		}
		memcpy(value_data, reinterpret_cast<data_ptr_t>(&total_offset), field_offset_size);
		value_data += field_offset_size;
		D_ASSERT(children_ptr - total_offset == value_data);
		value_data = children_ptr;
	} else if (type_id == VariantLogicalType::ARRAY) {
		auto nested_data = VariantUtils::DecodeNestedData(variant, row, values_index);

		//! -- Array value header --

		uint32_t last_offset = 0;
		if (nested_data.child_count) {
			last_offset = offsets[offset_index + nested_data.child_count];
		}
		offset_index += nested_data.child_count + 1;
		auto field_offset_size = CalculateByteLength(last_offset);

		auto num_elements = nested_data.child_count;
		const bool is_large = num_elements > NumericLimits<uint8_t>::Maximum();

		uint8_t value_header = 0;
		value_header |= static_cast<uint8_t>(VariantBasicType::ARRAY);
		value_header |= static_cast<uint8_t>(is_large) << 4;
		value_header |= (static_cast<uint8_t>(field_offset_size) - 1) << 2;

#ifdef DEBUG
		auto array_value_header = VariantValueMetadata::FromHeaderByte(value_header);
		D_ASSERT(array_value_header.basic_type == VariantBasicType::ARRAY);
		D_ASSERT(array_value_header.is_large == is_large);
		D_ASSERT(array_value_header.field_offset_size == field_offset_size);
#endif

		*value_data = value_header;
		value_data++;

		//! Write the 'num_elements'
		if (is_large) {
			Store<uint32_t>(static_cast<uint32_t>(num_elements), value_data);
			value_data += sizeof(uint32_t);
		} else {
			Store<uint8_t>(static_cast<uint8_t>(num_elements), value_data);
			value_data += sizeof(uint8_t);
		}

		//! Write the 'field_offset' entries and the child 'value's
		auto children_ptr = value_data + ((num_elements + 1) * field_offset_size);
		idx_t total_offset = 0;
		for (idx_t i = 0; i < nested_data.child_count; i++) {
			auto values_index = variant.GetValuesIndex(row, i + nested_data.children_idx);

			memcpy(value_data, reinterpret_cast<data_ptr_t>(&total_offset), field_offset_size);
			value_data += field_offset_size;
			auto start_ptr = children_ptr;
			WriteValueData(variant, row, values_index, children_ptr, offsets, offset_index, nullptr);
			total_offset += NumericCast<idx_t>(children_ptr - start_ptr);
		}
		memcpy(value_data, reinterpret_cast<data_ptr_t>(&total_offset), field_offset_size);
		value_data += field_offset_size;
		D_ASSERT(children_ptr - total_offset == value_data);
		value_data = children_ptr;
	} else {
		WritePrimitiveValueData(variant, row, values_index, value_data, offsets, offset_index);
	}
}

static void CreateValues(UnifiedVariantVectorData &variant, Vector &value, optional_ptr<const SelectionVector> sel,
                         optional_ptr<const SelectionVector> value_index_sel,
                         optional_ptr<const SelectionVector> result_sel,
                         optional_ptr<ParquetVariantShreddingState> shredding_state, idx_t count) {
	auto &validity = FlatVector::ValidityMutable(value);
	auto value_data = FlatVector::GetDataMutable<string_t>(value);

	for (idx_t i = 0; i < count; i++) {
		uint32_t value_index = 0;
		if (value_index_sel) {
			value_index = NumericCast<uint32_t>(value_index_sel->get_index(i));
		}

		auto row = NumericCast<uint32_t>(i);
		if (sel) {
			row = NumericCast<uint32_t>(sel->get_index(i));
		}

		auto result_index = NumericCast<uint32_t>(i);
		if (result_sel) {
			result_index = NumericCast<uint32_t>(result_sel->get_index(i));
		}

		bool is_shredded = false;
		if (variant.RowIsValid(row) && shredding_state && shredding_state->ValueIsShredded(variant, row, value_index)) {
			shredding_state->SetShredded(row, value_index, result_index);
			is_shredded = true;
			if (shredding_state->type.id() != LogicalTypeId::STRUCT) {
				//! Value is shredded, directly write a NULL to the 'value' if the type is not an OBJECT
				//! When the type is OBJECT, all excess fields would still need to be written to the 'value'
				validity.SetInvalid(result_index);
				continue;
			}
		}

		//! The (relative) offsets for each value, in the case of nesting
		vector<uint32_t> offsets;
		//! Determine the size of this 'value' blob
		idx_t blob_length = AnalyzeValueData(variant, row, value_index, offsets, shredding_state);
		if (!blob_length) {
			//! This is only allowed to happen for a shredded OBJECT, where there are no excess fields to write for the
			//! OBJECT
			(void)is_shredded;
			D_ASSERT(is_shredded);
			validity.SetInvalid(result_index);
			continue;
		}
		value_data[result_index] = StringVector::EmptyString(value, blob_length);
		auto &value_blob = value_data[result_index];
		auto value_blob_data = reinterpret_cast<data_ptr_t>(value_blob.GetDataWriteable());

		idx_t offset_index = 0;
		WriteValueData(variant, row, value_index, value_blob_data, offsets, offset_index, shredding_state);
		D_ASSERT(data_ptr_cast(value_blob.GetDataWriteable() + blob_length) == value_blob_data);
		value_blob.SetSizeAndFinalize(NumericCast<uint32_t>(blob_length), blob_length);
	}
}

void ParquetVariantShredding::WriteMissingField(Vector &vector, idx_t index) {
	//! The field is missing, set it to null
	FlatVector::SetNull(vector, index, true);
}

void ParquetVariantShredding::WriteVariantValues(UnifiedVariantVectorData &variant, Vector &result,
                                                 optional_ptr<const SelectionVector> sel,
                                                 optional_ptr<const SelectionVector> value_index_sel,
                                                 optional_ptr<const SelectionVector> result_sel, idx_t count) {
	optional_ptr<Vector> value;
	optional_ptr<Vector> typed_value;

	auto &result_type = result.GetType();
	D_ASSERT(result_type.id() == LogicalTypeId::STRUCT);
	auto &child_types = StructType::GetChildTypes(result_type);
	auto &child_vectors = StructVector::GetEntries(result);
	D_ASSERT(child_types.size() == child_vectors.size());
	for (idx_t i = 0; i < child_types.size(); i++) {
		auto &name = child_types[i].first;
		if (name == "value") {
			value = &child_vectors[i];
		} else if (name == "typed_value") {
			typed_value = &child_vectors[i];
		}
	}

	if (typed_value) {
		ParquetVariantShreddingState shredding_state(typed_value->GetType(), count);
		CreateValues(variant, *value, sel, value_index_sel, result_sel, &shredding_state, count);

		SelectionVector null_values;
		if (shredding_state.count) {
			WriteTypedValues(variant, *typed_value, shredding_state.shredded_sel, shredding_state.values_index_sel,
			                 shredding_state.result_sel, shredding_state.count);
			//! 'shredding_state.result_sel' will always be a subset of 'result_sel', set the rows not in the subset to
			//! NULL
			idx_t sel_idx = 0;
			for (idx_t i = 0; i < count; i++) {
				auto original_index = result_sel ? result_sel->get_index(i) : i;
				if (sel_idx < shredding_state.count && shredding_state.result_sel[sel_idx] == original_index) {
					sel_idx++;
					continue;
				}
				FlatVector::SetNull(*typed_value, original_index, true);
			}
		} else {
			//! Set all rows of the typed_value to NULL, nothing is shredded on
			for (idx_t i = 0; i < count; i++) {
				FlatVector::SetNull(*typed_value, result_sel ? result_sel->get_index(i) : i, true);
			}
		}
	} else {
		CreateValues(variant, *value, sel, value_index_sel, result_sel, nullptr, count);
	}
}

void ParquetVariantConversion::ToParquetVariant(const Vector &variant_vec, idx_t count, Vector &result) {
	// DuckDB Variant:
	// - keys = VARCHAR[]
	// - children = STRUCT(keys_index UINTEGER, values_index UINTEGER)[]
	// - values = STRUCT(type_id UTINYINT, byte_offset UINTEGER)[]
	// - data = BLOB

	// Parquet VARIANT:
	// - metadata = BLOB
	// - value = BLOB (+ typed_value when 'result' declares a shredded layout)

	RecursiveUnifiedVectorFormat recursive_format;
	Vector::RecursiveToUnifiedFormat(variant_vec, recursive_format);
	UnifiedVariantVectorData variant(recursive_format);

	auto &result_vectors = StructVector::GetEntries(result);
	auto &metadata = result_vectors[0];
	CreateMetadata(variant, metadata, count);

	ParquetVariantShredding shredding;
	shredding.WriteVariantValues(variant, result, nullptr, nullptr, nullptr, count);
}

} // namespace duckdb
