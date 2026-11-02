#include "duckdb/main/capi_v2/capi_v2_internal.hpp"

#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

// Out-param zeroing on failure:
//   - Pointer-bearing out-params (out_view, out_child, out_data) are
//     set to nullptr on every INVALID_INPUT path.
//   - Scalar out-params (out_count, out_size) are left unspecified on
//     failure; callers must consult the return code first.
//     vector_get_view zero-inits all three fields of out_view via
//     std::memset.

namespace duckdb::capiv2 {
namespace {

bool IsSupportedVectorType(VectorType vt) {
	return vt == VectorType::FLAT_VECTOR || vt == VectorType::CONSTANT_VECTOR || vt == VectorType::DICTIONARY_VECTOR;
}

// Map core's VectorType to the V2 surface.
// FSST / SEQUENCE / SHREDDED collapse into OTHER
// V2's untyped view rejects those kinds and requires an explicit duckdb_v2_vector_flatten first.

DUCKDB_V2_VECTOR_TYPE MapVectorType(VectorType vt) {
	switch (vt) {
	case VectorType::FLAT_VECTOR:
		return DUCKDB_V2_VECTOR_TYPE_FLAT;
	case VectorType::CONSTANT_VECTOR:
		return DUCKDB_V2_VECTOR_TYPE_CONSTANT;
	case VectorType::DICTIONARY_VECTOR:
		return DUCKDB_V2_VECTOR_TYPE_DICTIONARY;
	default:
		return DUCKDB_V2_VECTOR_TYPE_OTHER;
	}
}

} // anonymous namespace
} // namespace duckdb::capiv2

//----------------------------------------------------------------------------------------------------------------------
// Public API
//----------------------------------------------------------------------------------------------------------------------
using namespace duckdb::capiv2;

DUCKDB_V2_ERROR duckdb_v2_vector_get_vector_type(duckdb_v2_vector_handle vector, DUCKDB_V2_VECTOR_TYPE *out_type,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_type);
	return WithErrorHandler(err, [&]() { *out_type = MapVectorType(Convert(vector)->GetVectorType()); });
}

