#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/exception/conversion_exception.hpp"
#include "duckdb/function/cast/bound_cast_data.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

struct UnionMemberBoundCastData : public BoundCastData {
	// mapping from source member index to target member index
	// these are always the same size as the source member count
	// (since all source members must be present in the target)
	vector<union_tag_t> tag_map;
	vector<BoundCastInfo> member_casts;

	LogicalType type;

	bool is_member_to_member;

	UnionMemberBoundCastData(vector<union_tag_t> tag_map, vector<BoundCastInfo> member_casts, const LogicalType &type,
	                         bool is_member_to_member)
	    : tag_map(std::move(tag_map)), member_casts(std::move(member_casts)), type(type.Copy()),
	      is_member_to_member(is_member_to_member) {
	}

	UnionMemberBoundCastData(vector<union_tag_t> tag_map, vector<BoundCastInfo> member_cast_infos,
	                         const LogicalType &type)
	    : tag_map(std::move(tag_map)), member_casts(std::move(member_cast_infos)), type(type.Copy()),
	      is_member_to_member(true) {
	}

	// This is ugly to take a vector of BoundCastInfo, however otherwise the compiler complains
	UnionMemberBoundCastData(union_tag_t member_idx, BoundCastInfo member_cast_info, const LogicalType &type)
	    : tag_map(1, member_idx), type(type.Copy()), is_member_to_member(false) {
		member_casts = vector<BoundCastInfo>();
		member_casts.emplace_back(member_cast_info.Copy());
	}

public:
	unique_ptr<BoundCastData> Copy() const override {
		vector<BoundCastInfo> member_casts_copy;
		for (auto &member_cast : member_casts) {
			member_casts_copy.push_back(member_cast.Copy());
		}
		return make_uniq<UnionMemberBoundCastData>(tag_map, std::move(member_casts_copy), type, is_member_to_member);
	}
};

//--------------------------------------------------------------------------------------------------
// ??? -> UNION
//--------------------------------------------------------------------------------------------------
// if the source can be implicitly cast to a member of the target union, the cast is valid

static unique_ptr<BoundCastData> BindToUnionMemberCast(BindCastInput &input, const LogicalType &source,
                                                       const LogicalType &target) {
	D_ASSERT(target.id() == LogicalTypeId::UNION);

	// Vector of names/candidates that are optimal for cost
	vector<string> names;
	vector<UnionMemberBoundCastData> candidates;

	int64_t best_cost = std::numeric_limits<int64_t>::max();

	for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(target); member_idx++) {
		auto member_type = UnionType::GetMemberType(target, member_idx);
		auto member_name = UnionType::GetMemberName(target, member_idx);
		auto member_cast_cost = input.function_set.ImplicitCastCost(nullptr, source, member_type);
		if (member_cast_cost >= 0 && member_cast_cost <= best_cost) {
			if (member_cast_cost != best_cost) {
				names.clear();
				candidates.clear();
				best_cost = member_cast_cost;
			}
			names.emplace_back(member_name);
			candidates.emplace_back(member_idx, input.GetCastFunction(source, member_type), member_type);
		}
	};

	// no possible casts found!
	if (candidates.empty()) {
		auto message = StringUtil::Format(
		    "Type %s can't be cast as %s. %s can't be implicitly cast to any of the union member types: ",
		    source.ToString(), target.ToString(), source.ToString());

		auto member_count = UnionType::GetMemberCount(target);
		for (idx_t member_idx = 0; member_idx < member_count; member_idx++) {
			auto member_type = UnionType::GetMemberType(target, member_idx);
			message += member_type.ToString();
			if (member_idx < member_count - 1) {
				message += ", ";
			}
		}
		throw ConversionException(message);
	}

	// check if the cast is ambiguous (2 or more casts have the same cost)
	if (candidates.size() > 1) {
		// collect all the ambiguous types
		auto message = StringUtil::Format(
		    "Type %s can't be cast as %s. The cast is ambiguous, multiple possible members in target: ", source,
		    target);
		for (size_t i = 0; i < candidates.size(); i++) {
			message += StringUtil::Format("'%s (%s)'", names[i], candidates[i].type.ToString());
			if (i < candidates.size() - 1) {
				message += ", ";
			}
		}
		message += ". Disambiguate the target type by using the 'union_value(<tag> := <arg>)' function to promote the "
		           "source value to a single member union before casting.";
		throw ConversionException(message);
	}

	// otherwise, return the selected cast, which is the only member cast that is applied
	input.stability = candidates[0].member_casts[0].GetStability();
	return make_uniq<UnionMemberBoundCastData>(std::move(candidates[0]));
}

