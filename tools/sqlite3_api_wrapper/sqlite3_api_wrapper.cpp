#include "sqlite3.h"
#include "udf_struct_sqlite3.h"
#include "sqlite3_udf_wrapper.hpp"

#include "duckdb.hpp"
#include "duckdb/parser/parser.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/operator/cast_operators.hpp"

#include "utf8proc_wrapper.hpp"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include <chrono>
#include <cassert>
#include <climits>

#include "extension_helper.hpp"

using namespace duckdb;
using namespace std;

static char *sqlite3_strdup(const char *str);

struct sqlite3_string_buffer {
	//! String data
	unique_ptr<char[]> data;
	//! String length
	int data_len;
};

struct sqlite3_stmt {
	//! The DB object that this statement belongs to
	sqlite3 *db;
	//! The query string
	string query_string;
	//! The prepared statement object, if successfully prepared
	unique_ptr<PreparedStatement> prepared;
	//! The result object, if successfully executed
	unique_ptr<QueryResult> result;
	//! The current chunk that we are iterating over
	unique_ptr<DataChunk> current_chunk;
	//! The current row into the current chunk that we are iterating over
	int64_t current_row;
	//! Bound values, used for binding to the prepared statement
	vector<Value> bound_values;
	//! Names of the prepared parameters
	vector<string> bound_names;
	//! The current column values converted to string, used and filled by sqlite3_column_text
	unique_ptr<sqlite3_string_buffer[]> current_text;
};

void sqlite3_randomness(int N, void *pBuf) {
	static bool init = false;
	if (!init) {
		srand(time(NULL));
		init = true;
	}
	unsigned char *zBuf = (unsigned char *)pBuf;
	while (N--) {
		unsigned char nextByte = rand() % 255;
		zBuf[N] = nextByte;
	}
}

int sqlite3_open(const char *filename, /* Database filename (UTF-8) */
                 sqlite3 **ppDb        /* OUT: SQLite db handle */
) {
	return sqlite3_open_v2(filename, ppDb, 0, NULL);
}

int sqlite3_open_v2(const char *filename, /* Database filename (UTF-8) */
                    sqlite3 **ppDb,       /* OUT: SQLite db handle */
                    int flags,            /* Flags */
                    const char *zVfs      /* Name of VFS module to use */
) {
	if (filename && strcmp(filename, ":memory:") == 0) {
		filename = NULL;
	}
	*ppDb = nullptr;
	if (zVfs) { /* unsupported so if set we complain */
		return SQLITE_ERROR;
	}
	sqlite3 *pDb = nullptr;
	try {
		pDb = new sqlite3();
		DBConfig config;
		config.access_mode = AccessMode::AUTOMATIC;
		if (flags & SQLITE_OPEN_READONLY) {
			config.access_mode = AccessMode::READ_ONLY;
		}
		pDb->db = make_unique<DuckDB>(filename, &config);
		pDb->con = make_unique<Connection>(*pDb->db);

		ExtensionHelper::LoadAllExtensions(*pDb->db);
	} catch (std::exception &ex) {
		if (pDb) {
			pDb->last_error = ex.what();
			pDb->errCode = SQLITE_ERROR;
		}
		return SQLITE_ERROR;
	}
	*ppDb = pDb;
	return SQLITE_OK;
}

int sqlite3_close(sqlite3 *db) {
	if (db) {
		delete db;
	}
	return SQLITE_OK;
}

int sqlite3_shutdown(void) {
	return SQLITE_OK;
}

/* In SQLite this function compiles the query into VDBE bytecode,
 * in the implementation it currently executes the query */
// TODO: prepare the statement instead of executing right away
int sqlite3_prepare_v2(sqlite3 *db,           /* Database handle */
                       const char *zSql,      /* SQL statement, UTF-8 encoded */
                       int nByte,             /* Maximum length of zSql in bytes. */
                       sqlite3_stmt **ppStmt, /* OUT: Statement handle */
                       const char **pzTail    /* OUT: Pointer to unused portion of zSql */
) {
	if (!db || !ppStmt || !zSql) {
		return SQLITE_MISUSE;
	}
	*ppStmt = nullptr;
	string query = nByte < 0 ? zSql : string(zSql, nByte);
	if (pzTail) {
		*pzTail = zSql + query.size();
	}
	try {
		Parser parser;
		parser.ParseQuery(query);
		if (parser.statements.size() == 0) {
			return SQLITE_OK;
		}
		// extract the remainder
		idx_t next_location = parser.statements[0]->stmt_location + parser.statements[0]->stmt_length;
		bool set_remainder = next_location < query.size();

		// extract the first statement
		vector<unique_ptr<SQLStatement>> statements;
		statements.push_back(move(parser.statements[0]));

		db->con->context->HandlePragmaStatements(statements);

		// if there are multiple statements here, we are dealing with an import database statement
		// we directly execute all statements besides the final one
		for (idx_t i = 0; i + 1 < statements.size(); i++) {
			auto res = db->con->Query(move(statements[i]));
			if (!res->success) {
				db->last_error = res->error;
				return SQLITE_ERROR;
			}
		}

		// now prepare the query
		auto prepared = db->con->Prepare(move(statements.back()));
		if (!prepared->success) {
			// failed to prepare: set the error message
			db->last_error = prepared->error;
			return SQLITE_ERROR;
		}

		// create the statement entry
		unique_ptr<sqlite3_stmt> stmt = make_unique<sqlite3_stmt>();
		stmt->db = db;
		stmt->query_string = query;
		stmt->prepared = move(prepared);
		stmt->current_row = -1;
		for (idx_t i = 0; i < stmt->prepared->n_param; i++) {
			stmt->bound_names.push_back("$" + to_string(i + 1));
			stmt->bound_values.push_back(Value());
		}

		// extract the remainder of the query and assign it to the pzTail
		if (pzTail && set_remainder) {
			*pzTail = zSql + next_location + 1;
		}

		*ppStmt = stmt.release();
		return SQLITE_OK;
	} catch (std::exception &ex) {
		db->last_error = ex.what();
		return SQLITE_ERROR;
	}
}

bool sqlite3_display_result(StatementType type) {
	switch (type) {
	case StatementType::EXECUTE_STATEMENT:
	case StatementType::EXPLAIN_STATEMENT:
	case StatementType::PRAGMA_STATEMENT:
	case StatementType::SELECT_STATEMENT:
	case StatementType::SHOW_STATEMENT:
		return true;
	default:
		return false;
	}
}

/* Prepare the next result to be retrieved */
int sqlite3_step(sqlite3_stmt *pStmt) {
	if (!pStmt) {
		return SQLITE_MISUSE;
	}
	if (!pStmt->prepared) {
		pStmt->db->last_error = "Attempting sqlite3_step() on a non-successfully prepared statement";
		return SQLITE_ERROR;
	}
	pStmt->current_text = nullptr;
	if (!pStmt->result) {
		// no result yet! call Execute()
		pStmt->result = pStmt->prepared->Execute(pStmt->bound_values, true);
		if (!pStmt->result->success) {
			// error in execute: clear prepared statement
			pStmt->db->last_error = pStmt->result->error;
			pStmt->prepared = nullptr;
			return SQLITE_ERROR;
		}
		// fetch a chunk
		if (!pStmt->result->TryFetch(pStmt->current_chunk, pStmt->db->last_error)) {
			pStmt->prepared = nullptr;
			return SQLITE_ERROR;
		}

		pStmt->current_row = -1;

		auto statement_type = pStmt->prepared->GetStatementType();
		if (StatementTypeReturnChanges(statement_type) && pStmt->current_chunk->size() > 0) {
			// update total changes
			auto row_changes = pStmt->current_chunk->GetValue(0, 0);
			if (!row_changes.is_null && row_changes.TryCastAs(LogicalType::BIGINT)) {
				pStmt->db->last_changes = row_changes.GetValue<int64_t>();
				pStmt->db->total_changes += row_changes.GetValue<int64_t>();
			}
		}
		if (!sqlite3_display_result(statement_type)) {
			// only SELECT statements return results
			sqlite3_reset(pStmt);
		}
	}
	if (!pStmt->current_chunk || pStmt->current_chunk->size() == 0) {
		return SQLITE_DONE;
	}
	pStmt->current_row++;
	if (pStmt->current_row >= (int32_t)pStmt->current_chunk->size()) {
		// have to fetch again!
		pStmt->current_row = 0;
		if (!pStmt->result->TryFetch(pStmt->current_chunk, pStmt->db->last_error)) {
			pStmt->prepared = nullptr;
			return SQLITE_ERROR;
		}
		if (!pStmt->current_chunk || pStmt->current_chunk->size() == 0) {
			sqlite3_reset(pStmt);
			return SQLITE_DONE;
		}
	}
	return SQLITE_ROW;
}

