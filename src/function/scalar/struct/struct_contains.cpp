#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

struct StructSearchBindData : public FunctionData {
	explicit StructSearchBindData(vector<idx_t> matching_members_p) : matching_members(std::move(matching_members_p)) {
	}

	//! The indexes of the members that can match the value - the bind casts these to the type of the value
	vector<idx_t> matching_members;

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<StructSearchBindData>(matching_members);
	}
	bool Equals(const FunctionData &other_p) const override {
		return matching_members == other_p.Cast<StructSearchBindData>().matching_members;
	}
};

//! Searches the target in the candidates - the members at the given indexes of the struct
template <class T, class RETURN_TYPE, bool FIND_NULLS>
static void TemplatedStructSearch(const Vector &input_vector, const vector<reference<const Vector>> &candidates,
                                  const vector<idx_t> &candidate_indexes, const Vector &target, const idx_t count,
                                  Vector &result) {
	// If the return type is not a bool, return the position
	const auto return_pos = std::is_same<RETURN_TYPE, int32_t>::value;

	UnifiedVectorFormat vector_format;
	input_vector.ToUnifiedFormat(vector_format);

	UnifiedVectorFormat target_format;
	target.ToUnifiedFormat(target_format);
	const auto target_data = UnifiedVectorFormat::GetData<T>(target_format);

	D_ASSERT(candidates.size() == candidate_indexes.size());
	vector<const T *> member_data_ptrs;
	vector<UnifiedVectorFormat> member_vectors;
	for (auto &candidate : candidates) {
		UnifiedVectorFormat member_format;
		candidate.get().ToUnifiedFormat(member_format);
		member_data_ptrs.push_back(UnifiedVectorFormat::GetData<T>(member_format));
		member_vectors.push_back(std::move(member_format));
	}

	if (candidates.empty() && return_pos) {
		// if there are no members that match the target type, we cannot return a position
		ConstantVector::SetNull(result, count_t(count));
		return;
	}

	auto result_data = FlatVector::Writer<RETURN_TYPE>(result, count);
	for (idx_t row = 0; row < count; row++) {
		const auto &member_row_idx = vector_format.sel->get_index(row);

		if (!vector_format.validity.RowIsValid(member_row_idx)) {
			result_data.WriteNull();
			continue;
		}

		const auto &target_row_idx = target_format.sel->get_index(row);
		const bool target_valid = target_format.validity.RowIsValid(target_row_idx);

		// We are finished if we are not looking for NULL, and the target is NULL.
		const auto finished = !FIND_NULLS && !target_valid;
		// We did not find the target (finished, or struct is empty).
		if (finished) {
			if (!target_valid || return_pos) {
				result_data.WriteNull();
			} else {
				result_data.WriteValue(RETURN_TYPE(false));
			}
			continue;
		}

		bool found = false;
		RETURN_TYPE found_value {};

		for (idx_t candidate_idx = 0; candidate_idx < candidates.size(); candidate_idx++) {
			auto &member_data_ptr = member_data_ptrs[candidate_idx];
			const auto &member_vector = member_vectors[candidate_idx];
			const auto member_data_idx = member_vector.sel->get_index(row);
			const auto col_valid = member_vector.validity.RowIsValid(member_data_idx);

			auto is_null = FIND_NULLS && !col_valid && !target_valid;
			auto both_valid_and_match =
			    col_valid && target_valid &&
			    Equals::Operation<T>(member_data_ptr[member_data_idx], target_data[target_row_idx]);

			if (is_null || both_valid_and_match) {
				found = true;
				if (return_pos) {
					found_value = UnsafeNumericCast<int32_t>(candidate_indexes[candidate_idx] + 1);
				} else {
					found_value = RETURN_TYPE(true);
				}
				break;
			}
		}

		if (!found) {
			if (return_pos) {
				result_data.WriteNull();
			} else {
				result_data.WriteValue(RETURN_TYPE(false));
			}
		} else {
			result_data.WriteValue(found_value);
		}
	}
}

