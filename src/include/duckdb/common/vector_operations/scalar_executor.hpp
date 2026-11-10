//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/scalar_executor.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/smaller_binary.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/selection_helpers.hpp"

#include <array>
#include <functional>
#include <tuple>
#include <type_traits>

#if defined(_MSC_VER)
#define DUCKDB_SCALAR_EXECUTOR_NOINLINE __declspec(noinline)
#elif defined(__GNUC__)
#define DUCKDB_SCALAR_EXECUTOR_NOINLINE __attribute__((noinline))
#else
#define DUCKDB_SCALAR_EXECUTOR_NOINLINE
#endif

namespace duckdb {

//! Internal execution engine shared by the named scalar executor facades.
struct ScalarExecutor {
	using VectorRef = std::reference_wrapper<const Vector>;

private:
	struct InputProfile {
		uint64_t constant_mask = 0;
		bool all_constant = true;
		bool all_flat_or_constant = true;
		bool any_constant_null = false;
	};

	template <size_t N, size_t... Is>
	static inline InputProfile GetInputProfile(const std::array<VectorRef, N> &inputs, std::index_sequence<Is...>) {
		InputProfile result;
		auto classify = [&](auto input_index) {
			constexpr idx_t INPUT_INDEX = decltype(input_index)::value;
			auto vector_type = inputs[INPUT_INDEX].get().GetVectorType();
			if (vector_type == VectorType::CONSTANT_VECTOR) {
				if constexpr (INPUT_INDEX < 64) {
					result.constant_mask |= uint64_t(1) << INPUT_INDEX;
				}
				result.any_constant_null |= ConstantVector::IsNull(inputs[INPUT_INDEX].get());
			} else {
				result.all_constant = false;
				result.all_flat_or_constant &= vector_type == VectorType::FLAT_VECTOR;
			}
		};
		(classify(std::integral_constant<idx_t, Is> {}), ...);
		return result;
	}

	template <idx_t INPUT_INDEX, uint64_t CONSTANT_MASK>
	static inline idx_t InputIndex(idx_t row) {
		if constexpr (CONSTANT_MASK & (uint64_t(1) << INPUT_INDEX)) {
			return 0;
		} else {
			return row;
		}
	}

	template <bool ADDS_NULLS, bool PRESERVE_RESULT_VALIDITY, uint64_t CONSTANT_MASK, size_t N, size_t... Is>
	static inline ValidityMask &PrepareFlatResultValidity(const std::array<VectorRef, N> &inputs, Vector &result,
	                                                      idx_t count, std::index_sequence<Is...>) {
		auto &result_validity = FlatVector::ValidityMutable(result);
		bool initialized = false;
		auto combine = [&](auto input_index) {
			constexpr idx_t INPUT_INDEX = decltype(input_index)::value;
			if constexpr (!(CONSTANT_MASK & (uint64_t(1) << INPUT_INDEX))) {
				auto &input_validity = FlatVector::Validity(inputs[INPUT_INDEX].get());
				if (input_validity.CanHaveNull()) {
					if (!initialized) {
						if constexpr (ADDS_NULLS) {
							result_validity.Copy(input_validity, count);
						} else {
							result_validity.Initialize(input_validity);
						}
						initialized = true;
					} else {
						result_validity.Combine(input_validity, count);
					}
				}
			}
		};
		(combine(std::integral_constant<idx_t, Is> {}), ...);
		if (!initialized) {
			if constexpr (PRESERVE_RESULT_VALIDITY) {
				if constexpr (ADDS_NULLS) {
					ValidityMask preserved(result_validity, count);
					result_validity.Initialize(preserved);
				}
			} else {
				result_validity.Reset(count);
			}
		}
		return result_validity;
	}

	template <uint64_t CONSTANT_MASK, size_t N, size_t... Is>
	static inline ValidityMask PrepareFlatInputValidity(const std::array<VectorRef, N> &inputs, idx_t count,
	                                                    std::index_sequence<Is...>) {
		ValidityMask result(count);
		bool initialized = false;
		auto combine = [&](auto input_index) {
			constexpr idx_t INPUT_INDEX = decltype(input_index)::value;
			if constexpr (!(CONSTANT_MASK & (uint64_t(1) << INPUT_INDEX))) {
				auto &input_validity = FlatVector::Validity(inputs[INPUT_INDEX].get());
				if (!initialized) {
					result.Initialize(input_validity);
					initialized = true;
				} else {
					result.Combine(input_validity, count);
				}
			}
		};
		(combine(std::integral_constant<idx_t, Is> {}), ...);
		return result;
	}

	//! Makes a reused result ready for the one-row generic path: like the constant path, a stale NULL does not survive
	DUCKDB_API static void PrepareConstantResult(Vector &result);
	DUCKDB_API static bool PrepareGenericResultValidity(const UnifiedVectorFormat *formats, idx_t format_count,
	                                                    Vector &result, idx_t count, bool preserve_result_validity,
	                                                    bool adds_nulls);

