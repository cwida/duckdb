#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "core_functions/scalar/list_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/planner/expression_binder.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/parser/query_error_context.hpp"
#include "duckdb/parser/tableref/at_clause.hpp"
#include "duckdb/planner/expression/bound_case_expression.hpp"

namespace duckdb {

namespace {

template <class T>
void TemplatedPopulateChild(DataChunk &args, Vector &result) {
	const auto column_count = args.ColumnCount();
	const auto row_count = args.size();

	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<T>(result, row_count * column_count);
	auto unified_format = args.ToUnifiedFormat();
	for (idx_t row = 0; row < row_count; row++) {
		for (idx_t col = 0; col < column_count; col++) {
			auto input_idx = unified_format[col].sel->get_index(row);
			if (!unified_format[col].validity.RowIsValid(input_idx)) {
				result_data.WriteNull();
				continue;
			}
			const auto input_data = UnifiedVectorFormat::GetData<T>(unified_format[col]);
			result_data.WriteValue(input_data[input_idx]);
		}
	}
}

void PopulateChildFallback(DataChunk &args, Vector &result) {
	auto &child_type = ListType::GetChildType(result.GetType());
	auto result_data = FlatVector::Writer<list_entry_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		const auto entry_offset = ListVector::GetListSize(result);
		for (idx_t col_idx = 0; col_idx < args.ColumnCount(); col_idx++) {
			auto val = args.GetValue(col_idx, i).DefaultCastAs(child_type);
			ListVector::PushBack(result, val);
		}
		result_data.WriteValue(list_entry_t(entry_offset, args.ColumnCount()));
	}
}

void ListFunction(DataChunk &args, Vector &result);
bool StructFunction(DataChunk &args, Vector &result);

bool PopulateChild(DataChunk &args, Vector &result) {
	switch (result.GetType().InternalType()) {
	case PhysicalType::BOOL:
	case PhysicalType::INT8:
		TemplatedPopulateChild<int8_t>(args, result);
		break;
	case PhysicalType::INT16:
		TemplatedPopulateChild<int16_t>(args, result);
		break;
	case PhysicalType::INT32:
		TemplatedPopulateChild<int32_t>(args, result);
		break;
	case PhysicalType::INT64:
		TemplatedPopulateChild<int64_t>(args, result);
		break;
	case PhysicalType::UINT8:
		TemplatedPopulateChild<uint8_t>(args, result);
		break;
	case PhysicalType::UINT16:
		TemplatedPopulateChild<uint16_t>(args, result);
		break;
	case PhysicalType::UINT32:
		TemplatedPopulateChild<uint32_t>(args, result);
		break;
	case PhysicalType::UINT64:
		TemplatedPopulateChild<uint64_t>(args, result);
		break;
	case PhysicalType::INT128:
		TemplatedPopulateChild<hugeint_t>(args, result);
		break;
	case PhysicalType::UINT128:
		TemplatedPopulateChild<uhugeint_t>(args, result);
		break;
	case PhysicalType::FLOAT:
		TemplatedPopulateChild<float>(args, result);
		break;
	case PhysicalType::DOUBLE:
		TemplatedPopulateChild<double>(args, result);
		break;
	case PhysicalType::INTERVAL:
		TemplatedPopulateChild<interval_t>(args, result);
		break;
	case PhysicalType::VARCHAR:
		TemplatedPopulateChild<string_t>(args, result);
		break;
	case PhysicalType::LIST:
		ListFunction(args, result);
		break;
	case PhysicalType::STRUCT:
		return StructFunction(args, result);
	case PhysicalType::UNKNOWN:
	case PhysicalType::INVALID:
		throw InternalException("Cannot create a list of types %s - an explicit cast is required",
		                        result.GetType().ToString());
	default:
		// execute the fallback instead, which requires the parent result
		return false;
	}
	return true;
}

void ListFunction(DataChunk &args, Vector &result) {
	const idx_t column_count = args.ColumnCount();

	vector<idx_t> col_offsets;
	idx_t offset_sum = 0;
	for (idx_t col = 0; col < column_count; col++) {
		col_offsets.push_back(offset_sum);
		auto &list = args.data[col];
		const auto length = ListVector::GetListSize(list);
		offset_sum += length;
	}

	ListVector::Reserve(result, offset_sum);

	// The result vector is [[a], [b], ...], result_child is [a, b, ...].
	auto &result_child = ListVector::GetChildMutable(result);

	auto unified_format = args.ToUnifiedFormat();
	vector<const list_entry_t *> col_data;
	for (idx_t col = 0; col < column_count; col++) {
		auto &list = args.data[col];
		col_data.push_back(UnifiedVectorFormat::GetData<list_entry_t>(unified_format[col]));

		const auto length = ListVector::GetListSize(list);
		if (length == 0) {
			continue;
		}
		auto &child_vector = ListVector::GetChildMutable(list);
		VectorOperations::Copy(child_vector, result_child, length, 0, col_offsets[col]);
	}

	D_ASSERT(result.GetVectorType() == VectorType::FLAT_VECTOR);

	auto result_data = FlatVector::Writer<list_entry_t>(result, args.size() * column_count);
	for (idx_t row = 0; row < args.size(); row++) {
		for (idx_t col = 0; col < column_count; col++) {
			const auto input_idx = unified_format[col].sel->get_index(row);
			if (!unified_format[col].validity.RowIsValid(input_idx)) {
				result_data.WriteNull();
				continue;
			}
			const auto input = col_data[col][input_idx];
			const auto length = input.length;
			const auto offset = col_offsets[col] + input.offset;
			result_data.WriteValue(list_entry_t(offset, length));
		}
	}
	ListVector::SetListSize(result, offset_sum);
}

bool StructFunction(DataChunk &args, Vector &result) {
	const idx_t column_count = args.ColumnCount();
	auto &result_members = StructVector::GetEntries(result);

	for (idx_t member_idx = 0; member_idx < result_members.size(); member_idx++) {
		// Same type for each column's member.
		vector<LogicalType> types;
		const auto member_type = result_members[member_idx].GetType();
		for (idx_t col = 0; col < column_count; col++) {
			types.push_back(member_type);
		}

		DataChunk chunk;
		chunk.InitializeEmpty(types);
		for (idx_t col = 0; col < column_count; col++) {
			auto &struct_vector = args.data[col];
			if (struct_vector.GetVectorType() != VectorType::CONSTANT_VECTOR) {
				struct_vector.Flatten();
			}
			auto &struct_vector_members = StructVector::GetEntries(struct_vector);
			chunk.data[col].Reference(struct_vector_members[member_idx]);
		}

		if (!PopulateChild(chunk, result_members[member_idx])) {
			return false;
		}
	}

	// Set the top level result validity
	const auto unified_format = args.ToUnifiedFormat();
	auto &result_validity = FlatVector::ValidityMutable(result);
	for (idx_t row = 0; row < args.size(); row++) {
		for (idx_t col = 0; col < column_count; col++) {
			const auto input_idx = unified_format[col].sel->get_index(row);
			const auto result_idx = row * column_count + col;
			if (!unified_format[col].validity.RowIsValid(input_idx)) {
				result_validity.SetInvalid(result_idx);
			}
		}
	}

	return true;
}

void ListValueFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);

	if (args.ColumnCount() == 0) {
		// Early out because the result is a constant empty list.
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		auto result_data = ConstantVector::GetData<list_entry_t>(result);
		result_data[0].length = 0;
		result_data[0].offset = 0;
		return;
	}

	ListVector::Reserve(result, args.size() * args.ColumnCount());
	auto &result_child = ListVector::GetChildMutable(result);

	if (!PopulateChild(args, result_child)) {
		PopulateChildFallback(args, result);
	}

	const idx_t column_count = args.ColumnCount();
	auto result_data = FlatVector::Writer<list_entry_t>(result, args.size());
	for (idx_t row = 0; row < args.size(); row++) {
		result_data.WriteValue(list_entry_t(row * column_count, column_count));
	}
	ListVector::SetListSize(result, column_count * args.size());
}

void UnpivotResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	// collect names and deconflict, construct return type
	LogicalType child_type =
	    arguments.empty() ? LogicalType::SQLNULL : ExpressionBinder::GetExpressionReturnType(*arguments[0]);
	for (idx_t i = 1; i < arguments.size(); i++) {
		auto arg_type = ExpressionBinder::GetExpressionReturnType(*arguments[i]);
		if (!LogicalType::TryGetMaxLogicalType(context, child_type, arg_type, child_type)) {
			string list_arguments = "Full list: ";
			idx_t error_index = list_arguments.size();
			for (idx_t k = 0; k < arguments.size(); k++) {
				if (k > 0) {
					list_arguments += ", ";
				}
				if (k == i) {
					error_index = list_arguments.size();
				}
				list_arguments += arguments[k]->ToString() + " " + arguments[k]->GetReturnType().ToString();
			}
			auto error = StringUtil::Format("Cannot unpivot columns of types %s and %s - an explicit cast is required",
			                                child_type.ToString(), arg_type.ToString());
			throw BinderException(arguments[i]->GetQueryLocation(),
			                      QueryErrorContext::Format(list_arguments, error, error_index, false));
		}
	}
	child_type = LogicalType::NormalizeType(child_type);

	auto &function_args = bound_function.GetArguments();
	function_args.clear();
	function_args.reserve(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		function_args.push_back(child_type);
	}
	bound_function.SetReturnType(LogicalType::LIST(child_type));
}