DUCKDB_V2_ERROR duckdb_v2_vector_get_logical_type(duckdb_v2_vector_handle vector,
                                                  duckdb_v2_logical_type_handle *out_type,
                                                  duckdb_v2_error_info_handle *err) {
	return WithErrorHandler(err, [&]() {
		if (!vector || !out_type) {
			throw duckdb::InvalidInputException("null argument to duckdb_v2_vector_get_logical_type");
		}
		*out_type = nullptr;
		auto *vec = Convert(vector);
		*out_type = Convert(vec->GetType());
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_flatten(duckdb_v2_vector_handle vector, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() { Convert(vector)->Flatten(); });
}

DUCKDB_V2_ERROR duckdb_v2_vector_cast(duckdb_v2_factory_handle factory, duckdb_v2_vector_handle source,
                                      duckdb_v2_vector_handle target, DUCKDB_V2_CAST_MODE mode, bool *out_converted,
                                      duckdb_v2_error_info_handle *cast_err, duckdb_v2_error_info_handle *err) {
	if (cast_err) {
		*cast_err = nullptr;
	}
	DUCKDB_CHECK_ARG(factory);
	DUCKDB_CHECK_ARG(source);
	DUCKDB_CHECK_ARG(target);
	return WithErrorHandler(err, [&]() {
		auto *src = Convert(source);
		auto *dst = Convert(target);
		if (src == dst) {
			throw duckdb::InvalidInputException("duckdb_v2_vector_cast: source and target must be different vectors");
		}
		if (mode != DUCKDB_V2_CAST_MODE_NORMAL && mode != DUCKDB_V2_CAST_MODE_TRY) {
			throw duckdb::InvalidInputException("duckdb_v2_vector_cast: invalid cast mode %d", static_cast<int>(mode));
		}
		const auto count = src->size();
		// a try cast collects the error instead of throwing, and NULLs the rows that failed
		duckdb::string error;
		auto error_ptr = mode == DUCKDB_V2_CAST_MODE_TRY ? &error : nullptr;
		auto context = Convert(factory)->TryGetClientContext();
		// the casts expect a fresh result, so stale validity or data in the target cannot leak into it
		duckdb::Vector result(dst->GetType(), count);
		auto converted = context ? duckdb::VectorOperations::TryCast(*context, *src, result, count, error_ptr)
		                         : duckdb::VectorOperations::DefaultTryCast(*src, result, count, error_ptr);
		dst->Reference(result);
		if (out_converted) {
			*out_converted = converted;
		}
		if (cast_err && !converted) {
			// rendered like any error the API reports, as a conversion error
			auto info = duckdb::make_uniq<CV2ErrorInfo>();
			try {
				throw duckdb::ConversionException(error);
			} catch (...) {
				RenderCaughtError(info->code, info->message, info->raw_message);
			}
			*cast_err = Convert(info.release());
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_reference(duckdb_v2_vector_handle vector, duckdb_v2_vector_handle source,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(source);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		auto *src = Convert(source);
		// The engine reports a mismatch as an INTERNAL error, and only by type id.
		if (vec->GetType() != src->GetType()) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_vector_reference: source type does not match the vector's logical type");
		}
		vec->Reference(*src);
	});
}

// ---------------------------------------------------------------------------
// The view-getter
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_get_view(duckdb_v2_vector_handle vector, duckdb_v2_vector_view *out_view,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_view);
	// Zeroed before any rejection, the null-vector one included, so a failure
	// leaves no stale pointers in the view.
	std::memset(out_view, 0, sizeof(*out_view));
	DUCKDB_CHECK_ARG(vector);

	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		auto vt = vec->GetVectorType();
		if (!IsSupportedVectorType(vt)) {
			throw duckdb::InvalidInputException("duckdb_v2_vector_get_view: vector is FSST / SEQUENCE / SHREDDED — "
			                                    "call duckdb_v2_vector_flatten first");
		}
		switch (vt) {
		case duckdb::VectorType::FLAT_VECTOR: {
			out_view->data = duckdb::FlatVector::GetData(*vec);
			out_view->validity = duckdb::FlatVector::Validity(*vec).GetData();
			out_view->sel = nullptr; // identity (UVF semantics)
			out_view->count = vec->size();
			break;
		}
		case duckdb::VectorType::CONSTANT_VECTOR: {
			out_view->data = duckdb::ConstantVector::GetData(*vec);
			out_view->validity = duckdb::ConstantVector::Validity(*vec).GetData();
			out_view->sel =
			    reinterpret_cast<const duckdb_v2_sel_t *>(duckdb::ConstantVector::ZeroSelectionVector()->data());
			out_view->count = vec->size();
			break;
		}
		case duckdb::VectorType::DICTIONARY_VECTOR: {
			// Flatten the dictionary child in-place if it isn't FLAT yet,
			// matching DictionaryBuffer::ToUnifiedFormat. The parent
			// vector stays DICTIONARY; only the underlying child is
			// flattened so the dictionary's sel pointer remains valid.
			auto &child = duckdb::DictionaryVector::Child(*vec);
			if (child.GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
				child.Flatten();
			}
			out_view->data = duckdb::FlatVector::GetData(child);
			out_view->validity = duckdb::FlatVector::Validity(child).GetData();
			out_view->sel = reinterpret_cast<const duckdb_v2_sel_t *>(duckdb::DictionaryVector::SelVector(*vec).data());
			out_view->count = vec->size();
			break;
		}
		default:
			// Unreachable thanks to IsSupportedVectorType above.
			break;
		}
	});
}

// ---------------------------------------------------------------------------
// Mutable Accessors
// ---------------------------------------------------------------------------
DUCKDB_V2_ERROR duckdb_v2_vector_get_data_mutable(duckdb_v2_vector_handle vector, void **out_data,
                                                  duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_data);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);

		const auto vec_type = vec->GetVectorType();
		if (vec_type != duckdb::VectorType::FLAT_VECTOR && vec_type != duckdb::VectorType::CONSTANT_VECTOR) {
			throw duckdb::InvalidInputException("Mutable data access is only supported for FLAT_VECTOR and "
			                                    "CONSTANT_VECTOR types. Please flatten the vector first.");
		}
		// We can get unsafe here cause we've already verified the vector type
		*out_data = duckdb::FlatVector::GetDataMutableUnsafe(*vec);
	});
}

