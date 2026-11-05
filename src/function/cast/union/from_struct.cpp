#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

namespace duckdb {

bool StructToUnionCast::AllowImplicitCastFromStruct(const LogicalType &source, const LogicalType &target) {
	if (source.id() != LogicalTypeId::STRUCT) {
		return false;
	}
	auto target_fields = StructType::GetChildTypes(target);
	auto fields = StructType::GetChildTypes(source);
	if (target_fields.size() != fields.size()) {
		// Struct should have the same amount of fields as the union
		return false;
	}
	for (idx_t i = 0; i < target_fields.size(); i++) {
		auto &target_field = target_fields[i].second;
		auto &target_field_name = target_fields[i].first;
		auto &field = fields[i].second;
		auto &field_name = fields[i].first;
		if (i == 0) {
			// For the tag field we don't accept a type substitute as varchar
			if (target_field != field) {
				return false;
			}
			continue;
		}
		if (target_field_name != field_name) {
			return false;
		}
		if (target_field != field && field != LogicalType::VARCHAR) {
			// We allow the field to be VARCHAR, since unsupported types get cast to VARCHAR by EXPORT DATABASE (format
			// PARQUET) i.e UNION(a BIT) becomes STRUCT(a VARCHAR)
			return false;
		}
	}
	return true;
}

// Physical Cast execution

static string GetUnionInvalidReasonMessage(UnionInvalidReason reason) {
	switch (reason) {
	case UnionInvalidReason::TAG_OUT_OF_RANGE:
		return "One or more of the tags do not point to a valid union member";
	case UnionInvalidReason::VALIDITY_OVERLAP:
		return "One or more rows in the produced UNION have validity set for more than 1 member";
	case UnionInvalidReason::TAG_MISMATCH:
		return "One or more rows in the produced UNION have tags that don't point to the valid member";
	case UnionInvalidReason::NULL_TAG:
		return "One or more rows in the produced UNION have a NULL tag";
	default:
		throw InternalException("Struct to union cast failed for unknown reason");
	}
}

bool StructToUnionCast::Cast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<StructBoundCastData>();
	auto &lstate = parameters.local_state->Cast<StructCastLocalState>();

	D_ASSERT(source.GetType().id() == LogicalTypeId::STRUCT);
	D_ASSERT(result.GetType().id() == LogicalTypeId::UNION);
	D_ASSERT(cast_data.target.id() == LogicalTypeId::UNION);

	auto &source_children = StructVector::GetEntries(source);
	auto &target_children = StructVector::GetEntries(result);
	bool all_converted = true;

	for (idx_t i = 0; i < source_children.size(); i++) {
		auto &result_child_vector = target_children[i];
		auto &source_child_vector = source_children[i];
		CastParameters child_parameters(parameters, cast_data.child_cast_info[i].GetCastData(), lstate.local_states[i]);
		if (!cast_data.child_cast_info[i].Cast(source_child_vector, result_child_vector, count, child_parameters)) {
			// the failing values have been set to NULL by the (TRY_CAST) member cast
			all_converted = false;
		}
		// we flatten the child because we use FlatVector::SetNull below and we may get non-flat from source/cast
		result_child_vector.Flatten();
	}

	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		ConstantVector::SetNull(result, ConstantVector::IsNull(source));

		// if the tag is NULL, the union should be NULL
		auto &tag_vec = target_children[0];
		ConstantVector::SetNull(result, ConstantVector::IsNull(tag_vec));
	} else {
		// if the tag is NULL, the union should be NULL
		auto &tag_vec = target_children[0];
		auto source_validity = source.Validity();
		auto tag_validity = tag_vec.Validity();

		for (idx_t i = 0; i < count; i++) {
			if (!source_validity.IsValid(i) || !tag_validity.IsValid(i)) {
				FlatVector::SetNull(result, i, true);
			}
		}
	}

	const bool is_constant = result.GetVectorType() == VectorType::CONSTANT_VECTOR;
	const idx_t row_count = is_constant ? 1 : count;
	if (UnionVector::CheckUnionValidity(result, row_count) != UnionInvalidReason::VALID) {
		// find the invalid rows - these raise an error, or become NULL for TRY_CAST
		SelectionVector row_sel(1);
		for (idx_t row = 0; row < row_count; row++) {
			row_sel.set_index(0, row);
			auto reason = UnionVector::CheckUnionValidity(result, 1, row_sel);
			if (reason == UnionInvalidReason::VALID) {
				continue;
			}
			HandleCastError::AssignError(GetUnionInvalidReasonMessage(reason), parameters);
			if (is_constant) {
				ConstantVector::SetNull(result, true);
			} else {
				FlatVector::SetNull(result, row, true);
			}
			all_converted = false;
		}
	}

	FlatVector::SetSize(result, count_t(count));
	result.Verify();
	return all_converted;
}

// Bind cast

unique_ptr<BoundCastData> StructToUnionCast::BindData(BindCastInput &input, const LogicalType &source,
                                                      const LogicalType &target) {
	vector<BoundCastInfo> child_cast_info;
	D_ASSERT(source.id() == LogicalTypeId::STRUCT);
	D_ASSERT(target.id() == LogicalTypeId::UNION);

	auto result_child_count = StructType::GetChildCount(target);
	D_ASSERT(result_child_count == StructType::GetChildCount(source));

	for (idx_t i = 0; i < result_child_count; i++) {
		auto &source_child = StructType::GetChildType(source, i);
		auto &target_child = StructType::GetChildType(target, i);

		auto child_cast = input.GetCastFunction(source_child, target_child);
		child_cast_info.push_back(std::move(child_cast));
	}
	return make_uniq<StructBoundCastData>(std::move(child_cast_info), target);
}

BoundCastInfo StructToUnionCast::Bind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto cast_data = StructToUnionCast::BindData(input, source, target);
	return BoundCastInfo(&StructToUnionCast::Cast, std::move(cast_data), StructBoundCastData::InitStructCastLocalState);
}

} // namespace duckdb
