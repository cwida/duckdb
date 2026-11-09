//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/column_qualifier.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/binder.hpp"

namespace duckdb {
class ColumnAliasBinder;
class HavingBinder;

class ColumnQualifier {
public:
	explicit ColumnQualifier(Binder &binder, optional_ptr<vector<DummyBinding>> lambda_bindings = nullptr,
	                         optional_ptr<ColumnAliasBinder> alias_binder = nullptr,
	                         optional_ptr<HavingBinder> having_binder = nullptr);
	virtual ~ColumnQualifier() = default;

	//! The function a bare SQL value function name (e.g. current_timestamp) converts to, or empty
	static Identifier SQLValueFunctionName(const Identifier &column_name);

	//! Returns the STRUCT_EXTRACT operator expression
	unique_ptr<ParsedExpression> CreateStructExtract(unique_ptr<ParsedExpression> base, const Identifier &field_name);
	//! Returns a STRUCT_PACK function expression
	unique_ptr<ParsedExpression> CreateStructPack(ColumnRefExpression &col_ref);

	//! Returns a qualified column reference from a column name
	unique_ptr<ParsedExpression> QualifyColumnName(const ParsedExpression &expr, const Identifier &column_name,
	                                               ErrorData &error);
	//! Returns a qualified column reference from a column reference with column_names.size() > 2
	unique_ptr<ParsedExpression> QualifyColumnNameWithManyDots(ColumnRefExpression &col_ref, ErrorData &error);
	//! Returns a qualified column reference from a column reference
	unique_ptr<ParsedExpression> QualifyColumnName(ColumnRefExpression &col_ref, ErrorData &error);
	//! Enables special-handling of lambda parameters by tracking them in the lambda_params vector
	void QualifyColumnNamesInLambda(FunctionExpression &function, vector<identifier_set_t> &lambda_params);
	//! Recursively qualifies the column references in the (children) of the expression. Passes on the
	//! within_function_expression state from outer expressions, or sets it
	void QualifyColumnNames(unique_ptr<ParsedExpression> &expr, vector<identifier_set_t> &lambda_params,
	                        const bool within_function_expression = false);

	optional_ptr<CatalogEntry> QualifyFunction(FunctionExpression &function);

private:
	Binder &binder;
	optional_ptr<vector<DummyBinding>> lambda_bindings;
	optional_ptr<ColumnAliasBinder> alias_binder;
	optional_ptr<HavingBinder> having_binder;

private:
	unique_ptr<ParsedExpression> QualifyColumnNameWithManyDotsInternal(ColumnRefExpression &col_ref, ErrorData &error,
	                                                                   idx_t &struct_extract_start);
	//! Whether the given (possibly nested) table qualification matches a binding in the current bind context
	bool TableBindingExists(const BindingAlias &alias);
	//! Whether any binding in the current bind context has the given table name (ignoring catalog/schema)
	bool TableNameExists(const Identifier &table_name);

	unique_ptr<ParsedExpression> QualifyColumnNameInternal(ColumnRefExpression &col_ref, ErrorData &error);
};

} // namespace duckdb
