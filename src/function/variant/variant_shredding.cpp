#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"

namespace duckdb {

static void WriteShreddedPrimitive(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
                                   const SelectionVector &value_index_sel, const SelectionVector &result_sel,
                                   idx_t count, idx_t type_size) {
	auto result_data = FlatVector::GetDataMutable(result);
	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto result_row = result_sel[i];
		auto value_index = value_index_sel[i];
		D_ASSERT(variant.RowIsValid(row));

		auto byte_offset = variant.GetByteOffset(row, value_index);
		auto &data = variant.GetData(row);
		auto value_ptr = data.GetData();
		auto result_offset = type_size * result_row;
		memcpy(result_data + result_offset, value_ptr + byte_offset, type_size);
	}
}

template <class T>
static void WriteShreddedDecimal(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
                                 const SelectionVector &value_index_sel, const SelectionVector &result_sel,
                                 idx_t count) {
	auto result_data = FlatVector::GetDataMutable(result);
	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto result_row = result_sel[i];
		auto value_index = value_index_sel[i];
		D_ASSERT(variant.RowIsValid(row) && variant.GetTypeId(row, value_index) == VariantLogicalType::DECIMAL);

		auto decimal_data = VariantUtils::DecodeDecimalData(variant, row, value_index);
		D_ASSERT(decimal_data.width <= DecimalWidth<T>::max);
		auto result_offset = sizeof(T) * result_row;
		memcpy(result_data + result_offset, decimal_data.value_ptr, sizeof(T));
	}
}

#ifdef D_ASSERT_IS_ENABLED
static bool IsVariantStringType(VariantLogicalType type_id) {
	switch (type_id) {
	case VariantLogicalType::GEOMETRY:
	case VariantLogicalType::GEOGRAPHY:
	case VariantLogicalType::BITSTRING:
	case VariantLogicalType::BLOB:
	case VariantLogicalType::VARCHAR:
	case VariantLogicalType::BIGNUM:
		return true;
	default:
		return false;
	}
}
#endif

static void WriteShreddedString(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
                                const SelectionVector &value_index_sel, const SelectionVector &result_sel,
                                idx_t count) {
	auto result_data = FlatVector::ScatterWriter<string_t>(result);
	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto result_row = result_sel[i];
		auto value_index = value_index_sel[i];
		D_ASSERT(variant.RowIsValid(row) && IsVariantStringType(variant.GetTypeId(row, value_index)));

		auto string_data = VariantUtils::DecodeStringData(variant, row, value_index);
		result_data[result_row] = string_data;
	}
}

static void WriteShreddedBoolean(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
                                 const SelectionVector &value_index_sel, const SelectionVector &result_sel,
                                 idx_t count) {
	auto result_data = FlatVector::ScatterWriter<bool>(result);
	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto result_row = result_sel[i];
		auto value_index = value_index_sel[i];
		D_ASSERT(variant.RowIsValid(row));
		auto type_id = variant.GetTypeId(row, value_index);
		D_ASSERT(type_id == VariantLogicalType::BOOL_FALSE || type_id == VariantLogicalType::BOOL_TRUE);

		result_data[result_row] = type_id == VariantLogicalType::BOOL_TRUE;
	}
}

void VariantShredding::WriteTypedPrimitiveValues(UnifiedVariantVectorData &variant, Vector &result,
                                                 const SelectionVector &sel, const SelectionVector &value_index_sel,
                                                 const SelectionVector &result_sel, idx_t count) {
	auto &type = result.GetType();
	D_ASSERT(!type.IsNested());
	switch (type.id()) {
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::UUID: {
		const auto physical_type = type.InternalType();
		WriteShreddedPrimitive(variant, result, sel, value_index_sel, result_sel, count, GetTypeIdSize(physical_type));
		break;
	}
	case LogicalTypeId::DECIMAL: {
		const auto physical_type = type.InternalType();
		switch (physical_type) {
		//! DECIMAL2 (doesn't exist in Parquet for some reason)
		case PhysicalType::INT16:
			WriteShreddedDecimal<int16_t>(variant, result, sel, value_index_sel, result_sel, count);
			break;
		//! DECIMAL4
		case PhysicalType::INT32:
			WriteShreddedDecimal<int32_t>(variant, result, sel, value_index_sel, result_sel, count);
			break;
		//! DECIMAL8
		case PhysicalType::INT64:
			WriteShreddedDecimal<int64_t>(variant, result, sel, value_index_sel, result_sel, count);
			break;
		//! DECIMAL16
		case PhysicalType::INT128:
			WriteShreddedDecimal<hugeint_t>(variant, result, sel, value_index_sel, result_sel, count);
			break;
		default:
			throw InvalidInputException("Can't shred on column of type '%s'", type.ToString());
		}
		break;
	}
	case LogicalTypeId::BLOB:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY:
	case LogicalTypeId::BIT:
	case LogicalTypeId::VARCHAR: {
		WriteShreddedString(variant, result, sel, value_index_sel, result_sel, count);
		break;
	}
	case LogicalTypeId::BOOLEAN:
		WriteShreddedBoolean(variant, result, sel, value_index_sel, result_sel, count);
		break;
	default:
		throw InvalidInputException("Can't shred on type: %s", type.ToString());
	}
}

