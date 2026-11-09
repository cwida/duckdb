//===--------------------------------------------------------------------===//
// hash.cpp
// Description: This file contains the vectorized hash implementations
//===--------------------------------------------------------------------===//

#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/dictionary_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/types/null_value.hpp"
#include "duckdb/common/uhugeint.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"

namespace duckdb {

namespace {

struct HashOp {
	static const hash_t NULL_HASH = 0xbf58476d1ce4e5b9;

	template <class T>
	static hash_t Operation(T input, bool is_null) {
		return is_null ? NULL_HASH : duckdb::Hash<T>(input);
	}
};

struct CachedHashOp {
	template <class T>
	static hash_t Operation(T) {
		throw InternalException("CachedHashOp::Operation called on non-hash input");
	}
};

template <>
hash_t CachedHashOp::Operation(hash_t input) {
	return input;
}

hash_t CombineHashScalar(hash_t a, hash_t b) {
	a ^= a >> 32;
	a *= 0xd6e8feb86659fd93U;
	return a ^ b;
}

template <bool HAS_RSEL, bool HAS_SEL_VECTOR, class T, bool INPUT_IS_ALREADY_HASH>
void TightLoopHash(const T *__restrict ldata, hash_t *__restrict result_data, const SelectionVector *rsel, idx_t count,
                   const SelectionVector *__restrict sel_vector, const ValidityMask &mask) {
	if (mask.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index_unsafe(i) : i;
			auto idx = HAS_SEL_VECTOR ? sel_vector->get_index_unsafe(ridx) : ridx;
			result_data[ridx] = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx])
			                                          : HashOp::Operation(ldata[idx], !mask.RowIsValidUnsafe(idx));
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index_unsafe(i) : i;
			auto idx = HAS_SEL_VECTOR ? sel_vector->get_index_unsafe(ridx) : ridx;
			result_data[ridx] =
			    INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx]) : duckdb::Hash<T>(ldata[idx]);
		}
	}
}

template <bool HAS_RSEL, class T, bool INPUT_IS_ALREADY_HASH = false>
void TemplatedLoopHash(const Vector &input, Vector &result, const SelectionVector *rsel, idx_t count) {
	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		FlatVector::SetSize(result, count_t(count));

		auto ldata = ConstantVector::GetData<T>(input);
		auto result_data = ConstantVector::GetData<hash_t>(result);
		*result_data = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(*ldata)
		                                     : HashOp::Operation(*ldata, ConstantVector::IsNull(input));
	} else {
		result.SetVectorType(VectorType::FLAT_VECTOR);
		FlatVector::SetSize(result, count_t(count));

		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);

		if (idata.sel->IsSet()) {
			TightLoopHash<HAS_RSEL, true, T, INPUT_IS_ALREADY_HASH>(UnifiedVectorFormat::GetData<T>(idata),
			                                                        FlatVector::GetDataMutable<hash_t>(result), rsel,
			                                                        count, idata.sel, idata.validity);
		} else {
			TightLoopHash<HAS_RSEL, false, T, INPUT_IS_ALREADY_HASH>(UnifiedVectorFormat::GetData<T>(idata),
			                                                         FlatVector::GetDataMutable<hash_t>(result), rsel,
			                                                         count, idata.sel, idata.validity);
		}
	}
}

template <bool HAS_RSEL, bool FIRST_HASH>
void StructLoopHash(const Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
	const auto &children = StructVector::GetEntries(input);

	if (children.empty()) {
		// an empty struct has no fields to hash: for the first hash every row gets the same constant value.
		if (FIRST_HASH) {
			hashes.SetVectorType(VectorType::CONSTANT_VECTOR);
			FlatVector::SetSize(hashes, count_t(count));
			*ConstantVector::GetData<hash_t>(hashes) = 0x9e3779b97f4a7c15ULL; // some (arbitrary) constant
		}
		return;
	}
	idx_t col_no = 0;
	if (HAS_RSEL) {
		if (FIRST_HASH) {
			VectorOperations::Hash(children[col_no++], hashes, *rsel, count);
		} else {
			VectorOperations::CombineHash(hashes, children[col_no++], *rsel, count);
		}
		while (col_no < children.size()) {
			VectorOperations::CombineHash(hashes, children[col_no++], *rsel, count);
		}
	} else {
		if (FIRST_HASH) {
			VectorOperations::Hash(children[col_no++], hashes, count);
		} else {
			VectorOperations::CombineHash(hashes, children[col_no++], count);
		}
		while (col_no < children.size()) {
			VectorOperations::CombineHash(hashes, children[col_no++], count);
		}
	}
}