template <class RETURN_TYPE, bool FIND_NULLS>
static void StructNestedOp(const Vector &input_vector, const vector<Vector> &members,
                           const vector<idx_t> &matching_members, const Vector &target, const idx_t count,
                           Vector &result) {
	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);

	// Set up sort keys for nested types.
	vector<Vector> member_sort_key_vectors;
	member_sort_key_vectors.reserve(matching_members.size());
	for (auto member_idx : matching_members) {
		member_sort_key_vectors.emplace_back(LogicalType::BLOB, count);
		CreateSortKeyHelpers::CreateSortKeyWithValidity(members[member_idx], member_sort_key_vectors.back(),
		                                                order_modifiers);
	}
	vector<reference<const Vector>> candidates;
	for (auto &sort_key_vector : member_sort_key_vectors) {
		candidates.push_back(sort_key_vector);
	}

	Vector target_sort_key_vec(LogicalType::BLOB, count);
	CreateSortKeyHelpers::CreateSortKeyWithValidity(target, target_sort_key_vec, order_modifiers);

	TemplatedStructSearch<string_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
	                                                         target_sort_key_vec, count, result);
}

template <class RETURN_TYPE, bool FIND_NULLS>
static void StructSearchOp(const Vector &input_vector, const vector<Vector> &members,
                           const vector<idx_t> &matching_members, const Vector &target, const idx_t count,
                           Vector &result) {
	const auto &target_type = target.GetType().InternalType();
	vector<reference<const Vector>> candidates;
	for (auto member_idx : matching_members) {
		candidates.push_back(members[member_idx]);
	}
	switch (target_type) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		return TemplatedStructSearch<int8_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                              target, count, result);
	case PhysicalType::INT16:
		return TemplatedStructSearch<int16_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                               target, count, result);
	case PhysicalType::INT32:
		return TemplatedStructSearch<int32_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                               target, count, result);
	case PhysicalType::INT64:
		return TemplatedStructSearch<int64_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                               target, count, result);
	case PhysicalType::INT128:
		return TemplatedStructSearch<hugeint_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                 target, count, result);
	case PhysicalType::UINT8:
		return TemplatedStructSearch<uint8_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                               target, count, result);
	case PhysicalType::UINT16:
		return TemplatedStructSearch<uint16_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                target, count, result);
	case PhysicalType::UINT32:
		return TemplatedStructSearch<uint32_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                target, count, result);
	case PhysicalType::UINT64:
		return TemplatedStructSearch<uint64_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                target, count, result);
	case PhysicalType::UINT128:
		return TemplatedStructSearch<uhugeint_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                  target, count, result);
	case PhysicalType::FLOAT:
		return TemplatedStructSearch<float, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members, target,
		                                                             count, result);
	case PhysicalType::DOUBLE:
		return TemplatedStructSearch<double, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                              target, count, result);
	case PhysicalType::VARCHAR:
		return TemplatedStructSearch<string_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                target, count, result);
	case PhysicalType::INTERVAL:
		return TemplatedStructSearch<interval_t, RETURN_TYPE, FIND_NULLS>(input_vector, candidates, matching_members,
		                                                                  target, count, result);
	case PhysicalType::STRUCT:
	case PhysicalType::LIST:
	case PhysicalType::ARRAY:
		return StructNestedOp<RETURN_TYPE, FIND_NULLS>(input_vector, members, matching_members, target, count, result);
	default:
		throw NotImplementedException("This function has not been implemented for logical type %s",
		                              TypeIdToString(target_type));
	}
}

template <class RETURN_TYPE, bool FIND_NULLS = false>
static void StructSearchFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	if (result.GetType().id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}

	const auto count = args.size();
	const auto &input_vector = args.data[0];
	const auto &members = StructVector::GetEntries(input_vector);
	const auto &target = args.data[1];
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<StructSearchBindData>();

	StructSearchOp<RETURN_TYPE, FIND_NULLS>(input_vector, members, info.matching_members, target, count, result);
}

