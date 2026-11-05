#include "duckdb/common/vector/union_vector.hpp"
#include "core_functions/scalar/union_functions.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"

namespace duckdb {

namespace {

void UnionTagResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw BinderException("Missing required arguments for union_tag function.");
	}

	if (LogicalTypeId::UNKNOWN == arguments[0]->GetReturnType().id()) {
		throw ParameterNotResolvedException();
	}

	if (LogicalTypeId::UNION != arguments[0]->GetReturnType().id()) {
		throw BinderException("First argument to union_tag function must be a union type.");
	}

	if (arguments.size() > 1) {
		throw BinderException("Too many arguments, union_tag takes at most one argument.");
	}

	auto member_count = UnionType::GetMemberCount(arguments[0]->GetReturnType());
	if (member_count == 0) {
		// this should never happen, empty unions are not allowed
		throw InternalException("Can't get tags from an empty union");
	}

	bound_function.GetArguments()[0] = arguments[0]->GetReturnType();

	auto varchar_vector = Vector(LogicalType::VARCHAR, member_count);
	auto result_data = FlatVector::Writer<string_t>(varchar_vector, member_count);
	for (idx_t i = 0; i < member_count; i++) {
		result_data.WriteValue(
		    string_t(UnionType::GetMemberName(arguments[0]->GetReturnType(), i).GetIdentifierName()));
	}
	auto enum_type = LogicalType::ENUM(varchar_vector, member_count);
	bound_function.SetReturnType(enum_type);
}

void UnionTagFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(result.GetType().id() == LogicalTypeId::ENUM);
	result.Reinterpret(UnionVector::GetTags(args.data[0]));
}

} // namespace

ScalarFunction UnionTagFun::GetFunction() {
	// TODO: Statistics?
	ScalarFunction fun({}, LogicalTypeId::ANY, UnionTagFunction, nullptr, nullptr, nullptr);
	fun.GetSignature().AddParameter("union", LogicalTypeId::UNION);
	fun.SetResolveTypesCallback(UnionTagResolveTypes);
	return fun;
}

} // namespace duckdb
