#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/swap.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/function/scalar/string_functions.hpp"
#include "duckdb/function/scalar/string_common.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"

namespace duckdb {

struct ListSliceBindData : public FunctionData {
	ListSliceBindData(const LogicalType &return_type_p, bool begin_is_empty_p, bool end_is_empty_p)
	    : return_type(return_type_p), begin_is_empty(begin_is_empty_p), end_is_empty(end_is_empty_p) {
	}
	LogicalType return_type;
	bool begin_is_empty;
	bool end_is_empty;

public:
	bool Equals(const FunctionData &other_p) const override;
	unique_ptr<FunctionData> Copy() const override;
};

bool ListSliceBindData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ListSliceBindData>();
	return return_type == other.return_type && begin_is_empty == other.begin_is_empty &&
	       end_is_empty == other.end_is_empty;
}

unique_ptr<FunctionData> ListSliceBindData::Copy() const {
	return make_uniq<ListSliceBindData>(return_type, begin_is_empty, end_is_empty);
}

namespace {

template <typename INDEX_TYPE>
idx_t CalculateSliceLength(idx_t begin, idx_t end, INDEX_TYPE step, bool svalid) {
	if (step < 0) {
		step = AbsValue(step);
	}
	if (step == 0 && svalid) {
		throw InvalidInputException("Slice step cannot be zero");
	}
	if (step == 1) {
		return NumericCast<idx_t>(end - begin);
	} else if (static_cast<idx_t>(step) >= (end - begin)) {
		return 1;
	}
	if ((end - begin) % UnsafeNumericCast<idx_t>(step) != 0) {
		return (end - begin) / UnsafeNumericCast<idx_t>(step) + 1;
	}
	return (end - begin) / UnsafeNumericCast<idx_t>(step);
}

struct BlobSliceOperations {
	static int64_t ValueLength(const string_t &value) {
		return UnsafeNumericCast<int64_t>(value.GetSize());
	}

	static string_t SliceValue(Vector &result, string_t input, int64_t begin, int64_t end) {
		return SubstringASCII(result, input, begin + 1, end - begin);
	}

	static string_t SliceValueWithSteps(Vector &result, SelectionVector &sel, string_t input, int64_t begin,
	                                    int64_t end, int64_t step, idx_t &sel_idx) {
		throw InternalException("Slicing with steps is not supported for strings");
	}
};

struct StringSliceOperations {
	static int64_t ValueLength(const string_t &value) {
		return Length<string_t, int64_t>(value);
	}

	static string_t SliceValue(Vector &result, string_t input, int64_t begin, int64_t end) {
		return SubstringUnicode(result, input, begin + 1, end - begin);
	}

	static string_t SliceValueWithSteps(Vector &result, SelectionVector &sel, string_t input, int64_t begin,
	                                    int64_t end, int64_t step, idx_t &sel_idx) {
		throw InternalException("Slicing with steps is not supported for strings");
	}
};

struct ListSliceOperations {
	static int64_t ValueLength(const list_entry_t &value) {
		return UnsafeNumericCast<int64_t>(value.length);
	}

	static list_entry_t SliceValue(Vector &result, list_entry_t input, int64_t begin, int64_t end) {
		input.offset = UnsafeNumericCast<uint64_t>(UnsafeNumericCast<int64_t>(input.offset) + begin);
		input.length = UnsafeNumericCast<uint64_t>(end - begin);
		return input;
	}

