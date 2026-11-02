#include "duckdb/common/vector/array_vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector/list_vector.hpp"
#include "duckdb/common/vector/map_vector.hpp"
#include "duckdb/common/vector/union_vector.hpp"
#include "duckdb/common/vector/struct_vector.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/function/cast/cast_function_set.hpp"
#include "duckdb/function/cast/default_casts.hpp"
#include "duckdb/function/function_binder.hpp"
#include "duckdb/function/scalar/strftime_format.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "json_common.hpp"
#include "duckdb/main/client_context.hpp"
#include "json_functions.hpp"
#include "json_geojson.hpp"

namespace duckdb {

static constexpr const char *JSON_COPY_TO_JSON_INTERNAL_NAME = "__internal_json_copy_to_json";
static constexpr const char *JSON_COPY_TO_GEOJSON_INTERNAL_NAME = "__internal_json_copy_to_geojson";

struct StructNames {
	void Insert(const string &name) {
		values[name] = make_uniq<Vector>(Value(name), count_t(0ULL));
	}

	StructNames Copy() const {
		StructNames result;
		// Have to do this because we can't implicitly copy Vector
		for (const auto &kv : values) {
			// The vectors are const vectors of the key value
			result.values[kv.first] = make_uniq<Vector>(Value(kv.first), count_t(0ULL));
		}
		return result;
	}

	bool Equals(const StructNames &other) const {
		if (values.size() != other.values.size()) {
			return false;
		}
		for (const auto &kv : values) {
			if (other.values.find(kv.first) == other.values.end()) {
				return false;
			}
		}
		return true;
	}

private:
	unordered_map<string, unique_ptr<Vector>> values;
};

struct JSONCreateFunctionData : public FunctionData {
public:
	explicit JSONCreateFunctionData(StructNames const_struct_names_p)
	    : const_struct_names(std::move(const_struct_names_p)) {
	}
	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONCreateFunctionData>(const_struct_names.Copy());
	}
	bool Equals(const FunctionData &other_p) const override {
		return true;
	}

public:
	// Const struct name vectors live here so they don't have to be re-initialized for every DataChunk
	StructNames const_struct_names;
};

//! How a GEOMETRY value is represented in JSON. WKT is the default so that plain JSON output is unchanged;
//! COPY ... TO ... (FORMAT GEOJSON) selects GEOJSON. A global setting could flip the default later.
enum class JSONGeometryFormat : uint8_t { WKT, GEOJSON };

struct JSONCopyFormatOptions {
	JSONGeometryFormat geometry_format = JSONGeometryFormat::WKT;
	mutable optional_ptr<StrfTimeFormat> date_format;
	mutable optional_ptr<StrfTimeFormat> timestamp_format;
	mutable optional_ptr<ClientContext> context;
	optional_ptr<const Expression> timestamptz_format_expression;
	optional_ptr<const Expression> timestamptz_ns_format_expression;
};

static JSONGeometryFormat ParseGeometryFormat(const Value &value) {
	if (value.IsNull()) {
		return JSONGeometryFormat::WKT;
	}
	const auto format = StringUtil::Lower(StringValue::Get(value));
	if (format == "wkt") {
		return JSONGeometryFormat::WKT;
	}
	if (format == "geojson") {
		return JSONGeometryFormat::GEOJSON;
	}
	throw InvalidInputException("Invalid value \"%s\" for json_geometry_format, expected 'wkt' or 'geojson'",
	                            StringValue::Get(value));
}

void JSONFunctions::ValidateGeometryFormat(ClientContext &, SetScope, Value &parameter) {
	ParseGeometryFormat(parameter);
}

//! GEOMETRY is written as WKT unless the json_geometry_format setting says otherwise. COPY ... TO ...
//! (FORMAT GEOJSON) writes GeoJSON regardless, by overriding this afterwards.
static JSONGeometryFormat ClientGeometryFormat(ClientContext &context) {
	Value value;
	if (!context.TryGetCurrentSetting("json_geometry_format", value)) {
		return JSONGeometryFormat::WKT;
	}
	return ParseGeometryFormat(value);
}

static JSONCopyFormatOptions ClientFormatOptions(ClientContext &context) {
	JSONCopyFormatOptions result;
	result.context = context;
	result.geometry_format = ClientGeometryFormat(context);
	return result;
}

struct JSONCopyToJSONFunctionData : public FunctionData {
public:
	JSONCopyToJSONFunctionData(StructNames const_struct_names_p, bool has_date_format_p, string date_format_string_p,
	                           StrfTimeFormat date_format_p, bool has_timestamp_format_p,
	                           string timestamp_format_string_p, StrfTimeFormat timestamp_format_p,
	                           unique_ptr<Expression> timestamptz_format_expression_p,
	                           unique_ptr<Expression> timestamptz_ns_format_expression_p)
	    : const_struct_names(std::move(const_struct_names_p)), has_date_format(has_date_format_p),
	      date_format_string(std::move(date_format_string_p)), date_format(std::move(date_format_p)),
	      has_timestamp_format(has_timestamp_format_p), timestamp_format_string(std::move(timestamp_format_string_p)),
	      timestamp_format(std::move(timestamp_format_p)),
	      timestamptz_format_expression(std::move(timestamptz_format_expression_p)),
	      timestamptz_ns_format_expression(std::move(timestamptz_ns_format_expression_p)) {
	}

	unique_ptr<FunctionData> Copy() const override {
		return make_uniq<JSONCopyToJSONFunctionData>(
		    const_struct_names.Copy(), has_date_format, date_format_string, date_format, has_timestamp_format,
		    timestamp_format_string, timestamp_format,
		    timestamptz_format_expression ? timestamptz_format_expression->Copy() : nullptr,
		    timestamptz_ns_format_expression ? timestamptz_ns_format_expression->Copy() : nullptr);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONCopyToJSONFunctionData>();
		return has_date_format == other.has_date_format && has_timestamp_format == other.has_timestamp_format &&
		       date_format_string == other.date_format_string &&
		       timestamp_format_string == other.timestamp_format_string &&
		       const_struct_names.Equals(other.const_struct_names);
	}

	JSONCopyFormatOptions GetFormatOptions(ClientContext &context) {
		JSONCopyFormatOptions result;
		result.geometry_format = ClientGeometryFormat(context);
		result.date_format = has_date_format ? optional_ptr<StrfTimeFormat>(&date_format) : nullptr;
		result.timestamp_format = has_timestamp_format ? optional_ptr<StrfTimeFormat>(&timestamp_format) : nullptr;
		result.context = context;
		result.timestamptz_format_expression = timestamptz_format_expression
		                                           ? optional_ptr<const Expression>(timestamptz_format_expression.get())
		                                           : nullptr;
		result.timestamptz_ns_format_expression =
		    timestamptz_ns_format_expression ? optional_ptr<const Expression>(timestamptz_ns_format_expression.get())
		                                     : nullptr;
		return result;
	}

public:
	StructNames const_struct_names;
	bool has_date_format;
	string date_format_string;
	StrfTimeFormat date_format;
	bool has_timestamp_format;
	string timestamp_format_string;
	StrfTimeFormat timestamp_format;
	unique_ptr<Expression> timestamptz_format_expression;
	unique_ptr<Expression> timestamptz_ns_format_expression;
};

//! Bind data for the GeoJSON Feature writer: which payload child is the geometry, which are properties, plus the
//! regular date/timestamp formatting options used for the property values
struct JSONCopyToGeoJSONFunctionData : public FunctionData {
public:
	JSONCopyToGeoJSONFunctionData(unique_ptr<JSONCopyToJSONFunctionData> formats_p, optional_idx geometry_index_p,
	                              optional_idx id_index_p, vector<idx_t> property_indices_p, bool write_bbox_p)
	    : formats(std::move(formats_p)), geometry_index(geometry_index_p), id_index(id_index_p),
	      property_indices(std::move(property_indices_p)), write_bbox(write_bbox_p) {
	}