//--------------------------------------------------------------------------------------------------
// UNION -> UNION
//--------------------------------------------------------------------------------------------------
// if the source member tags is a subset of the target member tags, and all the source
// members can be implicitly cast to the corresponding target members, the cast is valid.
// If this fails and the source can be implicitly cast to a member of target, the cast is also
// valid.
//
// VALID: 	UNION(A, B) 	-> 	UNION(A, B, C)
// VALID: 	UNION(A, C) 	-> 	UNION(A, UNION(A, C)) will result in A -> A and C -> UNION(A, C)
// VALID: 	UNION(A, C) 	-> 	UNION(UNION(A, C)) will result in C -> UNION(A, C)
// VALID: 	UNION(A, B) 	-> 	UNION(A, C)		if B can be implicitly cast to C
//
// INVALID: UNION(A, B, C)	->	UNION(A, B)
// INVALID:	UNION(A, B) 	->	UNION(A, C)		if B can't be implicitly cast to C
// INVALID:	UNION(A, B, D) 	->	UNION(A, B, C)

static unique_ptr<BoundCastData> BindUnionToUnionCast(BindCastInput &input, const LogicalType &source,
                                                      const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::UNION);
	D_ASSERT(target.id() == LogicalTypeId::UNION);

	auto source_member_count = UnionType::GetMemberCount(source);

	auto tag_map = vector<union_tag_t>(source_member_count);
	auto member_casts = vector<BoundCastInfo>();

	for (idx_t source_idx = 0; source_idx < source_member_count; source_idx++) {
		auto &source_member_type = UnionType::GetMemberType(source, source_idx);
		auto &source_member_name = UnionType::GetMemberName(source, source_idx);

		bool found = false;
		for (idx_t target_idx = 0; target_idx < UnionType::GetMemberCount(target); target_idx++) {
			auto &target_member_name = UnionType::GetMemberName(target, target_idx);

			// found a matching member
			if (source_member_name == target_member_name) {
				auto &target_member_type = UnionType::GetMemberType(target, target_idx);
				tag_map[source_idx] = static_cast<union_tag_t>(target_idx);
				member_casts.push_back(input.GetCastFunction(source_member_type, target_member_type));
				found = true;
				break;
			}
		}
		if (!found) {
			// Try casting source to a member of target, but if it fails
			// use our error message.
			try {
				return BindToUnionMemberCast(input, source, target);
			} catch (ConversionException &_) {
			};
			// no matching member tag found in the target set
			auto message =
			    StringUtil::Format("Type %s can't be cast as %s. The member %s is not present in target union",
			                       source.ToString(), target.ToString(), source_member_name);
			throw ConversionException(message);
		}
	}

	return make_uniq<UnionMemberBoundCastData>(tag_map, std::move(member_casts), target);
}

static unique_ptr<FunctionLocalState> InitToUnionLocalState(CastLocalStateParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<UnionMemberBoundCastData>();

	if (cast_data.is_member_to_member) {
		auto result = make_uniq<StructCastLocalState>();

		for (auto &entry : cast_data.member_casts) {
			unique_ptr<FunctionLocalState> child_state;
			if (entry.HasInitLocalState()) {
				CastLocalStateParameters child_params(parameters, entry.GetCastData());
				child_state = entry.InitLocalState(child_params);
			}
			result->local_states.push_back(std::move(child_state));
		}
		return std::move(result);
	} else {
		auto member_cast_info = cast_data.member_casts[0].Copy();
		if (!member_cast_info.HasInitLocalState()) {
			return nullptr;
		}
		CastLocalStateParameters child_parameters(parameters, member_cast_info.GetCastData());
		return member_cast_info.InitLocalState(child_parameters);
	}
}

static bool ToUnionMemberCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::UNION);
	auto &cast_data = parameters.cast_data->Cast<UnionMemberBoundCastData>();
	auto &selected_member_vector = UnionVector::GetMember(result, cast_data.tag_map[0]);
	auto member_cast_info = cast_data.member_casts[0].Copy();

	CastParameters child_parameters(parameters, member_cast_info.GetCastData(), parameters.local_state);
	bool all_converted = member_cast_info.Cast(source, selected_member_vector, count, child_parameters);
	UnionVector::SetToMember(result, cast_data.tag_map[0], selected_member_vector, count, true);
	if (!all_converted) {
		// rows that failed to cast (under TRY_CAST) are NULL in the member - make the union NULL as well
		auto source_validity = source.Validity();
		auto member_validity = selected_member_vector.Validity();
		if (result.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (source_validity.IsValid(0) && !member_validity.IsValid(0)) {
				ConstantVector::SetNull(result, count_t(count));
			}
		} else {
			for (idx_t row_idx = 0; row_idx < count; row_idx++) {
				if (source_validity.IsValid(row_idx) && !member_validity.IsValid(row_idx)) {
					FlatVector::SetNull(result, row_idx, true);
				}
			}
		}
	}

	result.Verify();

	return all_converted;
}

