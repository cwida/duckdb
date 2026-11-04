#include "duckdb/catalog/default/default_functions.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/parser/parsed_data/create_macro_info.hpp"
#include "duckdb/parser/statement/create_statement.hpp"
#include "duckdb/catalog/catalog_entry/scalar_macro_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

static const DefaultMacro internal_macros[] = {
    {DEFAULT_SCHEMA, "current_role", "() AS 'duckdb'"}, // user name of current execution context
    {DEFAULT_SCHEMA, "current_user", "() AS 'duckdb'"}, // user name of current execution context
    {DEFAULT_SCHEMA, "current_catalog",
     "() AS main.current_database()"},              // name of current database (called "catalog" in the SQL standard)
    {DEFAULT_SCHEMA, "user", "() AS current_user"}, // equivalent to current_user
    {DEFAULT_SCHEMA, "session_user", "() AS 'duckdb'"}, // session user name
    {"pg_catalog", "inet_client_addr", "() AS NULL"},   // address of the remote connection
    {"pg_catalog", "inet_client_port", "() AS NULL"},   // port of the remote connection
    {"pg_catalog", "inet_server_addr", "() AS NULL"},   // address of the local connection
    {"pg_catalog", "inet_server_port", "() AS NULL"},   // port of the local connection
    {"pg_catalog", "pg_my_temp_schema", "() AS 0"},     // OID of session's temporary schema, or 0 if none
    {"pg_catalog", "pg_is_other_temp_schema", "(schema_id) AS false"}, // is schema another session's temporary schema?

    {"pg_catalog", "pg_conf_load_time", "() AS current_timestamp"},        // configuration load time
    {"pg_catalog", "pg_postmaster_start_time", "() AS current_timestamp"}, // server start time

    {"pg_catalog", "pg_typeof", "(expression) AS lower(typeof(expression))"}, // get the data type of any value

    {"pg_catalog", "current_database",
     "() AS system.main.current_database()"}, // name of current database (called "catalog" in the SQL standard)
    {"pg_catalog", "current_query",
     "() AS system.main.current_query()"}, // the currently executing query (NULL if not inside a plpgsql function)
    {"pg_catalog", "current_schema", "() AS system.main.current_schema()"}, // name of current schema
    {"pg_catalog", "current_schemas",
     "(include_implicit) AS system.main.current_schemas(include_implicit)"}, // names of schemas in search path

    // privilege functions
    {"pg_catalog", "has_any_column_privilege",
     "(\"table\", privilege) AS true, (user, \"table\", privilege) AS true"}, // boolean  //does current/named user have
                                                                              // privilege for any column of table
    {"pg_catalog", "has_column_privilege",
     "(\"table\", \"column\", privilege) AS true, (user, \"table\", \"column\", privilege) AS true"}, // boolean  //does
                                                                                                      // current/named
                                                                                                      // user have
                                                                                                      // privilege for
                                                                                                      // column
    {"pg_catalog", "has_database_privilege",
     "(database, privilege) AS true, (user, database, privilege) AS true"}, // boolean  //does current/named user have
                                                                            // privilege for database
    {"pg_catalog", "has_foreign_data_wrapper_privilege",
     "(fdw, privilege) AS true, (user, fdw, privilege) AS true"}, // boolean  //does current/named user have privilege
                                                                  // for foreign-data wrapper
    {"pg_catalog", "has_function_privilege",
     "(function, privilege) AS true, (user, function, privilege) AS true"}, // boolean  //does current/named user have
                                                                            // privilege for function
    {"pg_catalog", "has_language_privilege",
     "(language, privilege) AS true, (user, language, privilege) AS true"}, // boolean  //does current/named user have
                                                                            // privilege for language
    {"pg_catalog", "has_schema_privilege",
     "(schema, privilege) AS true, (user, schema, privilege) AS true"}, // boolean  //does current/named user have
                                                                        // privilege for schema
    {"pg_catalog", "has_sequence_privilege",
     "(sequence, privilege) AS true, (user, sequence, privilege) AS true"}, // boolean  //does current/named user have
                                                                            // privilege for sequence
    {"pg_catalog", "has_server_privilege",
     "(server, privilege) AS true, (user, server, privilege) AS true"}, // boolean  //does current/named user have
                                                                        // privilege for foreign server
    {"pg_catalog", "has_table_privilege",
     "(\"table\", privilege) AS true, (user, \"table\", privilege) AS true"}, // boolean  //does current/named user have
                                                                              // privilege for table
    {"pg_catalog", "has_tablespace_privilege",
     "(tablespace, privilege) AS true, (user, tablespace, privilege) AS true"}, // boolean  //does current/named user
                                                                                // have privilege for tablespace

    // various postgres system functions
    {"pg_catalog", "pg_get_viewdef", "(oid) AS (select sql from duckdb_views() v where v.view_oid=oid)"},
    {"pg_catalog", "pg_get_constraintdef",
     "(constraint_oid) AS (select constraint_text from duckdb_constraints() d_constraint where "
     "d_constraint.table_oid=constraint_oid//1000000 and d_constraint.constraint_index=constraint_oid%1000000), "
     "(constraint_oid, pretty_bool) AS pg_get_constraintdef(constraint_oid)"},
    {"pg_catalog", "pg_get_expr", "(pg_node_tree, relation_oid) AS pg_node_tree"},
    {"pg_catalog", "format_pg_type",
     "(logical_type, type_name) AS case upper(logical_type) when 'FLOAT' then 'float4' when 'DOUBLE' then 'float8' "
     "when 'DECIMAL' then 'numeric' when 'ENUM' then lower(type_name) when 'VARCHAR' then 'varchar' when 'BLOB' then "
     "'bytea' when 'TIMESTAMP' then 'timestamp' when 'TIME' then 'time' when 'TIMESTAMP WITH TIME ZONE' then "
     "'timestamptz' when 'TIME WITH TIME ZONE' then 'timetz' when 'SMALLINT' then 'int2' when 'INTEGER' then 'int4' "
     "when 'BIGINT' then 'int8' when 'BOOLEAN' then 'bool' else lower(logical_type) end"},
    {"pg_catalog", "format_type",
     "(type_oid, typemod) AS (select format_pg_type(t.logical_type, t.type_name) from (select d.logical_type, "
     "d.type_name, case when d.internal then coalesce(map_type_id_to_pg_oid(d.type_oid), d.type_oid + 16384) else "
     "d.type_oid end pg_oid from duckdb_types() d where d.type_oid IS NOT NULL) t where t.pg_oid=type_oid) || case "
     "when typemod>0 then concat('(', typemod//1000, ',', typemod%1000, ')') else '' end"},
    // map the id of a built-in type (its LogicalTypeId) to the PostgreSQL oid that map_to_pg_oid gives its name
    {"pg_catalog", "map_type_id_to_pg_oid",
     "(type_id) AS case type_id when 10 then 16 when 12 then 21 when 13 then 23 when 14 then 20 when 15 then 1082 when "
     "16 then 1083 when 19 then 1114 when 21 then 1700 when 22 then 700 when 23 then 701 when 25 then 1043 when 26 "
     "then 17 when 27 then 1186 when 32 then 1184 when 34 then 1266 when 36 then 1560 when 54 then 2950 else null end"},
    {"pg_catalog", "map_to_pg_oid",
     "(type_name) AS case type_name when 'bool' then 16 when 'int16' then 21 when 'int' then 23 when 'bigint' then 20 "
     "when 'date' then 1082 when 'time' then 1083 when 'datetime' then 1114 when 'dec' then 1700 when 'float' then 700 "
     "when 'double' then 701 when 'bpchar' then 1043 when 'binary' then 17 when 'interval' then 1186 when "
     "'timestamptz' then 1184 when 'timestamp with time zone' then 1184 when 'timetz' then 1266 when 'time with time "
     "zone' then 1266 when 'bit' then 1560 when 'guid' then 2950 else null end"}, // map duckdb_oid to pg_oid. If no
                                                                                  // corresponding type, return null

    {"pg_catalog", "pg_has_role",
     "(user, role, privilege) AS true, (role, privilege) AS true"}, // boolean  //does current/named user have privilege
                                                                    // for role

    {"pg_catalog", "col_description", "(table_oid, column_number) AS NULL"}, // get comment for a table column
    {"pg_catalog", "obj_description", "(object_oid, catalog_name) AS NULL"}, // get comment for a database object
    {"pg_catalog", "shobj_description",
     "(object_oid, catalog_name) AS NULL"}, // get comment for a shared database object

    // visibility functions
    {"pg_catalog", "pg_collation_is_visible", "(collation_oid) AS true"},
    {"pg_catalog", "pg_conversion_is_visible", "(conversion_oid) AS true"},
    {"pg_catalog", "pg_function_is_visible", "(function_oid) AS true"},
    {"pg_catalog", "pg_opclass_is_visible", "(opclass_oid) AS true"},
    {"pg_catalog", "pg_operator_is_visible", "(operator_oid) AS true"},
    {"pg_catalog", "pg_opfamily_is_visible", "(opclass_oid) AS true"},
    {"pg_catalog", "pg_table_is_visible", "(table_oid) AS true"},
    {"pg_catalog", "pg_ts_config_is_visible", "(config_oid) AS true"},
    {"pg_catalog", "pg_ts_dict_is_visible", "(dict_oid) AS true"},
    {"pg_catalog", "pg_ts_parser_is_visible", "(parser_oid) AS true"},
    {"pg_catalog", "pg_ts_template_is_visible", "(template_oid) AS true"},
    {"pg_catalog", "pg_type_is_visible", "(type_oid) AS true"},

    {"pg_catalog", "pg_size_pretty", "(bytes) AS format_bytes(bytes)"},
    {"pg_catalog", "pg_sleep", "(seconds) AS sleep_ms(CAST(seconds * 1000 AS BIGINT))"},

    {DEFAULT_SCHEMA, "nullif", "(a, b) AS CASE WHEN a=b THEN NULL ELSE a END"},
    {DEFAULT_SCHEMA, "if", "(a, b, c) AS CASE WHEN a THEN b ELSE c END"},
    {DEFAULT_SCHEMA, "assert_true",
     "(condition) AS CASE WHEN condition THEN NULL ELSE error('Assertion failed') END, "
     "(condition, message) AS CASE WHEN condition THEN NULL ELSE "
     "error(COALESCE('Assertion: ' || message, 'Assertion failed')) END"},
    {DEFAULT_SCHEMA, "list_append", "(l, e) AS list_concat(l, list_value(e))"},
    {DEFAULT_SCHEMA, "array_append", "(arr, el) AS list_append(arr, el)"},
    {DEFAULT_SCHEMA, "list_prepend", "(e, l) AS list_concat(list_value(e), l)"},
    {DEFAULT_SCHEMA, "array_prepend", "(el, arr) AS list_prepend(el, arr)"},
    {DEFAULT_SCHEMA, "array_pop_back", "(arr) AS arr[:LEN(arr)-1]"},
    {DEFAULT_SCHEMA, "array_pop_front", "(arr) AS arr[2:]"},
    {DEFAULT_SCHEMA, "array_push_back", "(arr, e) AS list_concat(arr, list_value(e))"},
    {DEFAULT_SCHEMA, "array_push_front", "(arr, e) AS list_concat(list_value(e), arr)"},
    {DEFAULT_SCHEMA, "array_to_string",
     "(arr, sep) AS case len(arr::varchar[]) when 0 then '' else list_aggr(arr::varchar[], 'string_agg', sep) end"},
    // Test default parameters
    {DEFAULT_SCHEMA, "array_to_string_comma_default",
     "(arr, sep := ',') AS case len(arr::varchar[]) when 0 then '' else list_aggr(arr::varchar[], 'string_agg', sep) "
     "end"},

    {DEFAULT_SCHEMA, "generate_subscripts", "(arr, dim := 1) AS unnest(generate_series(1, array_length(arr, dim)))"},
    {DEFAULT_SCHEMA, "fdiv", "(x, y) AS floor(x/y)"},
    {DEFAULT_SCHEMA, "split_part",
     "(string, delimiter, \"position\") AS if(string IS NOT NULL AND delimiter IS NOT NULL AND position IS NOT NULL, "
     "coalesce(string_split(string, delimiter)[position],''), NULL)"},
    {DEFAULT_SCHEMA, "geomean", "(x) AS exp(avg(ln(x)))"},
    {DEFAULT_SCHEMA, "geometric_mean", "(x) AS geomean(x)"},

    {DEFAULT_SCHEMA, "weighted_avg",
     "(value, weight) AS SUM(value * weight) / SUM(CASE WHEN value IS NOT NULL THEN weight ELSE 0 END)"},
    {DEFAULT_SCHEMA, "wavg", "(value, weight) AS weighted_avg(value, weight)"},
    {DEFAULT_SCHEMA, "variant_group_array", "(x) AS list(x)::VARIANT"},

    {DEFAULT_SCHEMA, "list_reverse", "(l) AS l[:-:-1]"},
    {DEFAULT_SCHEMA, "array_reverse", "(l) AS list_reverse(l)"},

    // algebraic list aggregates
    {DEFAULT_SCHEMA, "list_avg", "(l) AS list_aggr(l, 'avg')"},
    {DEFAULT_SCHEMA, "list_var_samp", "(l) AS list_aggr(l, 'var_samp')"},
    {DEFAULT_SCHEMA, "list_var_pop", "(l) AS list_aggr(l, 'var_pop')"},
    {DEFAULT_SCHEMA, "list_stddev_pop", "(l) AS list_aggr(l, 'stddev_pop')"},
    {DEFAULT_SCHEMA, "list_stddev_samp", "(l) AS list_aggr(l, 'stddev_samp')"},
    {DEFAULT_SCHEMA, "list_sem", "(l) AS list_aggr(l, 'sem')"},

    // distributive list aggregates
    {DEFAULT_SCHEMA, "list_approx_count_distinct", "(l) AS list_aggr(l, 'approx_count_distinct')"},
    {DEFAULT_SCHEMA, "list_bit_xor", "(l) AS list_aggr(l, 'bit_xor')"},
    {DEFAULT_SCHEMA, "list_bit_or", "(l) AS list_aggr(l, 'bit_or')"},
    {DEFAULT_SCHEMA, "list_bit_and", "(l) AS list_aggr(l, 'bit_and')"},
    {DEFAULT_SCHEMA, "list_bool_and", "(l) AS list_aggr(l, 'bool_and')"},
    {DEFAULT_SCHEMA, "list_bool_or", "(l) AS list_aggr(l, 'bool_or')"},
    {DEFAULT_SCHEMA, "list_count", "(l) AS list_aggr(l, 'count')"},
    {DEFAULT_SCHEMA, "list_entropy", "(l) AS list_aggr(l, 'entropy')"},
    {DEFAULT_SCHEMA, "list_last", "(l) AS list_aggr(l, 'last')"},
    {DEFAULT_SCHEMA, "list_first", "(l) AS list_aggr(l, 'first')"},
    {DEFAULT_SCHEMA, "list_any_value", "(l) AS list_aggr(l, 'any_value')"},
    {DEFAULT_SCHEMA, "list_kurtosis", "(l) AS list_aggr(l, 'kurtosis')"},
    {DEFAULT_SCHEMA, "list_kurtosis_pop", "(l) AS list_aggr(l, 'kurtosis_pop')"},
    {DEFAULT_SCHEMA, "list_min", "(l) AS list_aggr(l, 'min')"},
    {DEFAULT_SCHEMA, "list_max", "(l) AS list_aggr(l, 'max')"},
    {DEFAULT_SCHEMA, "list_product", "(l) AS list_aggr(l, 'product')"},
    {DEFAULT_SCHEMA, "list_skewness", "(l) AS list_aggr(l, 'skewness')"},
    {DEFAULT_SCHEMA, "list_sum", "(l) AS list_aggr(l, 'sum')"},
    {DEFAULT_SCHEMA, "list_string_agg", "(l) AS list_aggr(l, 'string_agg')"},

    // holistic list aggregates
    {DEFAULT_SCHEMA, "list_mode", "(l) AS list_aggr(l, 'mode')"},
    {DEFAULT_SCHEMA, "list_median", "(l) AS list_aggr(l, 'median')"},
    {DEFAULT_SCHEMA, "list_mad", "(l) AS list_aggr(l, 'mad')"},

    // nested list aggregates
    {DEFAULT_SCHEMA, "list_histogram", "(l) AS list_aggr(l, 'histogram')"},

    // map functions
    {DEFAULT_SCHEMA, "map_contains_entry",
     "(map, key, value) AS contains(map_entries(map), {'key': key, 'value': value})"},
    {DEFAULT_SCHEMA, "map_contains_value", "(map, value) AS contains(map_values(map), value)"},

    // date functions
    {DEFAULT_SCHEMA, "date_add", "(date, \"interval\") AS date + interval"},

    // date functions - convenience macro for getting days in month
    {DEFAULT_SCHEMA, "days_in_month", "(date) AS day(last_day(date))"},

    // timestamptz functions
    {DEFAULT_SCHEMA, "ago", "(i) AS current_timestamp - i::interval"},

    // regexp functions
    {DEFAULT_SCHEMA, "regexp_split_to_table", "(text, pattern) AS unnest(string_split_regex(text, pattern))"},

    // storage helper functions
    {DEFAULT_SCHEMA, "get_block_size",
     "(db_name) AS (SELECT block_size FROM pragma_database_size() WHERE database_name = db_name)"},

    // string functions
    {DEFAULT_SCHEMA, "md5_number_upper", "(param) AS ((md5_number(param)::bit::varchar)[65:])::bit::uint64"},
    {DEFAULT_SCHEMA, "md5_number_lower", "(param) AS ((md5_number(param)::bit::varchar)[:64])::bit::uint64"},

    {nullptr, nullptr, nullptr}};