	unique_ptr<FunctionData> Copy() const override {
		auto formats_copy = unique_ptr_cast<FunctionData, JSONCopyToJSONFunctionData>(formats->Copy());
		return make_uniq<JSONCopyToGeoJSONFunctionData>(std::move(formats_copy), geometry_index, id_index,
		                                                property_indices, write_bbox);
	}

	bool Equals(const FunctionData &other_p) const override {
		auto &other = other_p.Cast<JSONCopyToGeoJSONFunctionData>();
		return geometry_index == other.geometry_index && id_index == other.id_index &&
		       property_indices == other.property_indices && write_bbox == other.write_bbox &&
		       formats->Equals(*other.formats);
	}

public:
	unique_ptr<JSONCopyToJSONFunctionData> formats;
	//! Invalid when there is no geometry column, i.e. every Feature gets a null geometry
	optional_idx geometry_index;
	//! Invalid when no column is written as the Feature-level "id"
	optional_idx id_index;
	vector<idx_t> property_indices;
	//! Whether to generate a "bbox" member from the geometry
	bool write_bbox;
};

static LogicalType GetJSONType(StructNames &const_struct_names, const LogicalType &type) {
	if (type.IsJSONType()) {
		return type;
	}

	switch (type.id()) {
	// These types can go directly into JSON
	case LogicalTypeId::SQLNULL:
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT:
	case LogicalTypeId::HUGEINT:
	case LogicalTypeId::UHUGEINT:
	case LogicalTypeId::UTINYINT:
	case LogicalTypeId::USMALLINT:
	case LogicalTypeId::UINTEGER:
	case LogicalTypeId::UBIGINT:
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::BIT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::DATE:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::UUID:
	case LogicalTypeId::BIGNUM:
	case LogicalTypeId::DECIMAL:
	// GEOMETRY is emitted as either WKT or GeoJSON, so it must reach CreateValues untouched
	case LogicalTypeId::GEOMETRY:
		return type;
	case LogicalTypeId::VARIANT:
		return LogicalType::JSON();
	case LogicalTypeId::LIST:
		return LogicalType::LIST(GetJSONType(const_struct_names, ListType::GetChildType(type)));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(GetJSONType(const_struct_names, ArrayType::GetChildType(type)),
		                          ArrayType::GetSize(type));
	// Struct and MAP are treated as JSON values
	case LogicalTypeId::STRUCT: {
		child_list_t<LogicalType> child_types;
		for (const auto &child_type : StructType::GetChildTypes(type)) {
			const_struct_names.Insert(child_type.first.GetIdentifierName());
			child_types.emplace_back(child_type.first, GetJSONType(const_struct_names, child_type.second));
		}
		return LogicalType::STRUCT(child_types);
	}
	// A TUPLE is an unnamed struct: it is serialized as a JSON array (matching how Python serializes tuples)
	case LogicalTypeId::TUPLE: {
		vector<LogicalType> child_types;
		for (const auto &child_type : StructType::GetChildTypes(type)) {
			child_types.push_back(GetJSONType(const_struct_names, child_type.second));
		}
		return LogicalType::TUPLE(std::move(child_types));
	}
	case LogicalTypeId::MAP: {
		return LogicalType::MAP(MapType::KeyType(type), GetJSONType(const_struct_names, MapType::ValueType(type)));
	}
	case LogicalTypeId::UNION: {
		child_list_t<LogicalType> member_types;
		for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(type); member_idx++) {
			auto &member_name = UnionType::GetMemberName(type, member_idx);
			auto &member_type = UnionType::GetMemberType(type, member_idx);
			const_struct_names.Insert(member_name.GetIdentifierName());
			member_types.emplace_back(member_name, GetJSONType(const_struct_names, member_type));
		}
		return LogicalType::UNION(member_types);
	}
	// All other types (e.g. date) are cast to VARCHAR
	default:
		return LogicalTypeId::VARCHAR;
	}
}

//! Resolves the argument types and the struct names - this is called from both resolve_types and bind, which only
//! works because the JSON type of a JSON type is the same type
static unique_ptr<FunctionData> JSONCreateBindParams(BoundScalarFunction &bound_function,
                                                     const vector<unique_ptr<Expression>> &arguments, bool object) {
	StructNames const_struct_names;
	auto &bound_arguments = bound_function.GetArguments();
	bound_arguments.clear();
	bound_arguments.reserve(arguments.size());
	for (idx_t i = 0; i < arguments.size(); i++) {
		auto &type = arguments[i]->GetReturnType();
		if (arguments[i]->HasParameter()) {
			throw ParameterNotResolvedException();
		} else if (object && i % 2 == 0) {
			if (type != LogicalType::VARCHAR) {
				throw BinderException("json_object() keys must be VARCHAR, add an explicit cast to argument %s",
				                      arguments[i]->GetName());
			}
			bound_arguments.push_back(LogicalType::VARCHAR);
		} else {
			// Value, cast to types that we can put in JSON
			bound_arguments.push_back(GetJSONType(const_struct_names, type));
		}
	}
	return make_uniq<JSONCreateFunctionData>(std::move(const_struct_names));
}

template <class INPUT>
static unique_ptr<FunctionData> JSONObjectBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() % 2 != 0) {
		throw BinderException("json_object() requires an even number of arguments");
	}
	return JSONCreateBindParams(bound_function, arguments, true);
}

template <class INPUT>
static unique_ptr<FunctionData> JSONArrayBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	return JSONCreateBindParams(bound_function, arguments, false);
}

template <class INPUT>
static unique_ptr<FunctionData> ToJSONBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("to_json() takes exactly one argument");
	}
	return JSONCreateBindParams(bound_function, arguments, false);
}

template <class INPUT>
static unique_ptr<FunctionData> ArrayToJSONBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("array_to_json() takes exactly one argument");
	}
	auto arg_id = arguments[0]->GetReturnType().id();
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (arg_id != LogicalTypeId::LIST && arg_id != LogicalTypeId::ARRAY && arg_id != LogicalTypeId::SQLNULL) {
		throw BinderException("array_to_json() argument type must be LIST or ARRAY");
	}
	return JSONCreateBindParams(bound_function, arguments, false);
}

template <class INPUT>
static unique_ptr<FunctionData> RowToJSONBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 1) {
		throw BinderException("row_to_json() takes exactly one argument");
	}
	auto arg_id = arguments[0]->GetReturnType().id();
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (arguments[0]->GetReturnType().id() != LogicalTypeId::STRUCT && arg_id != LogicalTypeId::SQLNULL) {
		throw BinderException("row_to_json() argument type must be STRUCT");
	}
	return JSONCreateBindParams(bound_function, arguments, false);
}

template <unique_ptr<FunctionData> (*BIND)(ResolveScalarFunctionTypesInput &)>
static void JSONCreateResolveTypes(ResolveScalarFunctionTypesInput &input) {
	BIND(input);
}

static bool BindJSONCopyFormat(ClientContext &context, Expression &argument, const string &name, string &format_string,
                               StrfTimeFormat &format) {
	if (argument.HasParameter()) {
		throw ParameterNotResolvedException();
	}
	if (!argument.IsFoldable()) {
		throw InvalidInputException(argument, "json_copy_to_json %s format must be a constant", name);
	}
	auto value = ExpressionExecutor::EvaluateScalar(context, argument);
	if (value.IsNull()) {
		return false;
	}
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("json_copy_to_json %s format must be VARCHAR or NULL", name);
	}
	format_string = StringValue::Get(value);
	auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException(argument, "Failed to parse format specifier %s: %s", format_string, error);
	}
	return true;
}

static void ParseJSONCopyFormatString(const string &format_string, const string &name, StrfTimeFormat &format) {
	auto error = StrTimeFormat::ParseFormatSpecifier(format_string, format);
	if (!error.empty()) {
		throw InvalidInputException("Failed to parse %s format specifier %s: %s", name, format_string, error);
	}
}