template <bool HAS_RSEL>
hash_t *FlattenHashesForCombine(Vector &hashes, const SelectionVector *rsel, idx_t count) {
	const bool is_constant = hashes.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const hash_t constant_hash = is_constant ? *ConstantVector::GetData<hash_t>(hashes) : 0;
	hashes.Flatten();
	FlatVector::SetSize(hashes, count_t(count));
	auto hdata = FlatVector::GetDataMutable<hash_t>(hashes);
	if (is_constant) {
		// Flatten fills only the first Size() slots, but a selection reads and writes the hashes at its own positions.
		for (idx_t i = 0; i < count; i++) {
			hdata[HAS_RSEL ? rsel->get_index(i) : i] = constant_hash;
		}
	}
	return hdata;
}

template <bool HAS_RSEL, bool FIRST_HASH>
void ListLoopHash(const Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
	// FIXME: if we want to be more efficient we shouldn't flatten, but the logic here currently requires it
	auto hdata = FlattenHashesForCombine<HAS_RSEL>(hashes, rsel, count);

	UnifiedVectorFormat idata;
	input.ToUnifiedFormat(idata);
	const auto ldata = UnifiedVectorFormat::GetData<list_entry_t>(idata);

	// Hash the children into a temporary
	const auto &child = ListVector::GetChild(input);
	const auto child_count = ListVector::GetListSize(input);

	Vector child_hashes(LogicalType::HASH, child_count);
	if (child_count > 0) {
		VectorOperations::Hash(child, child_hashes, child_count);
		child_hashes.Flatten();
	}
	auto chdata = FlatVector::GetDataMutable<hash_t>(child_hashes);

	// Reduce the number of entries to check to the non-empty ones
	SelectionVector unprocessed(count);
	SelectionVector cursor(HAS_RSEL ? STANDARD_VECTOR_SIZE : count);
	idx_t remaining = 0;
	for (idx_t i = 0; i < count; ++i) {
		const idx_t ridx = HAS_RSEL ? rsel->get_index(i) : i;
		const auto lidx = idata.sel->get_index(ridx);
		const auto &entry = ldata[lidx];
		if (idata.validity.RowIsValid(lidx) && entry.length > 0) {
			unprocessed.set_index(remaining++, ridx);
			cursor.set_index(ridx, entry.offset);
		} else if (FIRST_HASH) {
			hdata[ridx] = HashOp::NULL_HASH;
		}
		// Empty or NULL non-first elements have no effect.
	}

	count = remaining;
	if (count == 0) {
		return;
	}

	// Merge the first position hash into the main hash
	idx_t position = 1;
	if (FIRST_HASH) {
		remaining = 0;
		for (idx_t i = 0; i < count; ++i) {
			const auto ridx = unprocessed.get_index(i);
			const auto cidx = cursor.get_index(ridx);
			hdata[ridx] = chdata[cidx];

			const auto lidx = idata.sel->get_index(ridx);
			const auto &entry = ldata[lidx];
			if (entry.length > position) {
				// Entry still has values to hash
				unprocessed.set_index(remaining++, ridx);
				cursor.set_index(ridx, cidx + 1);
			}
		}
		count = remaining;
		if (count == 0) {
			return;
		}
		++position;
	}

	// Combine the hashes for the remaining positions until there are none left
	for (;; ++position) {
		remaining = 0;
		for (idx_t i = 0; i < count; ++i) {
			const auto ridx = unprocessed.get_index(i);
			const auto cidx = cursor.get_index(ridx);
			hdata[ridx] = CombineHashScalar(hdata[ridx], chdata[cidx]);

			const auto lidx = idata.sel->get_index(ridx);
			const auto &entry = ldata[lidx];
			if (entry.length > position) {
				// Entry still has values to hash
				unprocessed.set_index(remaining++, ridx);
				cursor.set_index(ridx, cidx + 1);
			}
		}

		count = remaining;
		if (count == 0) {
			break;
		}
	}
}

