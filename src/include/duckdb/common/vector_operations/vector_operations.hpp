//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/vector_operations.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/types/vector.hpp"

namespace duckdb {
class CastFunctionSet;
struct GetCastFunctionInput;
class Context;

// VectorOperations contains a set of operations that operate on sets of
// vectors. In general, the operators must all have the same type, otherwise an
// exception is thrown. Note that the functions underneath use restrict
// pointers, hence the data that the vectors point to (and hence the vector
// themselves) should not be equal! For example, if you call the function Add(A,
// B, A) then ASSERT_RESTRICT will be triggered. Instead call AddInPlace(A, B)
// or Add(A, B, C)
struct VectorOperations {
	//===--------------------------------------------------------------------===//
	// In-Place Operators
	//===--------------------------------------------------------------------===//
	//! left += delta
	static void AddInPlace(Vector &left, int64_t delta);

	//===--------------------------------------------------------------------===//
	// NULL Operators
	//===--------------------------------------------------------------------===//
	//! result = IS NOT NULL(input)
	static void IsNotNull(const Vector &arg, Vector &result);
	//! result = IS NULL (input)
	static void IsNull(const Vector &input, Vector &result);
	// Returns whether or not arg vector has a NULL value
	static bool HasNull(const Vector &input);
	static bool HasNotNull(const Vector &input);
	//! Count the number of not-NULL values.
	static idx_t CountNotNull(const Vector &input);

	//===--------------------------------------------------------------------===//
	// Boolean Operations
	//===--------------------------------------------------------------------===//
	// result = left && right
	static void And(const Vector &left, const Vector &right, Vector &result);
	// result = left || right
	static void Or(const Vector &left, const Vector &right, Vector &result);
	// result = NOT(input)
	static void Not(const Vector &input, Vector &result);

	//===--------------------------------------------------------------------===//
	// Comparison Operations
	//===--------------------------------------------------------------------===//
	// result = left == right
	static void Equals(const Vector &left, const Vector &right, Vector &result);
	// result = left != right
	static void NotEquals(const Vector &left, const Vector &right, Vector &result);
	// result = left > right
	static void GreaterThan(const Vector &left, const Vector &right, Vector &result);
	// result = left >= right
	static void GreaterThanEquals(const Vector &left, const Vector &right, Vector &result);
	// result = left < right
	static void LessThan(const Vector &left, const Vector &right, Vector &result);
	// result = left <= right
	static void LessThanEquals(const Vector &left, const Vector &right, Vector &result);

	// result = -1 if left < right, 0 if left == right, 1 if left > right (stored in int8_t TINYINT result vector)
	// if inequality is set, then scalars inside nested types will be compared with NULLS LAST semantics.
	static void Comparator(const Vector &left, const Vector &right, Vector &result, const ExpressionType comp);
	// result = -1 if left < right, 0 if left == right, 1 if left > right; fills exactly count rows (for select paths)
	// if inequality is set, then scalars inside nested types will be compared with NULLS LAST semantics.
	static void ComparatorFill(const Vector &left, const Vector &right, Vector &result, idx_t count,
	                           const ExpressionType comp);

	// result = A != B with nulls being equal
	static void DistinctFrom(const Vector &left, const Vector &right, Vector &result);
	// result := A == B with nulls being equal
	static void NotDistinctFrom(const Vector &left, const Vector &right, Vector &result);
	// result := comparator(A, B) with nulls being maximal (NULLS LAST), returns -1/0/1 as int8_t
	static void DistinctComparator(const Vector &left, const Vector &right, Vector &result);
	// result := comparator(A, B) with nulls being minimal (NULLS FIRST), returns -1/0/1 as int8_t
	static void DistinctComparatorNullsFirst(const Vector &left, const Vector &right, Vector &result);
	// DistinctComparator/NullsFirst variants that fill exactly count rows (for select paths with SelectionVector)
	static void DistinctComparatorFill(const Vector &left, const Vector &right, Vector &result, idx_t count);
	static void DistinctComparatorNullsFirstFill(const Vector &left, const Vector &right, Vector &result, idx_t count);

