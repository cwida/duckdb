#include "duckdb/catalog/default/default_table_functions.hpp"
#include "duckdb/catalog/default/default_macro_qualifier.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/catalog/catalog_entry/table_macro_catalog_entry.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/function/table_macro_function.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

// clang-format off
static const DefaultTableMacro internal_table_macros[] = {
	{DEFAULT_SCHEMA, "histogram_values", {"source", "col_name", nullptr}, {{"bin_count", "10"}, {"technique", "'auto'"}, {nullptr, nullptr}},  R"(
WITH bins AS (
   SELECT
      CASE
      WHEN (NOT (can_cast_implicitly(MIN(col_name), NULL::BIGINT) OR
            can_cast_implicitly(MIN(col_name), NULL::DOUBLE) OR
            can_cast_implicitly(MIN(col_name), NULL::TIMESTAMP)) AND technique='auto')
            OR technique='sample'
      THEN
         approx_top_k(col_name, bin_count)
      WHEN technique='equi-height'
      THEN
         quantile(col_name, [x / bin_count::DOUBLE for x in generate_series(1, bin_count)])
      WHEN technique='equi-width'
      THEN
         equi_width_bins(MIN(col_name), MAX(col_name), bin_count, false)
      WHEN technique='equi-width-nice' OR technique='auto'
      THEN
         equi_width_bins(MIN(col_name), MAX(col_name), bin_count, true)
      ELSE
         error(concat('Unrecognized technique ', technique))
      END AS bins
   FROM query_table(source::VARCHAR)
   )
SELECT UNNEST(map_keys(histogram)) AS bin, UNNEST(map_values(histogram)) AS count
FROM (
   SELECT CASE
      WHEN (NOT (can_cast_implicitly(MIN(col_name), NULL::BIGINT) OR
            can_cast_implicitly(MIN(col_name), NULL::DOUBLE) OR
            can_cast_implicitly(MIN(col_name), NULL::TIMESTAMP)) AND technique='auto')
            OR technique='sample'
      THEN
            histogram_exact(col_name, bins)
      ELSE
            histogram(col_name, bins)
      END AS histogram
   FROM query_table(source::VARCHAR), bins
);
)"},
	{DEFAULT_SCHEMA, "histogram", {"source", "col_name", nullptr}, {{"bin_count", "10"}, {"technique", "'auto'"}, {nullptr, nullptr}},  R"(
SELECT
   CASE
   WHEN is_histogram_other_bin(bin)
   THEN '(other values)'
   WHEN (NOT (can_cast_implicitly(bin, NULL::BIGINT) OR
              can_cast_implicitly(bin, NULL::DOUBLE) OR
              can_cast_implicitly(bin, NULL::TIMESTAMP)) AND technique='auto')
              OR technique='sample'
   THEN bin::VARCHAR
   WHEN row_number() over () = 1
   THEN concat('x <= ', bin::VARCHAR)
   ELSE concat(lag(bin::VARCHAR) over (), ' < x <= ', bin::VARCHAR)
   END AS bin,
   count,
   bar(count, 0, max(count) over ()) AS bar
FROM histogram_values(source, col_name, bin_count := bin_count, technique := technique);
)"},
	{DEFAULT_SCHEMA, "duckdb_logs_parsed", {"log_type"}, {}, R"(
SELECT * EXCLUDE (message),
       UNNEST(CASE WHEN type ILIKE log_type THEN parse_duckdb_log_message(log_type, message) END)
FROM duckdb_logs(denormalized_table=1)
WHERE type ILIKE log_type
)"},
	{DEFAULT_SCHEMA, "duckdb_profiling_settings", {}, {}, R"(
SELECT * EXCLUDE(input_type, scope, aliases, typed_value)
  FROM duckdb_settings(deprecated := false)
  WHERE name IN (
      'enable_profiling',
      'profiling_coverage',
      'profiling_output',
      'profiling_mode',
      'tracked_metrics'
  );
)"},
	{nullptr, nullptr, {nullptr}, {{nullptr, nullptr}}, nullptr}
	};