unique_ptr<CreateMacroInfo> DefaultFunctionGenerator::CreateInternalMacroInfo(const DefaultMacro &default_macro) {
	auto parser = Parser::GetBuiltinParser();
	auto bind_info = make_uniq<CreateMacroInfo>(CatalogType::MACRO_ENTRY);
	// Build a full CREATE MACRO statement and let the parser handle parameters, types, and defaults.
	// macro_definition may contain multiple comma-separated overloads, e.g. "(x) AS x, (x, y) AS x+y".
	auto sql = StringUtil::Format("CREATE MACRO __dummy__%s", default_macro.macro_definition);
	parser.ParseQuery(sql);
	D_ASSERT(parser.statements.size() == 1);
	D_ASSERT(parser.statements[0]->type == StatementType::CREATE_STATEMENT);
	auto &create_stmt = parser.statements[0]->Cast<CreateStatement>();
	D_ASSERT(create_stmt.info->type == CatalogType::MACRO_ENTRY);
	auto &macro_info = create_stmt.info->Cast<CreateMacroInfo>();
	// Default-bind any typed parameters (e.g. DATE, TIMESTAMP) so overload resolution works correctly.
	// TryDefaultBind resolves built-in types without requiring a ClientContext.
	for (auto &macro : macro_info.macros) {
		for (auto &type : macro->types) {
			if (type.IsUnbound()) {
				type = UnboundType::TryDefaultBind(type);
			}
		}
	}
	bind_info->macros = std::move(macro_info.macros);
	bind_info->SetQualifiedName(QualifiedName({Identifier(default_macro.schema)}, Identifier(default_macro.name)));
	bind_info->temporary = true;
	bind_info->internal = true;
	return bind_info;
}

