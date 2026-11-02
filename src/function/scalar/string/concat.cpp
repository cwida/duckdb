#include "duckdb/common/vector/constant_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/function/scalar/string_functions.hpp"

#include "duckdb/planner/expression/bound_function_expression.hpp"

namespace duckdb {

namespace {

struct ConcatFunctionData : public FunctionData {
	ConcatFunctionData(const LogicalType &return_type_p, bool is_operator_p)
	    : return_type(return_type_p), is_operator(is_operator_p) {
	}

	LogicalType return_type;

	bool is_operator = false;

public:
	bool Equals(const FunctionData &other_p) const override;
	unique_ptr<FunctionData> Copy() const override;
};

bool ConcatFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<ConcatFunctionData>();
	return return_type == other.return_type && is_operator == other.is_operator;
}

unique_ptr<FunctionData> ConcatFunctionData::Copy() const {
	return make_uniq<ConcatFunctionData>(return_type, is_operator);
}

void StringConcatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// iterate over the vectors to count how large the final string will be
	idx_t constant_lengths = 0;
	vector<idx_t> result_lengths(args.size(), 0);
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		const auto &input = args.data[col_idx];
		D_ASSERT(input.GetType().InternalType() == PhysicalType::VARCHAR);
		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (ConstantVector::IsNull(input)) {
				// constant null, skip
				continue;
			}
			auto input_data = ConstantVector::GetData<string_t>(input);
			constant_lengths += input_data->GetSize();
		} else {
			// now get the lengths of each of the input elements
			for (auto entry : input.Values<string_t>()) {
				if (!entry.IsValid()) {
					continue;
				}
				result_lengths[entry.GetIndex()] += entry.GetValue().GetSize();
			}
		}
	}

	// first we allocate the empty strings for each of the values
	auto result_data = FlatVector::ScatterWriter<string_t>(result);
	for (idx_t i = 0; i < args.size(); i++) {
		// allocate an empty string of the required size
		idx_t str_length = constant_lengths + result_lengths[i];
		result_data[i].EmptyString(str_length);
		// we reuse the result_lengths vector to store the currently appended size
		result_lengths[i] = 0;
	}

	// now that the empty space for the strings has been allocated, perform the concatenation
	for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
		const auto &input = args.data[col_idx];

		// loop over the vector and concat to all results
		if (input.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// constant vector
			if (ConstantVector::IsNull(input)) {
				// constant null, skip
				continue;
			}
			// append the constant vector to each of the strings
			auto input_data = ConstantVector::GetData<string_t>(input);
			auto input_ptr = input_data->GetData();
			auto input_len = input_data->GetSize();
			for (idx_t i = 0; i < args.size(); i++) {
				memcpy(result_data[i].GetDataWriteable() + result_lengths[i], input_ptr, input_len);
				result_lengths[i] += input_len;
			}
		} else {
			for (auto entry : input.Values<string_t>()) {
				if (!entry.IsValid()) {
					continue;
				}
				auto &input_str = entry.GetValue();
				auto i = entry.GetIndex();
				auto input_ptr = input_str.GetData();
				auto input_len = input_str.GetSize();
				memcpy(result_data[i].GetDataWriteable() + result_lengths[i], input_ptr, input_len);
				result_lengths[i] += input_len;
			}
		}
	}
	for (idx_t i = 0; i < args.size(); i++) {
		result_data[i].Finalize();
	}
}

void ConcatOperator(DataChunk &args, ExpressionState &state, Vector &result) {
	BinaryExecutor::Execute<string_t, string_t, string_t>(
	    args.data[0], args.data[1], result, [&](string_t a, string_t b) {
		    auto a_data = a.GetData();
		    auto b_data = b.GetData();
		    auto a_length = a.GetSize();
		    auto b_length = b.GetSize();

		    auto target_length = a_length + b_length;
		    auto target = StringVector::EmptyString(result, target_length);
		    auto target_data = target.GetDataWriteable();

		    memcpy(target_data, a_data, a_length);
		    memcpy(target_data + a_length, b_data, b_length);
		    target.Finalize();
		    return target;
	    });
}

struct ListConcatInputData {
	ListConcatInputData(const Vector &input, idx_t size)
	    : input(input), child_vec(ListVector::GetChild(input)), list_data(input.Values<list_entry_t>()) {
	}

