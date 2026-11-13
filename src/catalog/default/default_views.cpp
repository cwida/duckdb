#include "duckdb/parser/parser.hpp"
#include "duckdb/catalog/default/default_views.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/catalog/catalog_entry/schema_catalog_entry.hpp"
#include "duckdb/catalog/catalog_entry/view_catalog_entry.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

struct DefaultView {
	const char *schema;
	const char *name;
	const char *sql;
};

static const DefaultView internal_views[] = {
    {DEFAULT_SCHEMA, "pragma_database_list",
     "SELECT database_oid AS seq, database_name AS name, path AS file FROM duckdb_databases() WHERE NOT internal ORDER "
     "BY 1"},
    {DEFAULT_SCHEMA, "sqlite_master",
     "select 'table' \"type\", table_name \"name\", table_name \"tbl_name\", 0 rootpage, sql from duckdb_tables union "
     "all select 'view' \"type\", view_name \"name\", view_name \"tbl_name\", 0 rootpage, sql from duckdb_views union "
     "all select 'index' \"type\", index_name \"name\", table_name \"tbl_name\", 0 rootpage, sql from duckdb_indexes;"},
    {DEFAULT_SCHEMA, "sqlite_schema", "SELECT * FROM sqlite_master"},
    {DEFAULT_SCHEMA, "sqlite_temp_master", "SELECT * FROM sqlite_master"},
    {DEFAULT_SCHEMA, "sqlite_temp_schema", "SELECT * FROM sqlite_master"},
    {DEFAULT_SCHEMA, "duckdb_constraints", "SELECT * FROM duckdb_constraints()"},
    {DEFAULT_SCHEMA, "duckdb_columns", "SELECT * FROM duckdb_columns() WHERE NOT internal"},
    {DEFAULT_SCHEMA, "duckdb_databases", "SELECT * FROM duckdb_databases() WHERE NOT internal"},
    {DEFAULT_SCHEMA, "duckdb_indexes", "SELECT * FROM duckdb_indexes()"},
    {DEFAULT_SCHEMA, "duckdb_schemas", "SELECT * FROM duckdb_schemas() WHERE NOT internal"},
    {DEFAULT_SCHEMA, "duckdb_tables", "SELECT * FROM duckdb_tables() WHERE NOT internal"},
    {DEFAULT_SCHEMA, "duckdb_types", "SELECT * FROM duckdb_types()"},
    {DEFAULT_SCHEMA, "duckdb_views", "SELECT * FROM duckdb_views() WHERE NOT internal"},
    {DEFAULT_SCHEMA, "duckdb_logs", "SELECT * FROM duckdb_logs(denormalized_table=true)"},
    {"pg_catalog", "pg_am", "SELECT 0 oid, 'art' amname, NULL amhandler, 'i' amtype"},
    {"pg_catalog", "pg_prepared_statements",
     "SELECT name, statement, NULL prepare_time, parameter_types, result_types, NULL from_sql, NULL generic_plans, "
     "NULL custom_plans from duckdb_prepared_statements()"},
    {"pg_catalog", "pg_attribute",
     "SELECT table_oid attrelid, column_name attname, "
     "coalesce(map_type_id_to_pg_oid(data_type_id), data_type_id + 16384) atttypid, 0 attstattarget, NULL attlen, "
     "column_index attnum, 0 attndims, -1 attcacheoff, case when data_type ilike '%decimal%' then "
     "numeric_precision*1000+numeric_scale else -1 end atttypmod, false attbyval, NULL attstorage, NULL attalign, NOT "
     "is_nullable attnotnull, column_default IS NOT NULL atthasdef, false atthasmissing, '' attidentity, '' "
     "attgenerated, false attisdropped, true attislocal, 0 attinhcount, 0 attcollation, NULL attcompression, NULL "
     "attacl, NULL attoptions, NULL attfdwoptions, NULL attmissingval FROM duckdb_columns()"},
    {"pg_catalog", "pg_attrdef",
     "SELECT column_index oid, table_oid adrelid, column_index adnum, column_default adbin from duckdb_columns() where "
     "column_default is not null;"},
    {"pg_catalog", "pg_class",
     "SELECT table_oid oid, table_name relname, schema_oid relnamespace, 0 reltype, 0 reloftype, 0 relowner, 0 relam, "
     "0 relfilenode, 0 reltablespace, 0 relpages, estimated_size::real reltuples, 0 relallvisible, 0 reltoastrelid, 0 "
     "reltoastidxid, index_count > 0 relhasindex, false relisshared, case when temporary then 't' else 'p' end "
     "relpersistence, 'r' relkind, column_count relnatts, check_constraint_count relchecks, false relhasoids, "
     "has_primary_key relhaspkey, false relhasrules, false relhastriggers, false relhassubclass, false relrowsecurity, "
     "true relispopulated, NULL relreplident, false relispartition, 0 relrewrite, 0 relfrozenxid, NULL relminmxid, "
     "NULL relacl, NULL reloptions, NULL relpartbound FROM duckdb_tables() UNION ALL SELECT view_oid oid, view_name "
     "relname, schema_oid relnamespace, 0 reltype, 0 reloftype, 0 relowner, 0 relam, 0 relfilenode, 0 reltablespace, 0 "
     "relpages, 0 reltuples, 0 relallvisible, 0 reltoastrelid, 0 reltoastidxid, false relhasindex, false relisshared, "
     "case when temporary then 't' else 'p' end relpersistence, 'v' relkind, column_count relnatts, 0 relchecks, false "
     "relhasoids, false relhaspkey, false relhasrules, false relhastriggers, false relhassubclass, false "
     "relrowsecurity, true relispopulated, NULL relreplident, false relispartition, 0 relrewrite, 0 relfrozenxid, NULL "
     "relminmxid, NULL relacl, NULL reloptions, NULL relpartbound FROM duckdb_views() UNION ALL SELECT sequence_oid "
     "oid, sequence_name relname, schema_oid relnamespace, 0 reltype, 0 reloftype, 0 relowner, 0 relam, 0 relfilenode, "
     "0 reltablespace, 0 relpages, 0 reltuples, 0 relallvisible, 0 reltoastrelid, 0 reltoastidxid, false relhasindex, "
     "false relisshared, case when temporary then 't' else 'p' end relpersistence, 'S' relkind, 0 relnatts, 0 "
     "relchecks, false relhasoids, false relhaspkey, false relhasrules, false relhastriggers, false relhassubclass, "
     "false relrowsecurity, true relispopulated, NULL relreplident, false relispartition, 0 relrewrite, 0 "
     "relfrozenxid, NULL relminmxid, NULL relacl, NULL reloptions, NULL relpartbound FROM duckdb_sequences() UNION ALL "
     "SELECT index_oid oid, index_name relname, schema_oid relnamespace, 0 reltype, 0 reloftype, 0 relowner, 0 relam, "
     "0 relfilenode, 0 reltablespace, 0 relpages, 0 reltuples, 0 relallvisible, 0 reltoastrelid, 0 reltoastidxid, "
     "false relhasindex, false relisshared, 't' relpersistence, 'i' relkind, NULL relnatts, 0 relchecks, false "
     "relhasoids, false relhaspkey, false relhasrules, false relhastriggers, false relhassubclass, false "
     "relrowsecurity, true relispopulated, NULL relreplident, false relispartition, 0 relrewrite, 0 relfrozenxid, NULL "
     "relminmxid, NULL relacl, NULL reloptions, NULL relpartbound FROM duckdb_indexes()"},
    {"pg_catalog", "pg_constraint",
     "SELECT table_oid*1000000+constraint_index oid, constraint_text conname, schema_oid connamespace, CASE "
     "constraint_type WHEN 'CHECK' then 'c' WHEN 'UNIQUE' then 'u' WHEN 'PRIMARY KEY' THEN 'p' WHEN 'FOREIGN KEY' THEN "
     "'f' ELSE 'x' END contype, constraint_check_mode = 'DEFERRED' condeferrable, constraint_check_mode = 'DEFERRED' "
     "condeferred, true convalidated, table_oid "
     "conrelid, 0 "
     "contypid, 0 conindid, 0 conparentid, 0 confrelid, NULL confupdtype, NULL confdeltype, NULL confmatchtype, true "
     "conislocal, 0 coninhcount, false connoinherit, constraint_column_indexes conkey, NULL confkey, NULL conpfeqop, "
     "NULL conppeqop, NULL conffeqop, NULL conexclop, expression conbin FROM duckdb_constraints()"},
    {"pg_catalog", "pg_collation", "SELECT NULL::OID oid, NULL::VARCHAR collname WHERE FALSE"},
    {"pg_catalog", "pg_database",
     "SELECT database_oid oid, database_name datname, true::boolean datallowconn, false::boolean AS datistemplate FROM "
     "duckdb_databases()"},
    {"pg_catalog", "pg_depend", "SELECT * FROM duckdb_dependencies()"},
    {"pg_catalog", "pg_description",
     "SELECT table_oid AS objoid, database_oid AS classoid, 0 AS objsubid, comment AS description FROM duckdb_tables() "
     "WHERE NOT internal UNION ALL SELECT table_oid AS objoid, database_oid AS classoid, column_index AS objsubid, "
     "comment AS description FROM duckdb_columns() WHERE NOT internal UNION ALL SELECT view_oid AS objoid, "
     "database_oid AS classoid, 0 AS objsubid, comment AS description FROM duckdb_views() WHERE NOT internal UNION ALL "
     "SELECT index_oid AS objoid, database_oid AS classoid, 0 AS objsubid, comment AS description FROM duckdb_indexes "
     "UNION ALL SELECT sequence_oid AS objoid, database_oid AS classoid, 0 AS objsubid, comment AS description FROM "
     "duckdb_sequences() UNION ALL SELECT type_oid AS objoid, database_oid AS classoid, 0 AS objsubid, comment AS "
     "description FROM duckdb_types() WHERE NOT internal UNION ALL SELECT function_oid AS objoid, database_oid AS "
     "classoid, 0 AS objsubid, comment AS description FROM duckdb_functions() WHERE NOT internal;"},
    {"pg_catalog", "pg_enum",
     "SELECT NULL oid, a.type_oid enumtypid, list_position(b.labels, a.elabel) enumsortorder, a.elabel enumlabel FROM "
     "(SELECT UNNEST(labels) elabel, type_oid FROM duckdb_types() WHERE logical_type='ENUM') a JOIN duckdb_types() b "
     "ON a.type_oid=b.type_oid;"},
    {"pg_catalog", "pg_index",
     "SELECT index_oid indexrelid, table_oid indrelid, 0 indnatts, 0 indnkeyatts, is_unique indisunique, is_primary "
     "indisprimary, false indisexclusion, true indimmediate, false indisclustered, true indisvalid, false "
     "indcheckxmin, true indisready, true indislive, false indisreplident, NULL::INT[] indkey, NULL::OID[] "
     "indcollation, NULL::OID[] indclass, NULL::INT[] indoption, expressions indexprs, NULL indpred FROM "
     "duckdb_indexes()"},
    {"pg_catalog", "pg_indexes",
     "SELECT schema_name schemaname, table_name tablename, index_name indexname, NULL \"tablespace\", sql indexdef "
     "FROM duckdb_indexes()"},
    {"pg_catalog", "pg_namespace",
     "SELECT oid, schema_name nspname, 0 nspowner, NULL nspacl FROM duckdb_schemas() where "
     "database_name=current_database()"},
    {"pg_catalog", "pg_proc",
     "SELECT f.function_oid oid, function_name proname, s.oid pronamespace,  NULL proowner, NULL prolang, 0 procost, 0 "
     "prorows, varargs provariadic,  0 prosupport, CASE function_type WHEN 'aggregate' THEN 'a' ELSE 'f' END prokind, "
     "false prosecdef, false proleakproof, false proisstrict, function_type = 'table' proretset,  case (stability) "
     "when 'CONSISTENT' then 'i' when 'CONSISTENT_WITHIN_QUERY' then 's' when 'VOLATILE' then 'v' end provolatile, 'u' "
     "proparallel, length(parameters)  pronargs, 0 pronargdefaults, return_type prorettype,  parameter_types "
     "proargtypes,  NULL proallargtypes, NULL proargmodes, parameters proargnames, NULL proargdefaults, NULL "
     "protrftypes, NULL prosrc, NULL probin, macro_definition prosqlbody, NULL proconfig, NULL proacl, function_type = "
     "'aggregate' proisagg,  FROM duckdb_functions() f LEFT JOIN duckdb_schemas() s USING (database_name, "
     "schema_name)"},
    {"pg_catalog", "pg_sequence",
     "SELECT sequence_oid seqrelid, 0 seqtypid, start_value seqstart, increment_by seqincrement, max_value seqmax, "
     "min_value seqmin, 0 seqcache, cycle seqcycle FROM duckdb_sequences()"},
    {"pg_catalog", "pg_sequences",
     "SELECT schema_name schemaname, sequence_name sequencename, 'duckdb' sequenceowner, 0 data_type, start_value, "
     "min_value, max_value, increment_by, cycle, 0 cache_size, last_value FROM duckdb_sequences()"},
    {"pg_catalog", "pg_settings",
     "SELECT name, value setting, description short_desc, CASE WHEN input_type = 'VARCHAR' THEN 'string' WHEN "
     "input_type = 'BOOLEAN' THEN 'bool' WHEN input_type IN ('BIGINT', 'UBIGINT') THEN 'integer' ELSE input_type END "
     "vartype FROM duckdb_settings()"},
    {"pg_catalog", "pg_tables",
     "SELECT schema_name schemaname, table_name tablename, 'duckdb' tableowner, NULL \"tablespace\", index_count > 0 "
     "hasindexes, false hasrules, false hastriggers FROM duckdb_tables()"},
    {"pg_catalog", "pg_tablespace", "SELECT 0 oid, 'pg_default' spcname, 0 spcowner, NULL spcacl, NULL spcoptions"},
    {"pg_catalog", "pg_type",
     "SELECT CASE WHEN type_oid IS NULL THEN NULL WHEN logical_type = 'ENUM' AND type_name <> 'enum' THEN type_oid "
     "WHEN internal THEN map_type_id_to_pg_oid(type_oid) END oid, format_pg_type(logical_type, type_name) typname, "
     "schema_oid typnamespace, "
     "0 typowner, type_size typlen, false typbyval, CASE WHEN logical_type='ENUM' THEN 'e' else 'b' end typtype, CASE "
     "WHEN type_category='NUMERIC' THEN 'N' WHEN type_category='STRING' THEN 'S' WHEN type_category='DATETIME' THEN "
     "'D' WHEN type_category='BOOLEAN' THEN 'B' WHEN type_category='COMPOSITE' THEN 'C' WHEN type_category='USER' THEN "
     "'U' ELSE 'X' END typcategory, false typispreferred, true typisdefined, NULL typdelim, NULL typrelid, NULL "
     "typsubscript, NULL typelem, NULL typarray, NULL typinput, NULL typoutput, NULL typreceive, NULL typsend, NULL "
     "typmodin, NULL typmodout, NULL typanalyze, 'd' typalign, 'p' typstorage, NULL typnotnull, NULL typbasetype, NULL "
     "typtypmod, NULL typndims, NULL typcollation, NULL typdefaultbin, NULL typdefault, NULL typacl FROM "
     "duckdb_types() WHERE type_oid IS NOT NULL;"},
    {"pg_catalog", "pg_views",
     "SELECT schema_name schemaname, view_name viewname, 'duckdb' viewowner, sql definition FROM duckdb_views()"},
    {"information_schema", "columns",
     "SELECT database_name table_catalog, schema_name table_schema, table_name, column_name, column_index "
     "ordinal_position, column_default, CASE WHEN is_nullable THEN 'YES' ELSE 'NO' END is_nullable, data_type, "
     "character_maximum_length, NULL::INT character_octet_length, numeric_precision, numeric_precision_radix, "
     "numeric_scale, NULL::INT datetime_precision, NULL::VARCHAR interval_type, NULL::INT interval_precision, "
     "NULL::VARCHAR character_set_catalog, NULL::VARCHAR character_set_schema, NULL::VARCHAR character_set_name, "
     "NULL::VARCHAR collation_catalog, NULL::VARCHAR collation_schema, NULL::VARCHAR collation_name, NULL::VARCHAR "
     "domain_catalog, NULL::VARCHAR domain_schema, NULL::VARCHAR domain_name, NULL::VARCHAR udt_catalog, NULL::VARCHAR "
     "udt_schema, NULL::VARCHAR udt_name, NULL::VARCHAR scope_catalog, NULL::VARCHAR scope_schema, NULL::VARCHAR "
     "scope_name, NULL::BIGINT maximum_cardinality, NULL::VARCHAR dtd_identifier, NULL::BOOL is_self_referencing, "
     "NULL::BOOL is_identity, NULL::VARCHAR identity_generation, NULL::VARCHAR identity_start, NULL::VARCHAR "
     "identity_increment, NULL::VARCHAR identity_maximum, NULL::VARCHAR identity_minimum, NULL::BOOL identity_cycle, "
     "CASE WHEN is_generated THEN 'ALWAYS' ELSE 'NEVER' END is_generated, generation_expression, NULL::BOOL "
     "is_updatable, comment AS COLUMN_COMMENT FROM duckdb_columns;"},
    {"information_schema", "schemata",
     "SELECT database_name catalog_name, schema_name, 'duckdb' schema_owner, NULL::VARCHAR "
     "default_character_set_catalog, NULL::VARCHAR default_character_set_schema, NULL::VARCHAR "
     "default_character_set_name, sql sql_path FROM duckdb_schemas()"},
    {"information_schema", "tables",
     "SELECT database_name table_catalog, schema_name table_schema, table_name, CASE WHEN temporary THEN 'LOCAL "
     "TEMPORARY' ELSE 'BASE TABLE' END table_type, NULL::VARCHAR self_referencing_column_name, NULL::VARCHAR "
     "reference_generation, NULL::VARCHAR user_defined_type_catalog, NULL::VARCHAR user_defined_type_schema, "
     "NULL::VARCHAR user_defined_type_name, 'YES' is_insertable_into, 'NO' is_typed, CASE WHEN temporary THEN "
     "'PRESERVE' ELSE NULL END commit_action, comment AS TABLE_COMMENT FROM duckdb_tables() UNION ALL SELECT "
     "database_name table_catalog, schema_name table_schema, view_name table_name, 'VIEW' table_type, NULL "
     "self_referencing_column_name, NULL reference_generation, NULL user_defined_type_catalog, NULL "
     "user_defined_type_schema, NULL user_defined_type_name, 'NO' is_insertable_into, 'NO' is_typed, NULL "
     "commit_action, comment AS TABLE_COMMENT FROM duckdb_views;"},
    {"information_schema", "character_sets",
     "SELECT NULL::VARCHAR character_set_catalog, NULL::VARCHAR character_set_schema, 'UTF8' character_set_name, 'UCS' "
     "character_repertoire, 'UTF8' form_of_use, current_database() default_collate_catalog, 'pg_catalog' "
     "default_collate_schema, 'ucs_basic' default_collate_name;"},
    {"information_schema", "referential_constraints",
     "SELECT f.database_name constraint_catalog, f.schema_name constraint_schema, f.constraint_name constraint_name, "
     "c.database_name unique_constraint_catalog, c.schema_name unique_constraint_schema, c.constraint_name "
     "unique_constraint_name, 'NONE' match_option, 'NO ACTION' update_rule, 'NO ACTION' delete_rule FROM "
     "duckdb_constraints() c, duckdb_constraints() f WHERE f.constraint_type = 'FOREIGN KEY' AND (c.constraint_type = "
     "'UNIQUE' OR c.constraint_type = 'PRIMARY KEY') AND f.database_oid = c.database_oid AND f.schema_oid = "
     "c.schema_oid AND lower(f.referenced_table) = lower(c.table_name) AND [lower(x) for x in "
     "f.referenced_column_names] = [lower(x) for x in c.constraint_column_names]"},
    {"information_schema", "key_column_usage",
     "SELECT database_name constraint_catalog, schema_name constraint_schema, constraint_name, database_name "
     "table_catalog, schema_name table_schema, table_name, UNNEST(constraint_column_names) column_name, "
     "UNNEST(generate_series(1, len(constraint_column_names))) ordinal_position, CASE constraint_type WHEN 'FOREIGN "
     "KEY' THEN UNNEST (generate_series(1, len(constraint_column_names))) ELSE NULL END position_in_unique_constraint "
     "FROM duckdb_constraints() WHERE constraint_type = 'FOREIGN KEY' OR constraint_type = 'PRIMARY KEY' OR "
     "constraint_type = 'UNIQUE';"},
    {"information_schema", "table_constraints",
     "SELECT database_name constraint_catalog, schema_name constraint_schema, constraint_name, database_name "
     "table_catalog, schema_name table_schema, table_name, CASE constraint_type WHEN 'NOT NULL' THEN 'CHECK' ELSE "
     "constraint_type END constraint_type, CASE WHEN constraint_check_mode = 'DEFERRED' THEN 'YES' ELSE 'NO' END "
     "is_deferrable, CASE WHEN "
     "constraint_check_mode = 'DEFERRED' THEN 'YES' ELSE 'NO' END initially_deferred, 'YES' enforced, 'YES' "
     "nulls_distinct FROM duckdb_constraints() WHERE constraint_type = 'PRIMARY KEY' OR constraint_type = 'FOREIGN "
     "KEY' OR constraint_type = 'UNIQUE' OR constraint_type = 'CHECK' OR constraint_type = 'NOT NULL';"},
    {"information_schema", "constraint_column_usage",
     "SELECT database_name AS table_catalog, schema_name AS table_schema, table_name, column_name, database_name AS "
     "constraint_catalog, schema_name AS constraint_schema, constraint_name, constraint_type, constraint_text FROM "
     "(SELECT dc.*, UNNEST(dc.constraint_column_names) AS column_name FROM duckdb_constraints() AS dc WHERE "
     "constraint_type NOT IN ('NOT NULL') );"},
    {"information_schema", "constraint_table_usage",
     "SELECT database_name AS table_catalog, schema_name AS table_schema, table_name, database_name AS "
     "constraint_catalog, schema_name AS constraint_schema, constraint_name, constraint_type FROM duckdb_constraints() "
     "WHERE constraint_type NOT IN ('NOT NULL');"},
    {"information_schema", "check_constraints",
     "SELECT database_name AS constraint_catalog, schema_name AS constraint_schema, constraint_name, CASE "
     "constraint_type WHEN 'NOT NULL' THEN constraint_column_names[1] || ' IS NOT NULL' ELSE constraint_text END AS "
     "check_clause FROM duckdb_constraints() WHERE constraint_type IN ('CHECK', 'NOT NULL');"},
    {"information_schema", "views",
     "SELECT database_name AS table_catalog, schema_name AS table_schema, view_name AS table_name, sql AS "
     "view_definition, 'NONE' AS check_option, 'NO' AS is_updatable, 'NO' AS is_insertable_into, 'NO' AS "
     "is_trigger_updatable, 'NO' AS is_trigger_deletable, 'NO' AS is_trigger_insertable_into FROM duckdb_views();"},
    {nullptr, nullptr, nullptr}};