/* Execute multiple semicolon separated SQL statements
 * and execute the passed callback for each produced result,
 * largely copied from the original sqlite3 source */
int sqlite3_exec(sqlite3 *db,                /* The database on which the SQL executes */
                 const char *zSql,           /* The SQL to be executed */
                 sqlite3_callback xCallback, /* Invoke this callback routine */
                 void *pArg,                 /* First argument to xCallback() */
                 char **pzErrMsg             /* Write error messages here */
) {
	int rc = SQLITE_OK;            /* Return code */
	const char *zLeftover;         /* Tail of unprocessed SQL */
	sqlite3_stmt *pStmt = nullptr; /* The current SQL statement */
	char **azCols = nullptr;       /* Names of result columns */
	char **azVals = nullptr;       /* Result values */

	if (zSql == nullptr) {
		zSql = "";
	}

	while (rc == SQLITE_OK && zSql[0]) {
		int nCol;

		pStmt = nullptr;
		rc = sqlite3_prepare_v2(db, zSql, -1, &pStmt, &zLeftover);
		if (rc != SQLITE_OK) {
			if (pzErrMsg) {
				auto errmsg = sqlite3_errmsg(db);
				*pzErrMsg = errmsg ? sqlite3_strdup(errmsg) : nullptr;
			}
			continue;
		}
		if (!pStmt) {
			/* this happens for a comment or white-space */
			zSql = zLeftover;
			continue;
		}

		nCol = sqlite3_column_count(pStmt);
		azCols = (char **)malloc(nCol * sizeof(const char *));
		azVals = (char **)malloc(nCol * sizeof(const char *));
		if (!azCols || !azVals) {
			goto exec_out;
		}
		for (int i = 0; i < nCol; i++) {
			azCols[i] = (char *)sqlite3_column_name(pStmt, i);
		}

		while (true) {
			rc = sqlite3_step(pStmt);

			/* Invoke the callback function if required */
			if (xCallback && rc == SQLITE_ROW) {
				for (int i = 0; i < nCol; i++) {
					azVals[i] = (char *)sqlite3_column_text(pStmt, i);
					if (!azVals[i] && sqlite3_column_type(pStmt, i) != SQLITE_NULL) {
						fprintf(stderr, "sqlite3_exec: out of memory.\n");
						goto exec_out;
					}
				}
				if (xCallback(pArg, nCol, azVals, azCols)) {
					/* EVIDENCE-OF: R-38229-40159 If the callback function to
					** sqlite3_exec() returns non-zero, then sqlite3_exec() will
					** return SQLITE_ABORT. */
					rc = SQLITE_ABORT;
					sqlite3_finalize(pStmt);
					pStmt = 0;
					fprintf(stderr, "sqlite3_exec: callback returned non-zero. "
					                "Aborting.\n");
					goto exec_out;
				}
			}
			if (rc == SQLITE_DONE) {
				rc = sqlite3_finalize(pStmt);
				pStmt = nullptr;
				zSql = zLeftover;
				while (isspace(zSql[0]))
					zSql++;
				break;
			} else if (rc != SQLITE_ROW) {
				// error
				if (pzErrMsg) {
					auto errmsg = sqlite3_errmsg(db);
					*pzErrMsg = errmsg ? sqlite3_strdup(errmsg) : nullptr;
				}
				goto exec_out;
			}
		}

		sqlite3_free(azCols);
		sqlite3_free(azVals);
		azCols = nullptr;
		azVals = nullptr;
	}

exec_out:
	if (pStmt) {
		sqlite3_finalize(pStmt);
	}
	sqlite3_free(azCols);
	sqlite3_free(azVals);
	if (rc != SQLITE_OK && pzErrMsg && !*pzErrMsg) {
		// error but no error message set
		*pzErrMsg = sqlite3_strdup("Unknown error in DuckDB!");
	}
	return rc;
}

/* Return the text of the SQL that was used to prepare the statement */
const char *sqlite3_sql(sqlite3_stmt *pStmt) {
	return pStmt->query_string.c_str();
}

int sqlite3_column_count(sqlite3_stmt *pStmt) {
	if (!pStmt || !pStmt->prepared) {
		return 0;
	}
	return (int)pStmt->prepared->ColumnCount();
}

////////////////////////////
//     sqlite3_column     //
////////////////////////////
int sqlite3_column_type(sqlite3_stmt *pStmt, int iCol) {
	if (!pStmt || !pStmt->result || !pStmt->current_chunk) {
		return 0;
	}
	if (FlatVector::IsNull(pStmt->current_chunk->data[iCol], pStmt->current_row)) {
		return SQLITE_NULL;
	}
	auto column_type = pStmt->result->types[iCol];
	switch (column_type.id()) {
	case LogicalTypeId::BOOLEAN:
	case LogicalTypeId::TINYINT:
	case LogicalTypeId::SMALLINT:
	case LogicalTypeId::INTEGER:
	case LogicalTypeId::BIGINT: /* TODO: Maybe blob? */
		return SQLITE_INTEGER;
	case LogicalTypeId::FLOAT:
	case LogicalTypeId::DOUBLE:
	case LogicalTypeId::DECIMAL:
		return SQLITE_FLOAT;
	case LogicalTypeId::DATE:
	case LogicalTypeId::TIME:
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_SEC:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::VARCHAR:
	case LogicalTypeId::LIST:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::MAP:
		return SQLITE_TEXT;
	case LogicalTypeId::BLOB:
		return SQLITE_BLOB;
	default:
		// TODO(wangfenjin): agg function don't have type?
		return SQLITE_TEXT;
	}
	return 0;
}

const char *sqlite3_column_name(sqlite3_stmt *pStmt, int N) {
	if (!pStmt || !pStmt->prepared) {
		return nullptr;
	}
	return pStmt->prepared->GetNames()[N].c_str();
}

static bool sqlite3_column_has_value(sqlite3_stmt *pStmt, int iCol, LogicalType target_type, Value &val) {
	if (!pStmt || !pStmt->result || !pStmt->current_chunk) {
		return false;
	}
	if (iCol < 0 || iCol >= (int)pStmt->result->types.size()) {
		return false;
	}
	if (FlatVector::IsNull(pStmt->current_chunk->data[iCol], pStmt->current_row)) {
		return false;
	}
	try {
		val = pStmt->current_chunk->data[iCol].GetValue(pStmt->current_row).CastAs(target_type);
	} catch (...) {
		return false;
	}
	return true;
}

double sqlite3_column_double(sqlite3_stmt *stmt, int iCol) {
	Value val;
	if (!sqlite3_column_has_value(stmt, iCol, LogicalType::DOUBLE, val)) {
		return 0;
	}
	return val.value_.double_;
}

int sqlite3_column_int(sqlite3_stmt *stmt, int iCol) {
	Value val;
	if (!sqlite3_column_has_value(stmt, iCol, LogicalType::INTEGER, val)) {
		return 0;
	}
	return val.value_.integer;
}

sqlite3_int64 sqlite3_column_int64(sqlite3_stmt *stmt, int iCol) {
	Value val;
	if (!sqlite3_column_has_value(stmt, iCol, LogicalType::BIGINT, val)) {
		return 0;
	}
	return val.value_.bigint;
}

const unsigned char *sqlite3_column_text(sqlite3_stmt *pStmt, int iCol) {
	Value val;
	if (!sqlite3_column_has_value(pStmt, iCol, LogicalType::VARCHAR, val)) {
		return nullptr;
	}
	try {
		if (!pStmt->current_text) {
			pStmt->current_text =
			    unique_ptr<sqlite3_string_buffer[]>(new sqlite3_string_buffer[pStmt->result->types.size()]);
		}
		auto &entry = pStmt->current_text[iCol];
		if (!entry.data) {
			// not initialized yet, convert the value and initialize it
			entry.data = unique_ptr<char[]>(new char[val.str_value.size() + 1]);
			memcpy(entry.data.get(), val.str_value.c_str(), val.str_value.size() + 1);
			entry.data_len = val.str_value.length();
		}
		return (const unsigned char *)entry.data.get();
	} catch (...) {
		// memory error!
		return nullptr;
	}
}

