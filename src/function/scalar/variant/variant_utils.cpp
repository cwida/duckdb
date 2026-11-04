#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/variant_vector.hpp"
#include "duckdb/function/scalar/variant_utils.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/types/string_type.hpp"
#include "duckdb/common/types/decimal.hpp"
#include "duckdb/common/serializer/varint.hpp"
#include "duckdb/common/types/variant_visitor.hpp"
#include "duckdb/function/variant/variant_value_convert.hpp"
#include "duckdb/common/type_visitor.hpp"
#include "duckdb/function/variant/variant_shredding.hpp"
#include "duckdb/common/types/variant/variant_builder.hpp"

namespace duckdb {

PhysicalType VariantDecimalData::GetPhysicalType() const {
	if (width > DecimalWidth<int64_t>::max) {
		return PhysicalType::INT128;
	} else if (width > DecimalWidth<int32_t>::max) {
		return PhysicalType::INT64;
	} else if (width > DecimalWidth<int16_t>::max) {
		return PhysicalType::INT32;
	} else {
		return PhysicalType::INT16;
	}
}

void VariantUtils::Available(idx_t blob_size, idx_t offset, idx_t size) {
	if (offset > blob_size || size > blob_size - offset) {
		throw DataCorruptionException("Variant byte_offset (%d) with size (%d) is out of bounds for a blob of size %d",
		                              offset, size, blob_size);
	}
}

bool VariantUtils::IsNestedType(const UnifiedVariantVectorData &variant, idx_t row, uint32_t value_index) {
	auto type_id = variant.GetTypeId(row, value_index);
	return type_id == VariantLogicalType::ARRAY || type_id == VariantLogicalType::OBJECT;
}

VariantDecimalData VariantUtils::DecodeDecimalData(const UnifiedVariantVectorData &variant, idx_t row,
                                                   uint32_t value_index) {
	D_ASSERT(variant.GetTypeId(row, value_index) == VariantLogicalType::DECIMAL);
	auto byte_offset = variant.GetByteOffset(row, value_index);
	auto &blob = variant.GetData(row);
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;

	auto width = VarintDecode<uint32_t>(ptr);
	auto scale = VarintDecode<uint32_t>(ptr);
	auto value_ptr = ptr;
	VariantUtils::Available(blob_size, NumericCast<idx_t>(value_ptr - data), 0);
	return VariantDecimalData(width, scale, value_ptr);
}

string_t VariantUtils::DecodeStringData(const UnifiedVariantVectorData &variant, idx_t row, uint32_t value_index) {
	auto byte_offset = variant.GetByteOffset(row, value_index);
	auto &blob = variant.GetData(row);
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;

	auto length = VarintDecode<uint32_t>(ptr);
	VariantUtils::Available(blob_size, NumericCast<idx_t>(ptr - data), length);
	return string_t(reinterpret_cast<const char *>(ptr), length);
}

VariantNestedData VariantUtils::DecodeNestedData(const UnifiedVariantVectorData &variant, idx_t row,
                                                 uint32_t value_index) {
	D_ASSERT(IsNestedType(variant, row, value_index));
	auto byte_offset = variant.GetByteOffset(row, value_index);
	auto &blob = variant.GetData(row);
	auto blob_size = blob.GetSize();
	VariantUtils::Available(blob_size, byte_offset, 1);
	auto data = const_data_ptr_cast(blob.GetData());
	auto ptr = data + byte_offset;

	VariantNestedData result;
	result.child_count = VarintDecode<uint32_t>(ptr);
	if (result.child_count) {
		VariantUtils::Available(blob_size, NumericCast<idx_t>(ptr - data), 1);
		result.children_idx = VarintDecode<uint32_t>(ptr);
	} else {
		result.children_idx = 0;
	}
	return result;
}

vector<string> VariantUtils::GetObjectKeys(const UnifiedVariantVectorData &variant, idx_t row,
                                           const VariantNestedData &nested_data) {
	vector<string> object_keys;
	for (idx_t child_idx = 0; child_idx < nested_data.child_count; child_idx++) {
		auto child_key_id = variant.GetKeysIndex(row, nested_data.children_idx + child_idx);
		object_keys.push_back(variant.GetKey(row, child_key_id).GetString());
	}
	return object_keys;
}

void VariantUtils::FindChildValues(const UnifiedVariantVectorData &variant, const VariantPathComponent &component,
                                   optional_ptr<const SelectionVector> sel, SelectionVector &res,
                                   ValidityMask &res_validity, const array_ptr<VariantNestedData> &nested_data,
                                   const ValidityMask &validity, idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		auto row_index = sel ? sel->get_index(i) : i;

		if (!validity.RowIsValid(i)) {
			continue;
		}
		auto &nested_data_entry = nested_data[i];
		if (component.lookup_mode == VariantChildLookupMode::BY_INDEX) {
			auto child_idx = component.index;
			if (child_idx >= nested_data_entry.child_count) {
				//! The list is too small to contain this index
				res_validity.SetInvalid(i);
				continue;
			}
			auto value_id = variant.GetValuesIndex(row_index, nested_data_entry.children_idx + child_idx);
			res[i] = value_id;
			continue;
		}
		bool found_child = false;
		for (idx_t child_idx = 0; child_idx < nested_data_entry.child_count; child_idx++) {
			auto value_id = variant.GetValuesIndex(row_index, nested_data_entry.children_idx + child_idx);
			auto key_id = variant.GetKeysIndex(row_index, nested_data_entry.children_idx + child_idx);

			auto &child_key = variant.GetKey(row_index, key_id);
			if (child_key == component.key) {
				//! Found the key we're looking for
				res[i] = value_id;
				found_child = true;
				break;
			}
		}
		if (!found_child) {
			res_validity.SetInvalid(i);
		}
	}
}

