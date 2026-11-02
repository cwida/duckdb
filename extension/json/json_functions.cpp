#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/string_vector.hpp"
#include "json_functions.hpp"

#include "duckdb/common/file_system.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/replacement_scan.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/parsed_data/create_pragma_function_info.hpp"
#include "duckdb/parser/tableref/table_function_ref.hpp"

namespace duckdb {

using JSONPathType = JSONCommon::JSONPathType;

JSONPathType JSONReadFunctionData::CheckPath(const Value &path_val, string &path, idx_t &len) {
	if (path_val.IsNull()) {
		throw BinderException("JSON path cannot be NULL");
	}
	const auto path_str_val = path_val.DefaultCastAs(LogicalType::VARCHAR);
	auto path_str = path_str_val.GetValueUnsafe<string_t>();
	len = path_str.GetSize();
	const auto ptr = path_str.GetData();
	JSONPathType path_type = JSONPathType::REGULAR;
	// Copy over string to the bind data
	if (len != 0) {
		if (*ptr == '/' || *ptr == '$') {
			path = string(ptr, len);
		} else if (path_val.type().IsIntegral()) {
			path = "$[" + string(ptr, len) + "]";
		} else if (memchr(ptr, '"', len)) {
			path = "/" + string(ptr, len);
		} else {
			path = "$.\"" + string(ptr, len) + "\"";
		}
		len = path.length();
		if (*path.c_str() == '$') {
			path_type = JSONCommon::ValidatePath(path.c_str(), len, true);
		}
	}
	return path_type;
}

JSONReadFunctionData::JSONReadFunctionData(bool constant, string path_p, idx_t len, JSONPathType path_type_p)
    : constant(constant), path(std::move(path_p)), path_type(path_type_p), ptr(path.c_str()), len(len) {
	if (constant && len != 0 && path[0] == '$' && path_type == JSONPathType::REGULAR) {
		// The path was validated in CheckPath, parse it once so execution can skip tokenizing it per row
		elements = JSONCommon::ParsePathElements(ptr, len, false);
		use_elements = true;
	}
}

unique_ptr<FunctionData> JSONReadFunctionData::Copy() const {
	return make_uniq<JSONReadFunctionData>(constant, path, len, path_type);
}

bool JSONReadFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<JSONReadFunctionData>();
	return constant == other.constant && path == other.path && len == other.len && path_type == other.path_type;
}

//! An integral path is an array index, other paths are cast to VARCHAR
void JSONReadFunctionData::ResolveTypes(ResolveScalarFunctionTypesInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	if (input.GetArgumentType(1).IsIntegral()) {
		bound_function.GetArguments()[1] = LogicalType::BIGINT;
	} else {
		bound_function.GetArguments()[1] = LogicalType::VARCHAR;
	}
}

unique_ptr<FunctionData> JSONReadFunctionData::Bind(BindScalarFunctionInput &input) {
	auto &bound_function = input.GetBoundFunction();
	D_ASSERT(bound_function.GetArguments().size() == 2);
	bool constant = false;
	string path;
	idx_t len = 0;
	JSONPathType path_type = JSONPathType::REGULAR;
	auto path_val = input.TryGetConstant(1);
	if (path_val && !path_val->IsNull()) {
		constant = true;
		path_type = CheckPath(*path_val, path, len);
	}
	if (path_type == JSONCommon::JSONPathType::WILDCARD) {
		bound_function.SetReturnType(LogicalType::LIST(bound_function.GetReturnType()));
	}
	return make_uniq<JSONReadFunctionData>(constant, std::move(path), len, path_type);
}

JSONReadManyFunctionData::JSONReadManyFunctionData(vector<string> paths_p, vector<idx_t> lens_p)
    : paths(std::move(paths_p)), lens(std::move(lens_p)) {
	for (const auto &path : paths) {
		ptrs.push_back(path.c_str());
	}
	for (idx_t i = 0; i < paths.size(); i++) {
		if (lens[i] != 0 && paths[i][0] == '$') {
			// The paths were validated in CheckPath, parse them once
			elements.push_back(JSONCommon::ParsePathElements(ptrs[i], lens[i], false));
			use_elements.push_back(true);
		} else {
			elements.emplace_back();
			use_elements.push_back(false);
		}
	}
}

unique_ptr<FunctionData> JSONReadManyFunctionData::Copy() const {
	return make_uniq<JSONReadManyFunctionData>(paths, lens);
}

bool JSONReadManyFunctionData::Equals(const FunctionData &other_p) const {
	auto &other = other_p.Cast<JSONReadManyFunctionData>();
	return paths == other.paths && lens == other.lens;
}