static unique_ptr<CreateViewInfo> GetDefaultView(ClientContext &context, const Identifier &input_schema,
                                                 const Identifier &input_name) {
	auto schema = StringUtil::Lower(input_schema.GetIdentifierName());
	auto name = StringUtil::Lower(input_name.GetIdentifierName());
	for (idx_t index = 0; internal_views[index].name != nullptr; index++) {
		if (internal_views[index].schema == schema && internal_views[index].name == name) {
			auto result = make_uniq<CreateViewInfo>();
			result->SetQualifiedName(QualifiedName({Identifier(schema)}, Identifier(name)));
			result->sql = internal_views[index].sql;
			result->temporary = true;
			result->internal = true;

			auto parser = Parser::GetBuiltinParser();
			return CreateViewInfo::FromSelect(parser, std::move(result));
		}
	}
	return nullptr;
}

DefaultViewGenerator::DefaultViewGenerator(Catalog &catalog, SchemaCatalogEntry &schema)
    : DefaultGenerator(catalog), schema(schema) {
}

unique_ptr<CatalogEntry> DefaultViewGenerator::CreateDefaultEntry(ClientContext &context,
                                                                  const Identifier &entry_name) {
	auto info = GetDefaultView(context, schema.name, entry_name);
	if (info) {
		return make_uniq_base<CatalogEntry, ViewCatalogEntry>(catalog, schema, *info);
	}
	return nullptr;
}

vector<Identifier> DefaultViewGenerator::GetDefaultEntries() {
	vector<Identifier> result;
	for (idx_t index = 0; internal_views[index].name != nullptr; index++) {
		if (internal_views[index].schema == schema.name) {
			result.emplace_back(internal_views[index].name);
		}
	}
	return result;
}

} // namespace duckdb