static unique_ptr<Expression> BindJSONCopyTimestampTZFormatter(ClientContext &context, const string &format_string,
                                                               const LogicalType &type) {
	vector<unique_ptr<Expression>> children;
	children.push_back(make_uniq<BoundReferenceExpression>(type, 0));
	children.push_back(make_uniq<BoundConstantExpression>(Value(format_string)));

	FunctionBinder function_binder(context);
	return function_binder.BindScalarFunction(Identifier::DefaultSchema(), "strftime", std::move(children));
}

static unique_ptr<JSONCopyToJSONFunctionData>
CreateJSONCopyToJSONFunctionData(ClientContext &context, StructNames const_struct_names, bool has_date_format,
                                 string date_format_string, bool has_timestamp_format, string timestamp_format_string) {
	StrfTimeFormat date_format;
	StrfTimeFormat timestamp_format;
	if (has_date_format) {
		ParseJSONCopyFormatString(date_format_string, "date", date_format);
	}
	if (has_timestamp_format) {
		ParseJSONCopyFormatString(timestamp_format_string, "timestamp", timestamp_format);
	}

	unique_ptr<Expression> timestamptz_format_expression;
	unique_ptr<Expression> timestamptz_ns_format_expression;
	if (has_timestamp_format) {
		timestamptz_format_expression =
		    BindJSONCopyTimestampTZFormatter(context, timestamp_format_string, LogicalType::TIMESTAMP_TZ);
		timestamptz_ns_format_expression =
		    BindJSONCopyTimestampTZFormatter(context, timestamp_format_string, LogicalType::TIMESTAMP_TZ_NS);
	}

	return make_uniq<JSONCopyToJSONFunctionData>(
	    std::move(const_struct_names), has_date_format, std::move(date_format_string), std::move(date_format),
	    has_timestamp_format, std::move(timestamp_format_string), std::move(timestamp_format),
	    std::move(timestamptz_format_expression), std::move(timestamptz_ns_format_expression));
}

static unique_ptr<JSONCopyToJSONFunctionData> CreateJSONCopyToJSONFunctionData(ClientContext &context,
                                                                               StructNames const_struct_names,
                                                                               Expression &date_argument,
                                                                               Expression &timestamp_argument) {
	StrfTimeFormat date_format;
	StrfTimeFormat timestamp_format;
	string date_format_string;
	string timestamp_format_string;
	auto has_date_format = BindJSONCopyFormat(context, date_argument, "date", date_format_string, date_format);
	auto has_timestamp_format =
	    BindJSONCopyFormat(context, timestamp_argument, "timestamp", timestamp_format_string, timestamp_format);
	return CreateJSONCopyToJSONFunctionData(context, std::move(const_struct_names), has_date_format,
	                                        std::move(date_format_string), has_timestamp_format,
	                                        std::move(timestamp_format_string));
}

static unique_ptr<FunctionData> JSONCopyToJSONBind(BindScalarFunctionInput &) {
	throw BinderException("%s is for internal use only", JSON_COPY_TO_JSON_INTERNAL_NAME);
}

template <class INPUT_TYPE, class RESULT_TYPE>
struct CreateJSONValue {
	static inline RESULT_TYPE Operation(const INPUT_TYPE &input) {
		throw NotImplementedException("Unsupported type for CreateJSONValue");
	}
};

template <class INPUT_TYPE>
struct CreateJSONValue<INPUT_TYPE, bool> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const INPUT_TYPE &input) {
		return yyjson_mut_bool(doc, input);
	}
};

template <class INPUT_TYPE>
struct CreateJSONValue<INPUT_TYPE, uint64_t> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const INPUT_TYPE &input) {
		return yyjson_mut_uint(doc, input);
	}
};

template <class INPUT_TYPE>
struct CreateJSONValue<INPUT_TYPE, int64_t> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const INPUT_TYPE &input) {
		return yyjson_mut_sint(doc, input);
	}
};

template <class INPUT_TYPE>
struct CreateJSONValue<INPUT_TYPE, double> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const INPUT_TYPE &input) {
		return yyjson_mut_real(doc, input);
	}
};

template <>
struct CreateJSONValue<string_t, string_t> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const string_t &input) {
		return yyjson_mut_strncpy(doc, input.GetData(), input.GetSize());
	}
};

template <>
struct CreateJSONValue<hugeint_t, string_t> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const hugeint_t &input) {
		const auto input_string = input.ToString();
		return yyjson_mut_rawncpy(doc, input_string.c_str(), input_string.length());
	}
};

template <>
struct CreateJSONValue<uhugeint_t, string_t> {
	static inline yyjson_mut_val *Operation(yyjson_mut_doc *doc, const uhugeint_t &input) {
		const auto input_string = input.ToString();
		return yyjson_mut_rawncpy(doc, input_string.c_str(), input_string.length());
	}
};

template <class T>
static inline yyjson_mut_val *CreateJSONValueFromJSON(yyjson_mut_doc *doc, const T &value) {
	return nullptr; // This function should only be called with string_t as template
}

template <>
inline yyjson_mut_val *CreateJSONValueFromJSON(yyjson_mut_doc *doc, const string_t &value) {
	auto value_doc = JSONCommon::ReadDocument(value, JSONCommon::READ_FLAG, &doc->alc);
	auto result = yyjson_val_mut_copy(doc, value_doc->root);
	return result;
}

// Forward declaration so we can recurse for nested types
static void CreateValues(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                         idx_t count, const JSONCopyFormatOptions &options = JSONCopyFormatOptions());

static void AddKeyValuePairs(yyjson_mut_doc *doc, yyjson_mut_val *objs[], const Vector &key_v, yyjson_mut_val *vals[],
                             idx_t count) {
	auto keys = key_v.Values<string_t>();
	for (idx_t i = 0; i < count; i++) {
		auto key_entry = keys[i];
		if (!key_entry.IsValid()) {
			throw InvalidInputException("JSON key cannot be NULL");
		}
		auto key = CreateJSONValue<string_t, string_t>::Operation(doc, key_entry.GetValue());
		yyjson_mut_obj_add(objs[i], key, vals[i]);
	}
}

static void CreateKeyValuePairs(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *objs[],
                                yyjson_mut_val *vals[], const Vector &key_v, Vector &value_v, idx_t count,
                                const JSONCopyFormatOptions &options = JSONCopyFormatOptions()) {
	CreateValues(names, doc, vals, value_v, count, options);
	AddKeyValuePairs(doc, objs, key_v, vals, count);
}

static void CreateValuesNull(yyjson_mut_doc *doc, yyjson_mut_val *vals[], idx_t count) {
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_null(doc);
	}
}

template <class INPUT_TYPE, class TARGET_TYPE>
static void TemplatedCreateValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], const Vector &value_v, idx_t count) {
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	auto values = UnifiedVectorFormat::GetData<INPUT_TYPE>(value_data);

	const auto type_is_json = value_v.GetType().IsJSONType();
	for (idx_t i = 0; i < count; i++) {
		idx_t val_idx = value_data.sel->get_index(i);
		if (!value_data.validity.RowIsValid(val_idx)) {
			vals[i] = yyjson_mut_null(doc);
		} else if (type_is_json) {
			vals[i] = CreateJSONValueFromJSON(doc, values[val_idx]);
		} else {
			vals[i] = CreateJSONValue<INPUT_TYPE, TARGET_TYPE>::Operation(doc, values[val_idx]);
		}
		D_ASSERT(vals[i] != nullptr);
	}
}

static void CreateRawValues(yyjson_mut_doc *doc, yyjson_mut_val *vals[], const Vector &value_v, idx_t count) {
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	auto values = UnifiedVectorFormat::GetData<string_t>(value_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t val_idx = value_data.sel->get_index(i);
		if (!value_data.validity.RowIsValid(val_idx)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			const auto &str = values[val_idx];
			vals[i] = yyjson_mut_rawncpy(doc, str.GetData(), str.GetSize());
		}
		D_ASSERT(vals[i] != nullptr);
	}
}