unique_ptr<FunctionData> JSONReadManyFunctionData::Bind(BindScalarFunctionInput &input) {
	D_ASSERT(input.GetBoundFunction().GetArguments().size() == 2);

	vector<string> paths;
	vector<idx_t> lens;
	auto paths_val = input.GetConstant(1);

	for (auto &path_val : ListValue::GetChildren(paths_val)) {
		paths.emplace_back("");
		lens.push_back(0);
		if (JSONReadFunctionData::CheckPath(path_val, paths.back(), lens.back()) == JSONPathType::WILDCARD) {
			throw BinderException("Cannot have wildcards in JSON path when supplying multiple paths");
		}
	}

	return make_uniq<JSONReadManyFunctionData>(std::move(paths), std::move(lens));
}

JSONFunctionLocalState::JSONFunctionLocalState(Allocator &allocator)
    : json_allocator(make_shared_ptr<JSONAllocator>(allocator)) {
}

JSONFunctionLocalState::JSONFunctionLocalState(ClientContext &context)
    : JSONFunctionLocalState(BufferAllocator::Get(context)) {
}

unique_ptr<FunctionLocalState> JSONFunctionLocalState::Init(ExpressionState &state, const BoundFunctionExpression &expr,
                                                            FunctionData *bind_data) {
	return make_uniq<JSONFunctionLocalState>(state.GetContext());
}

unique_ptr<FunctionLocalState> JSONFunctionLocalState::InitCastLocalState(CastLocalStateParameters &parameters) {
	return parameters.context ? make_uniq<JSONFunctionLocalState>(*parameters.context)
	                          : make_uniq<JSONFunctionLocalState>(Allocator::DefaultAllocator());
}

JSONFunctionLocalState &JSONFunctionLocalState::ResetAndGet(ExpressionState &state) {
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<JSONFunctionLocalState>();
	lstate.json_allocator->Reset();
	return lstate;
}

vector<ScalarFunctionSet> JSONFunctions::GetScalarFunctions() {
	vector<ScalarFunctionSet> functions;

	// Extract functions
	AddAliases({"json_extract", "json_extract_path"}, GetExtractFunction(), functions);
	AddAliases({"json_extract_string", "json_extract_path_text", "->>"}, GetExtractStringFunction(), functions);

	// Create functions
	functions.push_back(GetArrayFunction());
	functions.push_back(GetObjectFunction());
	AddAliases({"to_json", "json_quote"}, GetToJSONFunction(), functions);
	functions.push_back(ScalarFunctionSet(GetJSONCopyToJSONFunction()));
	functions.push_back(ScalarFunctionSet(GetJSONCopyToGeoJSONFunction()));
	functions.push_back(GetArrayToJSONFunction());
	functions.push_back(GetRowToJSONFunction());
	functions.push_back(GetMergePatchFunction());
	functions.push_back(GetMergePatchDiffFunction());
	functions.push_back(GetDeepMergeFunction());

	// Structure/Transform
	functions.push_back(GetStructureFunction());
	functions.push_back(GetAsGeoJSONFunction());
	functions.push_back(GetGeomFromGeoJSONFunction());
	AddAliases({"json_transform", "from_json"}, GetTransformFunction(), functions);
	AddAliases({"json_transform_strict", "from_json_strict"}, GetTransformStrictFunction(), functions);

	// Other
	functions.push_back(GetArrayLengthFunction());
	functions.push_back(GetContainsFunction());
	functions.push_back(GetExistsFunction());
	functions.push_back(GetKeysFunction());
	functions.push_back(GetTypeFunction());
	functions.push_back(GetValidFunction());
	functions.push_back(GetValueFunction());
	functions.push_back(GetSerializePlanFunction());
	functions.push_back(GetSerializeSqlFunction());
	functions.push_back(GetDeserializeSqlFunction());

	functions.push_back(GetPrettyPrintFunction());
	functions.push_back(GetNormalizeFunction());
	functions.push_back(GetStripNullsFunction());
	functions.push_back(GetInsertFunction());
	functions.push_back(GetRemoveFunction());
	functions.push_back(GetReplaceFunction());
	functions.push_back(GetSetFunction());

	return functions;
}

vector<PragmaFunctionSet> JSONFunctions::GetPragmaFunctions() {
	vector<PragmaFunctionSet> functions;
	functions.push_back(GetExecuteJsonSerializedSqlPragmaFunction());
	return functions;
}