	template <bool PRESERVE_RESULT_VALIDITY, uint64_t CONSTANT_MASK, class RESULT_TYPE, class ADAPTER, class... ARGS,
	          size_t... Is>
	static void ExecuteFlat(const std::array<VectorRef, sizeof...(ARGS)> &inputs, Vector &result, idx_t count,
	                        ADAPTER &adapter, std::index_sequence<Is...> indices) {
		result.SetVectorType(VectorType::FLAT_VECTOR);
		if (result.size() != count) {
			FlatVector::SetSize(result, count);
		}
		auto result_data = FlatVector::GetDataMutable<RESULT_TYPE>(result);
		auto input_data = std::make_tuple(FlatVector::GetData<ARGS>(inputs[Is].get())...);
		auto &result_validity = PrepareFlatResultValidity<ADAPTER::ADDS_NULLS, PRESERVE_RESULT_VALIDITY, CONSTANT_MASK>(
		    inputs, result, count, indices);

#ifdef DEBUG
		auto assert_restrict = [&](auto input_index) {
			constexpr idx_t INPUT_INDEX = decltype(input_index)::value;
			if constexpr (!(CONSTANT_MASK & (uint64_t(1) << INPUT_INDEX))) {
				auto data = std::get<INPUT_INDEX>(input_data);
				ASSERT_RESTRICT(data, data + count, result_data, result_data + count);
			}
		};
		(assert_restrict(std::integral_constant<idx_t, Is> {}), ...);
#endif

		if (result_validity.CanHaveNull()) {
			idx_t base_idx = 0;
			auto entry_count = ValidityMask::EntryCount(count);
			for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
				auto validity_entry = result_validity.GetValidityEntry(entry_idx);
				auto next = MinValue<idx_t>(base_idx + ValidityMask::BITS_PER_VALUE, count);
				if (ValidityMask::AllValid(validity_entry)) {
					for (; base_idx < next; base_idx++) {
						result_data[base_idx] =
						    adapter.Operation(result_validity, base_idx,
						                      std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(base_idx)]...);
					}
				} else if (ValidityMask::NoneValid(validity_entry)) {
					base_idx = next;
				} else {
					auto start = base_idx;
					for (; base_idx < next; base_idx++) {
						if (ValidityMask::RowIsValid(validity_entry, base_idx - start)) {
							result_data[base_idx] =
							    adapter.Operation(result_validity, base_idx,
							                      std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(base_idx)]...);
						}
					}
				}
			}
		} else {
			for (idx_t row = 0; row < count; row++) {
				result_data[row] = adapter.Operation(result_validity, row,
				                                     std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(row)]...);
			}
		}
	}

#if !DUCKDB_SMALLER_BINARY(scalar_executor_constant)
	template <class RESULT_TYPE, class ADAPTER, class... ARGS, size_t... Is>
	static void ExecuteConstant(const std::array<VectorRef, sizeof...(ARGS)> &inputs, Vector &result, idx_t count,
	                            bool input_is_null, ADAPTER &adapter, std::index_sequence<Is...>) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		if (result.size() != count) {
			FlatVector::SetSize(result, count);
		}
		if (input_is_null) {
			ConstantVector::SetNull(result, true);
			return;
		}
		auto &result_validity = ConstantVector::Validity(result);
		result_validity.SetValid(0);
		auto result_data = ConstantVector::GetData<RESULT_TYPE>(result);
		result_data[0] = adapter.Operation(result_validity, 0, *ConstantVector::GetData<ARGS>(inputs[Is].get())...);
	}