static bool DefaultFunctionMatches(const DefaultMacro &macro, const Identifier &schema, const Identifier &name) {
	return macro.schema == schema && macro.name == name;
}

static unique_ptr<CreateFunctionInfo> GetDefaultFunction(const Identifier &input_schema, const Identifier &input_name) {
	auto &schema = input_schema;
	auto &name = input_name;
	for (idx_t index = 0; internal_macros[index].name != nullptr; index++) {
		if (DefaultFunctionMatches(internal_macros[index], schema, name)) {
			return DefaultFunctionGenerator::CreateInternalMacroInfo(internal_macros[index]);
		}
	}
	return nullptr;
}

DefaultFunctionGenerator::DefaultFunctionGenerator(Catalog &catalog, SchemaCatalogEntry &schema)
    : DefaultGenerator(catalog), schema(schema) {
}

unique_ptr<CatalogEntry> DefaultFunctionGenerator::CreateDefaultEntry(ClientContext &context,
                                                                      const Identifier &entry_name) {
	auto info = GetDefaultFunction(schema.name, entry_name);
	if (info) {
		return make_uniq_base<CatalogEntry, ScalarMacroCatalogEntry>(catalog, schema, info->Cast<CreateMacroInfo>());
	}
	return nullptr;
}

vector<Identifier> DefaultFunctionGenerator::GetDefaultEntries() {
	vector<Identifier> result;
	for (idx_t index = 0; internal_macros[index].name != nullptr; index++) {
		if (StringUtil::Lower(internal_macros[index].name) != internal_macros[index].name) {
			throw InternalException("Default macro name %s should be lowercase", internal_macros[index].name);
		}
		if (internal_macros[index].schema == schema.name) {
			result.emplace_back(internal_macros[index].name);
		}
	}
	return result;
}

} // namespace duckdb
