#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/function/scalar/system_functions.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/types/value.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "duckdb/logging/log_type.hpp"
#include "duckdb/function/cast/vector_cast_helpers.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/main/extension_entries.hpp"
#include "duckdb/main/extension_helper.hpp"

#include "utf8proc.hpp"

namespace duckdb {

namespace {

struct ParseLogMessageData : FunctionData {
	explicit ParseLogMessageData(const LogType &log_type_p) : log_type(log_type_p) {
	}
	const LogType &log_type;

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<ParseLogMessageData>(log_type);
	}
	bool Equals(const FunctionData &other_p) const override {
		return log_type.name == other_p.Cast<ParseLogMessageData>().log_type.name;
	}
};

void ParseLogMessageResolveTypes(ResolveScalarFunctionTypesInput &input) {
	if (input.GetArgumentCount() != 2) {
		throw BinderException("structured_log_schema: expects 1 argument", input.GetArgument(0).GetAlias());
	}
	// resolve the constant first, so that unresolved parameters are deferred
	input.GetConstant(0);
	if (input.GetArgumentType(0).id() != LogicalTypeId::VARCHAR) {
		throw BinderException("structured_log_schema: 'log_type' argument must be a string");
	}
}

unique_ptr<FunctionData> ParseLogMessageBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();

	auto type_val = input.GetConstant(0);

	auto type = StringValue::Get(type_val);

	auto lookup = LogManager::Get(context).LookupLogType(type);
	if (!lookup) {
		// the log type may belong to an extension that is not loaded yet
		ExtensionHelper::TryAutoloadFromEntry(*context.db, Identifier(type), EXTENSION_LOG_TYPES);
		lookup = LogManager::Get(context).LookupLogType(type);
	}

	if (!lookup) {
		throw InvalidInputException("structured_log_schema: '%s' not found", type);
	}

	if (!lookup->is_structured) {
		// Unstructured types we simply wrap in a struct with a single field called message
		child_list_t<LogicalType> children = {{"message", LogicalType::VARCHAR}};
		bound_function.SetReturnType(LogicalType::STRUCT(children));
	} else {
		D_ASSERT(lookup->type.IsNested());
		bound_function.SetReturnType(lookup->type);
	}

	return make_uniq<ParseLogMessageData>(*lookup);
}

void ParseLogMessageFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<ParseLogMessageData>();

	if (info.log_type.is_structured) {
		// TODO: allow more complex parsing operations than DefaultCast
		VectorOperations::DefaultCast(args.data[1], result, args.size(), true);
	} else {
		auto &struct_entries = StructVector::GetEntries(result);
		struct_entries[0].Reference(args.data[1]);
	}
}

} // namespace

ScalarFunction ParseLogMessage::GetFunction() {
	auto fun = ScalarFunction({}, LogicalType::ANY, ParseLogMessageFunction, ParseLogMessageBind, nullptr, nullptr,
	                          LogicalType(LogicalTypeId::INVALID));
	fun.GetSignature().AddParameter("type", LogicalType::VARCHAR).AddParameter("message", LogicalType::VARCHAR);
	fun.SetResolveTypesCallback(ParseLogMessageResolveTypes);
	fun.SetErrorMode(FunctionErrors::CAN_THROW_RUNTIME_ERROR);
	return fun;
}

} // namespace duckdb