static bool UnionMemberToMemberCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &cast_data = parameters.cast_data->Cast<UnionMemberBoundCastData>();
	auto &lstate = parameters.local_state->Cast<StructCastLocalState>();

	auto source_member_count = UnionType::GetMemberCount(source.GetType());
	auto target_member_count = UnionType::GetMemberCount(result.GetType());

	auto target_member_is_mapped = vector<bool>(target_member_count);

	// Perform the casts from source to target members
	bool all_converted = true;
	for (idx_t member_idx = 0; member_idx < source_member_count; member_idx++) {
		auto target_member_idx = cast_data.tag_map[member_idx];

		auto &source_member_vector = UnionVector::GetMember(source, member_idx);
		auto &target_member_vector = UnionVector::GetMember(result, target_member_idx);
		auto &member_cast = cast_data.member_casts[member_idx];

		CastParameters child_parameters(parameters, member_cast.GetCastData(), lstate.local_states[member_idx]);
		if (!member_cast.Cast(source_member_vector, target_member_vector, count, child_parameters)) {
			all_converted = false;
		}

		target_member_is_mapped[target_member_idx] = true;
	}

	// Set the unmapped target members to constant NULL.
	// If we cast UNION(A, B) -> UNION(A, B, C) we need to invalidate C so that
	// the invariants of the result union hold. (only member columns "selected"
	// by the rowwise corresponding tag in the tag vector should be valid)
	for (idx_t target_member_idx = 0; target_member_idx < target_member_count; target_member_idx++) {
		if (!target_member_is_mapped[target_member_idx]) {
			auto &target_member_vector = UnionVector::GetMember(result, target_member_idx);
			ConstantVector::SetNull(target_member_vector, count_t(count));
		}
	}

	// Update the tags in the result vector
	auto &source_tag_vector = UnionVector::GetTags(source);
	auto &result_tag_vector = UnionVector::GetTags(result);

	if (source.GetVectorType() == VectorType::CONSTANT_VECTOR) {
		// Constant vector case optimization
		// member casts can emit non-flat vectors (e.g. a shredded VARIANT) - flatten before marking constant
		result.FlattenAndSetConstant();
		if (ConstantVector::IsNull(source)) {
			ConstantVector::SetNull(result, count_t(count));
		} else {
			// map the tag
			auto source_tag = ConstantVector::GetData<union_tag_t>(source_tag_vector)[0];
			auto mapped_tag = cast_data.tag_map[source_tag];
			ConstantVector::GetData<union_tag_t>(result_tag_vector)[0] = UnsafeNumericCast<union_tag_t>(mapped_tag);
			// the selected member failed to cast (under TRY_CAST) - the union becomes NULL
			if (!all_converted && UnionVector::GetMember(source, source_tag).Validity().IsValid(0) &&
			    !UnionVector::GetMember(result, mapped_tag).Validity().IsValid(0)) {
				ConstantVector::SetNull(result, count_t(count));
			}
		}
	} else {
		// Otherwise, use the unified vector format to access the source vector.

		// Ensure that all the result members are flat vectors
		// This is not always the case, e.g. when a member is cast using the default TryNullCast function
		// the resulting member vector will be a constant null vector.
		for (idx_t target_member_idx = 0; target_member_idx < target_member_count; target_member_idx++) {
			UnionVector::GetMember(result, target_member_idx).Flatten();
		}

		// We assume that a union tag vector validity matches the union vector validity.
		auto source_tag_entries = source_tag_vector.Values<union_tag_t>();
		vector<unique_ptr<VectorValidityIterator>> source_member_validity;
		if (!all_converted) {
			for (idx_t member_idx = 0; member_idx < source_member_count; member_idx++) {
				source_member_validity.push_back(
				    make_uniq<VectorValidityIterator>(UnionVector::GetMember(source, member_idx)));
			}
		}

		for (idx_t row_idx = 0; row_idx < count; row_idx++) {
			auto entry = source_tag_entries[row_idx];
			if (entry.IsValid()) {
				// map the tag
				auto target_tag = cast_data.tag_map[entry.GetValue()];
				FlatVector::GetDataMutable<union_tag_t>(result_tag_vector)[row_idx] =
				    UnsafeNumericCast<union_tag_t>(target_tag);
				// the selected member failed to cast (under TRY_CAST) - the union becomes NULL
				if (!all_converted && source_member_validity[entry.GetValue()]->IsValid(row_idx) &&
				    !FlatVector::Validity(UnionVector::GetMember(result, target_tag)).RowIsValid(row_idx)) {
					FlatVector::SetNull(result, row_idx, true);
				}
			} else {
				// Issue: The members of the result is not always flatvectors
				// In the case of TryNullCast, the result member is constant.
				FlatVector::SetNull(result, row_idx, true);
			}
		}
	}

	FlatVector::SetSize(result, count_t(count));
	result.Verify();

	return all_converted;
}

