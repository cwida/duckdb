#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/parser.hpp"
#include "json_deserializer.hpp"
#include "json_functions.hpp"
#include "json_serializer.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"

namespace duckdb {

struct JsonSerializeBindData : public FunctionData {
	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;

	JsonSerializeBindData(bool skip_if_null_p, bool skip_if_empty_p, bool skip_if_default_p, bool format_p)
	    : skip_if_null(skip_if_null_p), skip_if_empty(skip_if_empty_p), skip_if_default(skip_if_default_p),
	      format(format_p) {
	}

public:
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JsonSerializeBindData>(skip_if_null, skip_if_empty, skip_if_default, format);
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}
};

static unique_ptr<FunctionData> JsonSerializeBind(BindScalarFunctionInput &input) {
	auto &arguments = input.GetArguments();
	if (arguments.empty()) {
		throw BinderException("json_serialize_sql takes at least one argument");
	}

	if (arguments[0]->GetReturnType() != LogicalType::VARCHAR) {
		throw InvalidTypeException("json_serialize_sql first argument must be a VARCHAR");
	}

	// Optional arguments

	bool skip_if_null = false;
	bool skip_if_empty = false;
	bool skip_if_default = false;
	bool format = false;

	for (idx_t i = 1; i < arguments.size(); i++) {
		auto &arg = arguments[i];
		auto &alias = arg->GetAlias();
		if (alias == "skip_null") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_null' argument must be a boolean");
			}
			skip_if_null = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_empty") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_empty' argument must be a boolean");
			}
			skip_if_empty = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "format") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'format' argument must be a boolean");
			}
			format = BooleanValue::Get(input.GetConstant(i));
		} else if (alias == "skip_default") {
			if (arg->GetReturnType().id() != LogicalTypeId::BOOLEAN) {
				throw BinderException("json_serialize_sql: 'skip_default' argument must be a boolean");
			}
			skip_if_default = BooleanValue::Get(input.GetConstant(i));
		} else {
			throw BinderException(StringUtil::Format("json_serialize_sql: Unknown argument %s", alias));
		}
	}
	return make_uniq<JsonSerializeBindData>(skip_if_null, skip_if_empty, skip_if_default, format);
}