static void StructContainsResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	auto &child_type = arguments[0]->GetReturnType();
	if (child_type.id() == LogicalTypeId::UNKNOWN) {
		throw ParameterNotResolvedException();
	}

	if (child_type.id() == LogicalTypeId::SQLNULL) {
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.GetArguments()[1] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		return;
	}

	auto &struct_children = StructType::GetChildTypes(arguments[0]->GetReturnType());
	if (struct_children.empty()) {
		// an empty struct contains nothing, the search always returns false (or position 0)
		bound_function.GetArguments()[0] = child_type;
		return;
	}
	if (child_type.id() != LogicalTypeId::TUPLE) {
		throw BinderException("%s can only be used on unnamed structs", bound_function.GetName());
	}
	bound_function.GetArguments()[0] = child_type;

	// find the type that the value and all the children it can be compared with can be cast to
	LogicalType target_type = arguments[1]->GetReturnType();
	for (auto &child : struct_children) {
		LogicalType max_type;
		if (LogicalType::TryGetMaxLogicalType(context, child.second, target_type, max_type)) {
			target_type = max_type;
		}
	}
	bound_function.GetArguments()[1] = target_type;
	// cast the children that can be compared with the value to that type - the others never match
	vector<LogicalType> new_child_types;
	for (idx_t child_idx = 0; child_idx < struct_children.size(); child_idx++) {
		auto &struct_child_type = struct_children[child_idx].second;
		LogicalType max_type;
		if (LogicalType::TryGetMaxLogicalType(context, struct_child_type, target_type, max_type) &&
		    max_type == target_type) {
			new_child_types.push_back(target_type);
		} else {
			new_child_types.push_back(struct_child_type);
		}
	}

	child_list_t<LogicalType> cast_children;
	for (idx_t i = 0; i < new_child_types.size(); i++) {
		cast_children.push_back(make_pair(struct_children[i].first, new_child_types[i]));
	}

	// the input is an unnamed struct - represent it as a TUPLE
	bound_function.GetArguments()[0] = LogicalType::TUPLE(cast_children);
}

static unique_ptr<FunctionData> StructContainsBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	vector<idx_t> matching_members;
	auto &child_type = arguments[0]->GetReturnType();
	if (child_type.id() != LogicalTypeId::SQLNULL) {
		// the members that can match the value were cast to the type of the value when resolving the types
		auto &target_type = arguments[1]->GetReturnType();
		auto &struct_children = StructType::GetChildTypes(child_type);
		for (idx_t child_idx = 0; child_idx < struct_children.size(); child_idx++) {
			if (struct_children[child_idx].second == target_type) {
				matching_members.push_back(child_idx);
			}
		}
	}
	return make_uniq<StructSearchBindData>(std::move(matching_members));
}

ScalarFunction StructContainsFun::GetFunction() {
	ScalarFunction fun("struct_contains", {}, LogicalType::BOOLEAN, StructSearchFunction<bool>);
	fun.GetSignature().AddParameter("struct", LogicalTypeId::TUPLE).AddParameter("entry", LogicalType::ANY);
	fun.SetResolveTypesCallback(StructContainsResolveTypes);
	fun.SetBindCallback(StructContainsBind);
	return fun;
}

ScalarFunction StructPositionFun::GetFunction() {
	ScalarFunction fun("struct_contains", {}, LogicalType::INTEGER, StructSearchFunction<int32_t, true>);
	fun.GetSignature().AddParameter("struct", LogicalTypeId::TUPLE).AddParameter("entry", LogicalType::ANY);
	fun.SetResolveTypesCallback(StructContainsResolveTypes);
	fun.SetBindCallback(StructContainsBind);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
