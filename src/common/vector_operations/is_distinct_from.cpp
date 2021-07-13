#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"

namespace duckdb {

struct DistinctBinaryLambdaWrapper {
	template <class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static inline RESULT_TYPE Operation(LEFT_TYPE left, RIGHT_TYPE right, bool is_left_null, bool is_right_null) {
		return OP::template Operation<LEFT_TYPE>(left, right, is_left_null, is_right_null);
	}
};

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
static void DistinctExecuteGenericLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                       RESULT_TYPE *__restrict result_data, const SelectionVector *__restrict lsel,
                                       const SelectionVector *__restrict rsel, idx_t count, ValidityMask &lmask,
                                       ValidityMask &rmask, ValidityMask &result_mask) {
	for (idx_t i = 0; i < count; i++) {
		auto lindex = lsel->get_index(i);
		auto rindex = rsel->get_index(i);
		auto lentry = ldata[lindex];
		auto rentry = rdata[rindex];
		result_data[i] =
		    OP::template Operation<LEFT_TYPE>(lentry, rentry, !lmask.RowIsValid(lindex), !rmask.RowIsValid(rindex));
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
static void DistinctExecuteConstant(Vector &left, Vector &right, Vector &result) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);

	auto ldata = ConstantVector::GetData<LEFT_TYPE>(left);
	auto rdata = ConstantVector::GetData<RIGHT_TYPE>(right);
	auto result_data = ConstantVector::GetData<RESULT_TYPE>(result);
	*result_data =
	    OP::template Operation<LEFT_TYPE>(*ldata, *rdata, ConstantVector::IsNull(left), ConstantVector::IsNull(right));
}

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
static void DistinctExecuteGeneric(Vector &left, Vector &right, Vector &result, idx_t count) {
	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		DistinctExecuteConstant<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OP>(left, right, result);
	} else {
		VectorData ldata, rdata;

		left.Orrify(count, ldata);
		right.Orrify(count, rdata);

		result.SetVectorType(VectorType::FLAT_VECTOR);
		auto result_data = FlatVector::GetData<RESULT_TYPE>(result);
		DistinctExecuteGenericLoop<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OP>(
		    (LEFT_TYPE *)ldata.data, (RIGHT_TYPE *)rdata.data, result_data, ldata.sel, rdata.sel, count, ldata.validity,
		    rdata.validity, FlatVector::Validity(result));
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
static void DistinctExecuteSwitch(Vector &left, Vector &right, Vector &result, idx_t count) {
	DistinctExecuteGeneric<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OP>(left, right, result, count);
}

template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE, class OP>
static void DistinctExecute(Vector &left, Vector &right, Vector &result, idx_t count) {
	DistinctExecuteSwitch<LEFT_TYPE, RIGHT_TYPE, RESULT_TYPE, OP>(left, right, result, count);
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool NO_NULL, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL>
static inline idx_t
DistinctSelectGenericLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                          const SelectionVector *__restrict lsel, const SelectionVector *__restrict rsel,
                          const SelectionVector *__restrict result_sel, idx_t count, ValidityMask &lmask,
                          ValidityMask &rmask, SelectionVector *true_sel, SelectionVector *false_sel) {
	idx_t true_count = 0, false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		auto result_idx = result_sel->get_index(i);
		const auto idx = DENSE ? i : result_idx;
		auto lindex = lsel->get_index(idx);
		auto rindex = rsel->get_index(idx);
		if (NO_NULL) {
			if (OP::Operation(ldata[lindex], rdata[rindex], false, false)) {
				if (HAS_TRUE_SEL) {
					true_sel->set_index(true_count++, result_idx);
				}
			} else {
				if (HAS_FALSE_SEL) {
					false_sel->set_index(false_count++, result_idx);
				}
			}
		} else {
			if (OP::Operation(ldata[lindex], rdata[rindex], !lmask.RowIsValid(lindex), !rmask.RowIsValid(rindex))) {
				if (HAS_TRUE_SEL) {
					true_sel->set_index(true_count++, result_idx);
				}
			} else {
				if (HAS_FALSE_SEL) {
					false_sel->set_index(false_count++, result_idx);
				}
			}
		}
	}
	if (HAS_TRUE_SEL) {
		return true_count;
	} else {
		return count - false_count;
	}
}
template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool NO_NULL>
static inline idx_t
DistinctSelectGenericLoopSelSwitch(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                   const SelectionVector *__restrict lsel, const SelectionVector *__restrict rsel,
                                   const SelectionVector *__restrict result_sel, idx_t count, ValidityMask &lmask,
                                   ValidityMask &rmask, SelectionVector *true_sel, SelectionVector *false_sel) {
	if (true_sel && false_sel) {
		return DistinctSelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, NO_NULL, true, true>(
		    ldata, rdata, lsel, rsel, result_sel, count, lmask, rmask, true_sel, false_sel);
	} else if (true_sel) {
		return DistinctSelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, NO_NULL, true, false>(
		    ldata, rdata, lsel, rsel, result_sel, count, lmask, rmask, true_sel, false_sel);
	} else {
		D_ASSERT(false_sel);
		return DistinctSelectGenericLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, NO_NULL, false, true>(
		    ldata, rdata, lsel, rsel, result_sel, count, lmask, rmask, true_sel, false_sel);
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE>
static inline idx_t
DistinctSelectGenericLoopSwitch(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                const SelectionVector *__restrict lsel, const SelectionVector *__restrict rsel,
                                const SelectionVector *__restrict result_sel, idx_t count, ValidityMask &lmask,
                                ValidityMask &rmask, SelectionVector *true_sel, SelectionVector *false_sel) {
	if (!lmask.AllValid() || rmask.AllValid()) {
		return DistinctSelectGenericLoopSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, false>(
		    ldata, rdata, lsel, rsel, result_sel, count, lmask, rmask, true_sel, false_sel);
	} else {
		return DistinctSelectGenericLoopSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, true>(
		    ldata, rdata, lsel, rsel, result_sel, count, lmask, rmask, true_sel, false_sel);
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE>
static idx_t DistinctSelectGeneric(Vector &left, Vector &right, idx_t vcount, const SelectionVector *sel, idx_t count,
                                   SelectionVector *true_sel, SelectionVector *false_sel) {
	VectorData ldata, rdata;

	left.Orrify(vcount, ldata);
	right.Orrify(vcount, rdata);

	return DistinctSelectGenericLoopSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE>(
	    (LEFT_TYPE *)ldata.data, (RIGHT_TYPE *)rdata.data, ldata.sel, rdata.sel, sel, count, ldata.validity,
	    rdata.validity, true_sel, false_sel);
}
template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool LEFT_CONSTANT, bool RIGHT_CONSTANT,
          bool NO_NULL, bool HAS_TRUE_SEL, bool HAS_FALSE_SEL>
static inline idx_t DistinctSelectFlatLoop(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                           const SelectionVector *sel, idx_t count, ValidityMask &lmask,
                                           ValidityMask &rmask, SelectionVector *true_sel, SelectionVector *false_sel) {
	idx_t true_count = 0, false_count = 0;
	for (idx_t i = 0; i < count; i++) {
		idx_t result_idx = sel->get_index(i);
		const auto idx = DENSE ? i : result_idx;
		idx_t lidx = LEFT_CONSTANT ? 0 : idx;
		idx_t ridx = RIGHT_CONSTANT ? 0 : idx;
		const bool lnull = !lmask.RowIsValid(lidx);
		const bool rnull = !rmask.RowIsValid(ridx);
		bool comparison_result = OP::Operation(ldata[lidx], rdata[ridx], lnull, rnull);
		if (HAS_TRUE_SEL) {
			true_sel->set_index(true_count, result_idx);
			true_count += comparison_result;
		}
		if (HAS_FALSE_SEL) {
			false_sel->set_index(false_count, result_idx);
			false_count += !comparison_result;
		}
	}
	if (HAS_TRUE_SEL) {
		return true_count;
	} else {
		return count - false_count;
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool LEFT_CONSTANT, bool RIGHT_CONSTANT,
          bool NO_NULL>
static inline idx_t DistinctSelectFlatLoopSelSwitch(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                                    const SelectionVector *sel, idx_t count, ValidityMask &lmask,
                                                    ValidityMask &rmask, SelectionVector *true_sel,
                                                    SelectionVector *false_sel) {
	if (true_sel && false_sel) {
		return DistinctSelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT, NO_NULL, true,
		                              true>(ldata, rdata, sel, count, lmask, rmask, true_sel, false_sel);
	} else if (true_sel) {
		return DistinctSelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT, NO_NULL, true,
		                              false>(ldata, rdata, sel, count, lmask, rmask, true_sel, false_sel);
	} else {
		D_ASSERT(false_sel);
		return DistinctSelectFlatLoop<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT, NO_NULL, false,
		                              true>(ldata, rdata, sel, count, lmask, rmask, true_sel, false_sel);
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
static inline idx_t DistinctSelectFlatLoopSwitch(LEFT_TYPE *__restrict ldata, RIGHT_TYPE *__restrict rdata,
                                                 const SelectionVector *sel, idx_t count, ValidityMask &lmask,
                                                 ValidityMask &rmask, SelectionVector *true_sel,
                                                 SelectionVector *false_sel) {
	return DistinctSelectFlatLoopSelSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT, true>(
	    ldata, rdata, sel, count, lmask, rmask, true_sel, false_sel);
}
template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE, bool LEFT_CONSTANT, bool RIGHT_CONSTANT>
static idx_t DistinctSelectFlat(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                SelectionVector *true_sel, SelectionVector *false_sel) {
	auto ldata = FlatVector::GetData<LEFT_TYPE>(left);
	auto rdata = FlatVector::GetData<RIGHT_TYPE>(right);
	if (LEFT_CONSTANT) {
		ValidityMask validity;
		if (ConstantVector::IsNull(left)) {
			validity.SetAllInvalid(1);
		}
		return DistinctSelectFlatLoopSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT>(
		    ldata, rdata, sel, count, validity, FlatVector::Validity(right), true_sel, false_sel);
	} else if (RIGHT_CONSTANT) {
		ValidityMask validity;
		if (ConstantVector::IsNull(right)) {
			validity.SetAllInvalid(1);
		}
		return DistinctSelectFlatLoopSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT>(
		    ldata, rdata, sel, count, FlatVector::Validity(left), validity, true_sel, false_sel);
	} else {
		return DistinctSelectFlatLoopSwitch<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, LEFT_CONSTANT, RIGHT_CONSTANT>(
		    ldata, rdata, sel, count, FlatVector::Validity(left), FlatVector::Validity(right), true_sel, false_sel);
	}
}
template <class LEFT_TYPE, class RIGHT_TYPE, class OP>
static idx_t DistinctSelectConstant(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                    SelectionVector *true_sel, SelectionVector *false_sel) {
	auto ldata = ConstantVector::GetData<LEFT_TYPE>(left);
	auto rdata = ConstantVector::GetData<RIGHT_TYPE>(right);

	// both sides are constant, return either 0 or the count
	// in this case we do not fill in the result selection vector at all
	if (!OP::Operation(*ldata, *rdata, ConstantVector::IsNull(left), ConstantVector::IsNull(right))) {
		if (false_sel) {
			for (idx_t i = 0; i < count; i++) {
				false_sel->set_index(i, sel->get_index(i));
			}
		}
		return 0;
	} else {
		if (true_sel) {
			for (idx_t i = 0; i < count; i++) {
				true_sel->set_index(i, sel->get_index(i));
			}
		}
		return count;
	}
}

template <class LEFT_TYPE, class RIGHT_TYPE, class OP, bool DENSE>
static idx_t DistinctSelect(Vector &left, Vector &right, idx_t vcount, const SelectionVector *sel, idx_t count,
                            SelectionVector *true_sel, SelectionVector *false_sel) {
	if (!sel) {
		sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	}
	if (left.GetVectorType() == VectorType::CONSTANT_VECTOR && right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		return DistinctSelectConstant<LEFT_TYPE, RIGHT_TYPE, OP>(left, right, sel, count, true_sel, false_sel);
	} else if (left.GetVectorType() == VectorType::CONSTANT_VECTOR &&
	           right.GetVectorType() == VectorType::FLAT_VECTOR) {
		return DistinctSelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, true, false>(left, right, sel, count, true_sel,
		                                                                         false_sel);
	} else if (left.GetVectorType() == VectorType::FLAT_VECTOR &&
	           right.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		return DistinctSelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, false, true>(left, right, sel, count, true_sel,
		                                                                         false_sel);
	} else if (left.GetVectorType() == VectorType::FLAT_VECTOR && right.GetVectorType() == VectorType::FLAT_VECTOR) {
		return DistinctSelectFlat<LEFT_TYPE, RIGHT_TYPE, OP, DENSE, false, false>(left, right, sel, count, true_sel,
		                                                                          false_sel);
	} else {
		return DistinctSelectGeneric<LEFT_TYPE, RIGHT_TYPE, OP, DENSE>(left, right, vcount, sel, count, true_sel,
		                                                               false_sel);
	}
}

template <class OP>
static idx_t DistinctSelectNotNull(VectorData &lvdata, VectorData &rvdata, const idx_t count, idx_t &true_count,
                                   const SelectionVector &sel, SelectionVector &maybe_vec, OptionalSelection &true_vec,
                                   OptionalSelection &false_vec) {
	auto &lmask = lvdata.validity;
	auto &rmask = rvdata.validity;

	idx_t remaining = 0;
	if (lmask.AllValid() && rmask.AllValid()) {
		//	None are NULL, distinguish values.
		for (idx_t i = 0; i < count; ++i) {
			const auto idx = sel.get_index(i);
			maybe_vec.set_index(remaining++, idx);
		}
		return remaining;
	}

	idx_t false_count = 0;
	for (idx_t i = 0; i < count; ++i) {
		const auto idx = sel.get_index(i);
		const auto lidx = lvdata.sel->get_index(idx);
		const auto ridx = rvdata.sel->get_index(idx);
		const auto lnull = !lmask.RowIsValid(lidx);
		const auto rnull = !rmask.RowIsValid(ridx);
		if (lnull || rnull) {
			// If either is NULL then we can major distinguish them
			if (!OP::Operation(lnull, rnull, false, false)) {
				false_vec.Append(false_count, idx);
			} else {
				true_vec.Append(true_count, idx);
			}
		} else {
			//	Neither is NULL, distinguish values.
			maybe_vec.set_index(remaining++, idx);
		}
	}
	true_vec.Advance(true_count);
	false_vec.Advance(false_count);

	return remaining;
}

template <bool DENSE>
static inline void ScatterSelection(SelectionVector *target, const idx_t count, const SelectionVector &sel,
                                    const SelectionVector &dense_vec) {
	if (DENSE && target) {
		for (idx_t i = 0; i < count; ++i) {
			target->set_index(i, sel.get_index(dense_vec.get_index(i)));
		}
	}
}

struct PositionComparator {
	// Select the rows that definitely match.
	// Default to the same as the final row
	template <typename OP>
	static idx_t Definite(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel, idx_t count,
	                      SelectionVector *true_sel, SelectionVector &false_sel) {
		return Final<OP>(left, right, vcount, sel, count, true_sel, &false_sel);
	}

	// Select the possible rows that need further testing.
	// Usually this means Is Not Distinct, as those are the semantics used by Postges
	template <typename OP>
	static idx_t Possible(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel, idx_t count,
	                      SelectionVector &true_sel, SelectionVector *false_sel) {
		return VectorOperations::NestedEquals(left, right, vcount, sel, count, &true_sel, false_sel);
	}

	// Select the matching rows for the final position.
	// This needs to be specialised.
	template <typename OP>
	static idx_t Final(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel, idx_t count,
	                   SelectionVector *true_sel, SelectionVector *false_sel) {
		return 0;
	}

	// Tie-break based on length when one of the sides has been exhausted, returning true if the LHS matches.
	// This essentially means that the existing positions compare equal.
	// Default to the same semantics as the OP for idx_t. This works in most cases.
	template <typename OP>
	static bool TieBreak(const idx_t lpos, const idx_t rpos) {
		return OP::Operation(lpos, rpos, false, false);
	}
};

// NotDistinctFrom must always check every column
template <>
idx_t PositionComparator::Definite<duckdb::NotDistinctFrom>(Vector &left, Vector &right, idx_t vcount,
                                                            const SelectionVector &sel, idx_t count,
                                                            SelectionVector *true_sel, SelectionVector &false_sel) {
	return 0;
}

template <>
idx_t PositionComparator::Final<duckdb::NotDistinctFrom>(Vector &left, Vector &right, idx_t vcount,
                                                         const SelectionVector &sel, idx_t count,
                                                         SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

// DistinctFrom must check everything that matched
template <>
idx_t PositionComparator::Possible<duckdb::DistinctFrom>(Vector &left, Vector &right, idx_t vcount,
                                                         const SelectionVector &sel, idx_t count,
                                                         SelectionVector &true_sel, SelectionVector *false_sel) {
	return count;
}

template <>
idx_t PositionComparator::Final<duckdb::DistinctFrom>(Vector &left, Vector &right, idx_t vcount,
                                                      const SelectionVector &sel, idx_t count,
                                                      SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedNotEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

// Non-strict inequalities must use strict comparisons for Definite
template <>
idx_t PositionComparator::Definite<duckdb::DistinctLessThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                                   const SelectionVector &sel, idx_t count,
                                                                   SelectionVector *true_sel,
                                                                   SelectionVector &false_sel) {
	return VectorOperations::NestedLessThan(left, right, vcount, sel, count, true_sel, &false_sel);
}

template <>
idx_t PositionComparator::Final<duckdb::DistinctLessThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                                const SelectionVector &sel, idx_t count,
                                                                SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedLessThanEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t PositionComparator::Definite<duckdb::DistinctGreaterThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                                      const SelectionVector &sel, idx_t count,
                                                                      SelectionVector *true_sel,
                                                                      SelectionVector &false_sel) {
	return VectorOperations::NestedGreaterThan(left, right, vcount, sel, count, true_sel, &false_sel);
}

template <>
idx_t PositionComparator::Final<duckdb::DistinctGreaterThanEquals>(Vector &left, Vector &right, idx_t vcount,
                                                                   const SelectionVector &sel, idx_t count,
                                                                   SelectionVector *true_sel,
                                                                   SelectionVector *false_sel) {
	return VectorOperations::NestedGreaterThanEquals(left, right, vcount, sel, count, true_sel, false_sel);
}

// Strict inequalities just use strict for both Definite and Final
template <>
idx_t PositionComparator::Final<duckdb::DistinctLessThan>(Vector &left, Vector &right, idx_t vcount,
                                                          const SelectionVector &sel, idx_t count,
                                                          SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedLessThan(left, right, vcount, sel, count, true_sel, false_sel);
}

template <>
idx_t PositionComparator::Final<duckdb::DistinctGreaterThan>(Vector &left, Vector &right, idx_t vcount,
                                                             const SelectionVector &sel, idx_t count,
                                                             SelectionVector *true_sel, SelectionVector *false_sel) {
	return VectorOperations::NestedGreaterThan(left, right, vcount, sel, count, true_sel, false_sel);
}

using StructEntries = vector<unique_ptr<Vector>>;

static StructEntries &StructVectorGetSlicedEntries(Vector &parent, StructEntries &sliced, const idx_t vcount) {
	// We have to manually slice STRUCT dictionaries.
	auto &children = StructVector::GetEntries(parent);
	if (parent.GetVectorType() == VectorType::DICTIONARY_VECTOR) {
		auto &dict_sel = DictionaryVector::SelVector(parent);
		for (auto &child : children) {
			auto v = make_unique<Vector>(*child, dict_sel, vcount);
			sliced.push_back(move(v));
		}

		return sliced;
	}

	return children;
}

template <class OP>
static idx_t DistinctSelectStruct(Vector &left, Vector &right, idx_t vcount, VectorData &lvdata, VectorData &rvdata,
                                  idx_t count, SelectionVector &maybe_vec, OptionalSelection &true_opt,
                                  OptionalSelection &false_opt) {
	// Avoid allocating in the 99% of the cases where we don't need to.
	StructEntries lsliced, rsliced;
	auto &lchildren = StructVectorGetSlicedEntries(left, lsliced, vcount);
	auto &rchildren = StructVectorGetSlicedEntries(right, rsliced, vcount);
	D_ASSERT(lchildren.size() == rchildren.size());

	idx_t match_count = 0;
	for (idx_t col_no = 0; col_no < lchildren.size(); ++col_no) {
		auto &lchild = *lchildren[col_no];
		auto &rchild = *rchildren[col_no];

		// Find everything that definitely matches
		auto true_count =
		    PositionComparator::Definite<OP>(lchild, rchild, vcount, maybe_vec, count, true_opt, maybe_vec);
		if (true_count > 0) {
			true_opt.Advance(true_count);
			match_count += true_count;
			count -= true_count;
		}

		if (col_no != lchildren.size() - 1) {
			// Find what might match on the next position
			true_count =
			    PositionComparator::Possible<OP>(lchild, rchild, vcount, maybe_vec, count, maybe_vec, false_opt);
			auto false_count = count - true_count;
			false_opt.Advance(false_count);
			count = true_count;
		} else {
			true_count = PositionComparator::Final<OP>(lchild, rchild, vcount, maybe_vec, count, true_opt, false_opt);
			match_count += true_count;
		}
	}

	return match_count;
}

static void PositionListCursor(SelectionVector &cursor, VectorData &vdata, const idx_t pos,
                               const SelectionVector &maybe_vec, const idx_t count) {
	const auto data = (const list_entry_t *)vdata.data;
	for (idx_t i = 0; i < count; ++i) {
		const auto idx = maybe_vec.get_index(i);

		const auto lidx = vdata.sel->get_index(idx);
		const auto &entry = data[lidx];
		cursor.set_index(idx, entry.offset + pos);
	}
}

template <class OP>
static idx_t DistinctSelectList(Vector &left, Vector &right, idx_t vcount, VectorData &lvdata, VectorData &rvdata,
                                idx_t count, SelectionVector &maybe_vec, OptionalSelection &true_opt,
                                OptionalSelection &false_opt) {
	if (count == 0) {
		return count;
	}

	// We use them to create dictionary views of the children so we can vectorise the positional comparisons.
	SelectionVector lcursor(vcount);
	SelectionVector rcursor(vcount);

	Vector lchild(ListVector::GetEntry(left), lcursor, count);
	Vector rchild(ListVector::GetEntry(right), rcursor, count);

	// To perform the positional comparison, we use a vectorisation of the following algorithm:
	// bool CompareLists(T *left, idx_t nleft, T *right, nright) {
	// 	for (idx_t pos = 0; ; ++pos) {
	// 		if (nleft == pos || nright == pos)
	// 			return OP::TieBreak(nleft, nright);
	// 		if (OP::Definite(*left, *right))
	// 			return true;
	// 		if (!OP::Maybe(*left, *right))
	// 			return false;
	// 		}
	//	 	++left, ++right;
	// 	}
	// }
	const auto ldata = (const list_entry_t *)lvdata.data;
	const auto rdata = (const list_entry_t *)rvdata.data;

	idx_t match_count = 0;
	for (idx_t pos = 0; count > 0; ++pos) {
		// Set up the cursors for the current position
		PositionListCursor(lcursor, lvdata, pos, maybe_vec, count);
		PositionListCursor(rcursor, rvdata, pos, maybe_vec, count);

		// Tie-break the pairs where one of the LISTs is exhausted.
		idx_t true_count = 0;
		idx_t false_count = 0;
		idx_t maybe_count = 0;
		for (idx_t i = 0; i < count; ++i) {
			const auto idx = maybe_vec.get_index(i);
			const auto lidx = lvdata.sel->get_index(idx);
			const auto &lentry = ldata[lidx];
			const auto ridx = rvdata.sel->get_index(idx);
			const auto &rentry = rdata[ridx];
			if (lentry.length == pos || rentry.length == pos) {
				if (PositionComparator::TieBreak<OP>(lentry.length, rentry.length)) {
					true_opt.Append(true_count, idx);
				} else {
					false_opt.Append(false_count, idx);
				}
			} else {
				maybe_vec.set_index(maybe_count++, idx);
			}
		}
		true_opt.Advance(true_count);
		false_opt.Advance(false_count);
		count = maybe_count;
		match_count += true_count;

		// Find everything that definitely matches
		true_count = PositionComparator::Definite<OP>(lchild, rchild, vcount, maybe_vec, count, true_opt, maybe_vec);
		true_opt.Advance(true_count);
		match_count += true_count;
		count -= true_count;

		// Find what might match on the next position
		maybe_count = PositionComparator::Possible<OP>(lchild, rchild, vcount, maybe_vec, count, maybe_vec, false_opt);
		false_count = count - maybe_count;
		false_opt.Advance(false_count);
		count = maybe_count;
	}

	return match_count;
}

template <class OP, bool DENSE>
static idx_t DistinctSelectNested(Vector &left, Vector &right, idx_t vcount, const SelectionVector *sel, idx_t count,
                                  SelectionVector *true_sel, SelectionVector *false_sel) {
	// We need multiple, real selections
	if (!sel) {
		sel = &FlatVector::INCREMENTAL_SELECTION_VECTOR;
	}

	SelectionVector true_vec;
	SelectionVector false_vec;
	if (DENSE) {
		true_vec.Initialize(vcount);
		false_vec.Initialize(vcount);
	}

	OptionalSelection true_opt(DENSE ? &true_vec : true_sel);
	OptionalSelection false_opt(DENSE ? &false_vec : false_sel);

	// Handle NULL STRUCTs
	VectorData lvdata, rvdata;
	left.Orrify(vcount, lvdata);
	right.Orrify(vcount, rvdata);

	sel_t maybe_one;
	SelectionVector maybe_vec(&maybe_one);
	if (count > 1) {
		maybe_vec.Initialize(count);
	}
	idx_t match_count = 0;
	idx_t no_match_count = count;
	count = DistinctSelectNotNull<OP>(lvdata, rvdata, count, match_count, *sel, maybe_vec, true_opt, false_opt);
	no_match_count -= (count + match_count);

	idx_t true_count = 0;
	if (PhysicalType::LIST == left.GetType().InternalType()) {
		true_count = DistinctSelectList<OP>(left, right, vcount, lvdata, rvdata, count, maybe_vec, true_opt, false_opt);
	} else {
		true_count =
		    DistinctSelectStruct<OP>(left, right, vcount, lvdata, rvdata, count, maybe_vec, true_opt, false_opt);
	}

	auto false_count = count - true_count;
	match_count += true_count;
	no_match_count += false_count;

	ScatterSelection<DENSE>(true_sel, match_count, *sel, true_vec);
	ScatterSelection<DENSE>(false_sel, no_match_count, *sel, false_vec);

	return match_count;
}

template <typename OP>
static void NestedDistinctExecute(Vector &left, Vector &right, Vector &result, idx_t count);

template <class T, class OP>
static inline void TemplatedDistinctExecute(Vector &left, Vector &right, Vector &result, idx_t count) {
	DistinctExecute<T, T, bool, OP>(left, right, result, count);
}
template <class OP>
static void ExecuteDistinct(Vector &left, Vector &right, Vector &result, idx_t count) {
	D_ASSERT(left.GetType() == right.GetType() && result.GetType() == LogicalType::BOOLEAN);
	// the inplace loops take the result as the last parameter
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedDistinctExecute<int8_t, OP>(left, right, result, count);
		break;
	case PhysicalType::INT16:
		TemplatedDistinctExecute<int16_t, OP>(left, right, result, count);
		break;
	case PhysicalType::INT32:
		TemplatedDistinctExecute<int32_t, OP>(left, right, result, count);
		break;
	case PhysicalType::INT64:
		TemplatedDistinctExecute<int64_t, OP>(left, right, result, count);
		break;
	case PhysicalType::UINT8:
		TemplatedDistinctExecute<uint8_t, OP>(left, right, result, count);
		break;
	case PhysicalType::UINT16:
		TemplatedDistinctExecute<uint16_t, OP>(left, right, result, count);
		break;
	case PhysicalType::UINT32:
		TemplatedDistinctExecute<uint32_t, OP>(left, right, result, count);
		break;
	case PhysicalType::UINT64:
		TemplatedDistinctExecute<uint64_t, OP>(left, right, result, count);
		break;
	case PhysicalType::INT128:
		TemplatedDistinctExecute<hugeint_t, OP>(left, right, result, count);
		break;
	case PhysicalType::POINTER:
		TemplatedDistinctExecute<uintptr_t, OP>(left, right, result, count);
		break;
	case PhysicalType::FLOAT:
		TemplatedDistinctExecute<float, OP>(left, right, result, count);
		break;
	case PhysicalType::DOUBLE:
		TemplatedDistinctExecute<double, OP>(left, right, result, count);
		break;
	case PhysicalType::INTERVAL:
		TemplatedDistinctExecute<interval_t, OP>(left, right, result, count);
		break;
	case PhysicalType::VARCHAR:
		TemplatedDistinctExecute<string_t, OP>(left, right, result, count);
		break;
	case PhysicalType::LIST:
	case PhysicalType::MAP:
	case PhysicalType::STRUCT:
		NestedDistinctExecute<OP>(left, right, result, count);
		break;
	default:
		throw InternalException("Invalid type for distinct comparison");
	}
}

template <class OP, bool DENSE>
static idx_t TemplatedDistinctSelectOperation(Vector &left, Vector &right, idx_t vcount, const SelectionVector *sel,
                                              idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	// the inplace loops take the result as the last parameter
	switch (left.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return DistinctSelect<int8_t, int8_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::INT16:
		return DistinctSelect<int16_t, int16_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::INT32:
		return DistinctSelect<int32_t, int32_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::INT64:
		return DistinctSelect<int64_t, int64_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::UINT8:
		return DistinctSelect<uint8_t, uint8_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::UINT16:
		return DistinctSelect<uint16_t, uint16_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::UINT32:
		return DistinctSelect<uint32_t, uint32_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::UINT64:
		return DistinctSelect<uint64_t, uint64_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::INT128:
		return DistinctSelect<hugeint_t, hugeint_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::POINTER:
		return DistinctSelect<uintptr_t, uintptr_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::FLOAT:
		return DistinctSelect<float, float, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::DOUBLE:
		return DistinctSelect<double, double, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::INTERVAL:
		return DistinctSelect<interval_t, interval_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::VARCHAR:
		return DistinctSelect<string_t, string_t, OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	case PhysicalType::MAP:
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
		return DistinctSelectNested<OP, DENSE>(left, right, vcount, sel, count, true_sel, false_sel);
	default:
		throw InternalException("Invalid type for distinct selection");
	}
}

template <typename OP>
static void NestedDistinctExecute(Vector &left, Vector &right, Vector &result, idx_t count) {
	const auto left_constant = left.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const auto right_constant = right.GetVectorType() == VectorType::CONSTANT_VECTOR;

	if (left_constant && right_constant) {
		// both sides are constant, so just compare one element.
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		auto result_data = ConstantVector::GetData<bool>(result);
		SelectionVector true_sel(1);
		auto match_count = TemplatedDistinctSelectOperation<OP, true>(left, right, 1, nullptr, 1, &true_sel, nullptr);
		result_data[0] = match_count > 0;
		return;
	}

	SelectionVector true_sel(count);
	SelectionVector false_sel(count);

	// DISTINCT is either true or false
	idx_t match_count =
	    TemplatedDistinctSelectOperation<OP, true>(left, right, count, nullptr, count, &true_sel, &false_sel);

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::GetData<bool>(result);

	for (idx_t i = 0; i < match_count; ++i) {
		const auto idx = true_sel.get_index(i);
		result_data[idx] = true;
	}

	const idx_t no_match_count = count - match_count;
	for (idx_t i = 0; i < no_match_count; ++i) {
		const auto idx = false_sel.get_index(i);
		result_data[idx] = false;
	}
}

void VectorOperations::DistinctFrom(Vector &left, Vector &right, Vector &result, idx_t count) {
	ExecuteDistinct<duckdb::DistinctFrom>(left, right, result, count);
}

void VectorOperations::NotDistinctFrom(Vector &left, Vector &right, Vector &result, idx_t count) {
	ExecuteDistinct<duckdb::NotDistinctFrom>(left, right, result, count);
}

// true := A != B with nulls being equal
idx_t VectorOperations::DistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                     SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctFrom, true>(left, right, count, sel, count, true_sel,
	                                                                    false_sel);
}
// true := A == B with nulls being equal
idx_t VectorOperations::NotDistinctFrom(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                        SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::NotDistinctFrom, true>(left, right, count, sel, count, true_sel,
	                                                                       false_sel);
}

// true := A > B with nulls being maximal
idx_t VectorOperations::DistinctGreaterThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                            SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctGreaterThan, true>(left, right, count, sel, count, true_sel,
	                                                                           false_sel);
}
// true := A >= B with nulls being maximal
idx_t VectorOperations::DistinctGreaterThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                                  SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctGreaterThanEquals, true>(left, right, count, sel, count,
	                                                                                 true_sel, false_sel);
}
// true := A < B with nulls being maximal
idx_t VectorOperations::DistinctLessThan(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                         SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctLessThan, true>(left, right, count, sel, count, true_sel,
	                                                                        false_sel);
}
// true := A <= B with nulls being maximal
idx_t VectorOperations::DistinctLessThanEquals(Vector &left, Vector &right, const SelectionVector *sel, idx_t count,
                                               SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctLessThanEquals, true>(left, right, count, sel, count,
	                                                                              true_sel, false_sel);
}

// true := A != B with nulls being equal, inputs selected
idx_t VectorOperations::NestedNotEquals(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                        idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctFrom, false>(left, right, vcount, &sel, count, true_sel,
	                                                                     false_sel);
}
// true := A == B with nulls being equal, inputs selected
idx_t VectorOperations::NestedEquals(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel, idx_t count,
                                     SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::NotDistinctFrom, false>(left, right, vcount, &sel, count, true_sel,
	                                                                        false_sel);
}
// true := A > B with nulls being maximal, inputs selected
idx_t VectorOperations::NestedGreaterThan(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                          idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctGreaterThan, false>(left, right, vcount, &sel, count,
	                                                                            true_sel, false_sel);
}
// true := A >= B with nulls being maximal, inputs selected
idx_t VectorOperations::NestedGreaterThanEquals(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                                idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctGreaterThanEquals, false>(left, right, vcount, &sel, count,
	                                                                                  true_sel, false_sel);
}
// true := A < B with nulls being maximal, inputs selected
idx_t VectorOperations::NestedLessThan(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                       idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctLessThan, false>(left, right, vcount, &sel, count, true_sel,
	                                                                         false_sel);
}
// true := A <= B with nulls being maximal, inputs selected
idx_t VectorOperations::NestedLessThanEquals(Vector &left, Vector &right, idx_t vcount, const SelectionVector &sel,
                                             idx_t count, SelectionVector *true_sel, SelectionVector *false_sel) {
	return TemplatedDistinctSelectOperation<duckdb::DistinctLessThanEquals, false>(left, right, vcount, &sel, count,
	                                                                               true_sel, false_sel);
}

} // namespace duckdb