const void *sqlite3_column_blob(sqlite3_stmt *pStmt, int iCol) {
	Value val;
	if (!sqlite3_column_has_value(pStmt, iCol, LogicalType::BLOB, val)) {
		return nullptr;
	}
	try {
		if (!pStmt->current_text) {
			pStmt->current_text =
			    unique_ptr<sqlite3_string_buffer[]>(new sqlite3_string_buffer[pStmt->result->types.size()]);
		}
		auto &entry = pStmt->current_text[iCol];
		if (!entry.data) {
			// not initialized yet, convert the value and initialize it
			entry.data = unique_ptr<char[]>(new char[val.str_value.size() + 1]);
			memcpy(entry.data.get(), val.str_value.c_str(), val.str_value.size() + 1);
			entry.data_len = val.str_value.length();
		}
		return (const unsigned char *)entry.data.get();
	} catch (...) {
		// memory error!
		return nullptr;
	}
}

////////////////////////////
//      sqlite3_bind      //
////////////////////////////
int sqlite3_bind_parameter_count(sqlite3_stmt *stmt) {
	if (!stmt) {
		return 0;
	}
	return stmt->prepared->n_param;
}

const char *sqlite3_bind_parameter_name(sqlite3_stmt *stmt, int idx) {
	if (!stmt) {
		return nullptr;
	}
	if (idx < 1 || idx > (int)stmt->prepared->n_param) {
		return nullptr;
	}
	return stmt->bound_names[idx - 1].c_str();
}

int sqlite3_bind_parameter_index(sqlite3_stmt *stmt, const char *zName) {
	if (!stmt || !zName) {
		return 0;
	}
	for (idx_t i = 0; i < stmt->bound_names.size(); i++) {
		if (stmt->bound_names[i] == string(zName)) {
			return i + 1;
		}
	}
	return 0;
}

int sqlite3_internal_bind_value(sqlite3_stmt *stmt, int idx, Value value) {
	if (!stmt || !stmt->prepared || stmt->result) {
		return SQLITE_MISUSE;
	}
	if (idx < 1 || idx > (int)stmt->prepared->n_param) {
		return SQLITE_RANGE;
	}
	stmt->bound_values[idx - 1] = value;
	return SQLITE_OK;
}

int sqlite3_bind_int(sqlite3_stmt *stmt, int idx, int val) {
	return sqlite3_internal_bind_value(stmt, idx, Value::INTEGER(val));
}

int sqlite3_bind_int64(sqlite3_stmt *stmt, int idx, sqlite3_int64 val) {
	return sqlite3_internal_bind_value(stmt, idx, Value::BIGINT(val));
}

int sqlite3_bind_double(sqlite3_stmt *stmt, int idx, double val) {
	return sqlite3_internal_bind_value(stmt, idx, Value::DOUBLE(val));
}

int sqlite3_bind_null(sqlite3_stmt *stmt, int idx) {
	return sqlite3_internal_bind_value(stmt, idx, Value());
}

SQLITE_API int sqlite3_bind_value(sqlite3_stmt *, int, const sqlite3_value *) {
	fprintf(stderr, "sqlite3_bind_value: unsupported.\n");
	return SQLITE_ERROR;
}

int sqlite3_bind_text(sqlite3_stmt *stmt, int idx, const char *val, int length, void (*free_func)(void *)) {
	if (!val) {
		return SQLITE_MISUSE;
	}
	string value;
	if (length < 0) {
		value = string(val);
	} else {
		value = string(val, val + length);
	}
	if (free_func && ((ptrdiff_t)free_func) != -1) {
		free_func((void *)val);
		val = nullptr;
	}
	try {
		return sqlite3_internal_bind_value(stmt, idx, Value(value));
	} catch (std::exception &ex) {
		return SQLITE_ERROR;
	}
}

int sqlite3_bind_blob(sqlite3_stmt *stmt, int idx, const void *val, int length, void (*free_func)(void *)) {
	if (!val) {
		return SQLITE_MISUSE;
	}
	Value blob;
	if (length < 0) {
		blob = Value::BLOB(string((const char *)val));
	} else {
		blob = Value::BLOB((const_data_ptr_t)val, length);
	}
	if (free_func && ((ptrdiff_t)free_func) != -1) {
		free_func((void *)val);
		val = nullptr;
	}
	try {
		return sqlite3_internal_bind_value(stmt, idx, blob);
	} catch (std::exception &ex) {
		return SQLITE_ERROR;
	}
}

SQLITE_API int sqlite3_bind_zeroblob(sqlite3_stmt *stmt, int idx, int length) {
	fprintf(stderr, "sqlite3_bind_zeroblob: unsupported.\n");
	return SQLITE_ERROR;
}

int sqlite3_clear_bindings(sqlite3_stmt *stmt) {
	if (!stmt) {
		return SQLITE_MISUSE;
	}
	return SQLITE_OK;
}

int sqlite3_initialize(void) {
	return SQLITE_OK;
}

int sqlite3_finalize(sqlite3_stmt *pStmt) {
	if (pStmt) {
		if (pStmt->result && !pStmt->result->success) {
			pStmt->db->last_error = string(pStmt->result->error);
			delete pStmt;
			return SQLITE_ERROR;
		}

		delete pStmt;
	}
	return SQLITE_OK;
}

/*
** Some systems have stricmp().  Others have strcasecmp().  Because
** there is no consistency, we will define our own.
**
** IMPLEMENTATION-OF: R-30243-02494 The sqlite3_stricmp() and
** sqlite3_strnicmp() APIs allow applications and extensions to compare
** the contents of two buffers containing UTF-8 strings in a
** case-independent fashion, using the same definition of "case
** independence" that SQLite uses internally when comparing identifiers.
*/

const unsigned char sqlite3UpperToLower[] = {
    0,   1,   2,   3,   4,   5,   6,   7,   8,   9,   10,  11,  12,  13,  14,  15,  16,  17,  18,  19,  20,  21,
    22,  23,  24,  25,  26,  27,  28,  29,  30,  31,  32,  33,  34,  35,  36,  37,  38,  39,  40,  41,  42,  43,
    44,  45,  46,  47,  48,  49,  50,  51,  52,  53,  54,  55,  56,  57,  58,  59,  60,  61,  62,  63,  64,  97,
    98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119,
    120, 121, 122, 91,  92,  93,  94,  95,  96,  97,  98,  99,  100, 101, 102, 103, 104, 105, 106, 107, 108, 109,
    110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 120, 121, 122, 123, 124, 125, 126, 127, 128, 129, 130, 131,
    132, 133, 134, 135, 136, 137, 138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 149, 150, 151, 152, 153,
    154, 155, 156, 157, 158, 159, 160, 161, 162, 163, 164, 165, 166, 167, 168, 169, 170, 171, 172, 173, 174, 175,
    176, 177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187, 188, 189, 190, 191, 192, 193, 194, 195, 196, 197,
    198, 199, 200, 201, 202, 203, 204, 205, 206, 207, 208, 209, 210, 211, 212, 213, 214, 215, 216, 217, 218, 219,
    220, 221, 222, 223, 224, 225, 226, 227, 228, 229, 230, 231, 232, 233, 234, 235, 236, 237, 238, 239, 240, 241,
    242, 243, 244, 245, 246, 247, 248, 249, 250, 251, 252, 253, 254, 255};

int sqlite3StrICmp(const char *zLeft, const char *zRight) {
	unsigned char *a, *b;
	int c;
	a = (unsigned char *)zLeft;
	b = (unsigned char *)zRight;
	for (;;) {
		c = (int)sqlite3UpperToLower[*a] - (int)sqlite3UpperToLower[*b];
		if (c || *a == 0)
			break;
		a++;
		b++;
	}
	return c;
}

SQLITE_API int sqlite3_stricmp(const char *zLeft, const char *zRight) {
	if (zLeft == 0) {
		return zRight ? -1 : 0;
	} else if (zRight == 0) {
		return 1;
	}
	return sqlite3StrICmp(zLeft, zRight);
}

