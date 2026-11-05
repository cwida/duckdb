#include "duckdb/function/copy_function.hpp"
#include "duckdb/common/bind_helpers.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/constant_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/parser/expression/operator_expression.hpp"
#include "duckdb/parser/expression/star_expression.hpp"
#include "duckdb/parser/query_node/select_node.hpp"
#include "duckdb/parser/tableref/subqueryref.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/operator/logical_copy_to_file.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"
#include "duckdb/common/helper.hpp"
#include "json_functions.hpp"
#include "json_scan.hpp"
#include "json_transform.hpp"
#include "duckdb/common/multi_file/table_function_multi_file.hpp"
#include "duckdb/parser/expression/cast_expression.hpp"
#include "duckdb/main/query_result.hpp"

namespace duckdb {

//! JSON and GeoJSON share the same COPY implementation, differing only in the per-row payload expression and in
//! how the file is wrapped (a JSON array vs a GeoJSON FeatureCollection)
enum class JSONCopyToFormat { JSON, GEOJSON };

static const char *GetJSONCopyFormatName(const JSONCopyToFormat format) {
	return format == JSONCopyToFormat::GEOJSON ? "GEOJSON" : "JSON";
}

static void ThrowJSONCopyParameterException(const JSONCopyToFormat format, const Identifier &option_name) {
	throw BinderException("COPY (FORMAT %s) parameter %s expects a single argument.", GetJSONCopyFormatName(format),
	                      option_name);
}

static void ThrowJSONCopyNullException(const JSONCopyToFormat format, const Identifier &option_name) {
	throw BinderException("COPY (FORMAT %s) parameter %s cannot be NULL.", GetJSONCopyFormatName(format), option_name);
}

static void ThrowJSONCopyTypeException(const JSONCopyToFormat format, const Identifier &option_name, const Value &value,
                                       const string &expected_type) {
	throw BinderException("COPY (FORMAT %s) parameter %s expects a %s argument, but got %s.",
	                      GetJSONCopyFormatName(format), option_name, expected_type, value.type());
}

static const Value &GetSingleJSONCopyValue(const JSONCopyToFormat format, const Identifier &option_name,
                                           const vector<Value> &values) {
	if (values.size() != 1) {
		ThrowJSONCopyParameterException(format, option_name);
	}
	if (values.back().IsNull()) {
		ThrowJSONCopyNullException(format, option_name);
	}
	return values.back();
}

static string GetSingleJSONCopyString(const JSONCopyToFormat format, const Identifier &option_name,
                                      const vector<Value> &values) {
	auto &value = GetSingleJSONCopyValue(format, option_name, values);
	if (value.type().id() != LogicalTypeId::VARCHAR) {
		ThrowJSONCopyTypeException(format, option_name, value, "VARCHAR");
	}
	return StringValue::Get(value);
}

static bool GetJSONCopyBoolean(Binder &binder, const JSONCopyToFormat format, const Identifier &option_name,
                               const vector<Value> &values) {
	if (values.size() > 1) {
		throw InvalidInputException("Copy option %s did not expect a list as argument", option_name);
	}
	if (values.empty()) {
		return true;
	}
	if (values.back().IsNull()) {
		ThrowJSONCopyNullException(format, option_name);
	}
	return values.back().CastAs(binder.context, LogicalType::BOOLEAN).GetValue<bool>();
}

static unique_ptr<ParsedExpression> JSONCopyFormatExpression(unique_ptr<ParsedExpression> expr,
                                                             const Identifier &function_name, const string &format) {
	vector<unique_ptr<ParsedExpression>> args;
	args.push_back(std::move(expr));
	args.push_back(ConstantExpression::String(format));
	return make_uniq<FunctionExpression>(function_name, std::move(args));
}

static unique_ptr<ParsedExpression> JSONCopyPartitionColumnExpression(const string &partition_column,
                                                                      const string &date_format,
                                                                      const string &timestamp_format) {
	unique_ptr<ParsedExpression> result = make_uniq<ColumnRefExpression>(Identifier(partition_column));
	if (!date_format.empty()) {
		result = JSONCopyFormatExpression(std::move(result), "json_copy_strftime_if_date", date_format);
	}
	if (!timestamp_format.empty()) {
		result = JSONCopyFormatExpression(std::move(result), "json_copy_strftime_if_timestamp", timestamp_format);
	}
	result->SetAlias(Identifier(partition_column));
	return result;
}

static unique_ptr<Expression> JSONCopyBoundFormatConstant(const string &format) {
	if (format.empty()) {
		return make_uniq<BoundConstantExpression>(Value(LogicalType::VARCHAR));
	}
	return make_uniq<BoundConstantExpression>(Value(format));
}

//! Sets the names and types of the copied query for RETURN_COLUMN_TYPES. The binder described the rewrite, which writes
//! one JSON column, so they are replaced here: the types are the output of the query below the rewrite's projection,
//! and the names are the fields of its struct_pack, or the query's names when partitioning already looked them up.
static void SetJSONCopyQueryColumns(LogicalCopyToFile &copy, LogicalProjection &projection,
                                    const vector<Identifier> &original_column_names) {
	// The rewrite binds the query as the only child of its projection
	D_ASSERT(projection.children.size() == 1);
	auto &query = *projection.children[0];
	query.ResolveOperatorTypes();
	auto query_types = query.types;
	vector<Identifier> query_names;
	if (!original_column_names.empty()) {
		// With PARTITION_BY, the partition columns can be left out of the struct_pack
		query_names = original_column_names;
	} else {
		// The payload is to_json, or the GeoJSON writer, with the struct_pack of every column as its first argument
		auto &payload = projection.expressions[0]->Cast<BoundFunctionExpression>();
		D_ASSERT(!payload.GetChildren().empty());
		// Binding the payload can cast the struct_pack, e.g., to turn a VARIANT field into JSON
		const Expression *struct_pack = payload.GetChildren()[0].get();
		while (BoundCastExpression::IsCast(*struct_pack)) {
			struct_pack = &BoundCastExpression::Child(struct_pack->Cast<BoundFunctionExpression>());
		}
		auto &struct_pack_function = struct_pack->Cast<BoundFunctionExpression>();
		D_ASSERT(struct_pack_function.Function().GetName() == "struct_pack");
		for (auto &field : struct_pack_function.GetChildren()) {
			query_names.push_back(field->GetAlias());
		}
	}
	// Every column of the query is named, by the struct_pack or by the partition lookup
	D_ASSERT(query_names.size() == query_types.size());
	QueryResult::DeduplicateColumns(query_names);
	copy.query_names = std::move(query_names);
	copy.query_types = std::move(query_types);
}

static void BindJSONCopyToJSONFunction(Binder &binder, BoundStatement &bound, const string &date_format,
                                       const string &timestamp_format, bool is_geojson,
                                       const vector<Identifier> &original_column_names) {
	// Unlike to_json, the GeoJSON writer takes the formats as regular arguments, so it needs no post-bind fixup
	const auto fix_up_formats = !is_geojson && !(date_format.empty() && timestamp_format.empty());
	const auto set_column_types = bound.plan && bound.plan->type == LogicalOperatorType::LOGICAL_COPY_TO_FILE &&
	                              CopyFunctionReturnsColumnTypes(bound.plan->Cast<LogicalCopyToFile>().return_type);
	if (!fix_up_formats && !set_column_types) {
		return;
	}
	if (!bound.plan || bound.plan->type != LogicalOperatorType::LOGICAL_COPY_TO_FILE) {
		throw InternalException("Expected JSON COPY rewrite to bind to a LogicalCopyToFile");
	}
	auto &copy = bound.plan->Cast<LogicalCopyToFile>();
	if (copy.children.empty() || copy.children[0]->type != LogicalOperatorType::LOGICAL_PROJECTION) {
		throw InternalException("Expected JSON COPY rewrite to bind a top-level projection");
	}
	auto &projection = copy.children[0]->Cast<LogicalProjection>();
	if (projection.expressions.empty()) {
		throw InternalException("Expected JSON COPY rewrite projection to contain a JSON payload expression");
	}
	if (set_column_types) {
		SetJSONCopyQueryColumns(copy, projection, original_column_names);
	}
	if (!fix_up_formats) {
		return;
	}
	auto to_json = std::move(projection.expressions[0]);
	if (to_json->GetExpressionClass() != ExpressionClass::BOUND_FUNCTION) {
		throw InternalException("Expected JSON COPY payload expression to be a bound function");
	}
	auto &to_json_function = to_json->Cast<BoundFunctionExpression>();
	if (to_json_function.Function().GetName() != "to_json" || to_json_function.GetChildren().size() != 1) {
		throw InternalException("Expected JSON COPY payload expression to be to_json with one argument");
	}

	auto alias = to_json->GetAlias();
	auto &to_json_children = to_json_function.GetChildrenMutable();
	auto replacement = JSONFunctions::CreateJSONCopyToJSONExpression(binder.context, std::move(to_json_children[0]),
	                                                                 JSONCopyBoundFormatConstant(date_format),
	                                                                 JSONCopyBoundFormatConstant(timestamp_format));
	replacement->SetAlias(std::move(alias));
	projection.expressions[0] = std::move(replacement);
	projection.ResolveOperatorTypes();
}

static BoundStatement CopyToJSONPlanInternal(Binder &binder, CopyStatement &stmt, const JSONCopyToFormat format) {
	static const identifier_set_t SUPPORTED_BASE_OPTIONS {
	    "compression",      "encoding",          "use_tmp_file",       "overwrite_or_ignore", "overwrite",
	    "append",           "filename_pattern",  "file_extension",     "per_thread_output",   "file_size_bytes",
	    "partition_by",     "return_files",      "preserve_order",     "return_stats",        "write_partition_columns",
	    "write_empty_file", "hive_file_pattern", "return_column_types"};

	auto &copy_info = *stmt.info;

	// Parse the options, creating options for the CSV writer while doing so
	string date_format;
	string timestamp_format;
	// The column-selecting GeoJSON options are three-state: unset means "use the default", NULL means "explicitly
	// none", and anything else names a column
	unique_ptr<ParsedExpression> geometry_column = ConstantExpression::FromValue(Value(LogicalType::VARCHAR));
	unique_ptr<ParsedExpression> id_column = ConstantExpression::FromValue(Value(LogicalType::VARCHAR));
	bool write_bbox = false;
	// Partition columns are kept as separate columns (instead of being packed into the JSON object), so that the
	// COPY writer can partition on them. By default they are excluded from the written JSON, matching the behavior
	// of the other formats. WRITE_PARTITION_COLUMNS keeps them inside the JSON object instead.
	vector<string> partition_columns;
	bool write_partition_columns = false;
	vector<Identifier> original_column_names;
	const auto is_geojson = format == JSONCopyToFormat::GEOJSON;
	const auto is_geojsonl = is_geojson && StringUtil::CIEquals(stmt.info->format, "geojsonl");
	// GeoJSON is wrapped in a FeatureCollection by default, but newline-delimited GeoJSON (geojsonl) and plain
	// JSON are not wrapped unless ARRAY is requested
	bool array_output = is_geojson && !is_geojsonl;
	// We insert the file extension here so it works properly with PER_THREAD_OUTPUT/FILE_SIZE_BYTES etc.
	identifier_map_t<vector<Value>> csv_copy_options {
	    {"file_extension", {is_geojsonl ? "geojsonl" : (is_geojson ? "geojson" : "json")}}};
	for (const auto &kv : copy_info.options) {
		auto &option_name = kv.first;
		auto &option_values = kv.second;
		if (option_name == "dateformat" || option_name == "date_format") {
			date_format = GetSingleJSONCopyString(format, option_name, option_values);
		} else if (option_name == "timestampformat" || option_name == "timestamp_format") {
			timestamp_format = GetSingleJSONCopyString(format, option_name, option_values);
		} else if (option_name == "array") {
			if (option_values.size() > 1) {
				ThrowJSONCopyParameterException(format, option_name);
			}
			if (!option_values.empty() && option_values.back().IsNull()) {
				ThrowJSONCopyNullException(format, option_name);
			}
			array_output =
			    option_values.empty() || BooleanValue::Get(option_values.back().DefaultCastAs(LogicalTypeId::BOOLEAN));
		} else if (is_geojson && (option_name == "geometry_column" || option_name == "id_column")) {
			// NULL explicitly unsets these, so unlike the other options a NULL value is meaningful here
			if (option_values.size() != 1) {
				ThrowJSONCopyParameterException(format, option_name);
			}
			auto &value = option_values.back();
			if (!value.IsNull() && value.type().id() != LogicalTypeId::VARCHAR) {
				ThrowJSONCopyTypeException(format, option_name, value, "VARCHAR");
			}
			// An empty string marks "explicitly none", which a column name can never be
			auto column = value.IsNull() ? Value("") : value;
			auto &target = option_name == "geometry_column" ? geometry_column : id_column;
			target = ConstantExpression::FromValue(column);
		} else if (is_geojson && option_name == "bbox") {
			write_bbox = GetJSONCopyBoolean(binder, format, option_name, option_values);
		} else if (option_name == "file_extension") {
			// Since we set the file extension above, we need to override it
			csv_copy_options["file_extension"] = {GetSingleJSONCopyString(format, option_name, option_values)};
		} else if (option_name == "partition_by") {
			for (const auto &val : option_values) {
				if (val.IsNull()) {
					ThrowJSONCopyNullException(format, option_name);
				}
			}
			if (original_column_names.empty()) {
				auto node_copy = copy_info.select_statement->Copy();
				auto child_binder = Binder::CreateBinder(binder.context, &binder);
				auto bound = child_binder->Bind(*node_copy);
				original_column_names = bound.names;
			}
			auto converted = ConvertVectorToValue(vector<Value>(option_values));
			auto partition_indices = ParseColumnsOrdered(converted, original_column_names, option_name);
			for (auto &partition_index : partition_indices) {
				partition_columns.emplace_back(original_column_names[partition_index]);
			}
			vector<Value> csv_partition_columns;
			for (const auto &partition_column : partition_columns) {
				csv_partition_columns.push_back(partition_column);
			}
			csv_copy_options["partition_by"] = std::move(csv_partition_columns);
		} else if (option_name == "write_partition_columns") {
			// Handled below by keeping the partition columns inside the JSON object. We do not forward this to the
			// CSV writer, as that would write the (separate) partition columns as their own JSON lines.
			write_partition_columns = GetJSONCopyBoolean(binder, format, option_name, option_values);
		} else if (SUPPORTED_BASE_OPTIONS.find(option_name) != SUPPORTED_BASE_OPTIONS.end()) {
			if (!option_values.empty() && option_values.back().IsNull()) {
				ThrowJSONCopyNullException(format, option_name);
			}
			// We support these base options
			csv_copy_options.insert(kv);
		} else {
			throw BinderException("Unknown option for COPY ... TO ... (FORMAT %s): %s.", GetJSONCopyFormatName(format),
			                      SQLIdentifier(option_name));
		}
	}
	if (array_output) {
		csv_copy_options["prefix"] = {is_geojson ? "{\"type\":\"FeatureCollection\",\"features\":[\n\t" : "[\n\t"};
		csv_copy_options["suffix"] = {is_geojson ? "\n]}\n" : "\n]\n"};
		csv_copy_options["new_line"] = {",\n\t"};
	}
	if (!write_partition_columns && !partition_columns.empty() &&
	    partition_columns.size() == original_column_names.size()) {
		throw NotImplementedException("No column to write as all columns are specified as partition columns. "
		                              "WRITE_PARTITION_COLUMNS option can be used to write partition columns.");
	}

	// Run the following query to convert everything into a single JSON column, then invoke the CSV writer
	// SELECT TO_JSON(STRUCT_PACK(*COLUMNS(*))) FROM <source>

	auto inner_select_stmt = make_uniq<SelectStatement>();
	inner_select_stmt->node = std::move(copy_info.select_statement);

	auto source_ref = make_uniq<SubqueryRef>(std::move(inner_select_stmt));

	// Build outer: SELECT TO_JSON(STRUCT_PACK(*COLUMNS(*))) FROM <source_ref>
	copy_info.select_statement = make_uniq_base<QueryNode, SelectNode>();
	auto &select_node = copy_info.select_statement->Cast<SelectNode>();
	select_node.from_table = std::move(source_ref);

	auto columns_star = make_uniq<StarExpression>();
	columns_star->IsColumnsMutable() = true;
	if (!write_partition_columns) {
		// Exclude the partition columns from the JSON object - they are kept as separate columns below
		for (const auto &partition_column : partition_columns) {
			columns_star->ExcludeListMutable().insert(QualifiedColumnName(Identifier(partition_column)));
		}
	}
	auto unpack = make_uniq<OperatorExpression>(ExpressionType::OPERATOR_UNPACK);
	unpack->GetChildrenMutable().push_back(std::move(columns_star));

	vector<unique_ptr<ParsedExpression>> struct_pack_args;
	struct_pack_args.push_back(std::move(unpack));
	auto struct_pack = make_uniq<FunctionExpression>("struct_pack", std::move(struct_pack_args));

	if (is_geojson) {
		vector<unique_ptr<ParsedExpression>> geojson_args;
		geojson_args.push_back(std::move(struct_pack));
		geojson_args.push_back(
		    ConstantExpression::FromValue(date_format.empty() ? Value(LogicalType::VARCHAR) : Value(date_format)));
		geojson_args.push_back(ConstantExpression::FromValue(timestamp_format.empty() ? Value(LogicalType::VARCHAR)
		                                                                              : Value(timestamp_format)));
		geojson_args.push_back(std::move(geometry_column));
		geojson_args.push_back(std::move(id_column));
		geojson_args.push_back(ConstantExpression::Boolean(write_bbox));
		select_node.select_list.push_back(
		    make_uniq<FunctionExpression>("__internal_json_copy_to_geojson", std::move(geojson_args)));
	} else {
		vector<unique_ptr<ParsedExpression>> to_json_args;
		to_json_args.push_back(std::move(struct_pack));
		select_node.select_list.push_back(make_uniq<FunctionExpression>("to_json", std::move(to_json_args)));
	}

	// Keep the partition columns as separate columns so the COPY writer can partition on them. The writer routes rows
	// into the right files based on these columns but does not write them to disk (WRITE_PARTITION_COLUMNS is handled
	// above by keeping the columns inside the JSON object instead).
	for (const auto &partition_column : partition_columns) {
		select_node.select_list.push_back(
		    JSONCopyPartitionColumnExpression(partition_column, date_format, timestamp_format));
	}

	// Now we can just use the CSV writer
	copy_info.format = "csv";
	copy_info.options = std::move(csv_copy_options);
	copy_info.options["quote"] = {""};
	copy_info.options["escape"] = {""};
	copy_info.options["delimiter"] = {"\n"};
	copy_info.options["header"] = {{0}};

	auto result = binder.Bind(stmt);
	BindJSONCopyToJSONFunction(binder, result, date_format, timestamp_format, is_geojson, original_column_names);
	return result;
}

static BoundStatement CopyToJSONPlan(Binder &binder, CopyStatement &stmt) {
	return CopyToJSONPlanInternal(binder, stmt, JSONCopyToFormat::JSON);
}

static BoundStatement CopyToGeoJSONPlan(Binder &binder, CopyStatement &stmt) {
	return CopyToJSONPlanInternal(binder, stmt, JSONCopyToFormat::GEOJSON);
}

//! COPY ... FROM reads through the multi-file wrapper around read_single_json_file. COPY takes its columns from
//! the target table, so the reader binds every file against those columns instead of auto-detecting a schema
static void SetJSONCopyFromFunction(CopyFunction &function, Identifier name) {
	function.copy_from_bind = TableFunctionMultiFileWrapper::MultiFileBindCopy;
	function.copy_from_function = JSONFunctions::GetJSONTableFunction(
	    std::move(name), make_shared_ptr<JSONScanInfo>(JSONScanType::READ_JSON, JSONFormat::AUTO_DETECT,
	                                                   JSONRecordType::RECORDS, false));
}

CopyFunction JSONFunctions::GetGeoJSONCopyFunction() {
	CopyFunction function("geojson");
	function.extension = "geojson";

	function.plan = CopyToGeoJSONPlan;

	SetJSONCopyFromFunction(function, "read_json");

	return function;
}

CopyFunction JSONFunctions::GetJSONCopyFunction() {
	CopyFunction function("json");
	function.extension = "json";

	function.plan = CopyToJSONPlan;
	SetJSONCopyFromFunction(function, "read_json");

	return function;
}

} // namespace duckdb