#endif

	template <idx_t INPUT_INDEX, uint64_t SELECTION_MASK>
	static inline idx_t GenericInputIndex(const std::array<const sel_t *, 2> &selections, idx_t row) {
		if constexpr (SELECTION_MASK & (uint64_t(1) << INPUT_INDEX)) {
			return selections[INPUT_INDEX][row];
		}
		return row;
	}

	template <uint64_t SELECTION_MASK, class RESULT_TYPE, class ADAPTER, class LEFT_TYPE, class RIGHT_TYPE>
	static void
	ExecuteGenericBinaryNullable(const LEFT_TYPE *__restrict left_data, const RIGHT_TYPE *__restrict right_data,
	                             const std::array<UnifiedVectorFormat, 2> &formats,
	                             const std::array<const sel_t *, 2> &selections, RESULT_TYPE *__restrict result_data,
	                             idx_t count, ValidityMask &result_validity, ADAPTER &adapter) {
		auto &left_validity = formats[0].validity;
		auto &right_validity = formats[1].validity;
		for (idx_t row = 0; row < count; row++) {
			auto left_index = GenericInputIndex<0, SELECTION_MASK>(selections, row);
			auto right_index = GenericInputIndex<1, SELECTION_MASK>(selections, row);
			if (left_validity.RowIsValid(left_index) && right_validity.RowIsValid(right_index)) {
				result_data[row] =
				    adapter.Operation(result_validity, row, left_data[left_index], right_data[right_index]);
			} else {
				result_validity.SetInvalid(row);
			}
		}
	}

	template <class RESULT_TYPE, class ADAPTER, class LEFT_TYPE, class RIGHT_TYPE>
	static void ExecuteGenericBinaryNullableSwitch(const LEFT_TYPE *__restrict left_data,
	                                               const RIGHT_TYPE *__restrict right_data,
	                                               const std::array<UnifiedVectorFormat, 2> &formats,
	                                               RESULT_TYPE *__restrict result_data, idx_t count,
	                                               ValidityMask &result_validity, ADAPTER &adapter) {
		std::array<const sel_t *, 2> selections = {{formats[0].sel->data(), formats[1].sel->data()}};
		uint64_t selection_mask = 0;
		selection_mask |= selections[0] ? 1 : 0;
		selection_mask |= selections[1] ? 2 : 0;
		switch (selection_mask) {
		case 0:
			ExecuteGenericBinaryNullable<0>(left_data, right_data, formats, selections, result_data, count,
			                                result_validity, adapter);
			return;
		case 1:
			ExecuteGenericBinaryNullable<1>(left_data, right_data, formats, selections, result_data, count,
			                                result_validity, adapter);
			return;
		case 2:
			ExecuteGenericBinaryNullable<2>(left_data, right_data, formats, selections, result_data, count,
			                                result_validity, adapter);
			return;
		case 3:
			ExecuteGenericBinaryNullable<3>(left_data, right_data, formats, selections, result_data, count,
			                                result_validity, adapter);
			return;
		default:
			InternalException::Throw("Invalid nullable generic scalar executor selection profile");
		}
	}

	template <uint64_t SELECTION_MASK, class RESULT_TYPE, class ADAPTER, class LEFT_TYPE, class RIGHT_TYPE>
	static void
	ExecuteGenericBinaryNoNull(const LEFT_TYPE *__restrict left_data, const RIGHT_TYPE *__restrict right_data,
	                           const std::array<const sel_t *, 2> &selections, RESULT_TYPE *__restrict result_data,
	                           idx_t count, ValidityMask &result_validity, ADAPTER &adapter) {
		auto left_selection = selections[0];
		auto right_selection = selections[1];
		auto result_ptr = result_data;
		auto result_end = result_data + count;
		idx_t result_idx = 0;
		while (result_ptr != result_end) {
			idx_t left_index;
			idx_t right_index;
			if constexpr (SELECTION_MASK & 1) {
				left_index = *left_selection++;
			} else {
				left_index = result_idx;
			}
			if constexpr (SELECTION_MASK & 2) {
				right_index = *right_selection++;
			} else {
				right_index = result_idx;
			}
			*result_ptr =
			    adapter.Operation(result_validity, result_idx, left_data[left_index], right_data[right_index]);
			result_ptr++;
			result_idx++;
		}
	}

	template <class RESULT_TYPE, class ADAPTER, class LEFT_TYPE, class RIGHT_TYPE>
	static void ExecuteGenericBinaryNoNullSwitch(const LEFT_TYPE *__restrict left_data,
	                                             const RIGHT_TYPE *__restrict right_data,
	                                             const std::array<UnifiedVectorFormat, 2> &formats,
	                                             RESULT_TYPE *__restrict result_data, idx_t count,
	                                             ValidityMask &result_validity, ADAPTER &adapter) {
		std::array<const sel_t *, 2> selections = {{formats[0].sel->data(), formats[1].sel->data()}};
		uint64_t selection_mask = 0;
		selection_mask |= selections[0] ? 1 : 0;
		selection_mask |= selections[1] ? 2 : 0;
		switch (selection_mask) {
		case 0:
			ExecuteGenericBinaryNoNull<0>(left_data, right_data, selections, result_data, count, result_validity,
			                              adapter);
			return;
		case 1:
			ExecuteGenericBinaryNoNull<1>(left_data, right_data, selections, result_data, count, result_validity,
			                              adapter);
			return;
		case 2:
			ExecuteGenericBinaryNoNull<2>(left_data, right_data, selections, result_data, count, result_validity,
			                              adapter);
			return;
		case 3:
			ExecuteGenericBinaryNoNull<3>(left_data, right_data, selections, result_data, count, result_validity,
			                              adapter);
			return;
		default:
			InternalException::Throw("Invalid non-null generic scalar executor selection profile");
		}
	}

	template <bool SPECIALIZE_NULLABLE_GENERIC_SELECTIONS, bool SPECIALIZE_NON_NULL_GENERIC_SELECTIONS,
	          bool PRESERVE_RESULT_VALIDITY, class RESULT_TYPE, class ADAPTER, class... ARGS, size_t... Is>
	static void ExecuteGeneric(const std::array<VectorRef, sizeof...(ARGS)> &inputs, Vector &result, idx_t count,
	                           ADAPTER &adapter, std::index_sequence<Is...>) {
		constexpr idx_t N = sizeof...(ARGS);
		std::array<UnifiedVectorFormat, N> formats;
		for (idx_t i = 0; i < N; i++) {
			inputs[i].get().ToUnifiedFormat(formats[i]);
		}
		auto input_data = std::make_tuple(UnifiedVectorFormat::GetData<ARGS>(formats[Is])...);
		result.SetVectorType(VectorType::FLAT_VECTOR);
		if (result.size() != count) {
			FlatVector::SetSize(result, count);
		}
		auto result_data = FlatVector::GetDataMutable<RESULT_TYPE>(result);
		auto &result_validity = FlatVector::ValidityMutable(result);
		auto inputs_can_have_null = PrepareGenericResultValidity(formats.data(), N, result, count,
		                                                         PRESERVE_RESULT_VALIDITY, ADAPTER::ADDS_NULLS);

		if (inputs_can_have_null) {
			if constexpr (SPECIALIZE_NULLABLE_GENERIC_SELECTIONS && N == 2) {
				ExecuteGenericBinaryNullableSwitch(std::get<0>(input_data), std::get<1>(input_data), formats,
				                                   result_data, count, result_validity, adapter);
			} else {
				for (idx_t row = 0; row < count; row++) {
					std::array<idx_t, N> input_indices = {{formats[Is].sel->get_index(row)...}};
					if ((... && formats[Is].validity.RowIsValid(input_indices[Is]))) {
						result_data[row] =
						    adapter.Operation(result_validity, row, std::get<Is>(input_data)[input_indices[Is]]...);
					} else {
						result_validity.SetInvalid(row);
					}
				}
			}
		} else if constexpr (SPECIALIZE_NON_NULL_GENERIC_SELECTIONS && N == 2) {
			ExecuteGenericBinaryNoNullSwitch(std::get<0>(input_data), std::get<1>(input_data), formats, result_data,
			                                 count, result_validity, adapter);
		} else {
			for (idx_t row = 0; row < count; row++) {
				result_data[row] = adapter.Operation(result_validity, row,
				                                     std::get<Is>(input_data)[formats[Is].sel->get_index(row)]...);
			}
		}
	}

	template <class POLICY, class RESULT_TYPE, class ADAPTER, class... ARGS, size_t... Is>
	static void ExecuteInternal(const std::array<VectorRef, sizeof...(ARGS)> &inputs, Vector &result, idx_t count,
	                            ADAPTER &adapter, std::index_sequence<Is...> indices) {
		constexpr idx_t N = sizeof...(ARGS);
		static_assert(N > 0, "ScalarExecutor requires at least one input");
		auto profile = GetInputProfile(inputs, indices);
		if (profile.all_constant) {
#if !DUCKDB_SMALLER_BINARY(scalar_executor_constant)
			ExecuteConstant<RESULT_TYPE, ADAPTER, ARGS...>(inputs, result, count, profile.any_constant_null, adapter,
			                                               indices);
#else
			// the generic path computes the one row, which then becomes the constant
			PrepareConstantResult(result);
			ExecuteGeneric<POLICY::SPECIALIZE_NULLABLE_GENERIC_SELECTIONS,
			               POLICY::SPECIALIZE_NON_NULL_GENERIC_SELECTIONS, POLICY::PRESERVE_RESULT_VALIDITY,
			               RESULT_TYPE, ADAPTER, ARGS...>(inputs, result, 1, adapter, indices);
			result.SetVectorType(VectorType::CONSTANT_VECTOR);
			if (result.size() != count) {
				FlatVector::SetSize(result, count);
			}
#endif
			return;
		}
		if constexpr (POLICY::SPECIALIZE_FLAT && N <= 3) {
			if (profile.all_flat_or_constant) {
				if (profile.any_constant_null) {
					result.SetVectorType(VectorType::CONSTANT_VECTOR);
					if (result.size() != count) {
						FlatVector::SetSize(result, count);
					}
					ConstantVector::SetNull(result, true);
					return;
				}
				switch (profile.constant_mask) {
				case 0:
					ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 0, RESULT_TYPE, ADAPTER, ARGS...>(
					    inputs, result, count, adapter, indices);
					return;
				case 1:
					if constexpr (N >= 2) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 1, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				case 2:
					if constexpr (N >= 2) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 2, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				case 3:
					if constexpr (N >= 3) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 3, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				case 4:
					if constexpr (N >= 3) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 4, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				case 5:
					if constexpr (N >= 3) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 5, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				case 6:
					if constexpr (N >= 3) {
						ExecuteFlat<POLICY::PRESERVE_RESULT_VALIDITY, 6, RESULT_TYPE, ADAPTER, ARGS...>(
						    inputs, result, count, adapter, indices);
						return;
					}
					break;
				default:
					InternalException::Throw("Invalid flat/constant scalar executor profile");
				}
			}
		}
		ExecuteGeneric<POLICY::SPECIALIZE_NULLABLE_GENERIC_SELECTIONS, POLICY::SPECIALIZE_NON_NULL_GENERIC_SELECTIONS,
		               POLICY::PRESERVE_RESULT_VALIDITY, RESULT_TYPE, ADAPTER, ARGS...>(inputs, result, count, adapter,
		                                                                                indices);
	}

	template <bool HAS_TRUE_SELECTION, bool HAS_FALSE_SELECTION>
	struct StaticSelectionSink {
		static_assert(HAS_TRUE_SELECTION || HAS_FALSE_SELECTION, "A selection sink requires an output");

		StaticSelectionSink(SelectionVector *true_selection_p, SelectionVector *false_selection_p)
		    : true_selection(true_selection_p ? true_selection_p->data() : nullptr),
		      false_selection(false_selection_p ? false_selection_p->data() : nullptr),
		      true_capacity(true_selection_p ? true_selection_p->Capacity() : 0),
		      false_capacity(false_selection_p ? false_selection_p->Capacity() : 0) {
			D_ASSERT(!HAS_TRUE_SELECTION || true_selection);
			D_ASSERT(!HAS_FALSE_SELECTION || false_selection);
		}

		inline void Append(bool comparison_result, idx_t result_idx) {
			if constexpr (HAS_TRUE_SELECTION) {
				true_selection[true_count] = UnsafeNumericCast<sel_t>(result_idx);
				true_count += comparison_result;
			}
			if constexpr (HAS_FALSE_SELECTION) {
				false_selection[false_count] = UnsafeNumericCast<sel_t>(result_idx);
				false_count += !comparison_result;
			}
		}

		inline bool CanAppendBatch() const {
			return HAS_TRUE_SELECTION;
		}

		inline void AppendBatch(const bool *matches, idx_t count, idx_t offset) {
			D_ASSERT(CanAppendBatch());
			D_ASSERT(true_count <= true_capacity);
			array_ptr<sel_t> destination(true_selection + true_count, true_capacity - true_count);
			if constexpr (HAS_FALSE_SELECTION) {
				D_ASSERT(false_count <= false_capacity);
				array_ptr<sel_t> false_destination(false_selection + false_count, false_capacity - false_count);
				auto selected =
				    SelectionVector::SelectFromBoolean(matches, nullptr, count, destination, false_destination, offset);
				true_count += selected;
				false_count += count - selected;
			} else {
				true_count += SelectionVector::SelectFromBoolean(matches, nullptr, count, destination, {}, offset);
			}
		}

		inline void AppendInvalidRange(const SelectionVector &sel, idx_t start, idx_t end) {
			if constexpr (HAS_FALSE_SELECTION) {
				for (idx_t row = start; row < end; row++) {
					false_selection[false_count++] = UnsafeNumericCast<sel_t>(sel.get_index(row));
				}
			}
		}

		idx_t FillConstant(bool comparison_result, const SelectionVector &sel, idx_t count) {
			if (comparison_result) {
				if constexpr (HAS_TRUE_SELECTION) {
					for (idx_t row = 0; row < count; row++) {
						true_selection[row] = UnsafeNumericCast<sel_t>(sel.get_index(row));
					}
					true_count = count;
				}
			} else if constexpr (HAS_FALSE_SELECTION) {
				for (idx_t row = 0; row < count; row++) {
					false_selection[row] = UnsafeNumericCast<sel_t>(sel.get_index(row));
				}
				false_count = count;
			}
			return Result(count);
		}

		inline idx_t Result(idx_t count) const {
			if constexpr (HAS_TRUE_SELECTION) {
				return true_count;
			}
			return count - false_count;
		}

		sel_t *true_selection;
		sel_t *false_selection;
		idx_t true_capacity;
		idx_t false_capacity;
		idx_t true_count = 0;
		idx_t false_count = 0;
	};

	struct RuntimeSelectionSink {
		RuntimeSelectionSink(SelectionVector *true_selection_p, SelectionVector *false_selection_p)
		    : true_selection(true_selection_p), false_selection(false_selection_p) {
		}

		inline void Append(bool comparison_result, idx_t result_idx) {
			if (true_selection) {
				true_selection->set_index(true_count, result_idx);
				true_count += comparison_result;
			}
			if (false_selection) {
				false_selection->set_index(false_count, result_idx);
				false_count += !comparison_result;
			}
		}

		inline bool CanAppendBatch() const {
			return true_selection;
		}

		inline void AppendBatch(const bool *matches, idx_t count, idx_t offset) {
			D_ASSERT(CanAppendBatch());
			D_ASSERT(true_count <= true_selection->Capacity());
			array_ptr<sel_t> destination(true_selection->data() + true_count, true_selection->Capacity() - true_count);
			if (false_selection) {
				D_ASSERT(false_count <= false_selection->Capacity());
				array_ptr<sel_t> false_destination(false_selection->data() + false_count,
				                                   false_selection->Capacity() - false_count);
				auto selected =
				    SelectionVector::SelectFromBoolean(matches, nullptr, count, destination, false_destination, offset);
				true_count += selected;
				false_count += count - selected;
			} else {
				true_count += SelectionVector::SelectFromBoolean(matches, nullptr, count, destination, {}, offset);
			}
		}

		inline void AppendInvalidRange(const SelectionVector &sel, idx_t start, idx_t end) {
			if (false_selection) {
				for (idx_t row = start; row < end; row++) {
					false_selection->set_index(false_count++, sel.get_index(row));
				}
			}
		}

		idx_t FillConstant(bool comparison_result, const SelectionVector &sel, idx_t count) {
			if (comparison_result) {
				if (true_selection) {
					for (idx_t row = 0; row < count; row++) {
						true_selection->set_index(row, sel.get_index(row));
					}
					true_count = count;
				}
			} else if (false_selection) {
				for (idx_t row = 0; row < count; row++) {
					false_selection->set_index(row, sel.get_index(row));
				}
				false_count = count;
			}
			return Result(count);
		}

		inline idx_t Result(idx_t count) const {
			return true_selection ? true_count : count - false_count;
		}

		SelectionVector *true_selection;
		SelectionVector *false_selection;
		idx_t true_count = 0;
		idx_t false_count = 0;
	};

	template <bool NO_NULL, class ADAPTER, class... ARGS>
	static inline bool SelectOperation(ADAPTER &adapter, ARGS... args) {
		if constexpr (NO_NULL) {
			return adapter.OperationNoNull(args...);
		}
		return adapter.Operation(args...);
	}

	template <class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectConstant(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                            idx_t count, const SINK &sink, ADAPTER &adapter, std::index_sequence<Is...>) {
		auto local_sink = sink;
		bool comparison_result = adapter.Operation(*ConstantVector::GetData<ARGS>(inputs[Is].get())...);
		return local_sink.FillConstant(comparison_result, sel, count);
	}

	template <uint64_t CONSTANT_MASK, class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectFlatLoop(const std::tuple<const ARGS *...> &input_data, const ValidityMask &input_validity,
	                            const SelectionVector &sel, idx_t count, const SINK &sink, ADAPTER &adapter,
	                            std::index_sequence<Is...>) {
		auto local_sink = sink;
		idx_t base_idx = 0;
		auto entry_count = ValidityMask::EntryCount(count);
		for (idx_t entry_idx = 0; entry_idx < entry_count; entry_idx++) {
			auto validity_entry = input_validity.GetValidityEntry(entry_idx);
			auto next = MinValue<idx_t>(base_idx + ValidityMask::BITS_PER_VALUE, count);
			if (ValidityMask::AllValid(validity_entry)) {
				if constexpr (UseBooleanSelectionBatch<ARGS...>()) {
					if (local_sink.CanAppendBatch() && !sel.IsSet() &&
					    next - base_idx == ValidityMask::BITS_PER_VALUE) {
						bool matches[ValidityMask::BITS_PER_VALUE];
						for (idx_t i = 0; i < ValidityMask::BITS_PER_VALUE; i++) {
							const bool comparison_result = adapter.Operation(
							    std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(base_idx + i)]...);
							matches[i] = comparison_result;
						}
						local_sink.AppendBatch(matches, ValidityMask::BITS_PER_VALUE, base_idx);
						base_idx = next;
						continue;
					}
				}
				for (; base_idx < next; base_idx++) {
					auto result_idx = sel.get_index(base_idx);
					bool comparison_result =
					    adapter.Operation(std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(base_idx)]...);
					local_sink.Append(comparison_result, result_idx);
				}
			} else if (ValidityMask::NoneValid(validity_entry)) {
				local_sink.AppendInvalidRange(sel, base_idx, next);
				base_idx = next;
			} else {
				auto start = base_idx;
				for (; base_idx < next; base_idx++) {
					auto result_idx = sel.get_index(base_idx);
					bool comparison_result =
					    ValidityMask::RowIsValid(validity_entry, base_idx - start) &&
					    adapter.Operation(std::get<Is>(input_data)[InputIndex<Is, CONSTANT_MASK>(base_idx)]...);
					local_sink.Append(comparison_result, result_idx);
				}
			}
		}
		return local_sink.Result(count);
	}

	template <uint64_t CONSTANT_MASK, class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectFlat(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                        idx_t count, const SINK &sink, ADAPTER &adapter, std::index_sequence<Is...> indices) {
		auto input_data = std::make_tuple(FlatVector::GetData<ARGS>(inputs[Is].get())...);
		auto input_validity = PrepareFlatInputValidity<CONSTANT_MASK>(inputs, count, indices);
		return SelectFlatLoop<CONSTANT_MASK, SINK, ADAPTER, ARGS...>(input_data, input_validity, sel, count, sink,
		                                                             adapter, indices);
	}

	template <uint64_t SPECIALIZED_MASKS, class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static bool TrySelectFlat(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                          idx_t count, uint64_t constant_mask, const SINK &sink, ADAPTER &adapter,
	                          std::index_sequence<Is...> indices, idx_t &result) {
		constexpr idx_t N = sizeof...(ARGS);
		switch (constant_mask) {
		case 0:
			if constexpr (SPECIALIZED_MASKS & (uint64_t(1) << 0)) {
				result = SelectFlat<0, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 1:
			if constexpr (N >= 2 && (SPECIALIZED_MASKS & (uint64_t(1) << 1))) {
				result = SelectFlat<1, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 2:
			if constexpr (N >= 2 && (SPECIALIZED_MASKS & (uint64_t(1) << 2))) {
				result = SelectFlat<2, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 3:
			if constexpr (N >= 3 && (SPECIALIZED_MASKS & (uint64_t(1) << 3))) {
				result = SelectFlat<3, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 4:
			if constexpr (N >= 3 && (SPECIALIZED_MASKS & (uint64_t(1) << 4))) {
				result = SelectFlat<4, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 5:
			if constexpr (N >= 3 && (SPECIALIZED_MASKS & (uint64_t(1) << 5))) {
				result = SelectFlat<5, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		case 6:
			if constexpr (N >= 3 && (SPECIALIZED_MASKS & (uint64_t(1) << 6))) {
				result = SelectFlat<6, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
				return true;
			}
			break;
		default:
			break;
		}
		return false;
	}

	template <bool RIGHT_CONSTANT, bool CAN_HAVE_NULL, class SINK, class CONSTANT_TYPE, class GENERIC_TYPE,
	          class ADAPTER>
	static idx_t SelectGenericConstantLoop(CONSTANT_TYPE constant, const GENERIC_TYPE *__restrict data,
	                                       const SelectionVector &generic_sel, const ValidityMask &validity,
	                                       const SelectionVector &sel, idx_t count, const SINK &sink,
	                                       ADAPTER &adapter) {
		auto local_sink = sink;
		for (idx_t row = 0; row < count; row++) {
			auto result_idx = sel.get_index(row);
			auto generic_index = generic_sel.get_index(row);
			bool comparison_result = !CAN_HAVE_NULL || validity.RowIsValid(generic_index);
			if (comparison_result) {
				if constexpr (RIGHT_CONSTANT) {
					comparison_result = CAN_HAVE_NULL ? adapter.Operation(data[generic_index], constant)
					                                  : adapter.OperationNoNull(data[generic_index], constant);
				} else {
					comparison_result = CAN_HAVE_NULL ? adapter.Operation(constant, data[generic_index])
					                                  : adapter.OperationNoNull(constant, data[generic_index]);
				}
			}
			local_sink.Append(comparison_result, result_idx);
		}
		return local_sink.Result(count);
	}

	template <uint64_t CONSTANT_MASK, class SINK, class ADAPTER, class LEFT_TYPE, class RIGHT_TYPE>
	static idx_t SelectGenericConstant(const std::array<VectorRef, 2> &inputs, const SelectionVector &sel, idx_t count,
	                                   const SINK &sink, ADAPTER &adapter) {
		static_assert(CONSTANT_MASK == 1 || CONSTANT_MASK == 2, "Exactly one binary input must be constant");
		constexpr idx_t GENERIC_INDEX = CONSTANT_MASK == 1 ? 1 : 0;
		UnifiedVectorFormat generic_format;
		inputs[GENERIC_INDEX].get().ToUnifiedFormat(generic_format);
		auto can_have_null = generic_format.validity.CanHaveNull();
		if constexpr (CONSTANT_MASK == 1) {
			auto constant = *ConstantVector::GetData<LEFT_TYPE>(inputs[0].get());
			auto data = UnifiedVectorFormat::GetData<RIGHT_TYPE>(generic_format);
			if (can_have_null) {
				return SelectGenericConstantLoop<false, true>(constant, data, *generic_format.sel,
				                                              generic_format.validity, sel, count, sink, adapter);
			}
			return SelectGenericConstantLoop<false, false>(constant, data, *generic_format.sel, generic_format.validity,
			                                               sel, count, sink, adapter);
		}
		auto constant = *ConstantVector::GetData<RIGHT_TYPE>(inputs[1].get());
		auto data = UnifiedVectorFormat::GetData<LEFT_TYPE>(generic_format);
		if (can_have_null) {
			return SelectGenericConstantLoop<true, true>(constant, data, *generic_format.sel, generic_format.validity,
			                                             sel, count, sink, adapter);
		}
		return SelectGenericConstantLoop<true, false>(constant, data, *generic_format.sel, generic_format.validity, sel,
		                                              count, sink, adapter);
	}

	template <bool NO_NULL, class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectGenericLoop(std::tuple<const ARGS *...> &input_data,
	                               std::array<UnifiedVectorFormat, sizeof...(ARGS)> &formats,
	                               const SelectionVector &sel, idx_t count, const SINK &sink, ADAPTER &adapter,
	                               std::index_sequence<Is...>) {
		auto local_sink = sink;
		constexpr idx_t N = sizeof...(ARGS);
		for (idx_t row = 0; row < count; row++) {
			auto result_idx = sel.get_index(row);
			std::array<idx_t, N> input_indices = {{formats[Is].sel->get_index(row)...}};
			bool comparison_result = (NO_NULL || (... && formats[Is].validity.RowIsValid(input_indices[Is]))) &&
			                         SelectOperation<NO_NULL>(adapter, std::get<Is>(input_data)[input_indices[Is]]...);
			local_sink.Append(comparison_result, result_idx);
		}
		return local_sink.Result(count);
	}

	template <class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectGeneric(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                           idx_t count, const SINK &sink, ADAPTER &adapter, std::index_sequence<Is...> indices) {
		std::array<UnifiedVectorFormat, sizeof...(ARGS)> formats;
		for (idx_t i = 0; i < sizeof...(ARGS); i++) {
			inputs[i].get().ToUnifiedFormat(formats[i]);
		}
		auto input_data = std::make_tuple(UnifiedVectorFormat::GetData<ARGS>(formats[Is])...);
		if ((... || formats[Is].validity.CanHaveNull())) {
			return SelectGenericLoop<false, SINK, ADAPTER, ARGS...>(input_data, formats, sel, count, sink, adapter,
			                                                        indices);
		}
		return SelectGenericLoop<true, SINK, ADAPTER, ARGS...>(input_data, formats, sel, count, sink, adapter, indices);
	}

	template <class POLICY, class SINK, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectInternal(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                            idx_t count, const InputProfile &profile, const SINK &sink, ADAPTER &adapter,
	                            std::index_sequence<Is...> indices) {
		constexpr idx_t N = sizeof...(ARGS);
		if (profile.all_constant) {
			if (profile.any_constant_null) {
				auto local_sink = sink;
				return local_sink.FillConstant(false, sel, count);
			}
			return SelectConstant<SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
		}
		if constexpr (POLICY::SPECIALIZED_MASKS != 0 && N <= 3) {
			if (profile.all_flat_or_constant) {
				if (profile.any_constant_null) {
					auto local_sink = sink;
					return local_sink.FillConstant(false, sel, count);
				}
				idx_t result;
				if (TrySelectFlat<POLICY::SPECIALIZED_MASKS, SINK, ADAPTER, ARGS...>(
				        inputs, sel, count, profile.constant_mask, sink, adapter, indices, result)) {
					return result;
				}
			}
		}
		if constexpr (POLICY::SPECIALIZED_MASKS != 0 && N == 2) {
			if (!profile.any_constant_null) {
				switch (profile.constant_mask) {
				case 1:
					return SelectGenericConstant<1, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter);
				case 2:
					return SelectGenericConstant<2, SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter);
				default:
					break;
				}
			}
		}
		return SelectGeneric<SINK, ADAPTER, ARGS...>(inputs, sel, count, sink, adapter, indices);
	}

	template <class POLICY, class ADAPTER, class... ARGS, size_t... Is>
	static idx_t SelectOutputDispatch(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel,
	                                  idx_t count, SelectionVector *true_sel, SelectionVector *false_sel,
	                                  const InputProfile &profile, ADAPTER &adapter,
	                                  std::index_sequence<Is...> indices) {
		if constexpr (POLICY::SPECIALIZE_OUTPUTS) {
			if (true_sel && false_sel) {
				StaticSelectionSink<true, true> sink(true_sel, false_sel);
				return SelectInternal<POLICY, decltype(sink), ADAPTER, ARGS...>(inputs, sel, count, profile, sink,
				                                                                adapter, indices);
			} else if (true_sel) {
				StaticSelectionSink<true, false> sink(true_sel, false_sel);
				return SelectInternal<POLICY, decltype(sink), ADAPTER, ARGS...>(inputs, sel, count, profile, sink,
				                                                                adapter, indices);
			}
			StaticSelectionSink<false, true> sink(true_sel, false_sel);
			return SelectInternal<POLICY, decltype(sink), ADAPTER, ARGS...>(inputs, sel, count, profile, sink, adapter,
			                                                                indices);
		}
		RuntimeSelectionSink sink(true_sel, false_sel);
		return SelectInternal<POLICY, decltype(sink), ADAPTER, ARGS...>(inputs, sel, count, profile, sink, adapter,
		                                                                indices);
	}

	template <class POLICY, class ADAPTER, class... ARGS, size_t... Is>
	DUCKDB_SCALAR_EXECUTOR_NOINLINE static idx_t
	SelectFallback(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector &sel, idx_t count,
	               SelectionVector *true_sel, SelectionVector *false_sel, const InputProfile &profile, ADAPTER &adapter,
	               std::index_sequence<Is...> indices) {
		return SelectOutputDispatch<POLICY, ADAPTER, ARGS...>(inputs, sel, count, true_sel, false_sel, profile, adapter,
		                                                      indices);
	}

public:
	template <class POLICY, class RESULT_TYPE, class ADAPTER, class... ARGS>
	static void Execute(const std::array<VectorRef, sizeof...(ARGS)> &inputs, Vector &result, idx_t count,
	                    ADAPTER &adapter) {
		ExecuteInternal<POLICY, RESULT_TYPE, ADAPTER, ARGS...>(inputs, result, count, adapter,
		                                                       std::index_sequence_for<ARGS...> {});
	}

	template <class POLICY, class ADAPTER, class... ARGS>
	static idx_t Select(const std::array<VectorRef, sizeof...(ARGS)> &inputs, const SelectionVector *sel, idx_t count,
	                    SelectionVector *true_sel, SelectionVector *false_sel, ADAPTER &adapter) {
		if (!true_sel && !false_sel) {
			InternalException::Throw("Either true or false sel must be set");
		}
		if (!sel) {
			sel = FlatVector::IncrementalSelectionVector();
		}
		auto indices = std::index_sequence_for<ARGS...> {};
		auto profile = GetInputProfile(inputs, indices);
		if constexpr (POLICY::DIRECT_TRUE_FLAT_MASKS != 0) {
			static_assert(sizeof...(ARGS) <= 3, "Direct true-only selection supports up to three inputs");
			if (true_sel && !false_sel) {
				if (profile.all_flat_or_constant &&
				    (POLICY::DIRECT_TRUE_FLAT_MASKS & (uint64_t(1) << profile.constant_mask))) {
					StaticSelectionSink<true, false> sink(true_sel, false_sel);
					if (profile.any_constant_null) {
						return sink.FillConstant(false, *sel, count);
					}
					idx_t result;
					if (TrySelectFlat<POLICY::DIRECT_TRUE_FLAT_MASKS, decltype(sink), ADAPTER, ARGS...>(
					        inputs, *sel, count, profile.constant_mask, sink, adapter, indices, result)) {
						return result;
					}
				}
				return SelectFallback<POLICY, ADAPTER, ARGS...>(inputs, *sel, count, true_sel, false_sel, profile,
				                                                adapter, indices);
			}
		}
		return SelectOutputDispatch<POLICY, ADAPTER, ARGS...>(inputs, *sel, count, true_sel, false_sel, profile,
		                                                      adapter, indices);
	}
};

} // namespace duckdb

#undef DUCKDB_SCALAR_EXECUTOR_NOINLINE
