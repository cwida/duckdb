#include "duckdb/parser/transformer.hpp"

#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/statement/list.hpp"
#include "duckdb/parser/tableref/emptytableref.hpp"

namespace duckdb {

bool Transformer::TransformParseTree(duckdb_libpgquery::PGList *tree, vector<unique_ptr<SQLStatement>> &statements) {
	for (auto entry = tree->head; entry != nullptr; entry = entry->next) {
		SetParamCount(0);
		auto stmt = TransformStatement((duckdb_libpgquery::PGNode *)entry->data.ptr_value);
		D_ASSERT(stmt);
		stmt->n_param = ParamCount();
		statements.push_back(move(stmt));
	}
	return true;
}

unique_ptr<SQLStatement> Transformer::TransformStatement(duckdb_libpgquery::PGNode *stmt) {
	switch (stmt->type) {
	case duckdb_libpgquery::T_PGRawStmt: {
		auto raw_stmt = (duckdb_libpgquery::PGRawStmt *)stmt;
		auto result = TransformStatement(raw_stmt->stmt);
		if (result) {
			result->stmt_location = raw_stmt->stmt_location;
			result->stmt_length = raw_stmt->stmt_len;
		}
		return result;
	}
	case duckdb_libpgquery::T_PGSelectStmt:
		return TransformSelect(stmt);
	case duckdb_libpgquery::T_PGCreateStmt:
		return TransformCreateTable(stmt);
	case duckdb_libpgquery::T_PGCreateSchemaStmt:
		return TransformCreateSchema(stmt);
	case duckdb_libpgquery::T_PGViewStmt:
		return TransformCreateView(stmt);
	case duckdb_libpgquery::T_PGCreateSeqStmt:
		return TransformCreateSequence(stmt);
	case duckdb_libpgquery::T_PGCreateFunctionStmt:
		return TransformCreateFunction(stmt);
	case duckdb_libpgquery::T_PGDropStmt:
		return TransformDrop(stmt);
	case duckdb_libpgquery::T_PGInsertStmt:
		return TransformInsert(stmt);
	case duckdb_libpgquery::T_PGCopyStmt:
		return TransformCopy(stmt);
	case duckdb_libpgquery::T_PGTransactionStmt:
		return TransformTransaction(stmt);
	case duckdb_libpgquery::T_PGDeleteStmt:
		return TransformDelete(stmt);
	case duckdb_libpgquery::T_PGUpdateStmt:
		return TransformUpdate(stmt);
	case duckdb_libpgquery::T_PGIndexStmt:
		return TransformCreateIndex(stmt);
	case duckdb_libpgquery::T_PGAlterTableStmt:
		return TransformAlter(stmt);
	case duckdb_libpgquery::T_PGRenameStmt:
		return TransformRename(stmt);
	case duckdb_libpgquery::T_PGPrepareStmt:
		return TransformPrepare(stmt);
	case duckdb_libpgquery::T_PGExecuteStmt:
		return TransformExecute(stmt);
	case duckdb_libpgquery::T_PGDeallocateStmt:
		return TransformDeallocate(stmt);
	case duckdb_libpgquery::T_PGCreateTableAsStmt:
		return TransformCreateTableAs(stmt);
	case duckdb_libpgquery::T_PGPragmaStmt:
		return TransformPragma(stmt);
	case duckdb_libpgquery::T_PGExportStmt:
		return TransformExport(stmt);
	case duckdb_libpgquery::T_PGImportStmt:
		return TransformImport(stmt);
	case duckdb_libpgquery::T_PGExplainStmt:
		return TransformExplain(stmt);
	case duckdb_libpgquery::T_PGVacuumStmt:
		return TransformVacuum(stmt);
	case duckdb_libpgquery::T_PGVariableShowStmt:
		return TransformShow(stmt);
	case duckdb_libpgquery::T_PGVariableShowSelectStmt:
		return TransformShowSelect(stmt);
	case duckdb_libpgquery::T_PGCallStmt:
		return TransformCall(stmt);
	case duckdb_libpgquery::T_PGVariableSetStmt:
		return TransformSet(stmt);
	case duckdb_libpgquery::T_PGCheckPointStmt:
		return TransformCheckpoint(stmt);
	case duckdb_libpgquery::T_PGLoadStmt:
		return TransformLoad(stmt);
	default:
		throw NotImplementedException(NodetypeToString(stmt->type));
	}
	return nullptr;
}

} // namespace duckdb