	//===--------------------------------------------------------------------===//
	// Select Comparisons
	//===--------------------------------------------------------------------===//
	static idx_t Equals(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel, idx_t count,
	                    optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel,
	                    optional_ptr<ValidityMask> null_mask = nullptr);
	static idx_t NotEquals(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                       idx_t count, optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel,
	                       optional_ptr<ValidityMask> null_mask = nullptr);
	static idx_t GreaterThan(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                         idx_t count, optional_ptr<SelectionVector> true_sel,
	                         optional_ptr<SelectionVector> false_sel, optional_ptr<ValidityMask> null_mask = nullptr);
	static idx_t GreaterThanEquals(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                               idx_t count, optional_ptr<SelectionVector> true_sel,
	                               optional_ptr<SelectionVector> false_sel,
	                               optional_ptr<ValidityMask> null_mask = nullptr);
	static idx_t LessThan(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel, idx_t count,
	                      optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel,
	                      optional_ptr<ValidityMask> null_mask = nullptr);
	static idx_t LessThanEquals(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                            idx_t count, optional_ptr<SelectionVector> true_sel,
	                            optional_ptr<SelectionVector> false_sel,
	                            optional_ptr<ValidityMask> null_mask = nullptr);

	// true := A != B with nulls being equal
	static idx_t DistinctFrom(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                          idx_t count, optional_ptr<SelectionVector> true_sel,
	                          optional_ptr<SelectionVector> false_sel);
	// true := A == B with nulls being equal
	static idx_t NotDistinctFrom(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                             idx_t count, optional_ptr<SelectionVector> true_sel,
	                             optional_ptr<SelectionVector> false_sel);
	// true := A > B with nulls being maximal
	static idx_t DistinctGreaterThan(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                                 idx_t count, optional_ptr<SelectionVector> true_sel,
	                                 optional_ptr<SelectionVector> false_sel,
	                                 optional_ptr<ValidityMask> null_mask = nullptr);
	// true := A >= B with nulls being maximal
	static idx_t DistinctGreaterThanEquals(const Vector &left, const Vector &right,
	                                       optional_ptr<const SelectionVector> sel, idx_t count,
	                                       optional_ptr<SelectionVector> true_sel,
	                                       optional_ptr<SelectionVector> false_sel,
	                                       optional_ptr<ValidityMask> null_mask = nullptr);
	// true := A < B with nulls being maximal
	static idx_t DistinctLessThan(const Vector &left, const Vector &right, optional_ptr<const SelectionVector> sel,
	                              idx_t count, optional_ptr<SelectionVector> true_sel,
	                              optional_ptr<SelectionVector> false_sel,
	                              optional_ptr<ValidityMask> null_mask = nullptr);
	// true := A <= B with nulls being maximal
	static idx_t DistinctLessThanEquals(const Vector &left, const Vector &right,
	                                    optional_ptr<const SelectionVector> sel, idx_t count,
	                                    optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel,
	                                    optional_ptr<ValidityMask> null_mask = nullptr);
	// true := A > B with nulls being minimal
	static idx_t DistinctGreaterThanNullsFirst(const Vector &left, const Vector &right,
	                                           optional_ptr<const SelectionVector> sel, idx_t count,
	                                           optional_ptr<SelectionVector> true_sel,
	                                           optional_ptr<SelectionVector> false_sel,
	                                           optional_ptr<ValidityMask> null_mask = nullptr);
	// true := A < B with nulls being minimal
	static idx_t DistinctLessThanNullsFirst(const Vector &left, const Vector &right,
	                                        optional_ptr<const SelectionVector> sel, idx_t count,
	                                        optional_ptr<SelectionVector> true_sel,
	                                        optional_ptr<SelectionVector> false_sel,
	                                        optional_ptr<ValidityMask> null_mask = nullptr);

