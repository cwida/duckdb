#include "core_functions/scalar/list_functions.hpp"

#include "duckdb/function/lambda_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"

namespace duckdb {

namespace {

struct ReduceExecuteInfo {
	ReduceExecuteInfo(LambdaFunctions::LambdaInfo &info, ClientContext &context)
	    : context(context), left_slice(make_uniq<Vector>(Vector::Ref(*info.child_vector))) {
		if (info.has_initial) {
			initial_value_offset = 0;
		}
		SelectionVector left_vector(info.row_count);
		active_rows.Resize(info.row_count);
		active_rows.SetAllValid(info.row_count);

		left_sel.Initialize(info.row_count);
		active_rows_sel.Initialize(info.row_count);

		idx_t reduced_row_idx = 0;

		if (info.has_initial) {
			left_vector.set_index(0, 0);
		}

		for (idx_t original_row_idx = 0; original_row_idx < info.row_count; original_row_idx++) {
			auto list_column_format_index = info.list_column_format.sel->get_index(original_row_idx);
			if (info.list_column_format.validity.RowIsValid(list_column_format_index)) {
				if (info.list_entries[list_column_format_index].length == 0 && !info.has_initial) {
					throw ParameterNotAllowedException("Cannot perform list_reduce on an empty input list");
				}
				left_vector.set_index(reduced_row_idx, info.list_entries[list_column_format_index].offset);
				reduced_row_idx++;
			} else {
				// Set the row as invalid and remove it from the active rows.
				FlatVector::SetNull(info.result, original_row_idx, true);
				active_rows.SetInvalid(original_row_idx);
			}
		}
		left_slice->Slice(left_vector, reduced_row_idx);

		// input_types layout: [optional index, element (T), accumulator (S), captured args...]
		if (info.has_index) {
			input_types.push_back(LogicalType::BIGINT);
		}
		input_types.push_back(info.child_vector->GetType());
		input_types.push_back(info.lambda_expr->GetReturnType());

		// info.column_infos includes the list column plus captured args (and the initial value if present).
		// skip the first entry if there is an initial value
		for (idx_t i = info.has_initial ? 1 : 0; i < info.column_infos.size(); i++) {
			input_types.push_back(info.column_infos[i].vector.get().GetType());
		}

		accumulator_cast = make_uniq<Vector>(info.lambda_expr->GetReturnType(), info.row_count);
		expr_executor = make_uniq<ExpressionExecutor>(context, *info.lambda_expr);
	};

	ValidityMask active_rows;
	ClientContext &context;
	unique_ptr<Vector> left_slice;
	unique_ptr<Vector> accumulator_cast;
	unique_ptr<ExpressionExecutor> expr_executor;
	vector<LogicalType> input_types;