vector<uint32_t> VariantUtils::ValueIsNull(const UnifiedVariantVectorData &variant, const SelectionVector &sel,
                                           idx_t count, optional_idx row) {
	vector<uint32_t> res;
	res.reserve(count);
	for (idx_t i = 0; i < count; i++) {
		auto row_index = row.IsValid() ? row.GetIndex() : i;

		if (!variant.RowIsValid(row_index)) {
			res.push_back(static_cast<uint32_t>(i));
			continue;
		}

		//! Get the index into 'values'
		auto type_id = variant.GetTypeId(row_index, sel[i]);
		if (type_id == VariantLogicalType::VARIANT_NULL) {
			res.push_back(static_cast<uint32_t>(i));
		}
	}
	return res;
}

VariantNestedDataCollectionResult
VariantUtils::CollectNestedData(const UnifiedVariantVectorData &variant, VariantLogicalType expected_type,
                                const SelectionVector &value_index_sel, idx_t count, optional_idx row, idx_t offset,
                                array_ptr<VariantNestedData> child_data, ValidityMask &validity) {
	VariantLogicalType wrong_type = VariantLogicalType::VARIANT_NULL;
	for (idx_t i = 0; i < count; i++) {
		auto row_index = row.IsValid() ? row.GetIndex() : i;

		//! NOTE: the validity is assumed to be from a FlatVector
		//! Is the input row NULL ?
		if (!variant.RowIsValid(row_index) || !validity.RowIsValid(i)) {
			validity.SetInvalid(i);
			continue;
		}

		//! Is the variant value NULL ?
		auto type_id = variant.GetTypeId(row_index, value_index_sel[i]);
		if (type_id == VariantLogicalType::VARIANT_NULL) {
			validity.SetInvalid(i);
			continue;
		}

		//! Is the type of the VARIANT correct?
		if (type_id != expected_type) {
			if (wrong_type == VariantLogicalType::VARIANT_NULL) {
				//! Record the type of the first row that doesn't have the expected type
				wrong_type = type_id;
			}
			validity.SetInvalid(i);
			continue;
		}

		child_data[i] = DecodeNestedData(variant, row_index, value_index_sel[i]);
	}

	if (wrong_type != VariantLogicalType::VARIANT_NULL) {
		return VariantNestedDataCollectionResult(wrong_type);
	}
	return VariantNestedDataCollectionResult();
}

