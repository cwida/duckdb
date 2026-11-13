#pragma once

#include "duckdb/function/cast/variant/list_to_variant.hpp"
#include "duckdb/function/cast/variant/array_to_variant.hpp"
#include "duckdb/function/cast/variant/json_to_variant.hpp"
#include "duckdb/function/cast/variant/struct_to_variant.hpp"
#include "duckdb/function/cast/variant/tuple_to_variant.hpp"
#include "duckdb/function/cast/variant/union_to_variant.hpp"
#include "duckdb/function/cast/variant/variant_to_variant.hpp"
#include "duckdb/function/cast/variant/primitive_to_variant.hpp"
#include "duckdb/function/cast/variant/variant_shredder.hpp"

namespace duckdb {
namespace variant {

//! * @param source The Vector of arbitrary type to process
//! * @param result The result Vector to write the variant data to
//! * @param count The amount of values we're converting
//! * @param selvec The selection vector from i (< count) to the index in the result Vector
//! * @param values_index_selvec The selection vector from i (< count) to the index in the children.values_index selvec,
//! to populate the parent's children
//! * @param is_root Whether we are writing to the root of the Variant, or a child value (in an OBJECT/ARRAY)
template <bool WRITE_DATA, bool IGNORE_NULLS>
bool ConvertToVariant(ToVariantSourceData &source, ToVariantGlobalResultData &result, idx_t count,
                      optional_ptr<const SelectionVector> selvec,
                      optional_ptr<const SelectionVector> values_index_selvec, const bool is_root) {
	auto &type = source.vec.GetType();

	auto logical_type = type.id();
	if (type.IsNested()) {
		switch (logical_type) {
		case LogicalTypeId::MAP:
		case LogicalTypeId::LIST:
			return ConvertListToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec, values_index_selvec,
			                                                      is_root);
		case LogicalTypeId::ARRAY:
			return ConvertArrayToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec,

			                                                       values_index_selvec, is_root);
		case LogicalTypeId::STRUCT:
			return ConvertStructToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec,

			                                                        values_index_selvec, is_root);
		case LogicalTypeId::TUPLE:
			return ConvertTupleToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec, values_index_selvec,
			                                                       is_root);
		case LogicalTypeId::UNION:
			return ConvertUnionToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec,

			                                                       values_index_selvec, is_root);
		case LogicalTypeId::VARIANT:
			return ConvertVariantToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec, values_index_selvec,
			                                                         is_root);
		default:
			throw NotImplementedException("Can't convert nested type '%s'", EnumUtil::ToString(logical_type));
		};
	} else {
		if (type.IsJSONType()) {
			return ConvertJSONToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec,

			                                                      values_index_selvec, is_root);
		} else {
			return ConvertPrimitiveToVariant<WRITE_DATA, IGNORE_NULLS>(source, result, count, selvec,
			                                                           values_index_selvec, is_root);
		}
	}
	return true;
}

} // namespace variant
} // namespace duckdb