	static list_entry_t SliceValueWithSteps(Vector &result, SelectionVector &sel, list_entry_t input, int64_t begin,
	                                        int64_t end, int64_t step, idx_t &sel_idx) {
		if (end - begin == 0) {
			input.length = 0;
			input.offset = sel_idx;
			return input;
		}
		input.length = CalculateSliceLength(UnsafeNumericCast<idx_t>(begin), UnsafeNumericCast<idx_t>(end), step, true);
		idx_t child_idx = input.offset + UnsafeNumericCast<idx_t>(begin);
		if (step < 0) {
			child_idx = input.offset + UnsafeNumericCast<idx_t>(end) - 1;
		}
		input.offset = sel_idx;
		for (idx_t i = 0; i < input.length; i++) {
			sel.set_index(sel_idx, child_idx);
			child_idx += static_cast<idx_t>(step); // intentional overflow??
			sel_idx++;
		}
		return input;
	}
};

template <typename INPUT_TYPE, typename INDEX_TYPE>
void ClampIndex(INDEX_TYPE &index, const INPUT_TYPE &value, const INDEX_TYPE length, bool is_min) {
	if (index < 0) {
		index = (!is_min) ? index + 1 : index;
		index = length + index;
		return;
	} else if (index > length) {
		index = length;
	}
	return;
}

template <typename INPUT_TYPE, typename INDEX_TYPE, typename OP>
bool ClampSlice(const INPUT_TYPE &value, INDEX_TYPE &begin, INDEX_TYPE &end) {
	// Clamp offsets
	begin = (begin != 0 && begin != (INDEX_TYPE)NumericLimits<int64_t>::Minimum()) ? begin - 1 : begin;

	bool is_min = false;
	if (begin == (INDEX_TYPE)NumericLimits<int64_t>::Minimum()) {
		begin++;
		is_min = true;
	}

	const auto length = OP::ValueLength(value);
	if (begin < 0 && -begin > length && end < 0 && end < -length) {
		begin = 0;
		end = 0;
		return true;
	}
	if (begin < 0 && -begin > length) {
		begin = 0;
	}
	ClampIndex(begin, value, length, is_min);
	ClampIndex(end, value, length, false);
	end = MaxValue<INDEX_TYPE>(begin, end);

	return true;
}

template <typename INPUT_TYPE, typename INDEX_TYPE, typename OP>
void ExecuteFlatSlice(Vector &result, Vector &list_vector, Vector &begin_vector, Vector &end_vector,
                      optional_ptr<Vector> step_vector, const idx_t count, SelectionVector &sel, idx_t &sel_idx,
                      optional_ptr<Vector> result_child_vector, bool begin_is_empty, bool end_is_empty) {
	UnifiedVectorFormat list_data, begin_data, end_data, step_data;
	idx_t sel_length = 0;

	list_vector.ToUnifiedFormat(list_data);
	begin_vector.ToUnifiedFormat(begin_data);
	end_vector.ToUnifiedFormat(end_data);
	if (step_vector) {
		step_vector->ToUnifiedFormat(step_data);
		sel.Initialize(ListVector::GetListSize(list_vector));
	}

	auto result_data = FlatVector::Writer<INPUT_TYPE>(result, count);
	for (idx_t i = 0; i < count; ++i) {
		auto list_idx = list_data.sel->get_index(i);
		auto begin_idx = begin_data.sel->get_index(i);
		auto end_idx = end_data.sel->get_index(i);
		auto step_idx = step_vector ? step_data.sel->get_index(i) : 0;

		auto list_valid = list_data.validity.RowIsValid(list_idx);
		auto begin_valid = begin_data.validity.RowIsValid(begin_idx);
		auto end_valid = end_data.validity.RowIsValid(end_idx);
		auto step_valid = step_vector && step_data.validity.RowIsValid(step_idx);

		if (!list_valid || !begin_valid || !end_valid || (step_vector && !step_valid)) {
			result_data.WriteNull();
			continue;
		}

		auto sliced = UnifiedVectorFormat::GetData<INPUT_TYPE>(list_data)[list_idx];
		auto begin = begin_is_empty ? 0 : UnifiedVectorFormat::GetData<INDEX_TYPE>(begin_data)[begin_idx];
		auto end = end_is_empty ? OP::ValueLength(sliced) : UnifiedVectorFormat::GetData<INDEX_TYPE>(end_data)[end_idx];
		auto step = step_vector ? UnifiedVectorFormat::GetData<INDEX_TYPE>(step_data)[step_idx] : 1;
		if (step == NumericLimits<INDEX_TYPE>::Minimum()) {
			// the minimum cannot be negated - any step that is at least as large as the list selects the same entry
			step = -NumericLimits<INDEX_TYPE>::Maximum();
		}

		if (step < 0) {
			swap(begin, end);
			begin = end_is_empty ? 0 : begin;
			end = begin_is_empty ? OP::ValueLength(sliced) : end;
		}

		bool clamp_result = false;
		if (step_valid || step == 1) {
			clamp_result = ClampSlice<INPUT_TYPE, INDEX_TYPE, OP>(sliced, begin, end);
		}

		idx_t length = 0;
		if (end - begin > 0) {
			length =
			    CalculateSliceLength(UnsafeNumericCast<idx_t>(begin), UnsafeNumericCast<idx_t>(end), step, step_valid);
		}
		sel_length += length;

		if (!clamp_result) {
			result_data.WriteNull();
		} else if (!step_vector) {
			result_data.WriteValue(OP::SliceValue(result, sliced, begin, end));
		} else {
			result_data.WriteValue(OP::SliceValueWithSteps(result, sel, sliced, begin, end, step, sel_idx));
		}
	}
	if (step_vector) {
		SelectionVector new_sel(sel_length);
		for (idx_t i = 0; i < sel_length; ++i) {
			new_sel.set_index(i, sel.get_index(i));
		}
		result_child_vector->Slice(new_sel, sel_length);
		result_child_vector->Flatten();
		ListVector::SetListSize(result, sel_length);
	}
}

template <typename INPUT_TYPE, typename INDEX_TYPE, typename OP>
void ExecuteSlice(Vector &result, Vector &list_or_str_vector, Vector &begin_vector, Vector &end_vector,
                  optional_ptr<Vector> step_vector, const idx_t count, bool begin_is_empty, bool end_is_empty) {
	optional_ptr<Vector> result_child_vector;
	if (step_vector) {
		result_child_vector = &ListVector::GetChildMutable(result);
	}

	SelectionVector sel;
	idx_t sel_idx = 0;

	ExecuteFlatSlice<INPUT_TYPE, INDEX_TYPE, OP>(result, list_or_str_vector, begin_vector, end_vector, step_vector,
	                                             count, sel, sel_idx, result_child_vector, begin_is_empty,
	                                             end_is_empty);
	result.Verify();
}

void ArraySliceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 3 || args.ColumnCount() == 4);
	D_ASSERT(args.data.size() == 3 || args.data.size() == 4);
	auto count = args.size();

	Vector &list_or_str_vector = result;
	// this ensures that we do not change the input chunk
	VectorOperations::Copy(args.data[0], list_or_str_vector, count, 0, 0);

	if (list_or_str_vector.GetType().id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(count));
		return;
	}

	Vector &begin_vector = args.data[1];
	Vector &end_vector = args.data[2];

	optional_ptr<Vector> step_vector;
	if (args.ColumnCount() == 4) {
		step_vector = &args.data[3];
	}

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ListSliceBindData>();
	auto begin_is_empty = info.begin_is_empty;
	auto end_is_empty = info.end_is_empty;
	switch (result.GetType().id()) {
	case LogicalTypeId::LIST: {
		// Share the value dictionary as we are just going to slice it
		if (list_or_str_vector.GetVectorType() != VectorType::FLAT_VECTOR &&
		    list_or_str_vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
			list_or_str_vector.Flatten();
		}
		ExecuteSlice<list_entry_t, int64_t, ListSliceOperations>(result, list_or_str_vector, begin_vector, end_vector,
		                                                         step_vector, count, begin_is_empty, end_is_empty);
		break;
	}
	case LogicalTypeId::BLOB:
		ExecuteSlice<string_t, int64_t, BlobSliceOperations>(result, list_or_str_vector, begin_vector, end_vector,
		                                                     step_vector, count, begin_is_empty, end_is_empty);
		break;
	case LogicalTypeId::VARCHAR:
		ExecuteSlice<string_t, int64_t, StringSliceOperations>(result, list_or_str_vector, begin_vector, end_vector,
		                                                       step_vector, count, begin_is_empty, end_is_empty);
		break;
	default:
		throw NotImplementedException("Specifier type not implemented");
	}
}