vector<TableFunctionSet> JSONFunctions::GetTableFunctions() {
	vector<TableFunctionSet> functions;

	// Reads JSON as string
	functions.push_back(GetReadJSONObjectsFunction());
	functions.push_back(GetReadNDJSONObjectsFunction());
	functions.push_back(GetReadJSONObjectsAutoFunction());

	// Read JSON as columnar data
	functions.push_back(GetReadJSONFunction());
	functions.push_back(GetReadNDJSONFunction());
	functions.push_back(GetReadJSONAutoFunction());
	functions.push_back(GetReadNDJSONAutoFunction());
	functions.push_back(GetReadSingleJSONFileFunction());

	// Table in-out
	functions.push_back(GetJSONEachFunction());
	functions.push_back(GetJSONTreeFunction());

	// Serialized plan
	functions.push_back(GetExecuteJsonSerializedSqlFunction());

	return functions;
}

unique_ptr<TableRef> JSONFunctions::ReadJSONReplacement(ClientContext &context, ReplacementScanInput &input,
                                                        optional_ptr<ReplacementScanData> data) {
	auto table_name = ReplacementScan::GetFullPath(input);
	if (!ReplacementScan::CanReplace(table_name, {"json", "jsonl", "ndjson", "geojson", "geojsonl"})) {
		return nullptr;
	}
	auto table_function = make_uniq<TableFunctionRef>();
	vector<unique_ptr<ParsedExpression>> children;
	children.push_back(ConstantExpression::String(table_name));
	table_function->function = make_uniq<FunctionExpression>("read_json_auto", std::move(children));

	if (!FileSystem::HasGlob(table_name)) {
		auto &fs = FileSystem::GetFileSystem(context);
		table_function->alias = Identifier(fs.ExtractBaseName(table_name));
	}

	return std::move(table_function);
}

static bool CastVarcharToJSON(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<JSONFunctionLocalState>();
	lstate.json_allocator->Reset();
	auto alc = lstate.json_allocator->GetYYAlc();

	bool success = true;
	UnaryExecutor::Execute<string_t, string_t>(source, result, count, [&](string_t input) -> optional<string_t> {
		auto data = input.GetDataWriteable();
		const auto length = input.GetSize();

		yyjson_read_err error;
		auto doc = JSONCommon::ReadDocumentUnsafe(data, length, JSONCommon::READ_FLAG, alc, &error);

		if (!doc) {
			if (success) {
				HandleCastError::AssignError(JSONCommon::FormatParseError(data, length, error), parameters);
				success = false;
			}
			return nullopt;
		}

		return input;
	});
	StringVector::AddHeapReference(result, source);
	return success;
}

static bool CastJSONListToVarchar(Vector &source, Vector &result, idx_t count, CastParameters &) {
	static constexpr char const *NULL_STRING = "NULL";
	static constexpr idx_t NULL_STRING_LENGTH = 4;

	auto input_jsons = source.Values<VectorListType<string_t>>();
	auto result_data = FlatVector::Writer<string_t>(result, count);
	for (idx_t r = 0; r < count; r++) {
		auto entry = input_jsons[r];
		if (!entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}
		// Compute len (start with [] and ,)
		idx_t len = 2;
		bool seen_value = false;
		for (auto child : entry.GetChildValues()) {
			if (seen_value) {
				len += 2;
			}
			if (child.IsValid()) {
				len += child.GetValue().GetSize();
			} else {
				len += NULL_STRING_LENGTH;
			}
			seen_value = true;
		}

		// Allocate string
		auto &res = result_data.WriteEmptyString(len);
		auto ptr = res.GetDataWriteable();

		// Populate string
		*ptr++ = '[';
		seen_value = false;
		for (auto child : entry.GetChildValues()) {
			if (seen_value) {
				*ptr++ = ',';
				*ptr++ = ' ';
			}
			if (child.IsValid()) {
				auto &input_json = child.GetValue();
				memcpy(ptr, input_json.GetData(), input_json.GetSize());
				ptr += input_json.GetSize();
			} else {
				memcpy(ptr, NULL_STRING, NULL_STRING_LENGTH);
				ptr += NULL_STRING_LENGTH;
			}
			seen_value = true;
		}
		*ptr = ']';

		res.Finalize();
	}
	return true;
}