template <bool HAS_RSEL, bool FIRST_HASH>
void ArrayLoopHash(const Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
	auto hdata = FlattenHashesForCombine<HAS_RSEL>(hashes, rsel, count);

	UnifiedVectorFormat idata;
	input.ToUnifiedFormat(idata);

	// Hash the children into a temporary
	const auto &child = ArrayVector::GetChild(input);
	auto array_size = ArrayType::GetSize(input.GetType());

	auto is_flat = input.GetVectorType() == VectorType::FLAT_VECTOR;
	auto is_constant = input.GetVectorType() == VectorType::CONSTANT_VECTOR;

	if (!HAS_RSEL && (is_flat || is_constant)) {
		// Fast path for contiguous vectors with no selection vector
		auto child_count = array_size * (is_constant ? 1 : count);

		Vector child_hashes(LogicalType::HASH, child_count);
		VectorOperations::Hash(child, child_hashes, child_count);
		child_hashes.Flatten();
		auto chdata = FlatVector::GetDataMutable<hash_t>(child_hashes);

		for (idx_t i = 0; i < count; i++) {
			auto lidx = idata.sel->get_index(i);
			if (idata.validity.RowIsValid(lidx)) {
				if (FIRST_HASH) {
					hdata[i] = 0;
				}
				for (idx_t j = 0; j < array_size; j++) {
					auto offset = lidx * array_size + j;
					hdata[i] = CombineHashScalar(hdata[i], chdata[offset]);
				}
			} else if (FIRST_HASH) {
				hdata[i] = HashOp::NULL_HASH;
			}
		}
	} else {
		// Hash the arrays one-by-one
		SelectionVector array_sel(array_size);
		Vector array_hashes(LogicalType::HASH, array_size);
		for (idx_t i = 0; i < count; i++) {
			const auto ridx = HAS_RSEL ? rsel->get_index(i) : i;
			const auto lidx = idata.sel->get_index(ridx);

			if (idata.validity.RowIsValid(lidx)) {
				// Create a selection vector for the array
				for (idx_t j = 0; j < array_size; j++) {
					array_sel.set_index(j, lidx * array_size + j);
				}

				// Hash the array slice
				Vector dict_vec(child, array_sel, array_size);
				VectorOperations::Hash(dict_vec, array_hashes, array_size);
				auto ahdata = FlatVector::GetDataMutable<hash_t>(array_hashes);

				if (FIRST_HASH) {
					hdata[ridx] = 0;
				}
				// Combine the hashes of the array
				for (idx_t j = 0; j < array_size; j++) {
					hdata[ridx] = CombineHashScalar(hdata[ridx], ahdata[j]);
					// Clear the hash for the next iteration
					ahdata[j] = 0;
				}
			} else if (FIRST_HASH) {
				hdata[ridx] = HashOp::NULL_HASH;
			}
		}
	}
}

template <bool HAS_RSEL>
void HashTypeSwitch(const Vector &input, Vector &result, const SelectionVector *rsel, idx_t count) {
	D_ASSERT(result.GetType().id() == LogicalType::HASH);
	switch (input.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedLoopHash<HAS_RSEL, int8_t>(input, result, rsel, count);
		break;
	case PhysicalType::INT16:
		TemplatedLoopHash<HAS_RSEL, int16_t>(input, result, rsel, count);
		break;
	case PhysicalType::INT32:
		TemplatedLoopHash<HAS_RSEL, int32_t>(input, result, rsel, count);
		break;
	case PhysicalType::INT64:
		TemplatedLoopHash<HAS_RSEL, int64_t>(input, result, rsel, count);
		break;
	case PhysicalType::UINT8:
		TemplatedLoopHash<HAS_RSEL, uint8_t>(input, result, rsel, count);
		break;
	case PhysicalType::UINT16:
		TemplatedLoopHash<HAS_RSEL, uint16_t>(input, result, rsel, count);
		break;
	case PhysicalType::UINT32:
		TemplatedLoopHash<HAS_RSEL, uint32_t>(input, result, rsel, count);
		break;
	case PhysicalType::UINT64:
		TemplatedLoopHash<HAS_RSEL, uint64_t>(input, result, rsel, count);
		break;
	case PhysicalType::INT128:
		TemplatedLoopHash<HAS_RSEL, hugeint_t>(input, result, rsel, count);
		break;
	case PhysicalType::UINT128:
		TemplatedLoopHash<HAS_RSEL, uhugeint_t>(input, result, rsel, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedLoopHash<HAS_RSEL, float>(input, result, rsel, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedLoopHash<HAS_RSEL, double>(input, result, rsel, count);
		break;
	case PhysicalType::INTERVAL:
		TemplatedLoopHash<HAS_RSEL, interval_t>(input, result, rsel, count);
		break;
	case PhysicalType::VARCHAR:
		TemplatedLoopHash<HAS_RSEL, string_t>(input, result, rsel, count);
		break;
	case PhysicalType::STRUCT:
		StructLoopHash<HAS_RSEL, true>(input, result, rsel, count);
		break;
	case PhysicalType::LIST:
		ListLoopHash<HAS_RSEL, true>(input, result, rsel, count);
		break;
	case PhysicalType::ARRAY:
		ArrayLoopHash<HAS_RSEL, true>(input, result, rsel, count);
		break;
	default:
		throw InvalidTypeException(input.GetType(), "Invalid type for hash");
	}
}

template <bool HAS_RSEL, class T, bool INPUT_IS_ALREADY_HASH>
void TightLoopCombineHashConstant(const T *__restrict ldata, hash_t constant_hash, hash_t *__restrict hash_data,
                                  const SelectionVector *rsel, idx_t count,
                                  const SelectionVector *__restrict sel_vector, const ValidityMask &mask) {
	if (mask.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index(i) : i;
			auto idx = sel_vector->get_index(ridx);
			auto other_hash = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx])
			                                        : HashOp::Operation(ldata[idx], !mask.RowIsValid(idx));
			hash_data[ridx] = CombineHashScalar(constant_hash, other_hash);
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index(i) : i;
			auto idx = sel_vector->get_index(ridx);
			auto other_hash = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx]) : duckdb::Hash<T>(ldata[idx]);
			hash_data[ridx] = CombineHashScalar(constant_hash, other_hash);
		}
	}
}