	const Vector &input;
	const Vector &child_vec;
	VectorIterator<list_entry_t> list_data;
};

void ListConcatFunction(DataChunk &args, ExpressionState &state, Vector &result, bool is_operator) {
	auto count = args.size();

	vector<ListConcatInputData> input_data;
	for (const auto &input : args.data) {
		if (!is_operator && input.GetType().id() == LogicalTypeId::SQLNULL) {
			// LIST_CONCAT ignores NULL values
			continue;
		}
		input_data.emplace_back(input, count);
	}

	// the || operator yields NULL whenever any input is NULL, while list_concat skips NULLs
	vector<bool> row_invalid(count, false);
	if (is_operator) {
		for (auto &input : input_data) {
			for (idx_t r = 0; r < count; r++) {
				if (!input.list_data[r].IsValid()) {
					row_invalid[r] = true;
				}
			}
		}
	}

	auto result_writer = FlatVector::Writer<list_entry_t>(result, count);
	for (idx_t r = 0; r < count; r++) {
		if (row_invalid[r]) {
			result_writer.WriteNull();
			continue;
		}
		auto list = result_writer.WriteDynamicList();
		for (auto &input : input_data) {
			auto list_val = input.list_data[r];
			if (!list_val.IsValid()) {
				continue;
			}
			const auto &list_entry = list_val.GetValue();
			list.Append(input.child_vec, *FlatVector::IncrementalSelectionVector(),
			            list_entry.offset + list_entry.length, list_entry.offset, list_entry.length);
		}
	}
}

void ConcatFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<ConcatFunctionData>();
	if (info.return_type.id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	if (info.return_type.id() == LogicalTypeId::LIST) {
		return ListConcatFunction(args, state, result, info.is_operator);
	}
	if (info.is_operator) {
		return ConcatOperator(args, state, result);
	}
	return StringConcatFunction(args, state, result);
}

void SetArgumentType(BoundScalarFunction &bound_function, const LogicalType &type, bool is_operator) {
	if (is_operator) {
		bound_function.GetArguments()[0] = type;
		bound_function.GetArguments()[1] = type;
		bound_function.SetReturnType(type);
		return;
	}

	for (auto &arg : bound_function.GetArguments()) {
		arg = type;
	}
	bound_function.SetReturnType(type);
}

void ResolveListConcatTypes(ResolveScalarFunctionTypesInput &input, bool is_operator) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	LogicalType child_type = LogicalType::SQLNULL;
	bool all_null = true;
	for (idx_t i = 0; i < input.GetArgumentCount(); i++) {
		auto &arg = input.GetArgument(i);
		auto &return_type = arg.GetReturnType();
		if (return_type == LogicalTypeId::SQLNULL) {
			// we mimic postgres behaviour: list_concat(NULL, my_list) = my_list
			continue;
		}
		all_null = false;
		LogicalType next_type = LogicalTypeId::INVALID;
		switch (return_type.id()) {
		case LogicalTypeId::UNKNOWN:
			throw ParameterNotResolvedException();
		case LogicalTypeId::LIST:
			next_type = ListType::GetChildType(return_type);
			break;
		case LogicalTypeId::ARRAY:
			next_type = ArrayType::GetChildType(return_type);
			break;
		default: {
			string type_list;
			for (idx_t arg_idx = 0; arg_idx < input.GetArgumentCount(); arg_idx++) {
				if (!type_list.empty()) {
					if (arg_idx + 1 == input.GetArgumentCount()) {
						// last argument
						type_list += " and ";
					} else {
						type_list += ", ";
					}
				}
				type_list += input.GetArgumentType(arg_idx).ToString();
			}
			throw BinderException(arg, "Cannot concatenate types %s - an explicit cast is required", type_list);
		}
		}
		if (!LogicalType::TryGetMaxLogicalType(context, child_type, next_type, child_type)) {
			throw BinderException(arg, "Cannot concatenate lists of types %s[] and %s[] - an explicit cast is required",
			                      child_type.ToString(), next_type.ToString());
		}
	}
	if (all_null) {
		// all arguments are NULL
		SetArgumentType(bound_function, LogicalTypeId::SQLNULL, is_operator);
		return;
	}
	SetArgumentType(bound_function, LogicalType::LIST(child_type), is_operator);
}