SQLITE_API int sqlite3_strnicmp(const char *zLeft, const char *zRight, int N) {
	unsigned char *a, *b;
	if (zLeft == 0) {
		return zRight ? -1 : 0;
	} else if (zRight == 0) {
		return 1;
	}
	a = (unsigned char *)zLeft;
	b = (unsigned char *)zRight;
	while (N-- > 0 && *a != 0 && sqlite3UpperToLower[*a] == sqlite3UpperToLower[*b]) {
		a++;
		b++;
	}
	return N < 0 ? 0 : sqlite3UpperToLower[*a] - sqlite3UpperToLower[*b];
}

char *sqlite3_strdup(const char *str) {
	char *result = (char *)sqlite3_malloc64(strlen(str) + 1);
	strcpy(result, str);
	return result;
}

void *sqlite3_malloc64(sqlite3_uint64 n) {
	return malloc(n);
}

void sqlite3_free(void *pVoid) {
	free(pVoid);
}

void *sqlite3_malloc(int n) {
	return sqlite3_malloc64(n);
}

void *sqlite3_realloc(void *ptr, int n) {
	return sqlite3_realloc64(ptr, n);
}

void *sqlite3_realloc64(void *ptr, sqlite3_uint64 n) {
	return realloc(ptr, n);
}

// TODO: stub
int sqlite3_config(int i, ...) {
	return SQLITE_OK;
}

int sqlite3_errcode(sqlite3 *db) {
	if (!db) {
		return SQLITE_NOMEM;
	}
	// return db->last_error.empty() ? SQLITE_OK : SQLITE_ERROR;
	return db->errCode; //! We should return the exact error code
}

int sqlite3_extended_errcode(sqlite3 *db) {
	return sqlite3_errcode(db);
}

const char *sqlite3_errmsg(sqlite3 *db) {
	if (!db) {
		return "";
	}
	return db->last_error.c_str();
}

void sqlite3_interrupt(sqlite3 *db) {
	if (db) {
		db->con->Interrupt();
	}
}

const char *sqlite3_libversion(void) {
	return DuckDB::LibraryVersion();
}

const char *sqlite3_sourceid(void) {
	return DuckDB::SourceID();
}

int sqlite3_reset(sqlite3_stmt *stmt) {
	if (stmt) {
		stmt->result = nullptr;
		stmt->current_chunk = nullptr;
	}
	return SQLITE_OK;
}

// support functions for shell.c
// most are dummies, we don't need them really

int sqlite3_db_status(sqlite3 *, int op, int *pCur, int *pHiwtr, int resetFlg) {
	fprintf(stderr, "sqlite3_db_status: unsupported.\n");
	return -1;
}

int sqlite3_changes(sqlite3 *db) {
	return db->last_changes;
}

int sqlite3_total_changes(sqlite3 *db) {
	return db->total_changes;
}

SQLITE_API sqlite3_int64 sqlite3_last_insert_rowid(sqlite3 *db) {
	return SQLITE_ERROR;
}

// some code borrowed from sqlite
// its probably best to match its behavior

typedef uint8_t u8;

/*
** Token types used by the sqlite3_complete() routine.  See the header
** comments on that procedure for additional information.
*/
#define tkSEMI  0
#define tkWS    1
#define tkOTHER 2

const unsigned char sqlite3CtypeMap[256] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 00..07    ........ */
    0x00, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00, /* 08..0f    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 10..17    ........ */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 18..1f    ........ */
    0x01, 0x00, 0x80, 0x00, 0x40, 0x00, 0x00, 0x80, /* 20..27     !"#$%&' */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 28..2f    ()*+,-./ */
    0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, 0x0c, /* 30..37    01234567 */
    0x0c, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* 38..3f    89:;<=>? */

    0x00, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x0a, 0x02, /* 40..47    @ABCDEFG */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 48..4f    HIJKLMNO */
    0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, /* 50..57    PQRSTUVW */
    0x02, 0x02, 0x02, 0x80, 0x00, 0x00, 0x00, 0x40, /* 58..5f    XYZ[\]^_ */
    0x80, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x2a, 0x22, /* 60..67    `abcdefg */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 68..6f    hijklmno */
    0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, 0x22, /* 70..77    pqrstuvw */
    0x22, 0x22, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, /* 78..7f    xyz{|}~. */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 80..87    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 88..8f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 90..97    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* 98..9f    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a0..a7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* a8..af    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b0..b7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* b8..bf    ........ */

    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c0..c7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* c8..cf    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d0..d7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* d8..df    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e0..e7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* e8..ef    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, /* f0..f7    ........ */
    0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40, 0x40  /* f8..ff    ........ */
};

// TODO this can probably be simplified
#define IdChar(C) ((sqlite3CtypeMap[(unsigned char)C] & 0x46) != 0)

int sqlite3_complete(const char *zSql) {
	u8 state = 0; /* Current state, using numbers defined in header comment */
	u8 token;     /* Value of the next token */

	/* If triggers are not supported by this compile then the statement machine
	 ** used to detect the end of a statement is much simpler
	 */
	static const u8 trans[3][3] = {
	    /* Token:           */
	    /* State:       **  SEMI  WS  OTHER */
	    /* 0 INVALID: */ {
	        1,
	        0,
	        2,
	    },
	    /* 1   START: */
	    {
	        1,
	        1,
	        2,
	    },
	    /* 2  NORMAL: */
	    {
	        1,
	        2,
	        2,
	    },
	};

	while (*zSql) {
		switch (*zSql) {
		case ';': { /* A semicolon */
			token = tkSEMI;
			break;
		}
		case ' ':
		case '\r':
		case '\t':
		case '\n':
		case '\f': { /* White space is ignored */
			token = tkWS;
			break;
		}
		case '/': { /* C-style comments */
			if (zSql[1] != '*') {
				token = tkOTHER;
				break;
			}
			zSql += 2;
			while (zSql[0] && (zSql[0] != '*' || zSql[1] != '/')) {
				zSql++;
			}
			if (zSql[0] == 0)
				return 0;
			zSql++;
			token = tkWS;
			break;
		}
		case '-': { /* SQL-style comments from "--" to end of line */
			if (zSql[1] != '-') {
				token = tkOTHER;
				break;
			}
			while (*zSql && *zSql != '\n') {
				zSql++;
			}
			if (*zSql == 0)
				return state == 1;
			token = tkWS;
			break;
		}
		case '[': { /* Microsoft-style identifiers in [...] */
			zSql++;
			while (*zSql && *zSql != ']') {
				zSql++;
			}
			if (*zSql == 0)
				return 0;
			token = tkOTHER;
			break;
		}
		case '`': /* Grave-accent quoted symbols used by MySQL */
		case '"': /* single- and double-quoted strings */
		case '\'': {
			int c = *zSql;
			zSql++;
			while (*zSql && *zSql != c) {
				zSql++;
			}
			if (*zSql == 0)
				return 0;
			token = tkOTHER;
			break;
		}
		default: {

			if (IdChar((u8)*zSql)) {
				/* Keywords and unquoted identifiers */
				int nId;
				for (nId = 1; IdChar(zSql[nId]); nId++) {
				}
				token = tkOTHER;

				zSql += nId - 1;
			} else {
				/* Operators and special symbols */
				token = tkOTHER;
			}
			break;
		}
		}
		state = trans[state][token];
		zSql++;
	}
	return state == 1;
}

// checks if input ends with ;
int sqlite3_complete_old(const char *sql) {
	fprintf(stderr, "sqlite3_complete: unsupported. '%s'\n", sql);
	return -1;
}

// length of varchar or blob value
int sqlite3_column_bytes(sqlite3_stmt *pStmt, int iCol) {
	// fprintf(stderr, "sqlite3_column_bytes: unsupported.\n");
	return pStmt->current_text[iCol].data_len;
	// return -1;
}

sqlite3_value *sqlite3_column_value(sqlite3_stmt *, int iCol) {
	fprintf(stderr, "sqlite3_column_value: unsupported.\n");
	return nullptr;
}

int sqlite3_db_config(sqlite3 *, int op, ...) {
	fprintf(stderr, "sqlite3_db_config: unsupported.\n");
	return -1;
}