static void CreateValuesStruct(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                               idx_t count, const JSONCopyFormatOptions &options) {
	// Structs become values, therefore we initialize vals to JSON values
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_obj(doc);
	}
	// Initialize re-usable array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);

	// Add the key/value pairs to the values
	auto &entries = StructVector::GetEntries(value_v);
	for (idx_t entry_i = 0; entry_i < entries.size(); entry_i++) {
		Vector struct_key_v(Value(StructType::GetChildName(value_v.GetType(), entry_i).GetIdentifierName()),
		                    count_t(count));
		auto &struct_val_v = entries[entry_i];
		CreateKeyValuePairs(names, doc, vals, nested_vals, struct_key_v, struct_val_v, count, options);
	}
	// Whole struct can be NULL
	UnifiedVectorFormat struct_data;
	value_v.ToUnifiedFormat(struct_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = struct_data.sel->get_index(i);
		if (!struct_data.validity.RowIsValid(idx)) {
			vals[i] = yyjson_mut_null(doc);
		}
	}
}

static void CreateValuesTuple(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                              idx_t count) {
	// a TUPLE becomes a JSON array (matching how Python serializes tuples)
	for (idx_t i = 0; i < count; i++) {
		vals[i] = yyjson_mut_arr(doc);
	}
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	auto &entries = StructVector::GetEntries(value_v);
	for (idx_t entry_i = 0; entry_i < entries.size(); entry_i++) {
		CreateValues(names, doc, nested_vals, entries[entry_i], count);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_arr_append(vals[i], nested_vals[i]);
		}
	}
	// Whole tuple can be NULL
	UnifiedVectorFormat tuple_data;
	value_v.ToUnifiedFormat(tuple_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = tuple_data.sel->get_index(i);
		if (!tuple_data.validity.RowIsValid(idx)) {
			vals[i] = yyjson_mut_null(doc);
		}
	}
}
static void CreateValuesMapKeys(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                const JSONCopyFormatOptions &options);

static void CreateValuesMap(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                            idx_t count, const JSONCopyFormatOptions &options) {
	// Create nested keys
	auto &map_key_v = MapVector::GetKeys(value_v);
	auto map_key_count = ListVector::GetListSize(value_v);
	auto nested_keys = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, map_key_count);
	CreateValuesMapKeys(doc, nested_keys, map_key_v, map_key_count, options);
	// Create nested values
	auto &map_val_v = MapVector::GetValues(value_v);
	auto map_val_count = ListVector::GetListSize(value_v);
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, map_val_count);
	CreateValues(names, doc, nested_vals, map_val_v, map_val_count, options);
	// Add the key/value pairs to the values
	UnifiedVectorFormat map_data;
	value_v.ToUnifiedFormat(map_data);
	auto map_key_list_entries = UnifiedVectorFormat::GetData<list_entry_t>(map_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = map_data.sel->get_index(i);
		if (!map_data.validity.RowIsValid(idx)) {
			// Whole map can be NULL
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = yyjson_mut_obj(doc);
			const auto &key_list_entry = map_key_list_entries[idx];
			for (idx_t child_i = key_list_entry.offset; child_i < key_list_entry.offset + key_list_entry.length;
			     child_i++) {
				if (!unsafe_yyjson_is_null(nested_keys[child_i])) {
					yyjson_mut_obj_add(vals[i], nested_keys[child_i], nested_vals[child_i]);
				}
			}
		}
	}
}

static void CreateValuesUnion(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                              idx_t count, const JSONCopyFormatOptions &options) {
	// Structs become values, therefore we initialize vals to JSON values
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	if (value_data.validity.CannotHaveNull()) {
		for (idx_t i = 0; i < count; i++) {
			vals[i] = yyjson_mut_obj(doc);
		}
	} else {
		for (idx_t i = 0; i < count; i++) {
			auto index = value_data.sel->get_index(i);
			if (!value_data.validity.RowIsValid(index)) {
				// Make the entry NULL if the Union value is NULL
				vals[i] = yyjson_mut_null(doc);
			} else {
				vals[i] = yyjson_mut_obj(doc);
			}
		}
	}

	// Initialize re-usable array for the nested values
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);

	auto &tag_v = UnionVector::GetTags(value_v);
	UnifiedVectorFormat tag_data;
	tag_v.ToUnifiedFormat(tag_data);

	// Add the key/value pairs to the values
	for (idx_t member_idx = 0; member_idx < UnionType::GetMemberCount(value_v.GetType()); member_idx++) {
		auto &member_val_v = UnionVector::GetMember(value_v, member_idx);
		Vector member_key_v(Value(UnionType::GetMemberName(value_v.GetType(), member_idx).GetIdentifierName()),
		                    count_t(count));

		// This implementation is not optimal since we convert the entire member vector,
		// and then skip the rows not matching the tag afterwards.

		CreateValues(names, doc, nested_vals, member_val_v, count, options);

		// This is a inlined copy of AddKeyValuePairs but we also skip null tags
		// and the rows where the member is not matching the tag
		UnifiedVectorFormat key_data;
		member_key_v.ToUnifiedFormat(key_data);
		auto keys = UnifiedVectorFormat::GetData<string_t>(key_data);

		for (idx_t i = 0; i < count; i++) {
			auto value_index = value_data.sel->get_index(i);
			if (!value_data.validity.RowIsValid(value_index)) {
				// This entry is just NULL in it's entirety
				continue;
			}
			auto tag_idx = tag_data.sel->get_index(i);
			if (!tag_data.validity.RowIsValid(tag_idx)) {
				continue;
			}
			auto tag = (UnifiedVectorFormat::GetData<uint8_t>(tag_data))[tag_idx];
			if (tag != member_idx) {
				continue;
			}
			auto key_idx = key_data.sel->get_index(i);
			if (!key_data.validity.RowIsValid(key_idx)) {
				continue;
			}
			auto key = CreateJSONValue<string_t, string_t>::Operation(doc, keys[key_idx]);
			yyjson_mut_obj_add(vals[i], key, nested_vals[i]);
		}
	}
}

static void CreateValuesList(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                             idx_t count, const JSONCopyFormatOptions &options) {
	// Initialize array for the nested values
	auto &child_v = ListVector::GetChildMutable(value_v);
	auto child_count = ListVector::GetListSize(value_v);
	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	// Fill nested_vals with list values
	CreateValues(names, doc, nested_vals, child_v, child_count, options);
	// Now we add the values to the appropriate JSON arrays
	UnifiedVectorFormat list_data;
	value_v.ToUnifiedFormat(list_data);
	auto list_entries = UnifiedVectorFormat::GetData<list_entry_t>(list_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(idx)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = yyjson_mut_arr(doc);
			const auto &entry = list_entries[idx];
			for (idx_t child_i = entry.offset; child_i < entry.offset + entry.length; child_i++) {
				yyjson_mut_arr_append(vals[i], nested_vals[child_i]);
			}
		}
	}
}

static void CreateValuesArray(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                              idx_t count, const JSONCopyFormatOptions &options) {
	value_v.Flatten();

	// Initialize array for the nested values
	auto &child_v = ArrayVector::GetChildMutable(value_v);
	auto array_size = ArrayType::GetSize(value_v.GetType());
	auto child_count = count * array_size;

	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, child_count);
	// Fill nested_vals with list values
	CreateValues(names, doc, nested_vals, child_v, child_count, options);
	// Now we add the values to the appropriate JSON arrays
	UnifiedVectorFormat list_data;
	value_v.ToUnifiedFormat(list_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = list_data.sel->get_index(i);
		if (!list_data.validity.RowIsValid(idx)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = yyjson_mut_arr(doc);
			auto offset = idx * array_size;
			for (idx_t child_i = offset; child_i < offset + array_size; child_i++) {
				yyjson_mut_arr_append(vals[i], nested_vals[child_i]);
			}
		}
	}
}

static void CreateValuesFromDefaultCast(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	Vector string_vector(LogicalTypeId::VARCHAR, count);
	VectorOperations::DefaultCast(value_v, string_vector, count);
	TemplatedCreateValues<string_t, string_t>(doc, vals, string_vector, count);
}

