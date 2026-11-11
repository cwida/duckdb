#include "duckdb/common/vector_operations/scalar_executor.hpp"

namespace duckdb {

void ScalarExecutor::PrepareConstantResult(Vector &result) {
	result.SetVectorType(VectorType::FLAT_VECTOR);
	FlatVector::ValidityMutable(result).SetValid(0);
}

bool ScalarExecutor::PrepareGenericResultValidity(const UnifiedVectorFormat *formats, idx_t format_count,
                                                  Vector &result, idx_t count, bool preserve_result_validity,
                                                  bool adds_nulls) {
	bool inputs_can_have_null = false;
	for (idx_t input_idx = 0; input_idx < format_count; input_idx++) {
		if (formats[input_idx].validity.CanHaveNull()) {
			inputs_can_have_null = true;
			break;
		}
	}

	auto &result_validity = FlatVector::ValidityMutable(result);
	if (inputs_can_have_null || !preserve_result_validity) {
		result_validity.Reset(count);
	} else if (adds_nulls && result_validity.CanHaveNull()) {
		ValidityMask preserved(result_validity, count);
		result_validity.Initialize(preserved);
	}
	return inputs_can_have_null;
}

} // namespace duckdb