void VariantUtils::TraversePath(const UnifiedVariantVectorData &variant, const vector<VariantPathComponent> &components,
                                const idx_t count, array_ptr<VariantNestedData> nested_data, ValidityMask &validity,
                                VariantPathSelection &path_selection) {
	for (idx_t i = 0; i < components.size(); i++) {
		auto &component = components[i];
		auto &input_indices = path_selection.Input(i);
		auto &output_indices = path_selection.Output(i);

		if (component.lookup_mode == VariantChildLookupMode::BY_INDEX) {
			throw InternalException("Path indexes are not supported for this function");
		}

		(void)VariantUtils::CollectNestedData(variant, VariantLogicalType::OBJECT, input_indices, count, optional_idx(),
		                                      0, nested_data, validity);

		ValidityMask lookup_validity(count);
		VariantUtils::FindChildValues(variant, component, nullptr, output_indices, lookup_validity, nested_data,
		                              validity, count);

		for (idx_t j = 0; j < count; j++) {
			if (!validity.RowIsValid(j)) {
				continue;
			}
			if (lookup_validity.CanHaveNull() && !lookup_validity.RowIsValid(j)) {
				validity.SetInvalid(j);
			}
		}
	}
}

Value VariantUtils::ConvertVariantToValue(const UnifiedVariantVectorData &variant, idx_t row, uint32_t values_idx) {
	return VariantVisitor<ValueConverter>::Visit(variant, row, values_idx, 0);
}

void VariantUtils::FinalizeVariantKeys(Vector &variant, OrderedOwningStringMap<uint32_t> &dictionary,
                                       SelectionVector &sel, idx_t sel_size) {
	auto &keys = VariantVector::GetKeys(variant);
	auto &keys_entry = ListVector::GetChildMutable(keys);
	auto keys_entry_data = FlatVector::GetDataMutable<string_t>(keys_entry);

	bool already_sorted = true;

	vector<uint32_t> unsorted_to_sorted(dictionary.size());
	auto it = dictionary.begin();
	for (uint32_t sorted_idx = 0; sorted_idx < dictionary.size(); sorted_idx++) {
		auto unsorted_idx = it->second;
		if (unsorted_idx != sorted_idx) {
			already_sorted = false;
		}
		unsorted_to_sorted[unsorted_idx] = sorted_idx;
		D_ASSERT(sorted_idx < ListVector::GetListSize(keys));
		keys_entry_data[sorted_idx] = it->first;
		auto size = static_cast<uint32_t>(keys_entry_data[sorted_idx].GetSize());
		keys_entry_data[sorted_idx].SetSizeAndFinalize(size, size);
		it++;
	}

	//! The caller slices this vector into a dictionary that still spans every key slot, but only the distinct keys are
	//! written above - zero the remaining slots so they hold empty strings instead of uninitialized memory
	auto keys_entry_size = ListVector::GetListSize(keys);
	if (dictionary.size() < keys_entry_size) {
		memset(keys_entry_data + dictionary.size(), 0, sizeof(string_t) * (keys_entry_size - dictionary.size()));
	}

	if (!already_sorted) {
		//! Adjust the selection vector to point to the right dictionary index
		for (idx_t i = 0; i < sel_size; i++) {
			auto &entry = sel[i];
			auto sorted_idx = unsorted_to_sorted[entry];
			entry = sorted_idx;
		}
	}
}