	idx_t initial_value_offset = 1;
	SelectionVector left_sel;
	SelectionVector active_rows_sel;
};

void ReferenceAccumulator(ReduceExecuteInfo &execute_info, Vector &target, Vector &source, const idx_t count,
                          const bool allow_cast) {
	if (source.GetType() != target.GetType()) {
		if (!allow_cast) {
			throw InternalException("list_reduce accumulator type mismatch: %s referenced %s",
			                        source.GetType().ToString(), target.GetType().ToString());
		}
		execute_info.accumulator_cast->SetVectorType(VectorType::FLAT_VECTOR);
		FlatVector::ValidityMutable(*execute_info.accumulator_cast).SetAllValid(count);
		VectorOperations::Cast(execute_info.context, source, *execute_info.accumulator_cast, count, true);
		target.Reference(*execute_info.accumulator_cast);
	} else {
		target.Reference(source);
	}
}

bool ExecuteReduce(const idx_t loops, ReduceExecuteInfo &execute_info, LambdaFunctions::LambdaInfo &info,
                   DataChunk &result_chunk) {
	idx_t original_row_idx = 0;
	idx_t reduced_row_idx = 0;
	idx_t valid_row_idx = 0;

	idx_t loops_offset = loops + execute_info.initial_value_offset;

	// create selection vectors for the left and right slice
	const auto data = execute_info.active_rows.GetData();

	// reset right_sel each iteration to prevent referencing issues
	SelectionVector right_sel;
	right_sel.Initialize(info.row_count);

	idx_t bits_per_entry = sizeof(idx_t) * 8;
	for (idx_t entry_idx = 0; original_row_idx < info.row_count; entry_idx++) {
		if (data[entry_idx] == 0) {
			original_row_idx += bits_per_entry;
			continue;
		}

		for (idx_t j = 0; entry_idx * bits_per_entry + j < info.row_count; j++) {
			if (!execute_info.active_rows.RowIsValid(original_row_idx)) {
				original_row_idx++;
				continue;
			}
			auto list_column_format_index = info.list_column_format.sel->get_index(original_row_idx);
			if (info.list_entries[list_column_format_index].length > loops_offset) {
				// While the list has more entries set the right slice to the next entry
				right_sel.set_index(reduced_row_idx, info.list_entries[list_column_format_index].offset + loops_offset);
				execute_info.left_sel.set_index(reduced_row_idx, valid_row_idx);
				execute_info.active_rows_sel.set_index(reduced_row_idx, original_row_idx);
				reduced_row_idx++;

			} else if (info.list_entries[list_column_format_index].length == 0 && info.has_initial &&
			           loops_offset == 0) {
				// If the list is empty and there is an initial value, use the initial value
				execute_info.active_rows.SetInvalid(original_row_idx);
				auto val = info.column_infos[0].vector.get().GetValue(original_row_idx);
				info.result.SetValue(original_row_idx, val);
			} else {
				// If the list has no more entries, write the result
				execute_info.active_rows.SetInvalid(original_row_idx);
				auto val = execute_info.left_slice->GetValue(valid_row_idx);
				info.result.SetValue(original_row_idx, val);
			}

			original_row_idx++;
			valid_row_idx++;
		}
	}

	if (reduced_row_idx == 0) {
		return true;
	}

	// create the index vector, where the index is that of the current node.
	Vector index_vector(Value::BIGINT(UnsafeNumericCast<int64_t>(loops_offset + 1)), count_t(reduced_row_idx));

	// slice the left and right slice
	execute_info.left_slice->Slice(*execute_info.left_slice, execute_info.left_sel, reduced_row_idx);
	Vector right_slice(*info.child_vector, right_sel, reduced_row_idx);

	// create the input chunk
	DataChunk input_chunk;
	input_chunk.InitializeEmpty(execute_info.input_types);
	input_chunk.SetChildCardinality(reduced_row_idx);

	const idx_t element_offset = info.has_index ? 1 : 0;
	const idx_t accumulator_offset = element_offset + 1;
	if (info.has_index) {
		input_chunk.data[0].Reference(index_vector);
	}

	if (loops == 0 && info.has_initial) {
		info.column_infos[0].vector.get().Slice(execute_info.active_rows_sel, reduced_row_idx);
		auto &initial_vector = info.column_infos[0].vector.get();
		ReferenceAccumulator(execute_info, input_chunk.data[accumulator_offset], initial_vector, reduced_row_idx, true);
	} else {
		ReferenceAccumulator(execute_info, input_chunk.data[accumulator_offset], *execute_info.left_slice,
		                     reduced_row_idx, loops == 0);
	}
	input_chunk.data[element_offset].Reference(right_slice);

	// add the other columns
	// skip the initial value if there is one
	vector<Vector> slices;
	const idx_t initial_offset = info.has_initial ? 1 : 0;
	for (idx_t i = 0; i < info.column_infos.size() - initial_offset; i++) {
		if (info.column_infos[initial_offset + i].vector.get().GetVectorType() == VectorType::CONSTANT_VECTOR) {
			// only reference constant vectors
			input_chunk.data[accumulator_offset + 1 + i].Reference(info.column_infos[initial_offset + i].vector);
		} else {
			// slice the other vectors
			slices.emplace_back(info.column_infos[initial_offset + i].vector, execute_info.active_rows_sel,
			                    reduced_row_idx);
			input_chunk.data[accumulator_offset + 1 + i].Reference(slices.back());
		}
	}

	result_chunk.Reset();
	result_chunk.SetChildCardinality(reduced_row_idx);
	execute_info.expr_executor->Execute(input_chunk, result_chunk);

	// We need to copy the result into left_slice to avoid data loss due to vector.Reference(...).
	// Otherwise, we only keep the data of the previous iteration alive, not that of previous iterations.
	execute_info.left_slice = make_uniq<Vector>(result_chunk.data[0].GetType(), reduced_row_idx);
	VectorOperations::Copy(result_chunk.data[0], *execute_info.left_slice, reduced_row_idx, 0, 0);
	return false;
}

LogicalType ResolveReduceAccumulatorType(ClientContext &context, const LogicalType &initial_type,
                                         const LogicalType &lambda_return_type) {
	LogicalType max_logical_type;
	if (LogicalType::TryGetMaxLogicalType(context, initial_type, lambda_return_type, max_logical_type)) {
		return max_logical_type;
	}
	throw BinderException("No common super type between initial value type %s and lambda return type %s",
	                      initial_type.ToString(), lambda_return_type.ToString());
}

unique_ptr<FunctionData> ListReduceBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// the list column and the bound lambda expression
	D_ASSERT(arguments.size() == 2 || arguments.size() == 3);
	if (arguments[1]->GetExpressionClass() != ExpressionClass::BOUND_LAMBDA) {
		throw BinderException("Invalid lambda expression!");
	}

	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));

	unique_ptr<FunctionData> bind_data = LambdaFunctions::ListLambdaPrepareBind(arguments, context, bound_function);
	if (bind_data) {
		return bind_data;
	}

	bool has_initial = arguments.size() == 3;
	LogicalType accumulator_type;
	if (has_initial) {
		const auto &initial_value_type = arguments[2]->GetReturnType();
		auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();
		auto &lambda_return_type = bound_lambda_expr.LambdaExpr()->GetReturnType();
		accumulator_type = ResolveReduceAccumulatorType(context, initial_value_type, lambda_return_type);
		arguments[2] = BoundCastExpression::AddCastToType(context, std::move(arguments[2]), accumulator_type);
	} else {
		auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();
		auto list_child_type = LambdaFunctions::DetermineListChildType(arguments[0]->GetReturnType());
		auto &lambda_return_type = bound_lambda_expr.LambdaExpr()->GetReturnType();
		if (!LogicalType::TryGetMaxLogicalType(context, list_child_type, lambda_return_type, accumulator_type)) {
			throw BinderException("No common super type between list element type %s and lambda return type %s",
			                      list_child_type.ToString(), lambda_return_type.ToString());
		}
	}

	auto &bound_lambda_expr = arguments[1]->Cast<BoundLambdaExpression>();
	if (bound_lambda_expr.ParameterCount() < 2 || bound_lambda_expr.ParameterCount() > 3) {
		throw BinderException("list_reduce expects a function with 2 or 3 arguments");
	}
	auto has_index = bound_lambda_expr.ParameterCount() == 3;

	const auto lambda_return_type = bound_lambda_expr.LambdaExpr()->GetReturnType();
	auto cast_lambda_expr =
	    BoundCastExpression::AddCastToType(context, std::move(bound_lambda_expr.LambdaExprMutable()), accumulator_type);
	if (!cast_lambda_expr) {
		throw BinderException("Could not cast lambda return type %s to accumulator type %s",
		                      lambda_return_type.ToString(), accumulator_type.ToString());
	}
	// cast in place, so that the bound lambda expression stays intact as a child of the function
	bound_lambda_expr.LambdaExprMutable() = std::move(cast_lambda_expr);
	bound_function.SetReturnType(bound_lambda_expr.LambdaExpr()->GetReturnType());
	if (bound_lambda_expr.LambdaExpr()->IsVolatile()) {
		bound_function.SetVolatile();
	}
	return make_uniq<ListLambdaBindData>(bound_function.GetReturnType(), bound_lambda_expr.LambdaExpr()->Copy(),
	                                     has_index, has_initial);
}