//! An omitted slice bound is parsed as an empty list constructor (see OperatorExpression::EmptySliceBound)
bool CheckIfParamIsEmpty(const Expression &param) {
	if (param.GetReturnType().id() != LogicalTypeId::LIST) {
		return false;
	}
	if (param.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
		auto &function = param.Cast<BoundFunctionExpression>();
		if (function.Function().GetName() == "list_value" && function.GetChildren().empty()) {
			return true;
		}
	}
	if (param.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
		auto &value = param.Cast<BoundConstantExpression>().GetValue();
		if (!value.IsNull() && ListValue::GetChildren(value).empty()) {
			return true;
		}
	}
	// the user has entered a list instead of a BIGINT
	throw BinderException("The upper and lower bounds of the slice must be a BIGINT");
}

void ArraySliceResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(input.GetArgumentCount() == 3 || input.GetArgumentCount() == 4);
	D_ASSERT(bound_function.GetArguments().size() == 3 || bound_function.GetArguments().size() == 4);

	auto &list_type = input.GetArgumentType(0);
	switch (list_type.id()) {
	case LogicalTypeId::ARRAY: {
		// Cast to list
		auto target_type = LogicalType::LIST(ArrayType::GetChildType(list_type));
		bound_function.GetArguments()[0] = target_type;
		bound_function.SetReturnType(target_type);
	} break;
	case LogicalTypeId::LIST:
		// The result is the same type
		bound_function.SetReturnType(list_type);
		break;
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
		// string slice returns a string
		if (bound_function.GetArguments().size() == 4) {
			throw NotImplementedException(
			    "Slice with steps has not been implemented for string types, you can consider rewriting your query as "
			    "follows:\n SELECT array_to_string((str_split(string, '')[begin:end:step], '');");
		}
		if (list_type.IsJSONType()) {
			// This is needed to avoid producing invalid JSON
			bound_function.GetArguments()[0] = LogicalType::VARCHAR;
			bound_function.SetReturnType(LogicalType::VARCHAR);
		} else {
			bound_function.SetReturnType(list_type);
		}
		for (idx_t i = 1; i < 3; i++) {
			if (input.GetArgumentType(i).id() != LogicalTypeId::LIST) {
				bound_function.GetArguments()[i] = LogicalType::BIGINT;
			}
		}
		break;
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::UNKNOWN:
		bound_function.GetArguments()[0] = LogicalTypeId::UNKNOWN;
		bound_function.SetReturnType(LogicalType::SQLNULL);
		break;
	default:
		throw BinderException("ARRAY_SLICE can only operate on LISTs and VARCHARs");
	}

	// empty bounds keep their (empty list) type - the other bounds are cast to BIGINT
	for (idx_t i = 1; i < 3; i++) {
		if (!CheckIfParamIsEmpty(input.GetArgument(i))) {
			bound_function.GetArguments()[i] = LogicalType::BIGINT;
		}
	}
}