bool VariantUtils::Verify(const Vector &variant, const SelectionVector &sel_p, idx_t count) {
	RecursiveUnifiedVectorFormat format;
	Vector::RecursiveToUnifiedFormat(variant, format);

	//! keys
	auto &keys = UnifiedVariantVector::GetKeys(format);
	auto keys_data = keys.GetData<list_entry_t>(keys);

	//! keys_entry
	auto &keys_entry = UnifiedVariantVector::GetKeysEntry(format);
	auto keys_entry_data = keys_entry.GetData<string_t>(keys_entry);
	D_ASSERT(keys_entry.validity.CannotHaveNull());

	//! children
	auto &children = UnifiedVariantVector::GetChildren(format);
	auto children_data = children.GetData<list_entry_t>(children);

	//! children.key_id
	auto &key_id = UnifiedVariantVector::GetChildrenKeysIndex(format);
	auto key_id_data = key_id.GetData<uint32_t>(key_id);

	//! children.value_id
	auto &value_id = UnifiedVariantVector::GetChildrenValuesIndex(format);
	auto value_id_data = value_id.GetData<uint32_t>(value_id);

	//! values
	auto &values = UnifiedVariantVector::GetValues(format);
	auto values_data = values.GetData<list_entry_t>(values);

	//! values.type_id
	auto &type_id = UnifiedVariantVector::GetValuesTypeId(format);
	auto type_id_data = type_id.GetData<uint8_t>(type_id);

	//! values.byte_offset
	auto &byte_offset = UnifiedVariantVector::GetValuesByteOffset(format);
	auto byte_offset_data = byte_offset.GetData<uint32_t>(byte_offset);

	//! data
	auto &data = UnifiedVariantVector::GetData(format);
	auto data_data = data.GetData<string_t>(data);

	for (idx_t i = 0; i < count; i++) {
		auto mapped_row = sel_p.get_index(i);
		auto index = format.unified.sel->get_index(mapped_row);

		if (!format.unified.validity.RowIsValid(index)) {
			continue;
		}

		auto keys_index = keys.sel->get_index(mapped_row);
		auto children_index = children.sel->get_index(mapped_row);
		auto values_index = values.sel->get_index(mapped_row);
		auto data_index = data.sel->get_index(mapped_row);

		D_ASSERT(keys.validity.RowIsValid(keys_index));
		D_ASSERT(children.validity.RowIsValid(children_index));
		D_ASSERT(values.validity.RowIsValid(values_index));
		D_ASSERT(data.validity.RowIsValid(data_index));

		auto keys_list_entry = keys_data[keys_index];
		auto children_list_entry = children_data[children_index];
		auto values_list_entry = values_data[values_index];
		auto &blob = data_data[data_index];

		//! verify keys
		for (idx_t j = 0; j < keys_list_entry.length; j++) {
			auto keys_entry_index = keys_entry.sel->get_index(j + keys_list_entry.offset);
			D_ASSERT(keys_entry.validity.RowIsValid(keys_entry_index));
			keys_entry_data[keys_entry_index].Verify();
		}
		//! verify children
		for (idx_t j = 0; j < children_list_entry.length; j++) {
			auto key_id_index = key_id.sel->get_index(j + children_list_entry.offset);
			if (key_id.validity.RowIsValid(key_id_index)) {
				auto children_key_id = key_id_data[key_id_index];
				D_ASSERT(children_key_id < keys_list_entry.length);
				(void)children_key_id;
			}

			auto value_id_index = value_id.sel->get_index(j + children_list_entry.offset);
			D_ASSERT(value_id.validity.RowIsValid(value_id_index));
			auto children_value_id = value_id_data[value_id_index];
			D_ASSERT(children_value_id < values_list_entry.length);
			(void)children_value_id;
		}

		//! verify values
		for (idx_t j = 0; j < values_list_entry.length; j++) {
			auto type_id_index = type_id.sel->get_index(j + values_list_entry.offset);
			D_ASSERT(type_id.validity.RowIsValid(type_id_index));
			auto value_type_id = type_id_data[type_id_index];
			D_ASSERT(value_type_id < static_cast<uint8_t>(VariantLogicalType::ENUM_SIZE));

			auto byte_offset_index = byte_offset.sel->get_index(j + values_list_entry.offset);
			D_ASSERT(byte_offset.validity.RowIsValid(byte_offset_index));
			auto value_byte_offset = byte_offset_data[byte_offset_index];
			D_ASSERT(value_byte_offset <= blob.GetSize());

			if (j == 0) {
				//! If the root value is NULL, the row itself should be NULL, not use VARIANT_NULL for the value
				D_ASSERT(value_type_id != static_cast<uint8_t>(VariantLogicalType::VARIANT_NULL));
			}

			auto blob_data = const_data_ptr_cast(blob.GetData()) + value_byte_offset;
			switch (static_cast<VariantLogicalType>(value_type_id)) {
			case VariantLogicalType::OBJECT:
			case VariantLogicalType::ARRAY: {
				auto length = VarintDecode<uint32_t>(blob_data);
				if (!length) {
					break;
				}
				auto children_start_index = VarintDecode<uint32_t>(blob_data);
				D_ASSERT(children_start_index + length <= children_list_entry.length);

				//! Verify the validity of array/object key_ids
				for (idx_t child_idx = 0; child_idx < length; child_idx++) {
					auto child_key_id_index =
					    key_id.sel->get_index(children_list_entry.offset + children_start_index + child_idx);
					if (value_type_id == static_cast<uint8_t>(VariantLogicalType::OBJECT)) {
						D_ASSERT(key_id.validity.RowIsValid(child_key_id_index));
					} else {
						D_ASSERT(!key_id.validity.RowIsValid(child_key_id_index));
					}
					(void)child_key_id_index;
				}
				break;
			}
			default:
				break;
			}
		}
	}

	return true;
}