//--------------------------------------------------------------------------------------------------
// Impossible casts: raise the bind error when executed, TRY_CAST turns it into NULL
//--------------------------------------------------------------------------------------------------
struct UnionImpossibleCastData : public BoundCastData {
	explicit UnionImpossibleCastData(string message_p) : message(std::move(message_p)) {
	}

	string message;

	unique_ptr<BoundCastData> Copy() const override {
		return make_uniq<UnionImpossibleCastData>(message);
	}
};

static bool UnionImpossibleCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	bool success = true;
	if (VectorOperations::HasNotNull(source)) {
		HandleCastError::AssignError(parameters.cast_data->Cast<UnionImpossibleCastData>().message, parameters);
		success = false;
	}
	ConstantVector::SetNull(result, count_t(count));
	return success;
}

static bool ToUnionCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::UNION);
	auto &cast_data = parameters.cast_data->Cast<UnionMemberBoundCastData>();

	if (cast_data.is_member_to_member) {
		return UnionMemberToMemberCast(source, result, count, parameters);
	}
	return ToUnionMemberCast(source, result, count, parameters);
}

BoundCastInfo DefaultCasts::ImplicitToUnionCast(BindCastInput &input, const LogicalType &source,
                                                const LogicalType &target) {
	D_ASSERT(target.id() == LogicalTypeId::UNION);
	if (StructToUnionCast::AllowImplicitCastFromStruct(source, target)) {
		return StructToUnionCast::Bind(input, source, target);
	}
	try {
		auto cast_data = BindToUnionMemberCast(input, source, target);
		return BoundCastInfo(&ToUnionCast, std::move(cast_data), InitToUnionLocalState);
	} catch (ConversionException &ex) {
		return BoundCastInfo(UnionImpossibleCast, make_uniq<UnionImpossibleCastData>(ErrorData(ex).RawMessage()));
	}
}

static bool UnionToVarcharCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	if (count == 0) {
		// the intermediate union vector would not have any storage
		FlatVector::SetSize(result, count_t(0));
		return true;
	}
	// first cast all union members to varchar
	auto &cast_data = parameters.cast_data->Cast<UnionMemberBoundCastData>();
	Vector varchar_union(cast_data.type, count);

	ToUnionCast(source, varchar_union, count, parameters);

	// now construct the actual varchar vector
	// varchar_union.Flatten(count);
	auto &tag_vector = UnionVector::GetTags(varchar_union);
	auto tag_entries = tag_vector.Values<union_tag_t>();

	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t i = 0; i < count; i++) {
		auto tag_entry = tag_entries[i];
		if (!tag_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		auto tag = tag_entry.GetValue();
		auto &member = UnionVector::GetMember(varchar_union, tag);
		auto member_entries = member.Values<string_t>();
		auto member_entry = member_entries[i];
		if (member_entry.IsValid()) {
			result_data.WriteValue(member_entry.GetValue());
		} else {
			result_data.WriteValue("NULL");
		}
	}
	result.Verify();
	return true;
}

BoundCastInfo DefaultCasts::UnionCastSwitch(BindCastInput &input, const LogicalType &source,
                                            const LogicalType &target) {
	D_ASSERT(source.id() == LogicalTypeId::UNION);
	switch (target.id()) {
	case LogicalTypeId::VARCHAR: {
		// bind a cast in which we convert all members to VARCHAR first
		child_list_t<LogicalType> varchar_members;
		for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(source); member_idx++) {
			varchar_members.emplace_back(make_pair(UnionType::GetMemberName(source, member_idx), LogicalType::VARCHAR));
		}
		auto varchar_type = LogicalType::UNION(std::move(varchar_members));
		return BoundCastInfo(UnionToVarcharCast, BindUnionToUnionCast(input, source, varchar_type),
		                     InitToUnionLocalState);
	}
	case LogicalTypeId::UNION:
		try {
			return BoundCastInfo(ToUnionCast, BindUnionToUnionCast(input, source, target), InitToUnionLocalState);
		} catch (ConversionException &ex) {
			return BoundCastInfo(UnionImpossibleCast, make_uniq<UnionImpossibleCastData>(ErrorData(ex).RawMessage()));
		}
	default:
		return TryVectorNullCast;
	}
}

} // namespace duckdb