// clang-format on

DefaultTableFunctionGenerator::DefaultTableFunctionGenerator(Catalog &catalog, SchemaCatalogEntry &schema)
    : DefaultGenerator(catalog), schema(schema) {
}

unique_ptr<CreateMacroInfo>
DefaultTableFunctionGenerator::CreateInternalTableMacroInfo(const DefaultTableMacro &default_macro,
                                                            unique_ptr<MacroFunction> function) {
	for (idx_t param_idx = 0; default_macro.parameters[param_idx] != nullptr; param_idx++) {
		function->parameters.push_back(make_uniq<ColumnRefExpression>(default_macro.parameters[param_idx]));
	}
	for (idx_t named_idx = 0; default_macro.named_parameters[named_idx].name != nullptr; named_idx++) {
		const auto &named_param = default_macro.named_parameters[named_idx];
		auto default_value = Parser::GetBuiltinParser().ParseSingleExpression(named_param.default_value);
		function->parameters.push_back(make_uniq<ColumnRefExpression>(named_param.name));
		function->default_parameters.insert(Identifier(named_param.name), std::move(default_value));
	}
	DefaultMacroQualifier::Qualify(*function, Identifier(default_macro.schema));

	auto type = CatalogType::TABLE_MACRO_ENTRY;
	auto bind_info = make_uniq<CreateMacroInfo>(type);
	bind_info->SetQualifiedName(QualifiedName({Identifier(default_macro.schema)}, Identifier(default_macro.name)));
	bind_info->temporary = true;
	bind_info->internal = true;
	bind_info->macros.push_back(std::move(function));
	return bind_info;
}

unique_ptr<CreateMacroInfo>
DefaultTableFunctionGenerator::CreateTableMacroInfo(const DefaultTableMacro &default_macro) {
	auto parser = Parser::GetBuiltinParser();
	auto result = make_uniq<TableMacroFunction>(parser.ParseSelectNode(default_macro.macro));
	return CreateInternalTableMacroInfo(default_macro, std::move(result));
}

optional_ptr<const DefaultTableMacro>
DefaultTableFunctionGenerator::FindTableMacro(const DefaultTableMacro macros[], const Identifier &name,
                                              optional_ptr<const Identifier> schema_name) {
	for (idx_t index = 0; macros[index].name != nullptr; index++) {
		if ((!schema_name || macros[index].schema == *schema_name) && macros[index].name == name) {
			return macros[index];
		}
	}
	return nullptr;
}

unique_ptr<CatalogEntry> DefaultTableFunctionGenerator::CreateTableMacroEntry(Catalog &catalog,
                                                                              SchemaCatalogEntry &schema,
                                                                              const DefaultTableMacro &default_macro) {
	auto info = CreateTableMacroInfo(default_macro);
	return make_uniq_base<CatalogEntry, TableMacroCatalogEntry>(catalog, schema, *info);
}

unique_ptr<CatalogEntry> DefaultTableFunctionGenerator::CreateDefaultEntry(ClientContext &,
                                                                           const Identifier &entry_name) {
	auto macro = FindTableMacro(internal_table_macros, entry_name, schema.name);
	if (macro) {
		return CreateTableMacroEntry(catalog, schema, *macro);
	}
	return nullptr;
}

vector<Identifier> DefaultTableFunctionGenerator::GetDefaultEntries() {
	vector<Identifier> result;
	for (idx_t index = 0; internal_table_macros[index].name != nullptr; index++) {
		if (StringUtil::Lower(internal_table_macros[index].name) != internal_table_macros[index].name) {
			throw InternalException("Default macro name %s should be lowercase", internal_table_macros[index].name);
		}
		if (internal_table_macros[index].schema == schema.name) {
			result.emplace_back(internal_table_macros[index].name);
		}
	}
	return result;
}

} // namespace duckdb