static bool CastVarcharToJSONList(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<JSONFunctionLocalState>();
	lstate.json_allocator->Reset();
	auto alc = lstate.json_allocator->GetYYAlc();

	bool success = true;
	UnaryExecutor::Execute<string_t, list_entry_t>(
	    source, result, count, [&](const string_t &input) -> optional<list_entry_t> {
		    // Figure out if the cast can succeed
		    yyjson_read_err error;
		    const auto doc = JSONCommon::ReadDocumentUnsafe(input.GetDataWriteable(), input.GetSize(),
		                                                    JSONCommon::READ_FLAG, alc, &error);
		    if (!doc || !unsafe_yyjson_is_arr(doc->root)) {
			    if (success) {
				    if (!doc) {
					    HandleCastError::AssignError(
					        JSONCommon::FormatParseError(input.GetDataWriteable(), input.GetSize(), error), parameters);
				    } else if (!unsafe_yyjson_is_arr(doc->root)) {
					    auto truncated_input =
					        input.GetSize() > 50 ? string(input.GetData(), 47) + "..." : input.GetString();
					    HandleCastError::AssignError(
					        StringUtil::Format("Cannot cast to list of JSON. Input \"%s\"", truncated_input),
					        parameters);
				    }
				    success = false;
			    }
			    return nullopt;
		    }

		    auto current_size = ListVector::GetListSize(result);
		    const auto arr_len = unsafe_yyjson_get_len(doc->root);
		    const auto new_size = current_size + arr_len;

		    // Grow list if needed
		    if (ListVector::GetListCapacity(result) < new_size) {
			    ListVector::Reserve(result, new_size);
		    }

		    // Populate list
		    const auto result_jsons = FlatVector::GetDataMutable<string_t>(ListVector::GetChildMutable(result));
		    size_t arr_idx, max;
		    yyjson_val *val;
		    yyjson_arr_foreach(doc->root, arr_idx, max, val) {
			    result_jsons[current_size + arr_idx] = JSONCommon::WriteVal(val, alc);
		    }

		    // Update size
		    ListVector::SetListSize(result, current_size + arr_len);

		    return list_entry_t {current_size, arr_len};
	    });

	JSONAllocator::AddBuffer(ListVector::GetChildMutable(result), alc);
	return success;
}

void JSONFunctions::RegisterSimpleCastFunctions(ExtensionLoader &loader) {
	auto &db = loader.GetDatabaseInstance();

	// JSON to VARCHAR is basically free
	loader.RegisterCastFunction(LogicalType::JSON(), LogicalType::VARCHAR, DefaultCasts::ReinterpretCast, 1);

	// VARCHAR to JSON requires a parse so it's not free. Let's make it 1 more than a cast to STRUCT
	const auto varchar_to_json_cost =
	    CastFunctionSet::ImplicitCastCost(db, LogicalType::SQLNULL, LogicalTypeId::STRUCT) + 1;
	BoundCastInfo varchar_to_json_info(CastVarcharToJSON, nullptr, JSONFunctionLocalState::InitCastLocalState);
	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::JSON(), std::move(varchar_to_json_info),
	                            varchar_to_json_cost);

	// Register NULL to JSON with a different cost than NULL to VARCHAR so the binder can disambiguate functions
	const auto null_to_json_cost =
	    CastFunctionSet::ImplicitCastCost(db, LogicalType::SQLNULL, LogicalTypeId::VARCHAR) + 1;
	loader.RegisterCastFunction(LogicalType::SQLNULL, LogicalType::JSON(), DefaultCasts::TryVectorNullCast,
	                            null_to_json_cost);

	// JSON[] to VARCHAR (this needs a special case otherwise the cast will escape quotes)
	const auto json_list_to_varchar_cost =
	    CastFunctionSet::ImplicitCastCost(db, LogicalType::LIST(LogicalType::JSON()), LogicalTypeId::VARCHAR) - 1;
	loader.RegisterCastFunction(LogicalType::LIST(LogicalType::JSON()), LogicalTypeId::VARCHAR, CastJSONListToVarchar,
	                            json_list_to_varchar_cost);

	// JSON[] to JSON is allowed implicitly
	loader.RegisterCastFunction(LogicalType::LIST(LogicalType::JSON()), LogicalType::JSON(), CastJSONListToVarchar,
	                            100);

	// VARCHAR to JSON[] (also needs a special case otherwise we get a VARCHAR -> VARCHAR[] cast first)
	const auto varchar_to_json_list_cost =
	    CastFunctionSet::ImplicitCastCost(db, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::JSON())) - 1;
	BoundCastInfo varchar_to_json_list_info(CastVarcharToJSONList, nullptr, JSONFunctionLocalState::InitCastLocalState);
	loader.RegisterCastFunction(LogicalType::VARCHAR, LogicalType::LIST(LogicalType::JSON()),
	                            std::move(varchar_to_json_list_info), varchar_to_json_list_cost);
}

} // namespace duckdb