int sqlite3_get_autocommit(sqlite3 *db) {
	return db->con->context->transaction.IsAutoCommit();
}

int sqlite3_limit(sqlite3 *, int id, int newVal) {
	fprintf(stderr, "sqlite3_limit: unsupported.\n");
	return -1;
}

int sqlite3_stmt_readonly(sqlite3_stmt *pStmt) {
	fprintf(stderr, "sqlite3_stmt_readonly: unsupported.\n");
	return -1;
}

// TODO pretty easy schema lookup
int sqlite3_table_column_metadata(sqlite3 *db,             /* Connection handle */
                                  const char *zDbName,     /* Database name or NULL */
                                  const char *zTableName,  /* Table name */
                                  const char *zColumnName, /* Column name */
                                  char const **pzDataType, /* OUTPUT: Declared data type */
                                  char const **pzCollSeq,  /* OUTPUT: Collation sequence name */
                                  int *pNotNull,           /* OUTPUT: True if NOT NULL constraint exists */
                                  int *pPrimaryKey,        /* OUTPUT: True if column part of PK */
                                  int *pAutoinc            /* OUTPUT: True if column is auto-increment */
) {
	fprintf(stderr, "sqlite3_table_column_metadata: unsupported.\n");
	return -1;
}

const char *sqlite3_column_decltype(sqlite3_stmt *pStmt, int iCol) {
	if (!pStmt || !pStmt->prepared) {
		return NULL;
	}
	auto column_type = pStmt->prepared->GetTypes()[iCol];
	switch (column_type.id()) {
	case LogicalTypeId::BOOLEAN:
		return "BOOLEAN";
	case LogicalTypeId::TINYINT:
		return "TINYINT";
	case LogicalTypeId::SMALLINT:
		return "SMALLINT";
	case LogicalTypeId::INTEGER:
		return "INTEGER";
	case LogicalTypeId::BIGINT:
		return "BIGINT";
	case LogicalTypeId::FLOAT:
		return "FLOAT";
	case LogicalTypeId::DOUBLE:
		return "DOUBLE";
	case LogicalTypeId::DECIMAL:
		return "DECIMAL";
	case LogicalTypeId::DATE:
		return "DATE";
	case LogicalTypeId::TIME:
		return "TIME";
	case LogicalTypeId::TIMESTAMP:
	case LogicalTypeId::TIMESTAMP_NS:
	case LogicalTypeId::TIMESTAMP_MS:
	case LogicalTypeId::TIMESTAMP_SEC:
		return "TIMESTAMP";
	case LogicalTypeId::VARCHAR:
		return "VARCHAR";
	case LogicalTypeId::LIST:
		return "LIST";
	case LogicalTypeId::MAP:
		return "MAP";
	case LogicalTypeId::STRUCT:
		return "STRUCT";
	case LogicalTypeId::BLOB:
		return "BLOB";
	default:
		return NULL;
	}
	return NULL;
}

int sqlite3_status64(int op, sqlite3_int64 *pCurrent, sqlite3_int64 *pHighwater, int resetFlag) {
	fprintf(stderr, "sqlite3_status64: unsupported.\n");
	return -1;
}

int sqlite3_status64(sqlite3 *, int op, int *pCur, int *pHiwtr, int resetFlg) {
	fprintf(stderr, "sqlite3_status64: unsupported.\n");
	return -1;
}

int sqlite3_stmt_status(sqlite3_stmt *, int op, int resetFlg) {
	fprintf(stderr, "sqlite3_stmt_status: unsupported.\n");
	return -1;
}

int sqlite3_file_control(sqlite3 *, const char *zDbName, int op, void *) {
	fprintf(stderr, "sqlite3_file_control: unsupported.\n");
	return -1;
}

int sqlite3_declare_vtab(sqlite3 *, const char *zSQL) {
	fprintf(stderr, "sqlite3_declare_vtab: unsupported.\n");
	return -1;
}

const char *sqlite3_vtab_collation(sqlite3_index_info *, int) {
	fprintf(stderr, "sqlite3_vtab_collation: unsupported.\n");
	return nullptr;
}

int sqlite3_sleep(int) {
	fprintf(stderr, "sqlite3_sleep: unsupported.\n");
	return -1;
}

int sqlite3_busy_timeout(sqlite3 *, int ms) {
	fprintf(stderr, "sqlite3_busy_timeout: unsupported.\n");
	return -1;
}

// unlikely to be supported

int sqlite3_trace_v2(sqlite3 *, unsigned uMask, int (*xCallback)(unsigned, void *, void *, void *), void *pCtx) {
	fprintf(stderr, "sqlite3_trace_v2: unsupported.\n");
	return -1;
}

int sqlite3_test_control(int op, ...) {
	fprintf(stderr, "sqlite3_test_control: unsupported.\n");
	return -1;
}

int sqlite3_enable_load_extension(sqlite3 *db, int onoff) {
	// fprintf(stderr, "sqlite3_enable_load_extension: unsupported.\n");
	return -1;
}

int sqlite3_load_extension(sqlite3 *db,       /* Load the extension into this database connection */
                           const char *zFile, /* Name of the shared library containing extension */
                           const char *zProc, /* Entry point.  Derived from zFile if 0 */
                           char **pzErrMsg    /* Put error message here if not 0 */
) {
	// fprintf(stderr, "sqlite3_load_extension: unsupported.\n");
	return -1;
}

int sqlite3_create_module(sqlite3 *db,             /* SQLite connection to register module with */
                          const char *zName,       /* Name of the module */
                          const sqlite3_module *p, /* Methods for the module */
                          void *pClientData        /* Client data for xCreate/xConnect */
) {
	// fprintf(stderr, "sqlite3_create_module: unsupported.\n");
	return -1;
}

int sqlite3_create_function(sqlite3 *db, const char *zFunctionName, int nArg, int eTextRep, void *pApp,
                            void (*xFunc)(sqlite3_context *, int, sqlite3_value **),
                            void (*xStep)(sqlite3_context *, int, sqlite3_value **),
                            void (*xFinal)(sqlite3_context *)) {
	if ((!xFunc && !xStep && !xFinal) || !zFunctionName || nArg < -1) {
		return SQLITE_MISUSE;
	}
	string fname = string(zFunctionName);

	// Scalar function
	if (xFunc) {
		auto udf_sqlite3 = SQLiteUDFWrapper::CreateSQLiteScalarFunction(xFunc, db, pApp);
		LogicalType varargs = LogicalType::INVALID;
		if (nArg == -1) {
			varargs = LogicalType::ANY;
			nArg = 0;
		}

		vector<LogicalType> argv_types(nArg);
		for (idx_t i = 0; i < (idx_t)nArg; ++i) {
			argv_types[i] = LogicalType::ANY;
		}

		UDFWrapper::RegisterFunction(fname, argv_types, LogicalType::VARCHAR, udf_sqlite3, *(db->con->context),
		                             varargs);

		return SQLITE_OK;
	}

	return SQLITE_MISUSE;
}

int sqlite3_create_function_v2(sqlite3 *db, const char *zFunctionName, int nArg, int eTextRep, void *pApp,
                               void (*xFunc)(sqlite3_context *, int, sqlite3_value **),
                               void (*xStep)(sqlite3_context *, int, sqlite3_value **),
                               void (*xFinal)(sqlite3_context *), void (*xDestroy)(void *)) {
	return -1;
}

int sqlite3_set_authorizer(sqlite3 *, int (*xAuth)(void *, int, const char *, const char *, const char *, const char *),
                           void *pUserData) {
	fprintf(stderr, "sqlite3_set_authorizer: unsupported.\n");
	return -1;
}

// needed in shell timer
static int unixCurrentTimeInt64(sqlite3_vfs *NotUsed, sqlite3_int64 *piNow) {
	using namespace std::chrono;
	*piNow = (sqlite3_int64)duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	return SQLITE_OK;
}