// ---------------------------------------------------------------------------
// Vector type setup
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_make_constant(duckdb_v2_vector_handle vector, duckdb_v2_value_handle value,
                                               idx_t count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		auto *val = Convert(value);
		// The engine only debug-asserts this in ConstantVector::Reference; a
		// mismatch in release would corrupt the vector silently.
		if (vec->GetType() != val->type()) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_vector_make_constant: value type does not match the vector's logical type");
		}
		duckdb::ConstantVector::Reference(*vec, *val, duckdb::count_t(count));
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_make_sequence(duckdb_v2_vector_handle vector, int64_t start, int64_t increment,
                                               idx_t count, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() { Convert(vector)->Sequence(start, increment, count); });
}

// ---------------------------------------------------------------------------
// Validity (mutable)
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_set_null(duckdb_v2_vector_handle vector, idx_t row, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		if (vec->GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			throw duckdb::InvalidInputException("duckdb_v2_vector_set_null: only supported for FLAT vectors");
		}
		if (row >= vec->size()) {
			throw duckdb::InvalidInputException("row out of range in duckdb_v2_vector_set_null");
		}
		duckdb::FlatVector::SetNull(*vec, row, true);
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_flat_get_validity_mutable(duckdb_v2_vector_handle vector, uint64_t **out_validity,
                                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_validity);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		if (vec->GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_vector_flat_get_validity_mutable: only supported for FLAT vectors");
		}
		auto &validity = duckdb::FlatVector::ValidityMutable(*vec);
		validity.EnsureWritable();
		*out_validity = validity.GetData();
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_constant_set_valid(duckdb_v2_vector_handle vector, bool validity,
                                                    duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		if (vec->GetVectorType() != duckdb::VectorType::CONSTANT_VECTOR) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_vector_constant_set_valid: only supported for CONSTANT vectors");
		}
		duckdb::ConstantVector::SetNull(*vec, !validity);
	});
}
// ---------------------------------------------------------------------------
// Generic structural accessors for nested kinds
//
// Per-kind child counts:
//   LIST    → 1 child  ([0] = elements)
//   MAP     → 1 child  ([0] = entries, a STRUCT(key, value))
//   ARRAY   → 1 child  ([0] = elements)
//   STRUCT  → N children ([i] = field i)
//   UNION   → N+1 children ([0] = tag, [1..N] = members)
//   others  → 0
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_get_child_count(duckdb_v2_vector_handle vector, idx_t *out_count,
                                                 duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_count);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		switch (vec->GetType().id()) {
		case duckdb::LogicalTypeId::LIST:
		case duckdb::LogicalTypeId::MAP:
		case duckdb::LogicalTypeId::ARRAY:
			*out_count = 1;
			return;
		case duckdb::LogicalTypeId::STRUCT:
		case duckdb::LogicalTypeId::TUPLE:
			*out_count = duckdb::StructType::GetChildCount(vec->GetType());
			return;
		case duckdb::LogicalTypeId::UNION:
			*out_count = duckdb::UnionType::GetMemberCount(vec->GetType()) + 1;
			return;
		default:
			*out_count = 0;
			return;
		}
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_get_child(duckdb_v2_vector_handle vector, idx_t index,
                                           duckdb_v2_vector_handle *out_child, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_child);
	*out_child = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		switch (vec->GetType().id()) {
		case duckdb::LogicalTypeId::LIST: {
			if (index != 0) {
				throw duckdb::InvalidInputException("duckdb_v2_vector_get_child: LIST has only child [0] (elements)");
			}
			auto &child = duckdb::ListVector::GetChildMutable(*vec);
			*out_child = Convert(&child);
			return;
		}
		case duckdb::LogicalTypeId::MAP: {
			if (index != 0) {
				throw duckdb::InvalidInputException(
				    "duckdb_v2_vector_get_child: MAP has only child [0] (entries, a STRUCT(key, value))");
			}
			auto &entries = duckdb::ListVector::GetChildMutable(*vec);
			*out_child = Convert(&entries);
			return;
		}
		case duckdb::LogicalTypeId::ARRAY: {
			if (index != 0) {
				throw duckdb::InvalidInputException("duckdb_v2_vector_get_child: ARRAY has only child [0] (elements)");
			}
			auto &child = duckdb::ArrayVector::GetChildMutable(*vec);
			*out_child = Convert(&child);
			return;
		}
		case duckdb::LogicalTypeId::STRUCT:
		case duckdb::LogicalTypeId::TUPLE: {
			auto &entries = duckdb::StructVector::GetEntries(*vec);
			if (index >= entries.size()) {
				throw duckdb::InvalidInputException("duckdb_v2_vector_get_child: field index out of range");
			}
			*out_child = Convert(&entries[index]);
			return;
		}
		case duckdb::LogicalTypeId::UNION: {
			// Child [0] is the tag vector; children [1..N] are the
			// member vectors.
			if (index == 0) {
				auto &tags = duckdb::UnionVector::GetTags(*vec);
				*out_child = Convert(&tags);
				return;
			}
			// Compute the member-space index first; bounds-check that
			// directly against GetMemberCount. Mixing the child-space
			// index with the member-space count is too easy to get
			// off-by-one wrong on later edits.
			idx_t member_idx = index - 1;
			if (member_idx >= duckdb::UnionType::GetMemberCount(vec->GetType())) {
				throw duckdb::InvalidInputException("duckdb_v2_vector_get_child: UNION member index out of range");
			}
			auto &member = duckdb::UnionVector::GetMember(*vec, member_idx);
			*out_child = Convert(&member);
			return;
		}
		default:
			throw duckdb::InvalidInputException("duckdb_v2_vector_get_child: vector has no children");
		}
	});
}

