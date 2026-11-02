#include "duckdb/common/storage_compatibility.hpp"
#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/planner/logical_plan_verifier.hpp"
#include "duckdb/planner/planner.hpp"
#include "json_common.hpp"
#include "json_deserializer.hpp"
#include "json_functions.hpp"
#include "json_serializer.hpp"

namespace duckdb {

//-----------------------------------------------------------------------------
// json_serialize_plan
//-----------------------------------------------------------------------------
struct JsonSerializePlanBindData : public FunctionData {
	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;
	bool optimize = false;

	JsonSerializePlanBindData(bool skip_if_null_p, bool skip_if_empty_p, bool skip_if_default_p, bool format_p,
	                          bool optimize_p)
	    : skip_if_null(skip_if_null_p), skip_if_empty(skip_if_empty_p), skip_if_default(skip_if_default_p),
	      format(format_p), optimize(optimize_p) {
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JsonSerializePlanBindData>(skip_if_null, skip_if_empty, skip_if_default, format, optimize);
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

static unique_ptr<FunctionData> JsonSerializePlanBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw BinderException("json_serialize_plan takes at least one argument");
	}

	if (arguments[0]->GetReturnType() != LogicalType::VARCHAR) {
		throw InvalidTypeException("json_serialize_plan first argument must be a VARCHAR");
	}

	// Optional arguments
	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;
	bool optimize = false;

	for (idx_t i = 1; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		auto &alias = arg->GetAlias();
		if (alias == "skip_null") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_plan: 'skip_null' argument must be a boolean");
			}
			skip_if_null = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_empty") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_plan: 'skip_empty' argument must be a boolean");
			}
			skip_if_empty = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_default") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_plan: 'skip_default' argument must be a boolean");
			}
			skip_if_default = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "format") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_plan: 'format' argument must be a boolean");
			}
			format = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "optimize") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_plan: 'optimize' argument must be a boolean");
			}
			optimize = BooleanValue::Get(input.GetConstant(i));
		} else {
			throw BinderException(StringUtil::Format("json_serialize_plan: Unknown argument %s", alias));
		}
	}
	return make_uniq<JsonSerializePlanBindData>(skip_if_null, skip_if_empty, skip_if_default, format, optimize);
}

static bool OperatorSupportsSerialization(LogicalOperator &op, string &operator_name) {
	for (auto &child : op.children) {
		if (!OperatorSupportsSerialization(*child, operator_name)) {
			return false;
		}
	}
	auto supported = op.SupportSerialization();
	if (!supported) {
		operator_name = EnumUtil::ToString(op.type);
	}
	return supported;
}

static void JsonSerializePlanFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = local_state.json_allocator->GetYYAlc();
	const auto &inputs = args.data[0];

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JsonSerializePlanBindData>();

	if (!state.HasContext()) {
		throw InvalidInputException("json_serialize_plan: No client context available");
	}
	auto &context = state.GetContext();

	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto doc = JSONCommon::CreateDocument(alc);
		auto result_obj = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, result_obj);

		try {
			Parser parser(state.GetContext());
			parser.ParseQuery(input.GetString());
			auto plans_arr = yyjson_mut_arr(doc);

			for (auto &statement : parser.statements) {
				auto stmt = std::move(statement);

				Planner planner(context);
				planner.CreatePlan(std::move(stmt));
				auto plan = std::move(planner.plan);
				if (!plan) {
					throw InvalidInputException("json_serialize_plan: statement cannot be planned, parameters need "
					                            "to have a known value to serialize the plan");
				}

				if (info.optimize && plan->RequireOptimizer()) {
					Optimizer optimizer(*planner.binder, context);
					plan = optimizer.Optimize(std::move(plan));
				}

				ColumnBindingResolver resolver;
				LogicalPlanVerifier::Verify(context, *plan);
				resolver.VisitOperator(*plan);
				plan->ResolveOperatorTypes();

				string operator_name;
				if (!OperatorSupportsSerialization(*plan, operator_name)) {
					throw InvalidInputException("Operator '%s' does not support serialization", operator_name);
				}

				// plan inspection needs the complete current representation, not the default
				// storage compatibility
				SerializationOptions serialization_options;
				serialization_options.storage_compatibility = StorageCompatibility::Latest();
				auto plan_json = JsonSerializer::Serialize(*plan, doc, info.skip_if_null, info.skip_if_empty,
				                                           info.skip_if_default, std::move(serialization_options));
				yyjson_mut_arr_append(plans_arr, plan_json);
			}

			yyjson_mut_obj_add_false(doc, result_obj, "error");
			yyjson_mut_obj_add_val(doc, result_obj, "plans", plans_arr);

			size_t len_size_t;
			auto data = yyjson_mut_val_write_opts(result_obj,
			                                      info.format ? JSONCommon::WRITE_PRETTY_FLAG : JSONCommon::WRITE_FLAG,
			                                      alc, &len_size_t, nullptr);
			idx_t len = len_size_t;
			if (data == nullptr) {
				throw SerializationException(
				    "Failed to serialize json, perhaps the query contains invalid utf8 characters?");
			}

			return heap.AddString(data, len);

		} catch (std::exception &ex) {
			ErrorData error(ex);
			yyjson_mut_obj_add_true(doc, result_obj, "error");
			// error type and message
			yyjson_mut_obj_add_strcpy(doc, result_obj, "error_type",
			                          StringUtil::Lower(Exception::ExceptionTypeToString(error.Type())).c_str());
			yyjson_mut_obj_add_strcpy(doc, result_obj, "error_message", error.RawMessage().c_str());
			// add extra info
			for (auto &entry : error.ExtraInfo()) {
				yyjson_mut_obj_add_strcpy(doc, result_obj, entry.first.c_str(), entry.second.c_str());
			}

			size_t len_size_t;
			auto data = yyjson_mut_val_write_opts(result_obj,
			                                      info.format ? JSONCommon::WRITE_PRETTY_FLAG : JSONCommon::WRITE_FLAG,
			                                      alc, &len_size_t, nullptr);
			idx_t len = len_size_t;
			return heap.AddString(data, len);
		}
	});
}

ScalarFunctionSet JSONFunctions::GetSerializePlanFunction() {
	ScalarFunctionSet set("json_serialize_plan");

	ScalarFunction func({}, LogicalType::JSON(), JsonSerializePlanFunction, JsonSerializePlanBind, nullptr,
	                    JSONFunctionLocalState::Init);
	func.GetProperties().SetRequiresExpressionNames(true);

	func.GetSignature()
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .AddParameter("skip_null", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_empty", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_default", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("format", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("optimize", LogicalType::BOOLEAN, Value::BOOLEAN(false));

	set.AddFunction(std::move(func));

	return set;
}

} // namespace duckdb