static sqlite3_vfs static_sqlite3_virtual_file_systems[] = {{
    3,                    // int iVersion;            /* Structure version number (currently 3) */
    0,                    // int szOsFile;            /* Size of subclassed sqlite3_file */
    0,                    // int mxPathname;          /* Maximum file pathname length */
    nullptr,              // sqlite3_vfs *pNext;      /* Next registered VFS */
    "dummy",              // const char *zName;       /* Name of this virtual file system */
    nullptr,              // void *pAppData;          /* Pointer to application-specific data */
    nullptr,              // int (*xOpen)(sqlite3_vfs*, const char *zName, sqlite3_file*, int flags, int *pOutFlags);
    nullptr,              // int (*xDelete)(sqlite3_vfs*, const char *zName, int syncDir);
    nullptr,              // int (*xAccess)(sqlite3_vfs*, const char *zName, int flags, int *pResOut);
    nullptr,              // int (*xFullPathname)(sqlite3_vfs*, const char *zName, int nOut, char *zOut);
    nullptr,              // void *(*xDlOpen)(sqlite3_vfs*, const char *zFilename);
    nullptr,              // void (*xDlError)(sqlite3_vfs*, int nByte, char *zErrMsg);
    nullptr,              // void (*(*xDlSym)(sqlite3_vfs*,void*, const char *zSymbol))(void);
    nullptr,              // void (*xDlClose)(sqlite3_vfs*, void*);
    nullptr,              // int (*xRandomness)(sqlite3_vfs*, int nByte, char *zOut);
    nullptr,              // int (*xSleep)(sqlite3_vfs*, int microseconds);
    nullptr,              // int (*xCurrentTime)(sqlite3_vfs*, double*);
    nullptr,              // int (*xGetLastError)(sqlite3_vfs*, int, char *);
    unixCurrentTimeInt64, // int (*xCurrentTimeInt64)(sqlite3_vfs*, sqlite3_int64*);
    nullptr,              // int (*xSetSystemCall)(sqlite3_vfs*, const char *zName, sqlite3_syscall_ptr);
    nullptr,              // sqlite3_syscall_ptr (*xGetSystemCall)(sqlite3_vfs*, const char *zName);
    nullptr               // const char *(*xNextSystemCall)(sqlite3_vfs*, const char *zName);
}};

// virtual file system, providing some dummies to avoid crashes
sqlite3_vfs *sqlite3_vfs_find(const char *zVfsName) {
	// return a dummy because the shell does not check the return code.
	return static_sqlite3_virtual_file_systems;
}

int sqlite3_vfs_register(sqlite3_vfs *, int makeDflt) {
	// fprintf(stderr, "sqlite3_vfs_register: unsupported.\n");
	return -1;
}

// backups, unused

int sqlite3_backup_step(sqlite3_backup *p, int nPage) {
	fprintf(stderr, "sqlite3_backup_step: unsupported.\n");
	return -1;
}

int sqlite3_backup_finish(sqlite3_backup *p) {
	fprintf(stderr, "sqlite3_backup_finish: unsupported.\n");
	return -1;
}

sqlite3_backup *sqlite3_backup_init(sqlite3 *pDest,         /* Destination database handle */
                                    const char *zDestName,  /* Destination database name */
                                    sqlite3 *pSource,       /* Source database handle */
                                    const char *zSourceName /* Source database name */
) {
	fprintf(stderr, "sqlite3_backup_init: unsupported.\n");
	return nullptr;
}

// UDF support stuff, unused for now. These cannot be called as create_function above is disabled

SQLITE_API sqlite3 *sqlite3_context_db_handle(sqlite3_context *) {
	return nullptr;
}

void *sqlite3_user_data(sqlite3_context *context) {
	assert(context);
	return context->pFunc.pUserData;
}

#ifdef _WIN32
#include <windows.h>

static void *sqlite3MallocZero(size_t n) {
	auto res = sqlite3_malloc(n);
	assert(res);
	memset(res, 0, n);
	return res;
}

static LPWSTR winUtf8ToUnicode(const char *zText) {
	int nChar;
	LPWSTR zWideText;

	nChar = MultiByteToWideChar(CP_UTF8, 0, zText, -1, NULL, 0);
	if (nChar == 0) {
		return 0;
	}
	zWideText = (LPWSTR)sqlite3MallocZero(nChar * sizeof(WCHAR));
	if (zWideText == 0) {
		return 0;
	}
	nChar = MultiByteToWideChar(CP_UTF8, 0, zText, -1, zWideText, nChar);
	if (nChar == 0) {
		sqlite3_free(zWideText);
		zWideText = 0;
	}
	return zWideText;
}

static char *winUnicodeToMbcs(LPCWSTR zWideText, int useAnsi) {
	int nByte;
	char *zText;
	int codepage = useAnsi ? CP_ACP : CP_OEMCP;

	nByte = WideCharToMultiByte(codepage, 0, zWideText, -1, 0, 0, 0, 0);
	if (nByte == 0) {
		return 0;
	}
	zText = (char *)sqlite3MallocZero(nByte);
	if (zText == 0) {
		return 0;
	}
	nByte = WideCharToMultiByte(codepage, 0, zWideText, -1, zText, nByte, 0, 0);
	if (nByte == 0) {
		sqlite3_free(zText);
		zText = 0;
	}
	return zText;
}

static char *winUtf8ToMbcs(const char *zText, int useAnsi) {
	char *zTextMbcs;
	LPWSTR zTmpWide;

	zTmpWide = winUtf8ToUnicode(zText);
	if (zTmpWide == 0) {
		return 0;
	}
	zTextMbcs = winUnicodeToMbcs(zTmpWide, useAnsi);
	sqlite3_free(zTmpWide);
	return zTextMbcs;
}

SQLITE_API char *sqlite3_win32_utf8_to_mbcs_v2(const char *zText, int useAnsi) {
	return winUtf8ToMbcs(zText, useAnsi);
}

LPWSTR sqlite3_win32_utf8_to_unicode(const char *zText) {
	return winUtf8ToUnicode(zText);
}

static LPWSTR winMbcsToUnicode(const char *zText, int useAnsi) {
	int nByte;
	LPWSTR zMbcsText;
	int codepage = useAnsi ? CP_ACP : CP_OEMCP;

	nByte = MultiByteToWideChar(codepage, 0, zText, -1, NULL, 0) * sizeof(WCHAR);
	if (nByte == 0) {
		return 0;
	}
	zMbcsText = (LPWSTR)sqlite3MallocZero(nByte * sizeof(WCHAR));
	if (zMbcsText == 0) {
		return 0;
	}
	nByte = MultiByteToWideChar(codepage, 0, zText, -1, zMbcsText, nByte);
	if (nByte == 0) {
		sqlite3_free(zMbcsText);
		zMbcsText = 0;
	}
	return zMbcsText;
}

static char *winUnicodeToUtf8(LPCWSTR zWideText) {
	int nByte;
	char *zText;

	nByte = WideCharToMultiByte(CP_UTF8, 0, zWideText, -1, 0, 0, 0, 0);
	if (nByte == 0) {
		return 0;
	}
	zText = (char *)sqlite3MallocZero(nByte);
	if (zText == 0) {
		return 0;
	}
	nByte = WideCharToMultiByte(CP_UTF8, 0, zWideText, -1, zText, nByte, 0, 0);
	if (nByte == 0) {
		sqlite3_free(zText);
		zText = 0;
	}
	return zText;
}

static char *winMbcsToUtf8(const char *zText, int useAnsi) {
	char *zTextUtf8;
	LPWSTR zTmpWide;

	zTmpWide = winMbcsToUnicode(zText, useAnsi);
	if (zTmpWide == 0) {
		return 0;
	}
	zTextUtf8 = winUnicodeToUtf8(zTmpWide);
	sqlite3_free(zTmpWide);
	return zTextUtf8;
}

SQLITE_API char *sqlite3_win32_mbcs_to_utf8_v2(const char *zText, int useAnsi) {
	return winMbcsToUtf8(zText, useAnsi);
}

SQLITE_API char *sqlite3_win32_unicode_to_utf8(LPCWSTR zWideText) {
	return winUnicodeToUtf8(zWideText);
}

#endif

// TODO complain
SQLITE_API void sqlite3_result_blob(sqlite3_context *context, const void *blob, int n_bytes, void (*)(void *)) {
	if (!blob) {
		context->isError = SQLITE_MISUSE;
		return;
	}
	context->result.type = SQLiteTypeValue::BLOB;
	context->result.n = n_bytes;
	string_t str = string_t((const char *)blob, n_bytes);
	context->result.str_t = str;
}

SQLITE_API void sqlite3_result_blob64(sqlite3_context *, const void *, sqlite3_uint64, void (*)(void *)) {
}