// ---------------------------------------------------------------------------
// Generic row-count
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_get_size(duckdb_v2_vector_handle vector, idx_t *out_size,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_size);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		*out_size = vec->size();
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_set_size(duckdb_v2_vector_handle vector, idx_t size,
                                          duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		// Grow the underlying buffer first so the new logical size fits; constant vectors
		// carry a single physical element and must not be reserved against.
		if (vec->GetVectorType() == duckdb::VectorType::FLAT_VECTOR) {
			vec->Reserve(size);
		}
		duckdb::FlatVector::SetSize(*vec, size);
	});
}

// ---------------------------------------------------------------------------
// Single-cell value bridge
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_vector_get_value(duckdb_v2_vector_handle vector, idx_t row, duckdb_v2_value_handle *out_value,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(out_value);
	*out_value = nullptr;
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		if (row >= vec->size()) {
			throw duckdb::InvalidInputException("row out of range in duckdb_v2_vector_get_value");
		}
		// Buffer-dispatched read: total over vector representations (constant
		// clamps, dictionary resolves through its selection vector).
		*out_value = Convert(new duckdb::Value(vec->GetValue(row)));
	});
}

DUCKDB_V2_ERROR duckdb_v2_vector_set_value(duckdb_v2_vector_handle vector, idx_t row, duckdb_v2_value_handle value,
                                           duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(vector);
	DUCKDB_CHECK_ARG(value);
	return WithErrorHandler(err, [&]() {
		auto *vec = Convert(vector);
		if (vec->GetVectorType() != duckdb::VectorType::FLAT_VECTOR) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_vector_set_value requires a FLAT vector; call duckdb_v2_vector_flatten first");
		}
		if (row >= vec->size()) {
			throw duckdb::InvalidInputException("row out of range in duckdb_v2_vector_set_value");
		}
		// The buffer casts the value to the vector's type; cast failures propagate.
		vec->SetValue(row, *Convert(value));
	});
}

