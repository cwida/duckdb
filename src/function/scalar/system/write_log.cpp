#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/function/scalar/system_functions.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/main/client_data.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/logging/log_manager.hpp"
#include "utf8proc.hpp"
#include "duckdb/main/client_context.hpp"

namespace duckdb {

namespace {

struct WriteLogBindData : FunctionData {
	//! Config
	bool disable_logging = false;
	string scope;
	LogLevel level = LogLevel::LOG_INFO;
	string type;

	//! Context
	optional_ptr<ClientContext> context;

	//! Output
	idx_t output_col = DConstants::INVALID_INDEX;
	LogicalType return_type;

	explicit WriteLogBindData() {};
	WriteLogBindData(const WriteLogBindData &other) : FunctionData(other) {
		disable_logging = other.disable_logging;
		scope = other.scope;
		level = other.level;
		type = other.type;

		context = other.context;

		output_col = other.output_col;
		return_type = other.return_type;
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<WriteLogBindData>(*this);
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

void WriteLogResolveTypes(ResolveScalarFunctionTypesInput &input) {
	if (input.GetArgumentCount() == 0) {
		throw BinderException("write_log takes at least one argument");
	}
	if (input.GetArgumentType(0) != LogicalType::VARCHAR) {
		throw InvalidTypeException("write_log first argument must be a VARCHAR");
	}
}

unique_ptr<FunctionData> WriteLogBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();

	// Used to replace the actual log call with a nop: useful for benchmarking
	auto result = make_uniq<WriteLogBindData>();

	// Default return type
	bound_function.SetReturnType(LogicalType::VARCHAR);

	for (idx_t i = 1; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		auto &name = input.GetArgumentName(i);
		if (arg->HasParameter()) {
			throw ParameterNotResolvedException();
		}
		if (name == "disable_logging") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("write_log: 'disable_logging' argument must be a boolean");
			}
			result->disable_logging = BooleanValue::Get(input.GetConstant(i));
		} else if (name == "scope") {
			if (arg->GetReturnType().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("write_log: 'scope' argument must be a string");
			}
			result->scope = StringValue::Get(input.GetConstant(i));
		} else if (name == "level") {
			if (arg->GetReturnType().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("write_log: 'level' argument must be a string");
			}
			result->level = EnumUtil::FromString<LogLevel>(StringValue::Get(input.GetConstant(i)));
		} else if (name == "log_type") {
			if (arg->GetReturnType().id() != LogicalTypeId::VARCHAR) {
				throw BinderException("write_log: 'log_type' argument must be a string");
			}
			result->type = StringValue::Get(input.GetConstant(i));
		} else if (name == "return_value") {
			result->return_type = arg->GetReturnType();
			result->output_col = i;
			bound_function.SetReturnType(result->return_type);
		} else {
			throw BinderException(StringUtil::Format("write_log: Unknown argument %s", name));
		}
	}

	result->context = context;

	return std::move(result);
}

template <class T>
void WriteLogValues(T &LogSource, LogLevel level, const string_t *data, const SelectionVector *sel, idx_t size,
                    const string &type) {
	for (idx_t i = 0; i < size; i++) {
		DUCKDB_LOG_INTERNAL(LogSource, type.c_str(), level, data[sel->get_index(i)]);
	}
}

void WriteLogFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() >= 1);

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<WriteLogBindData>();

	UnifiedVectorFormat idata;
	args.data[0].ToUnifiedFormat(idata);

	auto input_data = UnifiedVectorFormat::GetData<string_t>(idata);

	if (!info.disable_logging) {
		if (info.scope.empty() || info.scope == "connection") {
			WriteLogValues<const ClientContext>(*info.context, info.level, input_data, idata.sel, args.size(),
			                                    info.type);
		} else if (info.scope == "database") {
			WriteLogValues<const DatabaseInstance>(*info.context->db, info.level, input_data, idata.sel, args.size(),
			                                       info.type);
		} else if (info.scope == "file_opener") {
			WriteLogValues<const FileOpener>(*info.context->client_data->file_opener, info.level, input_data, idata.sel,
			                                 args.size(), info.type);
		} else {
			throw InvalidInputException(
			    "write_log: 'scope' argument unknown: '%s'. Valid values are [connection, database, file_opener]",
			    info.scope);
		}
	}

	if (info.output_col != DConstants::INVALID_INDEX) {
		result.Reference(args.data[info.output_col]);
	} else {
		ConstantVector::SetNull(result, count_t(args.size()));
	}
}

//! write_log captures its named arguments as child aliases rather than as named parameters, so they are reattached
//! as names when the call is rendered
static unique_ptr<ParsedExpression> WriteLogUnbind(FunctionUnbindInput &input) {
	vector<FunctionArgument> arguments;
	for (idx_t i = 0; i < input.children.size(); i++) {
		auto name = i == 0 ? Identifier() : input.expression.GetChildren()[i]->GetAlias();
		if (i > 0 && name.empty()) {
			return nullptr;
		}
		arguments.emplace_back(std::move(name), std::move(input.children[i]));
	}
	return make_uniq<FunctionExpression>(input.expression.Function().GetDefinition()->GetQualifiedName(),
	                                     std::move(arguments));
}

} // namespace

ScalarFunctionSet WriteLogFun::GetFunctions() {
	ScalarFunctionSet set("write_log");

	ScalarFunction fun({{"string", LogicalType::VARCHAR}}, LogicalType::ANY, WriteLogFunction, WriteLogBind, nullptr,
	                   nullptr, LogicalType(LogicalTypeId::INVALID), FunctionStability::VOLATILE);
	fun.GetSignature().AddKwargs("kwargs", LogicalType::ANY);
	fun.SetResolveTypesCallback(WriteLogResolveTypes);
	fun.GetProperties().SetRequiresExpressionNames(true);
	// an unknown scope is only detected while writing
	fun.SetFallible();
	fun.SetUnbindCallback(WriteLogUnbind);
	set.AddFunction(std::move(fun));

	return set;
}

} // namespace duckdb