unique_ptr<BaseStatistics> ListValueStats(ClientContext &context, FunctionStatisticsInput &input) {
	auto &child_stats = input.child_stats;
	auto &expr = input.expr;
	auto list_stats = ListStats::CreateEmpty(expr.GetReturnType());
	auto &list_child_stats = ListStats::GetChildStats(list_stats);
	for (idx_t i = 0; i < child_stats.size(); i++) {
		list_child_stats.Merge(child_stats[i]);
	}
	list_stats.SetHasNoNullFast();
	return list_stats.ToUnique();
}

} // namespace

//! The arguments are cast to the child type of the list at execution time - if any of those casts can throw the
//! function is fallible
void ListValueResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &child_type = ListType::GetChildType(bound_function.GetReturnType());
	for (auto &argument : input.GetArguments()) {
		auto argument_type = ExpressionBinder::GetExpressionReturnType(*argument);
		if (BoundCastExpression::CastCanThrow(argument_type, child_type, false)) {
			bound_function.SetFallible();
			break;
		}
	}
}

ScalarFunctionSet ListValueFun::GetFunctions() {
	ScalarFunctionSet set("list_value");

	// Overload for 0 arguments, which returns an empty list.
	ScalarFunction empty_fun({}, LogicalType::LIST(LogicalType::SQLNULL), ListValueFunction, nullptr, ListValueStats);
	set.AddFunction(empty_fun);

	// Overload for 1 + N arguments, which returns a list of the arguments.
	auto element_type = LogicalType::TEMPLATE("T");
	ScalarFunction value_fun({element_type}, LogicalType::LIST(element_type), ListValueFunction, nullptr,
	                         ListValueStats);
	value_fun.SetResolveTypesCallback(ListValueResolveTypes);
	value_fun.GetSignature().GetParameter(0).SetName("value");
	value_fun.GetSignature().AddArgs("args", element_type);
	value_fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	set.AddFunction(value_fun);

	return set;
}

ScalarFunction UnpivotListFun::GetFunction() {
	ScalarFunction fun("unpivot_list", {}, LogicalTypeId::LIST, ListValueFunction, VariableReturnBindData::Bind,
	                   ListValueStats);
	fun.SetResolveTypesCallback(UnpivotResolveTypes);
	fun.GetSignature().AddArgs("args", LogicalTypeId::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