	//===--------------------------------------------------------------------===//
	// Hash functions
	//===--------------------------------------------------------------------===//
	// hashes = HASH(input)
	// FIXME: this "count" needs to go
	static void Hash(const Vector &input, Vector &hashes, idx_t count);
	static void Hash(const Vector &input, Vector &hashes, const SelectionVector &rsel, idx_t count);
	//! Convenience overload without explicit count - count is derived from input.size()
	static void Hash(const Vector &input, Vector &hashes);
	// hashes ^= HASH(input)
	// FIXME: this "count" needs to go
	static void CombineHash(Vector &hashes, const Vector &input, idx_t count);
	static void CombineHash(Vector &hashes, const Vector &input, const SelectionVector &rsel, idx_t count);
	//! Convenience overload without explicit count - count is derived from the input vectors (with mismatch check)
	static void CombineHash(Vector &hashes, const Vector &input);

	//===--------------------------------------------------------------------===//
	// Generate functions
	//===--------------------------------------------------------------------===//
	static void GenerateSequence(Vector &result, idx_t count, int64_t start = 0, int64_t increment = 1);
	static void GenerateSequence(Vector &result, idx_t count, const SelectionVector &sel, int64_t start = 0,
	                             int64_t increment = 1);
	//===--------------------------------------------------------------------===//
	// Helpers
	//===--------------------------------------------------------------------===//
	//! Cast the data from the source type to the target type. Any elements that could not be converted are turned into
	//! NULLs. If any elements cannot be converted, returns false and fills in the error_message. If no error message is
	//! provided, an exception is thrown instead.
	DUCKDB_API static bool TryCast(CastFunctionSet &set, GetCastFunctionInput &input, Vector &source, Vector &result,
	                               idx_t count, string *error_message, bool strict = false,
	                               const bool nullify_parent = false);
	DUCKDB_API static bool DefaultTryCast(Vector &source, Vector &result, idx_t count, string *error_message,
	                                      bool strict = false);
	DUCKDB_API static bool TryCast(ClientContext &context, Vector &source, Vector &result, idx_t count,
	                               string *error_message, bool strict = false, const bool nullify_parent = false);
	//! Cast the data from the source type to the target type. Throws an exception if the cast fails.
	DUCKDB_API static void Cast(ClientContext &context, Vector &source, Vector &result, idx_t count,
	                            bool strict = false);
	DUCKDB_API static bool TryCast(Context &context, Vector &source, Vector &result, idx_t count, string *error_message,
	                               bool strict = false, const bool nullify_parent = false);
	DUCKDB_API static void Cast(Context &context, Vector &source, Vector &result, idx_t count, bool strict = false);
	DUCKDB_API static void DefaultCast(Vector &source, Vector &result, idx_t count, bool strict = false);

	// Copy the data of <source> to the target vector
	static void Copy(const Vector &source, Vector &target, idx_t source_count, idx_t source_offset,
	                 idx_t target_offset);
	static void Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
	                 idx_t source_offset, idx_t target_offset);
	static void Copy(const Vector &source, Vector &target, const SelectionVector &sel, idx_t source_count,
	                 idx_t source_offset, idx_t target_offset, idx_t copy_count);

	// Copy the data of <source> to the target location, setting null values to
	// NullValue<T>. Used to store data without separate NULL mask.
	static void WriteToStorage(const Vector &source, data_ptr_t target);
	// Reads the data of <source> to the target vector, setting the nullmask
	// for any NullValue<T> of source. Used to go back from storage to a proper vector
	static void ReadFromStorage(data_ptr_t source, idx_t count, Vector &result);