static void JsonSerializeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = local_state.json_allocator->GetYYAlc();
	const auto &inputs = args.data[0];

	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JsonSerializeBindData>();

	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto doc = JSONCommon::CreateDocument(alc);
		auto result_obj = yyjson_mut_obj(doc);
		yyjson_mut_doc_set_root(doc, result_obj);

		try {
			Parser parser(state.GetContext());
			parser.ParseQuery(input.GetString());

			auto statements_arr = yyjson_mut_arr(doc);

			for (auto &statement : parser.statements) {
				if (statement->type != StatementType::SELECT_STATEMENT) {
					throw NotImplementedException("Only SELECT statements can be serialized to json!");
				}
				auto &select = statement->Cast<SelectStatement>();

				auto options = make_uniq<SerializationOptions>();
				options->storage_compatibility = state.GetContext().db->config.options.storage_compatibility;
				auto json = JsonSerializer::Serialize(select, doc, info.skip_if_null, info.skip_if_empty,
				                                      info.skip_if_default, *options);

				yyjson_mut_arr_append(statements_arr, json);
			}

			yyjson_mut_obj_add_false(doc, result_obj, "error");
			yyjson_mut_obj_add_val(doc, result_obj, "statements", statements_arr);
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

ScalarFunctionSet JSONFunctions::GetSerializeSqlFunction() {
	ScalarFunctionSet set("json_serialize_sql");

	ScalarFunction func({}, LogicalType::JSON(), JsonSerializeFunction, JsonSerializeBind, nullptr,
	                    JSONFunctionLocalState::Init);
	func.GetProperties().SetRequiresExpressionNames(true);

	func.GetSignature()
	    .AddParameter("sql", LogicalType::VARCHAR)
	    .AddParameter("skip_null", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_empty", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("skip_default", LogicalType::BOOLEAN, Value::BOOLEAN(false))
	    .AddParameter("format", LogicalType::BOOLEAN, Value::BOOLEAN(false));

	set.AddFunction(std::move(func));

	return set;
}

//----------------------------------------------------------------------
// JSON DESERIALIZE
//----------------------------------------------------------------------
static vector<unique_ptr<SelectStatement>> DeserializeSelectStatement(string_t input, yyjson_alc *alc) {
	auto doc = yyjson_doc_ptr(JSONCommon::ReadDocument(input, JSONCommon::READ_FLAG, alc));
	if (!doc) {
		throw ParserException("Could not parse json");
	}
	auto root = doc->root;
	auto err = yyjson_obj_get(root, "error");
	if (err && yyjson_is_true(err)) {
		auto err_type = yyjson_obj_get(root, "error_type");
		auto err_msg = yyjson_obj_get(root, "error_message");
		if (err_type && err_msg) {
			throw ParserException("Error parsing json: %s: %s", yyjson_get_str(err_type), yyjson_get_str(err_msg));
		}
		throw ParserException(
		    "Error parsing json, expected error property to contain 'error_type' and 'error_message'");
	}

	auto statements = yyjson_obj_get(root, "statements");
	if (!statements || !yyjson_is_arr(statements)) {
		throw ParserException("Error parsing json: no statements array");
	}
	auto size = yyjson_arr_size(statements);
	if (size == 0) {
		throw ParserException("Error parsing json: no statements");
	}

	vector<unique_ptr<SelectStatement>> result;

	idx_t idx;
	idx_t max;
	yyjson_val *stmt_json;
	yyjson_arr_foreach(statements, idx, max, stmt_json) {
		JsonDeserializer deserializer(stmt_json, doc);
		auto stmt = SelectStatement::Deserialize(deserializer);
		if (!stmt->node) {
			throw ParserException("Error parsing json: no select node found in json");
		}
		ParsedExpressionIterator::EnumerateQueryNodeChildren(*stmt->node, [](unique_ptr<ParsedExpression> &child) {
			if (!child) {
				throw ParserException("Error parsing json: null expression found in json");
			}
		});
		result.push_back(std::move(stmt));
	}

	return result;
}

//----------------------------------------------------------------------
// JSON DESERIALIZE SQL FUNCTION
//----------------------------------------------------------------------
static void JsonDeserializeFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &local_state = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = local_state.json_allocator->GetYYAlc();
	const auto &inputs = args.data[0];

	auto &heap = StringVector::GetStringHeap(result);
	UnaryExecutor::Execute<string_t, string_t>(inputs, result, [&](string_t input) {
		auto stmts = DeserializeSelectStatement(input, alc);
		// Combine all statements into a single semicolon separated string
		string str;
		for (idx_t i = 0; i < stmts.size(); i++) {
			if (i > 0) {
				str += "; ";
			}
			str += stmts[i]->ToString();
		}

		return heap.AddString(str);
	});
}

ScalarFunctionSet JSONFunctions::GetDeserializeSqlFunction() {
	ScalarFunctionSet set("json_deserialize_sql");
	auto function = ScalarFunction({}, LogicalType::VARCHAR, JsonDeserializeFunction, nullptr, nullptr,
	                               JSONFunctionLocalState::Init);
	function.GetSignature().AddParameter("json", LogicalType::JSON());
	function.SetFallible();
	set.AddFunction(std::move(function));
	return set;
}

//----------------------------------------------------------------------
// JSON EXECUTE SERIALIZED SQL (PRAGMA)
//----------------------------------------------------------------------
static string ExecuteJsonSerializedSqlPragmaFunction(ClientContext &context, const FunctionParameters &parameters) {
	JSONFunctionLocalState local_state(context);
	auto alc = local_state.json_allocator->GetYYAlc();

	if (parameters.values[0].IsNull()) {
		throw BinderException("json_execute_serialized_sql cannot execute NULL plan");
	}

	auto input = parameters.values[0].GetValueUnsafe<string_t>();
	auto stmts = DeserializeSelectStatement(input, alc);
	if (stmts.size() != 1) {
		throw BinderException("json_execute_serialized_sql pragma expects exactly one statement");
	}
	return stmts[0]->ToString();
}

PragmaFunctionSet JSONFunctions::GetExecuteJsonSerializedSqlPragmaFunction() {
	return PragmaFunctionSet(
	    PragmaFunction::PragmaCall("json_execute_serialized_sql", ExecuteJsonSerializedSqlPragmaFunction,
	                               FunctionSignature().AddPositionalOnly("serialized_sql", LogicalType::VARCHAR)));
}

//----------------------------------------------------------------------
// JSON EXECUTE SERIALIZED SQL (TABLE FUNCTION)
//----------------------------------------------------------------------
struct ExecuteSqlTableFunction {
	struct BindData : public TableFunctionData {
		shared_ptr<Relation> plan;
		unique_ptr<QueryResult> result;
		unique_ptr<Connection> con;

		bool SupportStatementCache() const override {
			return false;
		}
	};

	static unique_ptr<FunctionData> Bind(ClientContext &context, TableFunctionBindInput &input,
	                                     vector<LogicalType> &return_types, vector<Identifier> &names) {
		JSONFunctionLocalState local_state(context);
		auto alc = local_state.json_allocator->GetYYAlc();

		auto result = make_uniq<BindData>();

		result->con = make_uniq<Connection>(*context.db);
		if (input.inputs[0].IsNull()) {
			throw BinderException("json_execute_serialized_sql cannot execute NULL plan");
		}
		auto serialized = input.inputs[0].GetValueUnsafe<string>();
		auto stmts = DeserializeSelectStatement(serialized, alc);
		if (stmts.size() != 1) {
			throw BinderException("json_execute_serialized_sql expects exactly one statement");
		}
		result->plan = result->con->RelationFromQuery(std::move(stmts[0]));

		for (auto &col : result->plan->Columns()) {
			return_types.emplace_back(col.Type());
			names.emplace_back(col.Name());
		}
		return std::move(result);
	}

	static void Function(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
		auto &data = data_p.bind_data->CastNoConst<BindData>();
		if (!data.result) {
			data.result = data.plan->Execute();
		}
		auto result_chunk = data.result->Fetch();
		if (!result_chunk) {
			return;
		}
		output.Move(*result_chunk);
	}
};

TableFunctionSet JSONFunctions::GetExecuteJsonSerializedSqlFunction() {
	TableFunction func("json_execute_serialized_sql",
	                   FunctionSignature().AddPositionalOnly("serialized_sql", LogicalType::VARCHAR),
	                   ExecuteSqlTableFunction::Function, ExecuteSqlTableFunction::Bind);
	return TableFunctionSet(func);
}

} // namespace duckdb