void VariantShredding::WriteMissingField(Vector &vector, idx_t index) {
	FlatVector::GetDataMutable<uint32_t>(vector)[index] = 0;
}

void VariantShredding::WriteTypedObjectValues(UnifiedVariantVectorData &variant, Vector &result,
                                              const SelectionVector &sel, const SelectionVector &value_index_sel,
                                              const SelectionVector &result_sel, idx_t count) {
	auto &type = result.GetType();
	D_ASSERT(type.id() == LogicalTypeId::STRUCT);

	auto &validity = FlatVector::ValidityMutable(result);
	(void)validity;

	//! Collect the nested data for the objects
	const auto owned_nested_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);
	array_ptr nested_data(owned_nested_data.get(), count);

	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		//! When we're shredding an object, the top-level struct of it should always be valid
		D_ASSERT(validity.RowIsValid(result_sel[i]));
		auto value_index = value_index_sel[i];
		D_ASSERT(variant.GetTypeId(row, value_index) == VariantLogicalType::OBJECT);
		nested_data[i] = VariantUtils::DecodeNestedData(variant, row, value_index);
	}

	auto &shredded_types = StructType::GetChildTypes(type);
	auto &shredded_fields = StructVector::GetEntries(result);
	D_ASSERT(shredded_types.size() == shredded_fields.size());

	SelectionVector child_values_indexes;
	SelectionVector child_row_sel;
	SelectionVector child_result_sel;
	child_values_indexes.Initialize(count);
	child_row_sel.Initialize(count);
	child_result_sel.Initialize(count);

	for (idx_t child_idx = 0; child_idx < shredded_types.size(); child_idx++) {
		auto &child_vec = shredded_fields[child_idx];
		D_ASSERT(child_vec.GetType() == shredded_types[child_idx].second);

		//! Prepare the path component to perform the lookup for
		auto &key = shredded_types[child_idx].first;
		VariantPathComponent path_component;
		path_component.lookup_mode = VariantChildLookupMode::BY_KEY;
		path_component.key = key.GetIdentifierName();

		ValidityMask lookup_validity(count);
		ValidityMask all_valid_validity(count);
		VariantUtils::FindChildValues(variant, path_component, sel, child_values_indexes, lookup_validity, nested_data,
		                              all_valid_validity, count);

		if (lookup_validity.CanHaveNull()) {
			optional_ptr<Vector> typed_value_vector;
			optional_ptr<Vector> untyped_value_vector;
			if (child_vec.GetType().id() == LogicalTypeId::STRUCT) {
				// this is a STRUCT(typed_value .., [untyped_value UINT])
				auto &child_variant_vectors = StructVector::GetEntries(child_vec);
				if (typed_value_index < child_variant_vectors.size()) {
					typed_value_vector = child_variant_vectors[typed_value_index];
				}
				if (untyped_value_index < child_variant_vectors.size()) {
					untyped_value_vector = child_variant_vectors[untyped_value_index];
				}
			} else {
				// this is a primitive type
				typed_value_vector = child_vec;
			}

			//! For some of the rows the field is missing, adjust the selection vector to exclude these rows.
			idx_t child_count = 0;
			for (idx_t i = 0; i < count; i++) {
				if (!lookup_validity.RowIsValid(i)) {
					//! The field is missing, set the untyped value index to 0
					if (typed_value_vector) {
						FlatVector::SetNull(*typed_value_vector, result_sel[i], true);
					}
					if (!untyped_value_vector) {
						throw InternalException("Field is missing but untyped_value_index is not set");
					}
					WriteMissingField(*untyped_value_vector, result_sel[i]);
					continue;
				}

				child_row_sel[child_count] = sel[i];
				child_values_indexes[child_count] = child_values_indexes[i];
				child_result_sel[child_count] = result_sel[i];
				child_count++;
			}

			if (child_count) {
				//! If not all rows are missing this field, write the values for it
				WriteVariantValues(variant, child_vec, child_row_sel, child_values_indexes, child_result_sel,
				                   child_count);
			}
		} else {
			WriteVariantValues(variant, child_vec, &sel, child_values_indexes, result_sel, count);
		}
	}
}