// ---------------------------------------------------------------------------
// String-backed kind decoders
//
// duckdb_v2_bytes (and its bit/bignum aliases) is the transparent
// 16-byte public storage type; the static_asserts in
// capi_v2_internal.hpp pin its layout to duckdb::string_t, so the
// reinterpret_casts here are guarded. Only BIGNUM keeps a C codec pair
// (its storage carries a header and stores negatives bit-inverted); BIT
// is a trivial client-side split and VARCHAR / BLOB reads are direct
// field reads on the transparent type.
//
// Both directions take plain byte ranges rather than a handle, so the
// same pair serves vector payloads and value payloads.
// ---------------------------------------------------------------------------

DUCKDB_V2_ERROR duckdb_v2_bignum_decode(const uint8_t *in_data, idx_t in_length, uint8_t *out_data, idx_t out_capacity,
                                        idx_t *out_length, bool *out_is_negative, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_length);
	DUCKDB_CHECK_ARG(out_is_negative);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		*out_is_negative = false;
		if (!in_data || in_length <= duckdb::Bignum::BIGNUM_HEADER_SIZE) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_bignum_decode requires at least %llu bytes of BIGNUM storage",
			    static_cast<uint64_t>(duckdb::Bignum::BIGNUM_HEADER_SIZE) + 1);
		}
		// The header's high bit is clear for negatives, which also store the
		// magnitude bit-inverted. Inverting is byte-wise, so the magnitude is
		// always exactly the payload length.
		const bool is_negative = (in_data[0] & 0x80) == 0;
		const idx_t magnitude_length = in_length - duckdb::Bignum::BIGNUM_HEADER_SIZE;
		*out_is_negative = is_negative;
		FillCallerBuffer(out_data, out_capacity, out_length, magnitude_length, "duckdb_v2_bignum_decode",
		                 [&](uint8_t *dst) {
			                 const auto *payload = in_data + duckdb::Bignum::BIGNUM_HEADER_SIZE;
			                 for (idx_t i = 0; i < magnitude_length; i++) {
				                 dst[i] = is_negative ? static_cast<uint8_t>(~payload[i]) : payload[i];
			                 }
		                 });
	});
}

DUCKDB_V2_ERROR duckdb_v2_bignum_encode(const uint8_t *in_data, idx_t in_length, bool is_negative, uint8_t *out_data,
                                        idx_t out_capacity, idx_t *out_length, duckdb_v2_error_info_handle *err) {
	DUCKDB_CHECK_ARG(out_length);
	return WithErrorHandler(err, [&]() {
		*out_length = 0;
		if (!in_data || in_length == 0) {
			throw duckdb::InvalidInputException("duckdb_v2_bignum_encode requires in_data != NULL and in_length >= 1");
		}
		if (in_length > duckdb::Bignum::MAX_DATA_SIZE) {
			throw duckdb::OutOfRangeException("duckdb_v2_bignum_encode: magnitude of %llu bytes exceeds the maximum "
			                                  "BIGNUM width of %llu bytes",
			                                  static_cast<uint64_t>(in_length),
			                                  static_cast<uint64_t>(duckdb::Bignum::MAX_DATA_SIZE));
		}
		// Canonical magnitude, matching what decode produces: no leading zero
		// byte, and zero is the single byte 0x00 with a positive sign.
		if (in_length > 1 && in_data[0] == 0) {
			throw duckdb::InvalidInputException(
			    "duckdb_v2_bignum_encode requires a magnitude with no leading zero bytes");
		}
		if (is_negative && in_length == 1 && in_data[0] == 0) {
			throw duckdb::InvalidInputException("duckdb_v2_bignum_encode cannot encode negative zero");
		}
		const idx_t storage_length = in_length + duckdb::Bignum::BIGNUM_HEADER_SIZE;
		FillCallerBuffer(out_data, out_capacity, out_length, storage_length, "duckdb_v2_bignum_encode",
		                 [&](uint8_t *dst) {
			                 duckdb::Bignum::SetHeader(reinterpret_cast<char *>(dst), in_length, is_negative);
			                 auto *payload = dst + duckdb::Bignum::BIGNUM_HEADER_SIZE;
			                 for (idx_t i = 0; i < in_length; i++) {
				                 payload[i] = is_negative ? static_cast<uint8_t>(~in_data[i]) : in_data[i];
			                 }
		                 });
	});
}