void ResolveConcatTypesInternal(ResolveScalarFunctionTypesInput &input, bool is_operator) {
	auto &bound_function = input.GetBoundFunction();
	bool list_concat = false;
	bool all_null = true;
	// blob concat is only supported for the concat operator - regular concat converts to varchar
	bool all_blob = is_operator ? true : false;
	for (auto &arg_type : input.GetArgumentTypes()) {
		if (arg_type.id() == LogicalTypeId::UNKNOWN) {
			throw ParameterNotResolvedException();
		}
		if (arg_type.id() == LogicalTypeId::LIST || arg_type.id() == LogicalTypeId::ARRAY) {
			list_concat = true;
		}
		if (arg_type.id() != LogicalTypeId::BLOB) {
			all_blob = false;
		}
		if (arg_type.id() != LogicalTypeId::SQLNULL) {
			all_null = false;
		}
	}
	if (list_concat) {
		ResolveListConcatTypes(input, is_operator);
		return;
	}
	if (all_null) {
		if (is_operator) {
			SetArgumentType(bound_function, LogicalTypeId::SQLNULL, is_operator);
			return;
		}

		const auto &func_args = bound_function.GetArguments();
		if (!func_args.empty() &&
		    (func_args[0].id() == LogicalTypeId::LIST || func_args[0].id() == LogicalTypeId::ARRAY)) {
			SetArgumentType(bound_function, LogicalTypeId::SQLNULL, is_operator);
			return;
		}

		SetArgumentType(bound_function, LogicalTypeId::VARCHAR, is_operator);
		return;
	}
	auto return_type = all_blob ? LogicalType::BLOB : LogicalType::VARCHAR;

	// we can now assume that the input is a string or castable to a string
	SetArgumentType(bound_function, return_type, is_operator);
}

void ConcatFunctionResolveTypes(ResolveScalarFunctionTypesInput &input) {
	ResolveConcatTypesInternal(input, false);
}

void ConcatOperatorResolveTypes(ResolveScalarFunctionTypesInput &input) {
	ResolveConcatTypesInternal(input, true);
}

unique_ptr<FunctionData> BindConcatFunction(BindScalarFunctionInput &input) {
	return make_uniq<ConcatFunctionData>(input.GetBoundFunction().GetReturnType(), false);
}

unique_ptr<FunctionData> BindConcatOperator(BindScalarFunctionInput &input) {
	return make_uniq<ConcatFunctionData>(input.GetBoundFunction().GetReturnType(), true);
}

unique_ptr<BaseStatistics> ListConcatStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto stats = child_stats[0].ToUnique();
	for (idx_t i = 1; i < child_stats.size(); i++) {
		stats->Merge(child_stats[i]);
	}
	return stats;
}

} // namespace

ScalarFunction ListConcatFun::GetFunction() {
	// The arguments and return types are set in the binder function.
	auto fun =
	    ScalarFunction({}, LogicalType::LIST(LogicalType::ANY), ConcatFunction, BindConcatFunction, ListConcatStats);
	fun.GetSignature().AddArgs("args", LogicalType::LIST(LogicalType::ANY));
	fun.SetResolveTypesCallback(ConcatFunctionResolveTypes);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

// the concat operator and concat function have different behavior regarding NULLs
// this is strange but seems consistent with postgresql and mysql
// (sqlite does not support the concat function, only the concat operator)

// the concat operator behaves as one would expect: any NULL value present results in a NULL
// i.e. NULL || 'hello' = NULL
// the concat function, however, treats NULL values as an empty string
// i.e. concat(NULL, 'hello') = 'hello'
ScalarFunction ConcatFun::GetFunction() {
	ScalarFunction concat = ScalarFunction("concat", {}, LogicalType::ANY, ConcatFunction, BindConcatFunction);
	concat.GetSignature().AddParameter("value", LogicalType::ANY);
	concat.GetSignature().AddArgs("args", LogicalType::ANY);
	concat.SetResolveTypesCallback(ConcatFunctionResolveTypes);
	concat.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return concat;
}

ScalarFunction ConcatOperatorFun::GetFunction() {
	ScalarFunction concat_op = ScalarFunction("||", {}, LogicalType::ANY, ConcatFunction, BindConcatOperator);
	concat_op.GetSignature().AddParameter("arg1", LogicalType::ANY).AddParameter("arg2", LogicalType::ANY);
	concat_op.SetResolveTypesCallback(ConcatOperatorResolveTypes);
	return concat_op;
}

} // namespace duckdb
