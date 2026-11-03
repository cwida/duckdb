//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/table_filter_function_helpers.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/filter/table_filter_functions.hpp"

#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/planner/table_filter_state.hpp"

namespace duckdb {

struct SelectivityTrackingLocalState : public FunctionLocalState {
	SelectivityTrackingLocalState(idx_t n_vectors_to_check, float selectivity_threshold)
	    : stats(n_vectors_to_check, selectivity_threshold) {
	}

	void Update(idx_t accepted, idx_t processed) {
		stats.Update(accepted, processed);
	}

	bool IsActive() const {
		return stats.IsActive();
	}

	SelectivityOptionalFilterState::SelectivityStats stats;
};

inline unique_ptr<FunctionLocalState> InitSelectivityTrackingLocalState(idx_t n_vectors_to_check,
                                                                        float selectivity_threshold) {
	if (n_vectors_to_check == 0) {
		return nullptr;
	}
	return make_uniq<SelectivityTrackingLocalState>(n_vectors_to_check, selectivity_threshold);
}

inline idx_t SetAllTrueSelection(idx_t count, optional_ptr<SelectionVector> true_sel,
                                 optional_ptr<SelectionVector> false_sel) {
	if (true_sel) {
		for (idx_t i = 0; i < count; i++) {
			true_sel->set_index(i, i);
		}
	}
	return count;
}

inline idx_t SetAllTrueSelection(idx_t count, optional_ptr<const SelectionVector> sel,
                                 optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	if (!sel) {
		return SetAllTrueSelection(count, true_sel, false_sel);
	}
	if (true_sel) {
		for (idx_t i = 0; i < count; i++) {
			true_sel->set_index(i, sel->get_index(i));
		}
	}
	return count;
}

inline idx_t FillSelectionInversion(idx_t count, const SelectionVector &true_sel, idx_t true_count,
                                    optional_ptr<SelectionVector> false_sel) {
	if (!false_sel) {
		return count - true_count;
	}
	return SelectionVector::Inverted(true_sel, *false_sel, true_count, count);
}

inline SelectionVector &GetFilterResultSelection(idx_t count, optional_ptr<const SelectionVector> input_sel,
                                                 optional_ptr<SelectionVector> true_sel, SelectionVector &scratch_sel) {
	if (true_sel && (!input_sel || true_sel.get() != input_sel.get())) {
		return *true_sel;
	}
	// Preserve the input selection until TranslateSelection has mapped the local matches.
	if (scratch_sel.Capacity() < count) {
		scratch_sel.Initialize(count);
	}
	return scratch_sel;
}

inline idx_t TranslateSelection(idx_t count, optional_ptr<const SelectionVector> input_sel,
                                const SelectionVector &local_true_sel, idx_t local_true_count,
                                optional_ptr<SelectionVector> true_sel, optional_ptr<SelectionVector> false_sel) {
	if (local_true_count == count) {
		return SetAllTrueSelection(count, input_sel, true_sel, false_sel);
	}
	if (!input_sel) {
		if (true_sel && true_sel.get() != &local_true_sel) {
			for (idx_t i = 0; i < local_true_count; i++) {
				true_sel->set_index(i, local_true_sel.get_index(i));
			}
		}
		if (false_sel) {
			FillSelectionInversion(count, local_true_sel, local_true_count, false_sel);
		}
		return local_true_count;
	}
	if (false_sel) {
		idx_t false_count = 0;
		idx_t true_offset = 0;
		for (idx_t i = 0; i < count; i++) {
			// Read both indices before either output can overwrite them.
			auto row = input_sel->get_index(i);
			if (true_offset < local_true_count && local_true_sel.get_index(true_offset) == i) {
				if (true_sel) {
					true_sel->set_index(true_offset, row);
				}
				true_offset++;
			} else {
				false_sel->set_index(false_count++, row);
			}
		}
	} else if (true_sel) {
		for (idx_t i = 0; i < local_true_count; i++) {
			true_sel->set_index(i, input_sel->get_index(local_true_sel.get_index(i)));
		}
	}
	return local_true_count;
}

inline void SetConstantBooleanResult(Vector &result, bool value) {
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ConstantVector::GetData<bool>(result)[0] = value;
}

inline void SelectionToBooleanResult(idx_t count, const SelectionVector &sel, idx_t sel_count, Vector &result) {
	if (count == 0 || sel_count == 0) {
		SetConstantBooleanResult(result, false);
		return;
	}
	if (sel_count == count) {
		SetConstantBooleanResult(result, true);
		return;
	}
	result.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::ValidityMutable(result).SetAllValid(count);
	auto result_data = FlatVector::GetDataMutable<bool>(result);
	for (idx_t i = 0; i < count; i++) {
		result_data[i] = false;
	}
	for (idx_t i = 0; i < sel_count; i++) {
		result_data[sel.get_index(i)] = true;
	}
}

inline void SetAllTrue(DataChunk &args, Vector &result) {
	SetConstantBooleanResult(result, true);
}

template <class TRACKING_STATE, class EXECUTOR>
inline void ExecuteWithSelectivityTracking(DataChunk &args, Vector &result, TRACKING_STATE *tracking_state,
                                           EXECUTOR &&execute) {
	if (tracking_state && !tracking_state->IsActive()) {
		SetAllTrue(args, result);
		tracking_state->Update(0, 0);
		return;
	}
	auto approved_count = execute();
	if (tracking_state) {
		tracking_state->Update(approved_count, args.size());
	}
}

void TableFilterFunctionSerialize(Serializer &serializer, const optional_ptr<FunctionData> bind_data,
                                  const BoundScalarFunction &function);
unique_ptr<FunctionData> TableFilterFunctionDeserialize(Deserializer &deserializer, BoundScalarFunction &function);

inline string FormatOptionalFilterString(const string &child_filter_string) {
	if (child_filter_string.empty()) {
		return "optional";
	}
	return "optional: " + child_filter_string;
}

} // namespace duckdb