unique_ptr<FunctionData> ArraySliceBind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	bool begin_is_empty = arguments[1]->GetReturnType().id() == LogicalTypeId::LIST;
	bool end_is_empty = arguments[2]->GetReturnType().id() == LogicalTypeId::LIST;
	return make_uniq<ListSliceBindData>(bound_function.GetReturnType(), begin_is_empty, end_is_empty);
}

bool TryGetConstantSliceIndex(const Expression &expression, int64_t &result) {
	if (expression.GetExpressionClass() != ExpressionClass::BOUND_CONSTANT) {
		return false;
	}
	auto &value = expression.Cast<BoundConstantExpression>().GetValue();
	if (value.IsNull()) {
		return false;
	}
	result = value.GetValue<int64_t>();
	return true;
}

unique_ptr<BaseStatistics> ArraySlicePropagateStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &expr = input.expr;
	auto &children = expr.GetChildren();
	if (expr.GetReturnType().id() != LogicalTypeId::VARCHAR || children.size() != 3 || !input.bind_data) {
		return nullptr;
	}

	auto &bind_data = input.bind_data->Cast<ListSliceBindData>();
	int64_t begin = 0;
	if (!bind_data.begin_is_empty && (!TryGetConstantSliceIndex(*children[1], begin) || begin < 0)) {
		return nullptr;
	}
	idx_t start_character_index = begin > 0 ? NumericCast<idx_t>(begin - 1) : 0;

	optional_idx character_count;
	if (!bind_data.end_is_empty) {
		int64_t end = 0;
		if (!TryGetConstantSliceIndex(*children[2], end) || end < 0) {
			return nullptr;
		}
		auto end_character_count = NumericCast<idx_t>(end);
		character_count =
		    end_character_count > start_character_index ? (end_character_count - start_character_index) : 0;
	}
	return PropagateStringSliceStats(input, start_character_index, character_count);
}

unique_ptr<ParsedExpression> ArraySliceUnbind(FunctionUnbindInput &input) {
	if ((input.children.size() != 3 && input.children.size() != 4) || !input.expression.BindInfo()) {
		return nullptr;
	}
	auto &data = input.expression.BindInfo()->Cast<ListSliceBindData>();
	for (idx_t i = 1; i < 3; i++) {
		auto &child = *input.expression.GetChildren()[i];
		if (child.GetReturnType().id() != LogicalTypeId::LIST) {
			continue;
		}
		if (child.GetExpressionClass() == ExpressionClass::BOUND_CONSTANT) {
			auto &value = child.Cast<BoundConstantExpression>().GetValue();
			if (!value.IsNull() && ListValue::GetChildren(value).empty()) {
				continue;
			}
		} else if (child.GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
			auto &function = child.Cast<BoundFunctionExpression>();
			auto &definition = function.Function().GetDefinition();
			if (definition && definition->GetQualifiedName() == QualifiedName("system", "main", "list_value") &&
			    function.GetChildren().empty()) {
				continue;
			}
		}
		return nullptr;
	}
	if (data.begin_is_empty) {
		input.children[1] = OperatorExpression::EmptySliceBound();
	}
	if (data.end_is_empty) {
		input.children[2] = OperatorExpression::EmptySliceBound();
	}
	return make_uniq<OperatorExpression>(ExpressionType::ARRAY_SLICE, std::move(input.children));
}

} // namespace
ScalarFunctionSet ListSliceFun::GetFunctions() {
	// the arguments and return types are actually set in the binder function
	ScalarFunction fun({}, LogicalType::ANY, ArraySliceFunction, ArraySliceBind);
	fun.GetSignature()
	    .AddParameter("list", LogicalType::ANY)
	    .AddParameter("begin", LogicalType::ANY)
	    .AddParameter("end", LogicalType::ANY);
	fun.SetResolveTypesCallback(ArraySliceResolveTypes);
	fun.SetStatisticsCallback(ArraySlicePropagateStats);
	fun.SetUnbindCallback(ArraySliceUnbind);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();
	ScalarFunctionSet set;
	set.AddFunction(fun);
	fun.GetSignature().AddParameter("step", LogicalType::BIGINT);
	set.AddFunction(fun);
	return set;
}

} // namespace duckdb