LogicalType VariantUtils::ShreddedType(const LogicalType &logical_type) {
	auto shredding_type = TypeVisitor::VisitReplace(logical_type, [](const LogicalType &type) {
		if (type.id() == LogicalTypeId::STRUCT) {
			return LogicalType::STRUCT({{"typed_value", type}});
		}
		return type;
	});
	return LogicalType::STRUCT({{"unshredded", VariantShredding::GetUnshreddedType()}, {"shredded", shredding_type}});
}

bool VariantUtils::VariantSupportsType(const LogicalType &type) {
	if (type.IsJSONType()) {
		return false;
	}
	switch (type.id()) {
	case LogicalTypeId::BOOLEAN:
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
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::UUID:
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::BIT:
	case LogicalTypeId::GEOMETRY:
	case LogicalTypeId::GEOGRAPHY:
	case LogicalTypeId::DECIMAL:
		return true;
	default:
		return false;
	}
}

bool VariantUtils::ShreddedCastMatchesVariantCast(const LogicalType &shredded_type, const LogicalType &target) {
	if (target.id() == LogicalTypeId::VARIANT || shredded_type == target) {
		return true;
	}
	// casts to JSON serialize the VARIANT, and the VARIANT cast has no conversion to e.g. ENUM or UNION
	return VariantSupportsType(target);
}

//===--------------------------------------------------------------------===//
// ToVariant sources
//===--------------------------------------------------------------------===//
// The single-pass build machinery (VariantBuilder / EmitIterator / BuildVariant) lives in
// variant_builder.hpp so it can be shared with the parquet reader. A "source" just implements
// 'bool Emit(idx_t row, VariantBuilder &builder)' (returning whether the row is a SQL NULL).
namespace {

struct VariantIteratorSource {
	explicit VariantIteratorSource(const VariantIterator &state) : state(state) {
	}
	bool Emit(idx_t row, VariantBuilder &builder) const {
		auto root = state.Root(row);
		//! Root() resolves a missing/absent root to a SQL NULL
		if (root.IsNull()) {
			return true;
		}
		EmitIterator(root, builder);
		return false;
	}

	const VariantIterator &state;
};

} // namespace

void VariantUtils::ToVariant(const VariantIterator &state, idx_t count, Vector &result) {
	VariantIteratorSource source(state);
	BuildVariant(source, count, result);
}

} // namespace duckdb
