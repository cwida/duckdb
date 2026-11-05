// Links the parquet archive and names only its describe function: that member has to register parquet with the engine.
// Exits non-zero unless parquet is loaded from this binary, which a build that downloaded it instead does not report.
// With STATIC_LINK_EXPLICIT the program registers the extensions itself instead of having the static initializer do it.
#include "duckdb.h"

#include <stdio.h>
#include <string.h>

#ifdef STATIC_LINK_EXPLICIT
int32_t duckdb_register_static_extensions(void);
#endif

int main(void) {
	duckdb_database db;
	duckdb_connection con;
	duckdb_result res;
#ifdef STATIC_LINK_EXPLICIT
	if (duckdb_register_static_extensions() != 0) {
		fprintf(stderr, "duckdb_register_static_extensions failed\n");
		return 1;
	}
#endif
	if (duckdb_open(NULL, &db) != DuckDBSuccess || duckdb_connect(db, &con) != DuckDBSuccess) {
		fprintf(stderr, "could not open an in-memory database\n");
		return 1;
	}
	if (duckdb_query(con, "SELECT extension_name, install_mode FROM duckdb_extensions() WHERE loaded ORDER BY 1",
	                 &res) != DuckDBSuccess) {
		fprintf(stderr, "query failed: %s\n", duckdb_result_error(&res));
		return 1;
	}
	int found = 0;
	for (idx_t i = 0; i < duckdb_row_count(&res); i++) {
		char *name = duckdb_value_varchar(&res, 0, i);
		char *install_mode = duckdb_value_varchar(&res, 1, i);
		printf("loaded: %s (%s)\n", name, install_mode ? install_mode : "no install mode");
		if (strcmp(name, "parquet") == 0) {
			found = install_mode && strcmp(install_mode, "STATICALLY_LINKED") == 0;
		}
		duckdb_free(name);
		duckdb_free(install_mode);
	}
	duckdb_destroy_result(&res);
	duckdb_disconnect(&con);
	duckdb_close(&db);
	if (!found) {
		fprintf(stderr,
		        "parquet was on the link line but is not loaded from it: its describe member did not register it\n");
		return 1;
	}
	return 0;
}