SQLITE_API void sqlite3_result_double(sqlite3_context *context, double val) {
	context->result.u.r = val;
	context->result.type = SQLiteTypeValue::FLOAT;
}

SQLITE_API void sqlite3_result_error(sqlite3_context *context, const char *msg, int n_bytes) {
	context->isError = SQLITE_ERROR;
	sqlite3_result_text(context, msg, n_bytes, nullptr);
}

SQLITE_API void sqlite3_result_error16(sqlite3_context *, const void *, int) {
}

SQLITE_API void sqlite3_result_error_toobig(sqlite3_context *) {
}

SQLITE_API void sqlite3_result_error_nomem(sqlite3_context *) {
}

SQLITE_API void sqlite3_result_error_code(sqlite3_context *, int) {
}

SQLITE_API void sqlite3_result_int(sqlite3_context *context, int val) {
	sqlite3_result_int64(context, val);
}

SQLITE_API void sqlite3_result_int64(sqlite3_context *context, sqlite3_int64 val) {
	context->result.u.i = val;
	context->result.type = SQLiteTypeValue::INTEGER;
}

SQLITE_API void sqlite3_result_null(sqlite3_context *context) {
	context->result.type = SQLiteTypeValue::NULL_VALUE;
}

SQLITE_API void sqlite3_result_text(sqlite3_context *context, const char *str_c, int n_chars, void (*)(void *)) {
	if (!str_c) {
		context->isError = SQLITE_MISUSE;
		return;
	}

	auto utf_type = Utf8Proc::Analyze(str_c, n_chars);
	if (utf_type == UnicodeType::INVALID) {
		context->isError = SQLITE_MISUSE;
		return;
	}
	context->result.type = SQLiteTypeValue::TEXT;
	context->result.n = n_chars;
	string_t str = string_t(str_c, n_chars);
	context->result.str_t = str;
}

SQLITE_API void sqlite3_result_text64(sqlite3_context *, const char *, sqlite3_uint64, void (*)(void *),
                                      unsigned char encoding) {
}

SQLITE_API void sqlite3_result_text16(sqlite3_context *, const void *, int, void (*)(void *)) {
}

SQLITE_API void sqlite3_result_text16le(sqlite3_context *, const void *, int, void (*)(void *)) {
}

SQLITE_API void sqlite3_result_text16be(sqlite3_context *, const void *, int, void (*)(void *)) {
}

SQLITE_API void sqlite3_result_value(sqlite3_context *, sqlite3_value *) {
}

SQLITE_API void sqlite3_result_pointer(sqlite3_context *, void *, const char *, void (*)(void *)) {
}

SQLITE_API void sqlite3_result_zeroblob(sqlite3_context *, int n) {
}

SQLITE_API int sqlite3_result_zeroblob64(sqlite3_context *, sqlite3_uint64 n) {
	return -1;
}

// TODO complain
const void *sqlite3_value_blob(sqlite3_value *pVal) {
	return sqlite3_value_text(pVal);
}

double sqlite3_value_double(sqlite3_value *pVal) {
	if (!pVal) {
		pVal->db->errCode = SQLITE_MISUSE;
		return 0.0;
	}
	switch (pVal->type) {
	case SQLiteTypeValue::FLOAT:
		return pVal->u.r;
	case SQLiteTypeValue::INTEGER:
		return (double)pVal->u.i;
	case SQLiteTypeValue::TEXT:
	case SQLiteTypeValue::BLOB:
		double res;
		if (TryCast::Operation<string_t, double>(pVal->str_t, res)) {
			return res;
		}
		break;
	default:
		break;
	}
	pVal->db->errCode = SQLITE_MISMATCH;
	return 0.0;
}

int sqlite3_value_int(sqlite3_value *pVal) {
	int64_t res = sqlite3_value_int64(pVal);
	if (res >= NumericLimits<int>::Minimum() && res <= NumericLimits<int>::Maximum()) {
		return res;
	}
	pVal->db->errCode = SQLITE_MISMATCH;
	return 0;
}

sqlite3_int64 sqlite3_value_int64(sqlite3_value *pVal) {
	if (!pVal) {
		pVal->db->errCode = SQLITE_MISUSE;
		return 0;
	}
	int64_t res;
	switch (pVal->type) {
	case SQLiteTypeValue::INTEGER:
		return pVal->u.i;
	case SQLiteTypeValue::FLOAT:
		if (TryCast::Operation<double, int64_t>(pVal->u.r, res)) {
			return res;
		}
		break;
	case SQLiteTypeValue::TEXT:
	case SQLiteTypeValue::BLOB:
		if (TryCast::Operation<string_t, int64_t>(pVal->str_t, res)) {
			return res;
		}
		break;
	default:
		break;
	}
	pVal->db->errCode = SQLITE_MISMATCH;
	return 0;
}

void *sqlite3_value_pointer(sqlite3_value *, const char *) {
	return nullptr;
}

const unsigned char *sqlite3_value_text(sqlite3_value *pVal) {
	if (!pVal) {
		pVal->db->errCode = SQLITE_MISUSE;
		return nullptr;
	}

	if (pVal->type == SQLiteTypeValue::TEXT || pVal->type == SQLiteTypeValue::BLOB) {
		// We need don't need to append \0 at the end of string
		// To know the number of chars or bytes, call function sqlite3_value_bytes()
		return (const unsigned char *)pVal->str_t.GetDataUnsafe();
	}

	if (pVal->type == SQLiteTypeValue::INTEGER || pVal->type == SQLiteTypeValue::FLOAT) {
		Value value = (pVal->type == SQLiteTypeValue::INTEGER) ? Value::BIGINT(pVal->u.i) : Value::DOUBLE(pVal->u.r);
		if (value.TryCastAs(LogicalType::VARCHAR) == false) {
			pVal->db->errCode = SQLITE_NOMEM;
			return nullptr;
		}
		pVal->zMalloc = (char *)malloc(sizeof(char) * value.str_value.size());
		if (!pVal->zMalloc) {
			pVal->db->errCode = SQLITE_NOMEM;
			return nullptr;
		}
		pVal->szMalloc = value.str_value.size();
		memcpy(pVal->zMalloc, value.str_value.c_str(), pVal->szMalloc);
		pVal->str_t = string_t(pVal->zMalloc, pVal->szMalloc);
		pVal->n = pVal->str_t.GetSize();
		pVal->type = SQLiteTypeValue::TEXT;
		return (const unsigned char *)pVal->str_t.GetDataUnsafe();
	}
	if (pVal->type == SQLiteTypeValue::NULL_VALUE) {
		return nullptr;
	}
	pVal->db->errCode = SQLITE_MISMATCH;
	return nullptr;
}

SQLITE_API const void *sqlite3_value_text16(sqlite3_value *) {
	return nullptr;
}

SQLITE_API const void *sqlite3_value_text16le(sqlite3_value *) {
	return nullptr;
}

SQLITE_API const void *sqlite3_value_text16be(sqlite3_value *) {
	return nullptr;
}

SQLITE_API int sqlite3_value_bytes(sqlite3_value *pVal) {
	if (pVal->type == SQLiteTypeValue::TEXT || pVal->type == SQLiteTypeValue::BLOB) {
		return pVal->n;
	}
	return 0;
}

SQLITE_API int sqlite3_value_bytes16(sqlite3_value *) {
	return 0;
}

SQLITE_API int sqlite3_value_type(sqlite3_value *pVal) {
	return (int)pVal->type;
}

SQLITE_API int sqlite3_value_numeric_type(sqlite3_value *) {
	return 0;
}

SQLITE_API int sqlite3_value_nochange(sqlite3_value *) {
	return 0;
}

SQLITE_API void *sqlite3_aggregate_context(sqlite3_context *, int nBytes) {
	fprintf(stderr, "sqlite3_aggregate_context: unsupported.\n");

	return nullptr;
}