static void CreateValuesGeometry(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count) {
	UnifiedVectorFormat value_data;
	value_v.ToUnifiedFormat(value_data);
	auto values = UnifiedVectorFormat::GetData<string_t>(value_data);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = value_data.sel->get_index(i);
		if (!value_data.validity.RowIsValid(idx)) {
			vals[i] = yyjson_mut_null(doc);
		} else {
			vals[i] = JSONGeometry::ToGeoJSON(values[idx], doc);
		}
	}
}

template <class FILL>
static void CreateValuesFormatted(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                  bool has_format, FILL &&fill_strings) {
	if (!has_format) {
		CreateValuesFromDefaultCast(doc, vals, value_v, count);
		return;
	}
	Vector string_vector(LogicalType::VARCHAR, count);
	fill_strings(string_vector);
	TemplatedCreateValues<string_t, string_t>(doc, vals, string_vector, count);
}

static void CreateValuesDate(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                             const JSONCopyFormatOptions &options) {
	CreateValuesFormatted(doc, vals, value_v, count, bool(options.date_format), [&](Vector &string_vector) {
		options.date_format->ConvertDateVector(value_v, string_vector);
	});
}

static void CreateValuesTimestamp(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                  const JSONCopyFormatOptions &options) {
	if (options.timestamp_format && value_v.GetType().id() != LogicalTypeId::TIMESTAMP) {
		Vector timestamp_vector(LogicalType::TIMESTAMP, count);
		VectorOperations::DefaultCast(value_v, timestamp_vector, count);
		CreateValuesTimestamp(doc, vals, timestamp_vector, count, options);
		return;
	}
	CreateValuesFormatted(doc, vals, value_v, count, bool(options.timestamp_format), [&](Vector &string_vector) {
		options.timestamp_format->ConvertTimestampVector(value_v, string_vector);
	});
}

static void CreateValuesTimestampNS(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                    const JSONCopyFormatOptions &options) {
	CreateValuesFormatted(doc, vals, value_v, count, bool(options.timestamp_format), [&](Vector &string_vector) {
		options.timestamp_format->ConvertTimestampNSVector(value_v, string_vector);
	});
}

static void CreateValuesTimestampTZ(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                    const JSONCopyFormatOptions &options) {
	if (!options.timestamp_format) {
		// no COPY format was given - use the full cast set so that the session time zone is applied
		Vector string_vector(LogicalType::VARCHAR, count);
		if (options.context) {
			VectorOperations::Cast(*options.context, value_v, string_vector, count, true);
		} else {
			VectorOperations::DefaultCast(value_v, string_vector, count);
		}
		TemplatedCreateValues<string_t, string_t>(doc, vals, string_vector, count);
		return;
	}
	CreateValuesFormatted(doc, vals, value_v, count, true, [&](Vector &string_vector) {
		auto format_expression = value_v.GetType().id() == LogicalTypeId::TIMESTAMP_TZ_NS
		                             ? options.timestamptz_ns_format_expression
		                             : options.timestamptz_format_expression;
		if (!options.context || !format_expression) {
			throw InternalException("Missing bound TIMESTAMPTZ formatter for JSON COPY");
		}
		DataChunk input;
		input.InitializeEmpty({value_v.GetType()});
		input.data[0].Reference(value_v);
		input.CheckCardinality(count);
		ExpressionExecutor executor(*options.context, *format_expression);
		executor.ExecuteExpression(input, string_vector);
	});
}