template <bool HAS_RSEL, bool HAS_SEL, class T, bool INPUT_IS_ALREADY_HASH>
static inline void TightLoopCombineHash(const T *__restrict ldata, hash_t *__restrict const hash_data,
                                        const SelectionVector *__restrict const rsel, const idx_t count,
                                        const SelectionVector *__restrict const sel_vector, const ValidityMask &mask) {
	if (mask.CanHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index_unsafe(i) : i;
			auto idx = HAS_SEL ? sel_vector->get_index_unsafe(ridx) : ridx;
			auto other_hash = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx])
			                                        : HashOp::Operation(ldata[idx], !mask.RowIsValid(idx));
			hash_data[ridx] = CombineHashScalar(hash_data[ridx], other_hash);
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto ridx = HAS_RSEL ? rsel->get_index_unsafe(i) : i;
			auto idx = HAS_SEL ? sel_vector->get_index_unsafe(ridx) : ridx;
			auto other_hash = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(ldata[idx]) : duckdb::Hash<T>(ldata[idx]);
			hash_data[ridx] = CombineHashScalar(hash_data[ridx], other_hash);
		}
	}
}

template <bool HAS_RSEL, class T, bool INPUT_IS_ALREADY_HASH = false>
void TemplatedLoopCombineHash(const Vector &input, Vector &hashes, const SelectionVector *rsel, idx_t count) {
	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR && hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		auto ldata = ConstantVector::GetData<T>(input);
		auto hash_data = ConstantVector::GetData<hash_t>(hashes);

		auto other_hash = INPUT_IS_ALREADY_HASH ? CachedHashOp::Operation(*ldata)
		                                        : HashOp::Operation(*ldata, ConstantVector::IsNull(input));
		*hash_data = CombineHashScalar(*hash_data, other_hash);
	} else {
		UnifiedVectorFormat idata;
		input.ToUnifiedFormat(idata);
		if (hashes.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// mix constant with non-constant, first get the constant value
			auto constant_hash = *ConstantVector::GetData<hash_t>(hashes);
			// now re-initialize the hashes vector to an empty flat vector
			hashes.SetVectorType(VectorType::FLAT_VECTOR);
			FlatVector::SetSize(hashes, count_t(count));
			TightLoopCombineHashConstant<HAS_RSEL, T, INPUT_IS_ALREADY_HASH>(
			    UnifiedVectorFormat::GetData<T>(idata), constant_hash, FlatVector::GetDataMutable<hash_t>(hashes), rsel,
			    count, idata.sel, idata.validity);
		} else {
			D_ASSERT(hashes.GetVectorType() == VectorType::FLAT_VECTOR);
			if (idata.sel->IsSet()) {
				TightLoopCombineHash<HAS_RSEL, true, T, INPUT_IS_ALREADY_HASH>(
				    UnifiedVectorFormat::GetData<T>(idata), FlatVector::GetDataMutable<hash_t>(hashes), rsel, count,
				    idata.sel, idata.validity);
			} else {
				TightLoopCombineHash<HAS_RSEL, false, T, INPUT_IS_ALREADY_HASH>(
				    UnifiedVectorFormat::GetData<T>(idata), FlatVector::GetDataMutable<hash_t>(hashes), rsel, count,
				    idata.sel, idata.validity);
			}
		}
	}
}

