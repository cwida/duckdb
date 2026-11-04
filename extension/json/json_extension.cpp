#include "json_extension.hpp"

#include "json_common.hpp"
#include "json_functions.hpp"

#include "duckdb/catalog/catalog_entry/macro_catalog_entry.hpp"
#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/function/copy_function.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

static const DefaultMacro JSON_MACROS[] = {
    {DEFAULT_SCHEMA, "json_group_array",
     "(x) AS CAST('[' || string_agg(CASE WHEN x IS NULL THEN 'null'::JSON ELSE to_json(x) END, ',') || ']' AS JSON)"},
    {DEFAULT_SCHEMA, "json_group_object",
     "(n, v) AS CAST('{' || string_agg(CASE WHEN n IS NULL THEN error('json_group_object key cannot be NULL') ELSE "
     "to_json(n::VARCHAR) END || ':' || CASE WHEN v IS NULL THEN 'null'::JSON ELSE to_json(v) END, ',') || '}' AS "
     "JSON)"},
    {DEFAULT_SCHEMA, "json_group_structure", "(x) AS json_structure(json_group_array(x))->0"},
    {DEFAULT_SCHEMA, "json", "(x) AS json_extract(x, '$')"},
    {DEFAULT_SCHEMA, "json_copy_strftime_if_date", "(x, format) AS x, (x DATE, format) AS strftime(x, format);"},
    {DEFAULT_SCHEMA, "json_copy_strftime_if_timestamp",
     "(x, format) AS x, (x TIMESTAMP, format) AS strftime(x, format), "
     "(x TIMESTAMP_NS, format) AS strftime(x, format), "
     "(x TIMESTAMPTZ, format) AS strftime(x, format), "
     "(x TIMESTAMPTZ_NS, format) AS strftime(x, format);"},
    {nullptr, nullptr, nullptr}};

static void LoadInternal(ExtensionLoader &loader) {
	// JSON settings
	auto &config = DBConfig::GetConfig(loader.GetDatabaseInstance());
	config.AddExtensionOption(
	    "json_geometry_format",
	    "How GEOMETRY values are written to JSON: 'wkt' for Well-Known Text, or 'geojson' for GeoJSON geometry "
	    "objects. COPY ... TO ... (FORMAT GEOJSON) always writes GeoJSON regardless of this setting.",
	    LogicalType::VARCHAR, Value("wkt"), JSONFunctions::ValidateGeometryFormat);

	// JSON type
	auto json_type = LogicalType::JSON();
	loader.RegisterType(LogicalType::JSON_TYPE_NAME, std::move(json_type));

	// JSON casts
	JSONFunctions::RegisterSimpleCastFunctions(loader);
	JSONFunctions::RegisterJSONCreateCastFunctions(loader);
	JSONFunctions::RegisterJSONTransformCastFunctions(loader);

	// JSON scalar functions
	for (auto &fun : JSONFunctions::GetScalarFunctions()) {
		loader.RegisterFunction(fun);
	}

	// JSON table functions
	for (auto &fun : JSONFunctions::GetTableFunctions()) {
		loader.RegisterFunction(fun);
	}

	// JSON pragma functions
	for (auto &fun : JSONFunctions::GetPragmaFunctions()) {
		loader.RegisterFunction(fun);
	}

	// JSON replacement scan
	DBConfig::GetConfig(loader.GetDatabaseInstance())
	    .replacement_scans.emplace_back(JSONFunctions::ReadJSONReplacement);

	// JSON copy function
	auto copy_fun = JSONFunctions::GetJSONCopyFunction();
	loader.RegisterFunction(copy_fun);
	copy_fun.extension = "ndjson";
	copy_fun.SetName("ndjson");
	loader.RegisterFunction(copy_fun);
	copy_fun.extension = "jsonl";
	copy_fun.SetName("jsonl");
	loader.RegisterFunction(copy_fun);

	// GeoJSON copy function
	auto geojson_copy_fun = JSONFunctions::GetGeoJSONCopyFunction();
	loader.RegisterFunction(geojson_copy_fun);
	geojson_copy_fun.extension = "geojsonl";
	geojson_copy_fun.SetName("geojsonl");
	loader.RegisterFunction(geojson_copy_fun);

	for (idx_t index = 0; JSON_MACROS[index].name != nullptr; index++) {
		auto info = DefaultFunctionGenerator::CreateInternalMacroInfo(JSON_MACROS[index]);
		loader.RegisterFunction(*info);
	}
}

// LCOV_EXCL_START
void JsonExtension::Load(ExtensionLoader &loader) {
	LoadInternal(loader);
}

std::string JsonExtension::Name() {
	return "json";
}

std::string JsonExtension::Version() const {
#ifdef EXT_VERSION_JSON
	return EXT_VERSION_JSON;
#else
	return "";
#endif
}
// LCOV_EXCL_STOP

} // namespace duckdb

extern "C" {

DUCKDB_CPP_EXTENSION_ENTRY(json, loader) {
	duckdb::LoadInternal(loader);
}
}