LogicalType BindReduceChildren(ClientContext &context, const vector<LogicalType> &function_child_types,
                               const idx_t parameter_idx, optional_ptr<BindLambdaContext> bind_lambda_context) {
	auto list_child_type = LambdaFunctions::DetermineListChildType(function_child_types[0]);
	const bool has_initial = function_child_types.size() == 3;

	switch (parameter_idx) {
	case 0:
		return has_initial ? function_child_types[2] : list_child_type;
	case 1:
		return list_child_type;
	case 2:
		return LogicalType::BIGINT;
	default:
		throw BinderException("This lambda function only supports up to three lambda parameters!");
	}
}

LogicalType ListReduceBindLambda(ClientContext &context, const vector<LogicalType> &function_child_types,
                                 const idx_t parameter_idx, optional_ptr<BindLambdaContext> bind_lambda_context) {
	return BindReduceChildren(context, function_child_types, parameter_idx, bind_lambda_context);
}

} // namespace

void LambdaFunctions::ListReduceFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	// Initializes the left slice from the list entries, active rows, the expression executor and the input types
	bool completed = false;
	LambdaInfo info(args, state, result, completed);
	if (completed) {
		return;
	}

	ReduceExecuteInfo execute_info(info, state.GetContext());

	// Since the left slice references the result chunk, we need to create two result chunks.
	// This means there is always an empty result chunk for the next iteration,
	// without the referenced chunk having to be reset until the current iteration is complete.
	DataChunk odd_result_chunk;
	odd_result_chunk.Initialize(Allocator::DefaultAllocator(), {info.lambda_expr->GetReturnType()});

	DataChunk even_result_chunk;
	even_result_chunk.Initialize(Allocator::DefaultAllocator(), {info.lambda_expr->GetReturnType()});

	// Execute reduce until all rows are finished.
	idx_t loops = 0;
	bool end = false;
	while (!end) {
		state.GetContext().InterruptCheck();
		auto &result_chunk = loops % 2 ? odd_result_chunk : even_result_chunk;
		auto &spare_result_chunk = loops % 2 ? even_result_chunk : odd_result_chunk;

		end = ExecuteReduce(loops, execute_info, info, result_chunk);
		spare_result_chunk.Reset();
		loops++;
	}

	if (info.is_all_constant && !info.is_volatile) {
		info.result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}
}

ScalarFunctionSet ListReduceFun::GetFunctions() {
	ScalarFunction fun({}, LogicalType::ANY, LambdaFunctions::ListReduceFunction, ListReduceBind, nullptr, nullptr);
	fun.GetSignature()
	    .AddParameter("list", LogicalType::LIST(LogicalType::ANY))
	    .AddParameter("lambda", LogicalType::LAMBDA);

	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetSerializeCallback(ListLambdaBindData::Serialize);
	fun.SetDeserializeCallback(ListLambdaBindData::Deserialize);
	fun.SetBindLambdaCallback(ListReduceBindLambda);
	fun.SetFallible();

	ScalarFunctionSet set;
	set.AddFunction(fun);
	fun.GetSignature().AddParameter("initial_value", LogicalType::ANY);
	set.AddFunction(fun);
	return set;
}

} // namespace duckdb