SQLITE_API int sqlite3_create_collation(sqlite3 *, const char *zName, int eTextRep, void *pArg,
                                        int (*xCompare)(void *, int, const void *, int, const void *)) {
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_create_window_function(sqlite3 *db, const char *zFunctionName, int nArg, int eTextRep,
                                              void *pApp, void (*xStep)(sqlite3_context *, int, sqlite3_value **),
                                              void (*xFinal)(sqlite3_context *), void (*xValue)(sqlite3_context *),
                                              void (*xInverse)(sqlite3_context *, int, sqlite3_value **),
                                              void (*xDestroy)(void *)) {
	// commented for now because such error message prevents the shell-test.py to pass
	//	fprintf(stderr, "sqlite3_create_window_function: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API sqlite3 *sqlite3_db_handle(sqlite3_stmt *s) {
	return s->db;
}

SQLITE_API char *sqlite3_expanded_sql(sqlite3_stmt *pStmt) {
	fprintf(stderr, "sqlite3_expanded_sql: unsupported.\n");
	return nullptr;
}

SQLITE_API int sqlite3_keyword_check(const char *str, int len) {
	return Parser::IsKeyword(std::string(str, len));
}

SQLITE_API int sqlite3_keyword_count(void) {
	fprintf(stderr, "sqlite3_keyword_count: unsupported.\n");
	return 0;
}

SQLITE_API int sqlite3_keyword_name(int, const char **, int *) {
	fprintf(stderr, "sqlite3_keyword_name: unsupported.\n");
	return 0;
}

SQLITE_API void sqlite3_progress_handler(sqlite3 *, int, int (*)(void *), void *) {
	fprintf(stderr, "sqlite3_progress_handler: unsupported.\n");
}

SQLITE_API int sqlite3_stmt_isexplain(sqlite3_stmt *pStmt) {
	if (!pStmt || !pStmt->prepared) {
		return 0;
	}
	return pStmt->prepared->GetStatementType() == StatementType::EXPLAIN_STATEMENT;
}

SQLITE_API int sqlite3_vtab_config(sqlite3 *, int op, ...) {
	fprintf(stderr, "sqlite3_vtab_config: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_busy_handler(sqlite3 *, int (*)(void *, int), void *) {
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_get_table(sqlite3 *db,       /* An open database */
                                 const char *zSql,  /* SQL to be evaluated */
                                 char ***pazResult, /* Results of the query */
                                 int *pnRow,        /* Number of result rows written here */
                                 int *pnColumn,     /* Number of result columns written here */
                                 char **pzErrmsg    /* Error msg written here */
) {
	fprintf(stderr, "sqlite3_get_table: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API void sqlite3_free_table(char **result) {
	fprintf(stderr, "sqlite3_free_table: unsupported.\n");
}

SQLITE_API int sqlite3_prepare(sqlite3 *db,           /* Database handle */
                               const char *zSql,      /* SQL statement, UTF-8 encoded */
                               int nByte,             /* Maximum length of zSql in bytes. */
                               sqlite3_stmt **ppStmt, /* OUT: Statement handle */
                               const char **pzTail    /* OUT: Pointer to unused portion of zSql */
) {
	return sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail);
}

SQLITE_API void *sqlite3_trace(sqlite3 *, void (*xTrace)(void *, const char *), void *) {
	fprintf(stderr, "sqlite3_trace: unsupported.\n");
	return nullptr;
}

SQLITE_API void *sqlite3_profile(sqlite3 *, void (*xProfile)(void *, const char *, sqlite3_uint64), void *) {
	fprintf(stderr, "sqlite3_profile: unsupported.\n");
	return nullptr;
}

SQLITE_API int sqlite3_libversion_number(void) {
	return SQLITE_VERSION_NUMBER;
}

SQLITE_API int sqlite3_threadsafe(void) {
	return SQLITE_OK;
}

SQLITE_API sqlite3_mutex *sqlite3_mutex_alloc(int) {
	fprintf(stderr, "sqlite3_mutex_alloc: unsupported.\n");
	return nullptr;
}

SQLITE_API void sqlite3_mutex_free(sqlite3_mutex *) {
	fprintf(stderr, "sqlite3_mutex_free: unsupported.\n");
}

SQLITE_API int sqlite3_extended_result_codes(sqlite3 *db, int onoff) {
	fprintf(stderr, "sqlite3_extended_result_codes: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API void *sqlite3_update_hook(sqlite3 *db, /* Attach the hook to this database */
                                     void (*xCallback)(void *, int, char const *, char const *, sqlite_int64),
                                     void *pArg /* Argument to the function */
) {
	fprintf(stderr, "sqlite3_update_hook: unsupported.\n");
	return nullptr;
}

SQLITE_API void sqlite3_log(int iErrCode, const char *zFormat, ...) {
	fprintf(stderr, "sqlite3_log: unsupported.\n");
}

SQLITE_API int sqlite3_unlock_notify(sqlite3 *db, void (*xNotify)(void **, int), void *pArg) {
	fprintf(stderr, "sqlite3_unlock_notify: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API void *sqlite3_get_auxdata(sqlite3_context *pCtx, int iArg) {
	fprintf(stderr, "sqlite3_get_auxdata: unsupported.\n");
	return nullptr;
}

SQLITE_API void *sqlite3_rollback_hook(sqlite3 *db,               /* Attach the hook to this database */
                                       void (*xCallback)(void *), /* Callback function */
                                       void *pArg                 /* Argument to the function */
) {
	fprintf(stderr, "sqlite3_rollback_hook: unsupported.\n");
	return nullptr;
}

SQLITE_API void *sqlite3_commit_hook(sqlite3 *db,              /* Attach the hook to this database */
                                     int (*xCallback)(void *), /* Function to invoke on each commit */
                                     void *pArg                /* Argument to the function */
) {
	fprintf(stderr, "sqlite3_commit_hook: unsupported.\n");
	return nullptr;
}

SQLITE_API int sqlite3_blob_open(sqlite3 *db,          /* The database connection */
                                 const char *zDb,      /* The attached database containing the blob */
                                 const char *zTable,   /* The table containing the blob */
                                 const char *zColumn,  /* The column containing the blob */
                                 sqlite_int64 iRow,    /* The row containing the glob */
                                 int wrFlag,           /* True -> read/write access, false -> read-only */
                                 sqlite3_blob **ppBlob /* Handle for accessing the blob returned here */
) {
	fprintf(stderr, "sqlite3_blob_open: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API const char *sqlite3_db_filename(sqlite3 *db, const char *zDbName) {
	fprintf(stderr, "sqlite3_db_filename: unsupported.\n");
	return nullptr;
}

SQLITE_API int sqlite3_stmt_busy(sqlite3_stmt *) {
	fprintf(stderr, "sqlite3_stmt_busy: unsupported.\n");
	return false;
}

SQLITE_API int sqlite3_bind_pointer(sqlite3_stmt *pStmt, int i, void *pPtr, const char *zPTtype,
                                    void (*xDestructor)(void *)) {
	fprintf(stderr, "sqlite3_bind_pointer: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_create_module_v2(sqlite3 *db,                   /* Database in which module is registered */
                                        const char *zName,             /* Name assigned to this module */
                                        const sqlite3_module *pModule, /* The definition of the module */
                                        void *pAux,                    /* Context pointer for xCreate/xConnect */
                                        void (*xDestroy)(void *)       /* Module destructor function */
) {
	fprintf(stderr, "sqlite3_create_module_v2: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_blob_write(sqlite3_blob *, const void *z, int n, int iOffset) {
	fprintf(stderr, "sqlite3_blob_write: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API void sqlite3_set_auxdata(sqlite3_context *, int N, void *, void (*)(void *)) {
	fprintf(stderr, "sqlite3_set_auxdata: unsupported.\n");
}

SQLITE_API sqlite3_stmt *sqlite3_next_stmt(sqlite3 *pDb, sqlite3_stmt *pStmt) {
	fprintf(stderr, "sqlite3_next_stmt: unsupported.\n");
	return nullptr;
}

SQLITE_API int sqlite3_collation_needed(sqlite3 *, void *, void (*)(void *, sqlite3 *, int eTextRep, const char *)) {
	fprintf(stderr, "sqlite3_collation_needed: unsupported.\n");
	return SQLITE_ERROR;
}

SQLITE_API int sqlite3_create_collation_v2(sqlite3 *, const char *zName, int eTextRep, void *pArg,
                                           int (*xCompare)(void *, int, const void *, int, const void *),
                                           void (*xDestroy)(void *)) {
	fprintf(stderr, "sqlite3_create_collation_v2: unsupported.\n");
	return SQLITE_ERROR;
}