	//===--------------------------------------------------------------------===//
	// Deprecated overloads (count parameter removed - use count-free versions)
	//===--------------------------------------------------------------------===//
	[[deprecated("count parameter is deprecated; call AddInPlace without count instead")]] static void
	AddInPlace(Vector &left, int64_t delta, idx_t count) {
		if (count != left.size()) {
			throw InternalException("AddInPlace: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		AddInPlace(left, delta);
	}
	[[deprecated("count parameter is deprecated; call IsNotNull without count instead")]] static void
	IsNotNull(const Vector &arg, Vector &result, idx_t count) {
		if (count != arg.size()) {
			throw InternalException("IsNotNull: count (%llu) does not match vector size (%llu)", count, arg.size());
		}
		IsNotNull(arg, result);
	}
	[[deprecated("count parameter is deprecated; call IsNull without count instead")]] static void
	IsNull(const Vector &input, Vector &result, idx_t count) {
		if (count != input.size()) {
			throw InternalException("IsNull: count (%llu) does not match vector size (%llu)", count, input.size());
		}
		IsNull(input, result);
	}
	[[deprecated("count parameter is deprecated; call HasNull without count instead")]] static bool
	HasNull(const Vector &input, idx_t count) {
		if (count != input.size()) {
			throw InternalException("HasNull: count (%llu) does not match vector size (%llu)", count, input.size());
		}
		return HasNull(input);
	}
	[[deprecated("count parameter is deprecated; call HasNotNull without count instead")]] static bool
	HasNotNull(const Vector &input, idx_t count) {
		if (count != input.size()) {
			throw InternalException("HasNotNull: count (%llu) does not match vector size (%llu)", count, input.size());
		}
		return HasNotNull(input);
	}
	[[deprecated("count parameter is deprecated; call CountNotNull without count instead")]] static idx_t
	CountNotNull(const Vector &input, const idx_t count) {
		if (count != input.size()) {
			throw InternalException("CountNotNull: count (%llu) does not match vector size (%llu)", count,
			                        input.size());
		}
		return CountNotNull(input);
	}
	[[deprecated("count parameter is deprecated; call And without count instead")]] static void
	And(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("And: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		And(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call Or without count instead")]] static void
	Or(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("Or: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		Or(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call Not without count instead")]] static void
	Not(const Vector &input, Vector &result, idx_t count) {
		if (count != input.size()) {
			throw InternalException("Not: count (%llu) does not match vector size (%llu)", count, input.size());
		}
		Not(input, result);
	}
	[[deprecated("count parameter is deprecated; call Equals without count instead")]] static void
	Equals(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("Equals: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		Equals(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call NotEquals without count instead")]] static void
	NotEquals(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("NotEquals: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		NotEquals(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call GreaterThan without count instead")]] static void
	GreaterThan(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("GreaterThan: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		GreaterThan(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call GreaterThanEquals without count instead")]] static void
	GreaterThanEquals(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("GreaterThanEquals: count (%llu) does not match vector size (%llu)", count,
			                        left.size());
		}
		GreaterThanEquals(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call LessThan without count instead")]] static void
	LessThan(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("LessThan: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		LessThan(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call LessThanEquals without count instead")]] static void
	LessThanEquals(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("LessThanEquals: count (%llu) does not match vector size (%llu)", count,
			                        left.size());
		}
		LessThanEquals(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call Comparator with ExpressionType instead")]] static void
	Comparator(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("Comparator: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		Comparator(left, right, result, ExpressionType::COMPARE_EQUAL);
	}
	[[deprecated("count parameter is deprecated; call DistinctFrom without count instead")]] static void
	DistinctFrom(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("DistinctFrom: count (%llu) does not match vector size (%llu)", count, left.size());
		}
		DistinctFrom(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call NotDistinctFrom without count instead")]] static void
	NotDistinctFrom(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("NotDistinctFrom: count (%llu) does not match vector size (%llu)", count,
			                        left.size());
		}
		NotDistinctFrom(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call DistinctComparator without count instead")]] static void
	DistinctComparator(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("DistinctComparator: count (%llu) does not match vector size (%llu)", count,
			                        left.size());
		}
		DistinctComparator(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call DistinctComparatorNullsFirst without count instead")]] static void
	DistinctComparatorNullsFirst(const Vector &left, const Vector &right, Vector &result, idx_t count) {
		if (count != left.size()) {
			throw InternalException("DistinctComparatorNullsFirst: count (%llu) does not match vector size (%llu)",
			                        count, left.size());
		}
		DistinctComparatorNullsFirst(left, right, result);
	}
	[[deprecated("count parameter is deprecated; call WriteToStorage without count instead")]] static void
	WriteToStorage(const Vector &source, idx_t count, data_ptr_t target) {
		if (count != source.size()) {
			throw InternalException("WriteToStorage: count (%llu) does not match vector size (%llu)", count,
			                        source.size());
		}
		WriteToStorage(source, target);
	}
};
} // namespace duckdb