template <bool HAS_RSEL>
void CombineHashTypeSwitch(Vector &hashes, const Vector &input, const SelectionVector *rsel, idx_t count) {
	D_ASSERT(hashes.GetType().id() == LogicalType::HASH);
	switch (input.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedLoopCombineHash<HAS_RSEL, int8_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::INT16:
		TemplatedLoopCombineHash<HAS_RSEL, int16_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::INT32:
		TemplatedLoopCombineHash<HAS_RSEL, int32_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::INT64:
		TemplatedLoopCombineHash<HAS_RSEL, int64_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::UINT8:
		TemplatedLoopCombineHash<HAS_RSEL, uint8_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::UINT16:
		TemplatedLoopCombineHash<HAS_RSEL, uint16_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::UINT32:
		TemplatedLoopCombineHash<HAS_RSEL, uint32_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::UINT64:
		TemplatedLoopCombineHash<HAS_RSEL, uint64_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::INT128:
		TemplatedLoopCombineHash<HAS_RSEL, hugeint_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::UINT128:
		TemplatedLoopCombineHash<HAS_RSEL, uhugeint_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedLoopCombineHash<HAS_RSEL, float>(input, hashes, rsel, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedLoopCombineHash<HAS_RSEL, double>(input, hashes, rsel, count);
		break;
	case PhysicalType::INTERVAL:
		TemplatedLoopCombineHash<HAS_RSEL, interval_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::VARCHAR:
		TemplatedLoopCombineHash<HAS_RSEL, string_t>(input, hashes, rsel, count);
		break;
	case PhysicalType::STRUCT:
		StructLoopHash<HAS_RSEL, false>(input, hashes, rsel, count);
		break;
	case PhysicalType::LIST:
		ListLoopHash<HAS_RSEL, false>(input, hashes, rsel, count);
		break;
	case PhysicalType::ARRAY:
		ArrayLoopHash<HAS_RSEL, false>(input, hashes, rsel, count);
		break;
	default:
		throw InvalidTypeException(input.GetType(), "Invalid type for hash");
	}
}

} // namespace

void VectorOperations::Hash(const Vector &input, Vector &result, idx_t count) {
	if (input.GetVectorType() == VectorType::DICTIONARY_VECTOR && DictionaryVector::CanCacheHashes(input)) {
		Vector input_hashes(DictionaryVector::GetCachedHashes(input), DictionaryVector::SelVector(input), count);
		TemplatedLoopHash<false, hash_t, true>(input_hashes, result, nullptr, count);
	} else {
		HashTypeSwitch<false>(input, result, nullptr, count);
	}
}

void VectorOperations::Hash(const Vector &input, Vector &result, const SelectionVector &sel, idx_t count) {
	if (input.GetVectorType() == VectorType::DICTIONARY_VECTOR && DictionaryVector::CanCacheHashes(input)) {
		Vector input_hashes(DictionaryVector::GetCachedHashes(input), DictionaryVector::SelVector(input), count);
		TemplatedLoopHash<true, hash_t, true>(input_hashes, result, &sel, count);
	} else {
		HashTypeSwitch<true>(input, result, &sel, count);
	}
}

void VectorOperations::Hash(const Vector &input, Vector &result) {
	Hash(input, result, input.size());
}

void VectorOperations::CombineHash(Vector &hashes, const Vector &input, idx_t count) {
	if (input.GetVectorType() == VectorType::DICTIONARY_VECTOR && DictionaryVector::CanCacheHashes(input)) {
		Vector input_hashes(DictionaryVector::GetCachedHashes(input), DictionaryVector::SelVector(input), count);
		TemplatedLoopCombineHash<false, hash_t, true>(input_hashes, hashes, nullptr, count);
	} else {
		CombineHashTypeSwitch<false>(hashes, input, nullptr, count);
	}
}

void VectorOperations::CombineHash(Vector &hashes, const Vector &input) {
	const bool hashes_const = hashes.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const bool input_const = input.GetVectorType() == VectorType::CONSTANT_VECTOR;
	if (!hashes_const && !input_const && hashes.size() != input.size()) {
		throw InternalException("Mismatch in input vector sizes for CombineHash - hashes has %d rows but input has %d",
		                        hashes.size(), input.size());
	}
	CombineHash(hashes, input, hashes_const ? input.size() : hashes.size());
}

void VectorOperations::CombineHash(Vector &hashes, const Vector &input, const SelectionVector &rsel, idx_t count) {
	if (input.GetVectorType() == VectorType::DICTIONARY_VECTOR && DictionaryVector::CanCacheHashes(input)) {
		Vector input_hashes(DictionaryVector::GetCachedHashes(input), DictionaryVector::SelVector(input), count);
		TemplatedLoopCombineHash<true, hash_t, true>(input_hashes, hashes, &rsel, count);
	} else {
		CombineHashTypeSwitch<true>(hashes, input, &rsel, count);
	}
}

} // namespace duckdb