static void CreateValuesMapKeys(yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v, idx_t count,
                                const JSONCopyFormatOptions &options) {
	switch (value_v.GetType().id()) {
	case LogicalTypeId::DATE:
		CreateValuesDate(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		CreateValuesTimestamp(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		CreateValuesTimestampNS(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		CreateValuesTimestampTZ(doc, vals, value_v, count, options);
		break;
	default:
		CreateValuesFromDefaultCast(doc, vals, value_v, count);
		break;
	}
}

static void CreateValues(const StructNames &names, yyjson_mut_doc *doc, yyjson_mut_val *vals[], Vector &value_v,
                         idx_t count, const JSONCopyFormatOptions &options) {
	const auto &type = value_v.GetType();
	switch (type.id()) {
	case LogicalTypeId::SQLNULL:
		CreateValuesNull(doc, vals, count);
		break;
	case LogicalTypeId::BOOLEAN:
		TemplatedCreateValues<bool, bool>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::TINYINT:
		TemplatedCreateValues<int8_t, int64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::SMALLINT:
		TemplatedCreateValues<int16_t, int64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::INTEGER:
		TemplatedCreateValues<int32_t, int64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::BIGINT:
		TemplatedCreateValues<int64_t, int64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::HUGEINT:
		TemplatedCreateValues<hugeint_t, string_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::UHUGEINT:
		TemplatedCreateValues<uhugeint_t, string_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::UTINYINT:
		TemplatedCreateValues<uint8_t, uint64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::USMALLINT:
		TemplatedCreateValues<uint16_t, uint64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::UINTEGER:
		TemplatedCreateValues<uint32_t, uint64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::UBIGINT:
		TemplatedCreateValues<uint64_t, uint64_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::FLOAT:
		TemplatedCreateValues<float, double>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::DOUBLE:
		TemplatedCreateValues<double, double>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::VARCHAR:
		TemplatedCreateValues<string_t, string_t>(doc, vals, value_v, count);
		break;
	case LogicalTypeId::STRUCT:
		CreateValuesStruct(names, doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TUPLE:
		CreateValuesTuple(names, doc, vals, value_v, count);
		break;
	case LogicalTypeId::MAP:
		CreateValuesMap(names, doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::LIST:
		CreateValuesList(names, doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::UNION:
		CreateValuesUnion(names, doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::ARRAY:
		CreateValuesArray(names, doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::DATE:
		CreateValuesDate(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		CreateValuesTimestamp(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP_NS:
		CreateValuesTimestampNS(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::TIMESTAMP_TZ:
	case LogicalTypeId::TIMESTAMP_TZ_NS:
		CreateValuesTimestampTZ(doc, vals, value_v, count, options);
		break;
	case LogicalTypeId::BIT:
	case LogicalTypeId::BLOB:
	case LogicalTypeId::LEGACY_AGGREGATE_STATE:
	case LogicalTypeId::ENUM:
	case LogicalTypeId::INTERVAL:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIME_NS:
	case LogicalTypeId::TIME_TZ:
	case LogicalTypeId::UUID: {
		CreateValuesFromDefaultCast(doc, vals, value_v, count);
		break;
	}
	case LogicalTypeId::GEOMETRY: {
		if (options.geometry_format == JSONGeometryFormat::GEOJSON) {
			CreateValuesGeometry(doc, vals, value_v, count);
		} else {
			CreateValuesFromDefaultCast(doc, vals, value_v, count);
		}
		break;
	}
	case LogicalTypeId::BIGNUM: {
		Vector string_vector(LogicalTypeId::VARCHAR, count);
		VectorOperations::DefaultCast(value_v, string_vector, count);
		CreateRawValues(doc, vals, string_vector, count);
		break;
	}
	case LogicalTypeId::VARIANT: {
		Vector json_vector(LogicalType::JSON(), count);
		VectorOperations::DefaultCast(value_v, json_vector, count);
		TemplatedCreateValues<string_t, string_t>(doc, vals, json_vector, count);
		break;
	}
	case LogicalTypeId::DECIMAL: {
		if (DecimalType::GetWidth(type) > 15) {
			Vector string_vector(LogicalTypeId::VARCHAR, count);
			VectorOperations::DefaultCast(value_v, string_vector, count);
			CreateRawValues(doc, vals, string_vector, count);
		} else {
			Vector double_vector(LogicalType::DOUBLE, count);
			VectorOperations::DefaultCast(value_v, double_vector, count);
			TemplatedCreateValues<double, double>(doc, vals, double_vector, count);
		}
		break;
	}
	case LogicalTypeId::INVALID:
	case LogicalTypeId::UNKNOWN:
	case LogicalTypeId::ANY:
	case LogicalTypeId::TEMPLATE:
	case LogicalTypeId::UNBOUND:
	case LogicalTypeId::TYPE:
	case LogicalTypeId::CHAR:
	case LogicalTypeId::STRING_LITERAL:
	case LogicalTypeId::INTEGER_LITERAL:
	case LogicalTypeId::POINTER:
	case LogicalTypeId::VALIDITY:
	case LogicalTypeId::TABLE:
	case LogicalTypeId::LAMBDA:
		throw InternalException("Unsupported type arrived at JSON create function");
	}
}

static void ObjectFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONCreateFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto options = ClientFormatOptions(state.GetContext());

	// Initialize values
	const idx_t count = args.size();
	auto doc = JSONCommon::CreateDocument(alc);
	auto objs = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		objs[i] = yyjson_mut_obj(doc);
	}
	// Initialize a re-usable value array
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	// Loop through key/value pairs
	for (idx_t pair_idx = 0; pair_idx < args.data.size() / 2; pair_idx++) {
		Vector &key_v = args.data[pair_idx * 2];
		Vector &value_v = args.data[pair_idx * 2 + 1];
		CreateKeyValuePairs(info.const_struct_names, doc, objs, vals, key_v, value_v, count, options);
	}
	// Write JSON values to string
	auto objects = FlatVector::GetDataMutable<string_t>(result);
	for (idx_t i = 0; i < count; i++) {
		objects[i] = JSONCommon::WriteVal<yyjson_mut_val>(objs[i], alc);
	}
	JSONAllocator::AddBuffer(result, alc);
}

static void ArrayFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONCreateFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto options = ClientFormatOptions(state.GetContext());

	// Initialize arrays
	const idx_t count = args.size();
	auto doc = JSONCommon::CreateDocument(alc);
	auto arrs = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		arrs[i] = yyjson_mut_arr(doc);
	}
	// Initialize a re-usable value array
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	// Loop through args
	for (auto &v : args.data) {
		CreateValues(info.const_struct_names, doc, vals, v, count, options);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_arr_append(arrs[i], vals[i]);
		}
	}
	// Write JSON arrays to string
	auto objects = FlatVector::GetDataMutable<string_t>(result);
	for (idx_t i = 0; i < count; i++) {
		objects[i] = JSONCommon::WriteVal<yyjson_mut_val>(arrs[i], alc);
	}
	JSONAllocator::AddBuffer(result, alc);
}

static void ToJSONFunctionInternal(const StructNames &names, Vector &input, const idx_t count, Vector &result,
                                   yyjson_alc *alc, const JSONCopyFormatOptions &options = JSONCopyFormatOptions()) {
	// Initialize array for values
	auto doc = JSONCommon::CreateDocument(alc);
	auto vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	CreateValues(names, doc, vals, input, count, options);

	// Write JSON values to string
	auto objects = FlatVector::GetDataMutable<string_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	UnifiedVectorFormat input_data;
	input.ToUnifiedFormat(input_data);
	for (idx_t i = 0; i < count; i++) {
		idx_t idx = input_data.sel->get_index(i);
		if (input_data.validity.RowIsValid(idx)) {
			objects[i] = JSONCommon::WriteVal<yyjson_mut_val>(vals[i], alc);
		} else {
			result_validity.SetInvalid(i);
		}
	}

	if (input.GetVectorType() == VectorType::CONSTANT_VECTOR || count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	JSONAllocator::AddBuffer(result, alc);
}

static void ToJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	const auto &info = func_expr.BindInfo()->Cast<JSONCreateFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto options = ClientFormatOptions(state.GetContext());

	ToJSONFunctionInternal(info.const_struct_names, args.data[0], args.size(), result, alc, options);
}

static void JSONCopyToJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<JSONCopyToJSONFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto options = info.GetFormatOptions(state.GetContext());

	ToJSONFunctionInternal(info.const_struct_names, args.data[0], args.size(), result, alc, options);
}

//===--------------------------------------------------------------------===//
// COPY ... TO ... (FORMAT GEOJSON)
//===--------------------------------------------------------------------===//
//! Generate a "bbox" from each geometry. GeoJSON orders it as all the minimums followed by all the maximums, and
//! includes Z when the geometry has it. Geometries without a finite extent (NULL or empty) get no bbox at all.
static void AddGeoJSONBoundingBoxes(yyjson_mut_doc *doc, yyjson_mut_val *features[], Vector &geometry_v,
                                    const idx_t count) {
	UnifiedVectorFormat geometry_data;
	geometry_v.ToUnifiedFormat(geometry_data);
	auto geometries = UnifiedVectorFormat::GetData<string_t>(geometry_data);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = geometry_data.sel->get_index(i);
		if (!geometry_data.validity.RowIsValid(idx)) {
			continue;
		}
		auto extent = GeometryExtent::Empty();
		Geometry::GetExtent(geometries[idx], extent);
		if (!extent.HasXY()) {
			continue;
		}
		auto bbox = yyjson_mut_arr(doc);
		const auto has_z = extent.HasZ();
		yyjson_mut_arr_add_real(doc, bbox, extent.x_min);
		yyjson_mut_arr_add_real(doc, bbox, extent.y_min);
		if (has_z) {
			yyjson_mut_arr_add_real(doc, bbox, extent.z_min);
		}
		yyjson_mut_arr_add_real(doc, bbox, extent.x_max);
		yyjson_mut_arr_add_real(doc, bbox, extent.y_max);
		if (has_z) {
			yyjson_mut_arr_add_real(doc, bbox, extent.z_max);
		}
		yyjson_mut_obj_add_val(doc, features[i], "bbox", bbox);
	}
}

//! Turns one row into a GeoJSON Feature: the geometry column becomes "geometry", the ID_COLUMN column becomes
//! "id", and every other column becomes a member of "properties".
static void JSONCopyToGeoJSONFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto &func_expr = state.expr.Cast<BoundFunctionExpression>();
	auto &info = func_expr.BindInfo()->Cast<JSONCopyToGeoJSONFunctionData>();
	auto &lstate = JSONFunctionLocalState::ResetAndGet(state);
	auto alc = lstate.json_allocator->GetYYAlc();
	auto options = info.formats->GetFormatOptions(state.GetContext());
	options.geometry_format = JSONGeometryFormat::GEOJSON;

	const auto count = args.size();
	auto &payload = args.data[0];
	auto &payload_type = payload.GetType();
	auto &entries = StructVector::GetEntries(payload);
	auto &names = info.formats->const_struct_names;

	auto doc = JSONCommon::CreateDocument(alc);
	auto features = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	auto properties = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);
	for (idx_t i = 0; i < count; i++) {
		features[i] = yyjson_mut_obj(doc);
		yyjson_mut_obj_add_str(doc, features[i], "type", "Feature");
		properties[i] = yyjson_mut_obj(doc);
	}

	auto nested_vals = JSONCommon::AllocateArray<yyjson_mut_val *>(doc, count);

	if (info.id_index.IsValid()) {
		CreateValues(names, doc, nested_vals, entries[info.id_index.GetIndex()], count, options);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_val(doc, features[i], "id", nested_vals[i]);
		}
	}

	if (info.geometry_index.IsValid()) {
		auto &geometry_v = entries[info.geometry_index.GetIndex()];
		CreateValues(names, doc, nested_vals, geometry_v, count, options);
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_val(doc, features[i], "geometry", nested_vals[i]);
		}
		if (info.write_bbox) {
			AddGeoJSONBoundingBoxes(doc, features, geometry_v, count);
		}
	} else {
		// A Feature without a geometry is still valid GeoJSON, it is simply unlocated
		for (idx_t i = 0; i < count; i++) {
			yyjson_mut_obj_add_null(doc, features[i], "geometry");
		}
	}

	for (const auto property_index : info.property_indices) {
		Vector key_v(Value(StructType::GetChildName(payload_type, property_index).GetIdentifierName()), count_t(count));
		CreateKeyValuePairs(names, doc, properties, nested_vals, key_v, entries[property_index], count, options);
	}
	for (idx_t i = 0; i < count; i++) {
		yyjson_mut_obj_add_val(doc, features[i], "properties", properties[i]);
	}

	auto objects = FlatVector::GetDataMutable<string_t>(result);
	auto &result_validity = FlatVector::ValidityMutable(result);
	UnifiedVectorFormat payload_data;
	payload.ToUnifiedFormat(payload_data);
	for (idx_t i = 0; i < count; i++) {
		const auto idx = payload_data.sel->get_index(i);
		if (payload_data.validity.RowIsValid(idx)) {
			objects[i] = JSONCommon::WriteVal<yyjson_mut_val>(features[i], alc);
		} else {
			result_validity.SetInvalid(i);
		}
	}
	if (payload.GetVectorType() == VectorType::CONSTANT_VECTOR || count == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	JSONAllocator::AddBuffer(result, alc);
}

//! The column-selecting options are three-state: a NULL argument means the option was not given (use the default
//! behavior), an empty string means it was explicitly unset, and anything else names a column
static bool TryGetGeoJSONColumnOption(ClientContext &context, const Expression &argument, string &result) {
	if (!argument.IsFoldable()) {
		throw BinderException("%s column options must be constants", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	const auto value = ExpressionExecutor::EvaluateScalar(context, argument);
	if (value.IsNull()) {
		return false;
	}
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		throw BinderException("%s column options must be VARCHAR or NULL", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	result = StringValue::Get(value);
	return true;
}

//! Look up a column that the user named in a COPY option
static optional_idx FindGeoJSONColumn(const child_list_t<LogicalType> &child_types, const string &name,
                                      const char *option_name) {
	for (idx_t i = 0; i < child_types.size(); i++) {
		if (child_types[i].first == name) {
			return i;
		}
	}
	vector<string> available;
	for (const auto &child_type : child_types) {
		available.push_back(child_type.first.GetIdentifierName());
	}
	throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option %s refers to column \"%s\", which is not being "
	                      "written. Available columns: %s",
	                      option_name, name, StringUtil::Join(available, ", "));
}

template <class INPUT>
static unique_ptr<FunctionData> JSONCopyToGeoJSONBind(INPUT &input) {
	auto &bound_function = input.GetBoundFunction();
	auto &arguments = input.GetArguments();
	if (arguments.size() != 6) {
		throw BinderException("%s is for internal use only", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	if (arguments[0]->HasParameter()) {
		throw ParameterNotResolvedException();
	}
	auto &payload_type = arguments[0]->GetReturnType();
	if (payload_type.id() != LogicalTypeId::STRUCT) {
		throw BinderException("%s is for internal use only", JSON_COPY_TO_GEOJSON_INTERNAL_NAME);
	}
	auto &child_types = StructType::GetChildTypes(payload_type);

	// GEOMETRY_COLUMN selects the feature geometry, defaulting to the first GEOMETRY column
	optional_idx geometry_index;
	string geometry_column;
	if (TryGetGeoJSONColumnOption(input.GetClientContext(), *arguments[3], geometry_column)) {
		if (!geometry_column.empty()) {
			geometry_index = FindGeoJSONColumn(child_types, geometry_column, "GEOMETRY_COLUMN");
			if (child_types[geometry_index.GetIndex()].second.id() != LogicalTypeId::GEOMETRY) {
				throw BinderException("COPY ... TO ... (FORMAT GEOJSON) option GEOMETRY_COLUMN refers to column "
				                      "\"%s\", which has type %s instead of GEOMETRY",
				                      geometry_column, child_types[geometry_index.GetIndex()].second.ToString());
			}
		}
	} else {
		for (idx_t i = 0; i < child_types.size(); i++) {
			if (child_types[i].second.id() == LogicalTypeId::GEOMETRY) {
				geometry_index = i;
				break;
			}
		}
		if (!geometry_index.IsValid()) {
			throw BinderException(
			    "COPY ... TO ... (FORMAT GEOJSON) requires a GEOMETRY column, but none of the columns "
			    "being written has type GEOMETRY."
			    "\n Use GEOMETRY_COLUMN NULL to write features without a geometry instead.");
		}
	}

	// ID_COLUMN selects the Feature-level "id", defaulting to a column named "id" if there is one, so that a
	// GeoJSON file read by DuckDB round-trips without needing the option
	optional_idx id_index;
	string id_column;
	if (TryGetGeoJSONColumnOption(input.GetClientContext(), *arguments[4], id_column)) {
		if (!id_column.empty()) {
			id_index = FindGeoJSONColumn(child_types, id_column, "ID_COLUMN");
		}
	} else {
		for (idx_t i = 0; i < child_types.size(); i++) {
			if (child_types[i].first == "id") {
				id_index = i;
				break;
			}
		}
	}
	if (geometry_index.IsValid() && id_index.IsValid() && geometry_index.GetIndex() == id_index.GetIndex()) {
		throw BinderException("COPY ... TO ... (FORMAT GEOJSON) cannot write the same column as both the geometry "
		                      "and the Feature id");
	}

	// Everything that is not the geometry or the id becomes a property
	vector<idx_t> property_indices;
	for (idx_t i = 0; i < child_types.size(); i++) {
		if ((geometry_index.IsValid() && geometry_index.GetIndex() == i) ||
		    (id_index.IsValid() && id_index.GetIndex() == i)) {
			continue;
		}
		property_indices.push_back(i);
	}

	// like the column options, a NULL write_bbox means the option was not given
	const auto write_bbox_value = input.GetConstant(5);
	const auto write_bbox = !write_bbox_value.IsNull() && BooleanValue::Get(write_bbox_value);

	StructNames const_struct_names;
	auto &bound_arguments = bound_function.GetArguments();
	bound_arguments[0] = GetJSONType(const_struct_names, payload_type);
	bound_function.SetReturnType(LogicalType::JSON());

	auto formats = CreateJSONCopyToJSONFunctionData(input.GetClientContext(), std::move(const_struct_names),
	                                                *arguments[1], *arguments[2]);
	return make_uniq<JSONCopyToGeoJSONFunctionData>(std::move(formats), geometry_index, id_index,
	                                                std::move(property_indices), write_bbox);
}

static void JSONCopyToJSONSerialize(Serializer &serializer, optional_ptr<FunctionData> bind_data,
                                    const BoundScalarFunction &) {
	if (!bind_data) {
		throw InternalException("%s is missing bind data during serialization", JSON_COPY_TO_JSON_INTERNAL_NAME);
	}
	auto &data = bind_data->Cast<JSONCopyToJSONFunctionData>();
	serializer.WriteProperty(100, "has_date_format", data.has_date_format);
	serializer.WriteProperty(101, "date_format", data.date_format_string);
	serializer.WriteProperty(102, "has_timestamp_format", data.has_timestamp_format);
	serializer.WriteProperty(103, "timestamp_format", data.timestamp_format_string);
}

static unique_ptr<FunctionData> JSONCopyToJSONDeserialize(Deserializer &deserializer, BoundScalarFunction &function) {
	auto has_date_format = deserializer.ReadProperty<bool>(100, "has_date_format");
	auto date_format_string = deserializer.ReadProperty<string>(101, "date_format");
	auto has_timestamp_format = deserializer.ReadProperty<bool>(102, "has_timestamp_format");
	auto timestamp_format_string = deserializer.ReadProperty<string>(103, "timestamp_format");

	auto &arguments = function.GetArguments();
	if (arguments.size() != 3) {
		throw InternalException("%s must have exactly three arguments during deserialization",
		                        JSON_COPY_TO_JSON_INTERNAL_NAME);
	}

	StructNames const_struct_names;
	GetJSONType(const_struct_names, arguments[0]);

	auto &context = deserializer.Get<ClientContext &>();
	return CreateJSONCopyToJSONFunctionData(context, std::move(const_struct_names), has_date_format,
	                                        std::move(date_format_string), has_timestamp_format,
	                                        std::move(timestamp_format_string));
}

unique_ptr<Expression> JSONFunctions::CreateJSONCopyToJSONExpression(ClientContext &context,
                                                                     unique_ptr<Expression> payload,
                                                                     unique_ptr<Expression> date_format,
                                                                     unique_ptr<Expression> timestamp_format) {
	StructNames const_struct_names;
	auto json_type = GetJSONType(const_struct_names, payload->GetReturnType());
	auto bind_data =
	    CreateJSONCopyToJSONFunctionData(context, std::move(const_struct_names), *date_format, *timestamp_format);

	payload = BoundCastExpression::AddCastToType(context, std::move(payload), json_type);

	vector<unique_ptr<Expression>> children;
	children.push_back(std::move(payload));
	children.push_back(std::move(date_format));
	children.push_back(std::move(timestamp_format));

	auto function = GetJSONCopyToJSONFunction();
	BoundScalarFunction bound_function(function);
	auto &arguments = bound_function.GetArguments();
	arguments.clear();
	arguments.push_back(std::move(json_type));
	arguments.push_back(LogicalType::VARCHAR);
	arguments.push_back(LogicalType::VARCHAR);
	bound_function.SetReturnType(LogicalType::JSON());

	return make_uniq<BoundFunctionExpression>(std::move(bound_function), std::move(children), std::move(bind_data));
}

ScalarFunctionSet JSONFunctions::GetObjectFunction() {
	ScalarFunction fun("json_object", {}, LogicalType::JSON(), ObjectFunction, JSONObjectBind<BindScalarFunctionInput>,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<JSONObjectBind<ResolveScalarFunctionTypesInput>>);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// throws if a key is NULL
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetArrayFunction() {
	ScalarFunction fun("json_array", {}, LogicalType::JSON(), ArrayFunction, JSONArrayBind<BindScalarFunctionInput>,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<JSONArrayBind<ResolveScalarFunctionTypesInput>>);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetToJSONFunction() {
	ScalarFunction fun("to_json", {}, LogicalType::JSON(), ToJSONFunction, ToJSONBind<BindScalarFunctionInput>, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<ToJSONBind<ResolveScalarFunctionTypesInput>>);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunction JSONFunctions::GetJSONCopyToJSONFunction() {
	ScalarFunction fun(JSON_COPY_TO_JSON_INTERNAL_NAME, {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::VARCHAR},
	                   LogicalType::JSON(), JSONCopyToJSONFunction, JSONCopyToJSONBind, nullptr,
	                   JSONFunctionLocalState::Init);
	fun.GetSignature().GetParameter(0).SetName("value");
	fun.GetSignature().GetParameter(1).SetName("date_format");
	fun.GetSignature().GetParameter(2).SetName("timestamp_format");
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	// formatting dates and timestamps can fail
	fun.SetFallible();
	fun.SetSerializeCallback(JSONCopyToJSONSerialize);
	fun.SetDeserializeCallback(JSONCopyToJSONDeserialize);
	return fun;
}

ScalarFunction JSONFunctions::GetJSONCopyToGeoJSONFunction() {
	ScalarFunction fun(JSON_COPY_TO_GEOJSON_INTERNAL_NAME,
	                   {LogicalType::ANY, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                    LogicalType::VARCHAR, LogicalType::BOOLEAN},
	                   LogicalType::JSON(), JSONCopyToGeoJSONFunction, JSONCopyToGeoJSONBind<BindScalarFunctionInput>,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().GetParameter(0).SetName("value");
	fun.GetSignature().GetParameter(1).SetName("date_format");
	fun.GetSignature().GetParameter(2).SetName("timestamp_format");
	fun.GetSignature().GetParameter(3).SetName("geometry_column");
	fun.GetSignature().GetParameter(4).SetName("id_column");
	fun.GetSignature().GetParameter(5).SetName("write_bbox");
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<JSONCopyToGeoJSONBind<ResolveScalarFunctionTypesInput>>);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	fun.SetFallible();
	return fun;
}

ScalarFunctionSet JSONFunctions::GetArrayToJSONFunction() {
	ScalarFunction fun("array_to_json", {}, LogicalType::JSON(), ToJSONFunction,
	                   ArrayToJSONBind<BindScalarFunctionInput>, nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<ArrayToJSONBind<ResolveScalarFunctionTypesInput>>);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

ScalarFunctionSet JSONFunctions::GetRowToJSONFunction() {
	ScalarFunction fun("row_to_json", {}, LogicalType::JSON(), ToJSONFunction, RowToJSONBind<BindScalarFunctionInput>,
	                   nullptr, JSONFunctionLocalState::Init);
	fun.GetSignature().AddArgs("args", LogicalType::ANY);
	fun.SetResolveTypesCallback(JSONCreateResolveTypes<RowToJSONBind<ResolveScalarFunctionTypesInput>>);
	fun.SetFallible();
	return ScalarFunctionSet(fun);
}

struct NestedToJSONCastData : public BoundCastData {
public:
	explicit NestedToJSONCastData(optional_ptr<ClientContext> client) : client(client) {
	}

	unique_ptr<BoundCastData> Copy() const override {
		auto result = make_uniq<NestedToJSONCastData>(client);
		result->const_struct_names = const_struct_names.Copy();
		return std::move(result);
	}

public:
	optional_ptr<ClientContext> client;
	StructNames const_struct_names;
};

static bool AnyToJSONCast(Vector &source, Vector &result, idx_t count, CastParameters &parameters) {
	auto &lstate = parameters.local_state->Cast<JSONFunctionLocalState>();
	lstate.json_allocator->Reset();
	auto alc = lstate.json_allocator->GetYYAlc();
	auto &cast_data = parameters.cast_data->Cast<NestedToJSONCastData>();
	const auto &names = cast_data.const_struct_names;

	JSONCopyFormatOptions options;
	options.context = cast_data.client;
	if (cast_data.client) {
		options.geometry_format = ClientGeometryFormat(*cast_data.client);
	}
	ToJSONFunctionInternal(names, source, count, result, alc, options);
	return true;
}

static BoundCastInfo AnyToJSONCastBind(BindCastInput &input, const LogicalType &source, const LogicalType &target) {
	auto cast_data = make_uniq<NestedToJSONCastData>(input.context);
	GetJSONType(cast_data->const_struct_names, source);
	return BoundCastInfo(AnyToJSONCast, std::move(cast_data), JSONFunctionLocalState::InitCastLocalState);
}

void JSONFunctions::RegisterJSONCreateCastFunctions(ExtensionLoader &loader) {
	// Anything can be cast to JSON
	for (const auto &type : LogicalType::AllTypes()) {
		LogicalType source_type;
		switch (type.id()) {
		case LogicalTypeId::STRUCT:
			source_type = LogicalType::STRUCT({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::TUPLE:
			source_type = LogicalType::TUPLE({LogicalType::ANY});
			break;
		case LogicalTypeId::LIST:
			source_type = LogicalType::LIST(LogicalType::ANY);
			break;
		case LogicalTypeId::MAP:
			source_type = LogicalType::MAP(LogicalType::ANY, LogicalType::ANY);
			break;
		case LogicalTypeId::UNION:
			source_type = LogicalType::UNION({{"any", LogicalType::ANY}});
			break;
		case LogicalTypeId::ARRAY:
			source_type = LogicalType::ARRAY(LogicalType::ANY, optional_idx());
			break;
		case LogicalTypeId::VARCHAR:
			// We skip this one here as it's handled in json_functions.cpp
			continue;
		default:
			source_type = type;
		}
		// We prefer going to JSON over going to VARCHAR if a function can do either
		const auto source_to_json_cost = MaxValue<int64_t>(
		    CastFunctionSet::ImplicitCastCost(loader.GetDatabaseInstance(), source_type, LogicalType::VARCHAR) - 1, 0);
		loader.RegisterCastFunction(source_type, LogicalType::JSON(), AnyToJSONCastBind, source_to_json_cost);
	}
}

} // namespace duckdb