void VariantShredding::WriteTypedArrayValues(UnifiedVariantVectorData &variant, Vector &result,
                                             const SelectionVector &sel, const SelectionVector &value_index_sel,
                                             const SelectionVector &result_sel, idx_t count) {
	auto nested_data = make_unsafe_uniq_array_uninitialized<VariantNestedData>(count);

	auto list_data = FlatVector::ScatterWriter<list_entry_t>(result);
	idx_t total_offset = 0;
	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto value_index = value_index_sel[i];
		auto result_row = result_sel[i];

		D_ASSERT(variant.GetTypeId(row, value_index) == VariantLogicalType::ARRAY);
		nested_data[i] = VariantUtils::DecodeNestedData(variant, row, value_index);

		list_entry_t list_entry;
		list_entry.length = nested_data[i].child_count;
		list_entry.offset = total_offset;
		list_data[result_row] = list_entry;

		total_offset += nested_data[i].child_count;
	}
	ListVector::Reserve(result, total_offset);
	ListVector::SetListSize(result, total_offset);

	SelectionVector child_sel;
	child_sel.Initialize(total_offset);

	SelectionVector child_value_index_sel;
	child_value_index_sel.Initialize(total_offset);

	SelectionVector child_result_sel;
	child_result_sel.Initialize(total_offset);

	for (idx_t i = 0; i < count; i++) {
		auto row = sel[i];
		auto result_row = result_sel[i];

		auto &array_data = nested_data[i];
		auto &entry = list_data[result_row];
		for (idx_t j = 0; j < entry.length; j++) {
			auto offset = entry.offset + j;
			child_sel[offset] = row;
			child_value_index_sel[offset] = variant.GetValuesIndex(row, array_data.children_idx + j);
			child_result_sel[offset] = NumericCast<uint32_t>(offset);
		}
	}

	auto &child_vector = ListVector::GetChildMutable(result);
	WriteVariantValues(variant, child_vector, child_sel, child_value_index_sel, child_result_sel, total_offset);
}

void VariantShredding::WriteTypedValues(UnifiedVariantVectorData &variant, Vector &result, const SelectionVector &sel,
                                        const SelectionVector &value_index_sel, const SelectionVector &result_sel,
                                        idx_t count) {
	auto &type = result.GetType();

	if (type.id() == LogicalTypeId::STRUCT) {
		//! Shredded OBJECT
		WriteTypedObjectValues(variant, result, sel, value_index_sel, result_sel, count);
	} else if (type.id() == LogicalTypeId::LIST) {
		//! Shredded ARRAY
		WriteTypedArrayValues(variant, result, sel, value_index_sel, result_sel, count);
	} else {
		//! Primitive types
		WriteTypedPrimitiveValues(variant, result, sel, value_index_sel, result_sel, count);
	}
}

VariantShreddingState::VariantShreddingState(const LogicalType &type, idx_t total_count)
    : type(type), shredded_sel(total_count), values_index_sel(total_count), result_sel(total_count) {
	if (type.id() != LogicalTypeId::STRUCT) {
		return;
	}
	for (auto &entry : StructType::GetChildTypes(type)) {
		auto &name = entry.first.GetIdentifierName();
		object_fields.emplace(name.c_str(), UnsafeNumericCast<uint32_t>(name.size()));
	}
}

bool VariantShreddingState::ValueIsShredded(UnifiedVariantVectorData &variant, idx_t row, uint32_t values_index) {
	auto type_id = variant.GetTypeId(row, values_index);
	if (!GetVariantTypes().count(type_id)) {
		return false;
	}
	if (type_id == VariantLogicalType::DECIMAL) {
		// the shredded column stores the unscaled value, so it only holds decimals of its scale that fit its width
		auto decimal_data = VariantUtils::DecodeDecimalData(variant, row, values_index);
		return type.InternalType() == decimal_data.GetPhysicalType() &&
		       decimal_data.scale == DecimalType::GetScale(type) && decimal_data.width <= DecimalType::GetWidth(type);
	}
	return true;
}

void VariantShreddingState::SetShredded(uint32_t row, uint32_t values_index, uint32_t result_idx) {
	shredded_sel[count] = row;
	values_index_sel[count] = values_index;
	result_sel[count] = result_idx;
	count++;
}

const string_set_t &VariantShreddingState::ObjectFields() const {
	D_ASSERT(type.id() == LogicalTypeId::STRUCT);
	return object_fields;
}

} // namespace duckdb
