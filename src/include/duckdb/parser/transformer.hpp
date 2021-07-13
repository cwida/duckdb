//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/transformer.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/enums/expression_type.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/parser/qualified_name.hpp"
#include "duckdb/parser/tokens.hpp"

#include "pg_definitions.hpp"
#include "nodes/parsenodes.hpp"

namespace duckdb {

class ColumnDefinition;
struct OrderByNode;
struct CopyInfo;
struct CommonTableExpressionInfo;

//! The transformer class is responsible for transforming the internal Postgres
//! parser representation into the DuckDB representation
class Transformer {
	static constexpr const idx_t DEFAULT_MAX_EXPRESSION_DEPTH = 1000;

public:
	explicit Transformer(Transformer *parent = nullptr, idx_t max_expression_depth_p = DEFAULT_MAX_EXPRESSION_DEPTH)
	    : parent(parent), max_expression_depth(parent ? parent->max_expression_depth : max_expression_depth_p) {
	}

	//! Transforms a Postgres parse tree into a set of SQL Statements
	bool TransformParseTree(duckdb_libpgquery::PGList *tree, vector<unique_ptr<SQLStatement>> &statements);
	string NodetypeToString(duckdb_libpgquery::PGNodeTag type);

	idx_t ParamCount() {
		return parent ? parent->ParamCount() : prepared_statement_parameter_index;
	}

private:
	Transformer *parent;
	idx_t max_expression_depth;
	//! The current prepared statement parameter index
	idx_t prepared_statement_parameter_index = 0;
	//! Holds window expressions defined by name. We need those when transforming the expressions referring to them.
	unordered_map<string, duckdb_libpgquery::PGWindowDef *> window_clauses;

