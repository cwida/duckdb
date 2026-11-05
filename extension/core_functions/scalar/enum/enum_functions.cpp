#include "core_functions/scalar/enum_functions.hpp"

namespace duckdb {

static void EnumFirstFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto types = input.GetTypes();
	D_ASSERT(types.size() == 1);
	auto enum_size = EnumType::GetSize(types[0]);
	auto &enum_vector = EnumType::GetValuesInsertOrder(types[0]);
	auto val = enum_size == 0 ? Value(LogicalType::VARCHAR) : enum_vector.GetValue(0);
	result.Reference(val, count_t(input.size()));
}

static void EnumLastFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto types = input.GetTypes();
	D_ASSERT(types.size() == 1);
	auto enum_size = EnumType::GetSize(types[0]);
	auto &enum_vector = EnumType::GetValuesInsertOrder(types[0]);
	auto val = enum_size == 0 ? Value(LogicalType::VARCHAR) : enum_vector.GetValue(enum_size - 1);
	result.Reference(val, count_t(input.size()));
}

static void EnumRangeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.ColumnCount() == 1);
	auto &enum_type = input.data[0].GetType();
	auto &enum_vector = EnumType::GetValuesInsertOrder(enum_type);
	auto enum_strings = enum_vector.Values<string_t>();
	const auto enum_size = EnumType::GetSize(enum_type);

	// every row gets the same list: write the enum's values into the result once and make it a constant, instead of
	// materializing them as Values (three copies of the enum per call, dozens of bytes per value)
	{
		auto writer = FlatVector::Writer<VectorListType<string_t>>(result, 1);
		idx_t enum_idx = 0;
		for (auto &child_writer : writer.WriteList(enum_size)) {
			child_writer.WriteValue(enum_strings[enum_idx++].GetValue());
		}
	}
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
}

static void EnumRangeBoundaryFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	auto types = input.GetTypes();
	D_ASSERT(types.size() == 2);

	// the binder guarantees that at least one of the parameters is an ENUM
	auto &enum_type = types[0].id() == LogicalTypeId::ENUM ? types[0] : types[1];
	auto &enum_vector = EnumType::GetValuesInsertOrder(enum_type);
	auto enum_strings = enum_vector.Values<string_t>();

	const auto count = input.size();
	auto writer = FlatVector::Writer<VectorListType<string_t>>(result, count);
	for (idx_t row = 0; row < count; row++) {
		// a NULL boundary means that the range starts at the first / ends at the last value of the enum
		auto first_param = input.GetValue(0, row);
		auto second_param = input.GetValue(1, row);
		idx_t start = first_param.IsNull() ? 0 : first_param.GetValue<uint32_t>();
		idx_t end = second_param.IsNull() ? EnumType::GetSize(enum_type) : second_param.GetValue<uint32_t>() + 1;
		idx_t enum_idx = start;
		for (auto &child_writer : writer.WriteList(end > start ? end - start : 0)) {
			child_writer.WriteValue(enum_strings[enum_idx++].GetValue());
		}
	}
}

static void EnumCodeFunction(DataChunk &input, ExpressionState &state, Vector &result) {
	D_ASSERT(input.GetTypes().size() == 1);
	result.Reinterpret(input.data[0]);
}

static void CheckEnumParameter(const Expression &expr) {
	if (expr.HasParameter()) {
		throw ParameterNotResolvedException();
	}
}

static unique_ptr<FunctionData> BindEnumFunction(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	CheckEnumParameter(*arguments[0]);
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::ENUM) {
		throw BinderException("This function needs an ENUM as an argument");
	}
	return nullptr;
}

static unique_ptr<FunctionData> BindEnumCodeFunction(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	CheckEnumParameter(*arguments[0]);
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::ENUM) {
		throw BinderException("This function needs an ENUM as an argument");
	}

	auto phy_type = EnumType::GetPhysicalType(arguments[0]->GetReturnType());
	switch (phy_type) {
	case PhysicalType::UINT8:
		bound_function.SetReturnType(LogicalType(LogicalTypeId::UTINYINT));
		break;
	case PhysicalType::UINT16:
		bound_function.SetReturnType(LogicalType(LogicalTypeId::USMALLINT));
		break;
	case PhysicalType::UINT32:
		bound_function.SetReturnType(LogicalType(LogicalTypeId::UINTEGER));
		break;
	case PhysicalType::UINT64:
		bound_function.SetReturnType(LogicalType(LogicalTypeId::UBIGINT));
		break;
	default:
		throw InternalException("Unsupported Enum Internal Type");
	}

	return nullptr;
}

static unique_ptr<FunctionData> BindEnumRangeBoundaryFunction(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	CheckEnumParameter(*arguments[0]);
	CheckEnumParameter(*arguments[1]);
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::ENUM &&
	    arguments[0]->GetReturnType() != LogicalType::SQLNULL) {
		throw BinderException("This function needs an ENUM as an argument");
	}
	if (arguments[1]->GetReturnType().id() != LogicalTypeId::ENUM &&
	    arguments[1]->GetReturnType() != LogicalType::SQLNULL) {
		throw BinderException("This function needs an ENUM as an argument");
	}
	if (arguments[0]->GetReturnType() == LogicalType::SQLNULL &&
	    arguments[1]->GetReturnType() == LogicalType::SQLNULL) {
		throw BinderException("This function needs an ENUM as an argument");
	}
	if (arguments[0]->GetReturnType().id() == LogicalTypeId::ENUM &&
	    arguments[1]->GetReturnType().id() == LogicalTypeId::ENUM &&
	    arguments[0]->GetReturnType() != arguments[1]->GetReturnType()) {
		throw BinderException("The parameters need to link to ONLY one enum OR be NULL ");
	}
	return nullptr;
}

ScalarFunction EnumFirstFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::VARCHAR, EnumFirstFunction, BindEnumFunction);
	fun.GetSignature().AddParameter("enum", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction EnumLastFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::VARCHAR, EnumLastFunction, BindEnumFunction);
	fun.GetSignature().AddParameter("enum", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction EnumCodeFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::ANY, EnumCodeFunction, BindEnumCodeFunction);
	fun.GetSignature().AddParameter("enum", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction EnumRangeFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::LIST(LogicalType::VARCHAR), EnumRangeFunction, BindEnumFunction);
	fun.GetSignature().AddParameter("enum", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction EnumRangeBoundaryFun::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::LIST(LogicalType::VARCHAR), EnumRangeBoundaryFunction,
	                          BindEnumRangeBoundaryFunction);
	fun.GetSignature().AddParameter("start", LogicalType::ANY).AddParameter("end", LogicalType::ANY);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