	void SetParamCount(idx_t new_count) {
		if (parent) {
			parent->SetParamCount(new_count);
		} else {
			this->prepared_statement_parameter_index = new_count;
		}
	}

private:
	//! Transforms a Postgres statement into a single SQL statement
	unique_ptr<SQLStatement> TransformStatement(duckdb_libpgquery::PGNode *stmt);
	//===--------------------------------------------------------------------===//
	// Statement transformation
	//===--------------------------------------------------------------------===//
	//! Transform a Postgres duckdb_libpgquery::T_PGSelectStmt node into a SelectStatement
	unique_ptr<SelectStatement> TransformSelect(duckdb_libpgquery::PGNode *node, bool isSelect = true);
	//! Transform a Postgres T_AlterStmt node into a AlterStatement
	unique_ptr<AlterStatement> TransformAlter(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGRenameStmt node into a RenameStatement
	unique_ptr<AlterStatement> TransformRename(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGCreateStmt node into a CreateStatement
	unique_ptr<CreateStatement> TransformCreateTable(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGCreateStmt node into a CreateStatement
	unique_ptr<CreateStatement> TransformCreateTableAs(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres node into a CreateStatement
	unique_ptr<CreateStatement> TransformCreateSchema(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGCreateSeqStmt node into a CreateStatement
	unique_ptr<CreateStatement> TransformCreateSequence(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGViewStmt node into a CreateStatement
	unique_ptr<CreateStatement> TransformCreateView(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGIndexStmt node into CreateStatement
	unique_ptr<CreateStatement> TransformCreateIndex(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGCreateFunctionStmt node into CreateStatement
	unique_ptr<CreateStatement> TransformCreateFunction(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGDropStmt node into a Drop[Table,Schema]Statement
	unique_ptr<SQLStatement> TransformDrop(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGInsertStmt node into a InsertStatement
	unique_ptr<InsertStatement> TransformInsert(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGCopyStmt node into a CopyStatement
	unique_ptr<CopyStatement> TransformCopy(duckdb_libpgquery::PGNode *node);
	void TransformCopyOptions(CopyInfo &info, duckdb_libpgquery::PGList *options);
	//! Transform a Postgres duckdb_libpgquery::T_PGTransactionStmt node into a TransactionStatement
	unique_ptr<TransactionStatement> TransformTransaction(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres T_DeleteStatement node into a DeleteStatement
	unique_ptr<DeleteStatement> TransformDelete(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGUpdateStmt node into a UpdateStatement
	unique_ptr<UpdateStatement> TransformUpdate(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGPragmaStmt node into a PragmaStatement
	unique_ptr<PragmaStatement> TransformPragma(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGExportStmt node into a ExportStatement
	unique_ptr<ExportStatement> TransformExport(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres duckdb_libpgquery::T_PGImportStmt node into a PragmaStatement
	unique_ptr<PragmaStatement> TransformImport(duckdb_libpgquery::PGNode *node);
	unique_ptr<ExplainStatement> TransformExplain(duckdb_libpgquery::PGNode *node);
	unique_ptr<VacuumStatement> TransformVacuum(duckdb_libpgquery::PGNode *node);
	unique_ptr<PragmaStatement> TransformShow(duckdb_libpgquery::PGNode *node);
	unique_ptr<ShowStatement> TransformShowSelect(duckdb_libpgquery::PGNode *node);

	unique_ptr<PrepareStatement> TransformPrepare(duckdb_libpgquery::PGNode *node);
	unique_ptr<ExecuteStatement> TransformExecute(duckdb_libpgquery::PGNode *node);
	unique_ptr<CallStatement> TransformCall(duckdb_libpgquery::PGNode *node);
	unique_ptr<DropStatement> TransformDeallocate(duckdb_libpgquery::PGNode *node);
	unique_ptr<SetStatement> TransformSet(duckdb_libpgquery::PGNode *node);
	unique_ptr<SQLStatement> TransformCheckpoint(duckdb_libpgquery::PGNode *node);
	unique_ptr<LoadStatement> TransformLoad(duckdb_libpgquery::PGNode *node);

	//===--------------------------------------------------------------------===//
	// Query Node Transform
	//===--------------------------------------------------------------------===//
	//! Transform a Postgres duckdb_libpgquery::T_PGSelectStmt node into a QueryNode
	unique_ptr<QueryNode> TransformSelectNode(duckdb_libpgquery::PGSelectStmt *node);

	//===--------------------------------------------------------------------===//
	// Expression Transform
	//===--------------------------------------------------------------------===//
	//! Transform a Postgres boolean expression into an Expression
	unique_ptr<ParsedExpression> TransformBoolExpr(duckdb_libpgquery::PGBoolExpr *root, idx_t depth);
	//! Transform a Postgres case expression into an Expression
	unique_ptr<ParsedExpression> TransformCase(duckdb_libpgquery::PGCaseExpr *root, idx_t depth);
	//! Transform a Postgres type cast into an Expression
	unique_ptr<ParsedExpression> TransformTypeCast(duckdb_libpgquery::PGTypeCast *root, idx_t depth);
	//! Transform a Postgres coalesce into an Expression
	unique_ptr<ParsedExpression> TransformCoalesce(duckdb_libpgquery::PGAExpr *root, idx_t depth);
	//! Transform a Postgres column reference into an Expression
	unique_ptr<ParsedExpression> TransformColumnRef(duckdb_libpgquery::PGColumnRef *root, idx_t depth);
	//! Transform a Postgres constant value into an Expression
	unique_ptr<ConstantExpression> TransformValue(duckdb_libpgquery::PGValue val, idx_t depth);
	//! Transform a Postgres operator into an Expression
	unique_ptr<ParsedExpression> TransformAExpr(duckdb_libpgquery::PGAExpr *root, idx_t depth);
	//! Transform a Postgres abstract expression into an Expression
	unique_ptr<ParsedExpression> TransformExpression(duckdb_libpgquery::PGNode *node, idx_t depth);
	//! Transform a Postgres function call into an Expression
	unique_ptr<ParsedExpression> TransformFuncCall(duckdb_libpgquery::PGFuncCall *root, idx_t depth);
	//! Transform a Postgres boolean expression into an Expression
	unique_ptr<ParsedExpression> TransformInterval(duckdb_libpgquery::PGIntervalConstant *root, idx_t depth);
	//! Transform a Postgres lambda node [e.g. (x, y) -> x + y] into a lambda expression
	unique_ptr<ParsedExpression> TransformLambda(duckdb_libpgquery::PGLambdaFunction *node, idx_t depth);
	//! Transform a Postgres array access node (e.g. x[1] or x[1:3])
	unique_ptr<ParsedExpression> TransformArrayAccess(duckdb_libpgquery::PGAIndirection *node, idx_t depth);
	//! Transform a positional reference (e.g. #1)
	unique_ptr<ParsedExpression> TransformPositionalReference(duckdb_libpgquery::PGPositionalReference *node,
	                                                          idx_t depth);

	//! Transform a Postgres constant value into an Expression
	unique_ptr<ParsedExpression> TransformConstant(duckdb_libpgquery::PGAConst *c, idx_t depth);

	unique_ptr<ParsedExpression> TransformResTarget(duckdb_libpgquery::PGResTarget *root, idx_t depth);
	unique_ptr<ParsedExpression> TransformNullTest(duckdb_libpgquery::PGNullTest *root, idx_t depth);
	unique_ptr<ParsedExpression> TransformParamRef(duckdb_libpgquery::PGParamRef *node, idx_t depth);
	unique_ptr<ParsedExpression> TransformNamedArg(duckdb_libpgquery::PGNamedArgExpr *root, idx_t depth);

	unique_ptr<ParsedExpression> TransformSQLValueFunction(duckdb_libpgquery::PGSQLValueFunction *node, idx_t depth);

	unique_ptr<ParsedExpression> TransformSubquery(duckdb_libpgquery::PGSubLink *root, idx_t depth);
	//===--------------------------------------------------------------------===//
	// Constraints transform
	//===--------------------------------------------------------------------===//
	unique_ptr<Constraint> TransformConstraint(duckdb_libpgquery::PGListCell *cell);

	unique_ptr<Constraint> TransformConstraint(duckdb_libpgquery::PGListCell *cell, ColumnDefinition &column,
	                                           idx_t index);

	//===--------------------------------------------------------------------===//
	// Collation transform
	//===--------------------------------------------------------------------===//
	unique_ptr<ParsedExpression> TransformCollateExpr(duckdb_libpgquery::PGCollateClause *collate, idx_t depth);

	string TransformCollation(duckdb_libpgquery::PGCollateClause *collate);

	ColumnDefinition TransformColumnDefinition(duckdb_libpgquery::PGColumnDef *cdef);
	//===--------------------------------------------------------------------===//
	// Helpers
	//===--------------------------------------------------------------------===//
	string TransformAlias(duckdb_libpgquery::PGAlias *root, vector<string> &column_name_alias);
	void TransformCTE(duckdb_libpgquery::PGWithClause *de_with_clause, QueryNode &select);
	unique_ptr<SelectStatement> TransformRecursiveCTE(duckdb_libpgquery::PGCommonTableExpr *node,
	                                                  CommonTableExpressionInfo &info);
	// Operator String to ExpressionType (e.g. + => OPERATOR_ADD)
	ExpressionType OperatorToExpressionType(const string &op);

	unique_ptr<ParsedExpression> TransformUnaryOperator(const string &op, unique_ptr<ParsedExpression> child);
	unique_ptr<ParsedExpression> TransformBinaryOperator(const string &op, unique_ptr<ParsedExpression> left,
	                                                     unique_ptr<ParsedExpression> right);
	//===--------------------------------------------------------------------===//
	// TableRef transform
	//===--------------------------------------------------------------------===//
	//! Transform a Postgres node into a TableRef
	unique_ptr<TableRef> TransformTableRefNode(duckdb_libpgquery::PGNode *node);
	//! Transform a Postgres FROM clause into a TableRef
	unique_ptr<TableRef> TransformFrom(duckdb_libpgquery::PGList *root);
	//! Transform a Postgres table reference into a TableRef
	unique_ptr<TableRef> TransformRangeVar(duckdb_libpgquery::PGRangeVar *root);
	//! Transform a Postgres table-producing function into a TableRef
	unique_ptr<TableRef> TransformRangeFunction(duckdb_libpgquery::PGRangeFunction *root);
	//! Transform a Postgres join node into a TableRef
	unique_ptr<TableRef> TransformJoin(duckdb_libpgquery::PGJoinExpr *root);
	//! Transform a table producing subquery into a TableRef
	unique_ptr<TableRef> TransformRangeSubselect(duckdb_libpgquery::PGRangeSubselect *root);
	//! Transform a VALUES list into a set of expressions
	unique_ptr<TableRef> TransformValuesList(duckdb_libpgquery::PGList *list);

	//! Transform a range var into a (schema) qualified name
	QualifiedName TransformQualifiedName(duckdb_libpgquery::PGRangeVar *root);

	//! Transform a Postgres TypeName string into a LogicalType
	LogicalType TransformTypeName(duckdb_libpgquery::PGTypeName *name);

	//! Transform a Postgres GROUP BY expression into a list of Expression
	bool TransformGroupBy(duckdb_libpgquery::PGList *group, vector<unique_ptr<ParsedExpression>> &result);
	//! Transform a Postgres ORDER BY expression into an OrderByDescription
	bool TransformOrderBy(duckdb_libpgquery::PGList *order, vector<OrderByNode> &result);

	//! Transform a Postgres SELECT clause into a list of Expressions
	void TransformExpressionList(duckdb_libpgquery::PGList &list, vector<unique_ptr<ParsedExpression>> &result,
	                             idx_t depth);

	//! Transform a Postgres PARTITION BY/ORDER BY specification into lists of expressions
	void TransformWindowDef(duckdb_libpgquery::PGWindowDef *window_spec, WindowExpression *expr, idx_t depth);
	//! Transform a Postgres window frame specification into frame expressions
	void TransformWindowFrame(duckdb_libpgquery::PGWindowDef *window_spec, WindowExpression *expr, idx_t depth);

	unique_ptr<SampleOptions> TransformSampleOptions(duckdb_libpgquery::PGNode *options);
};

} // namespace duckdb
