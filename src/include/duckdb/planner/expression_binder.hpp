//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_binder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/stack_checker.hpp"
#include "duckdb/common/error_data.hpp"
#include "duckdb/common/exception/binder_exception.hpp"
#include "duckdb/parser/expression/lambdaref_expression.hpp"
#include "duckdb/parser/expression/type_expression.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tokens.hpp"
#include "duckdb/planner/expression.hpp"
#include "duckdb/catalog/catalog_entry_retriever.hpp"
#include "duckdb/planner/expression/bound_lambda_expression.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/common/enums/collation_type.hpp"

namespace duckdb {

class Binder;
class ClientContext;
class ColumnQualifier;
class QueryNode;

class ScalarFunctionCatalogEntry;
class AggregateFunctionCatalogEntry;
class WindowFunctionCatalogEntry;
class ScalarMacroCatalogEntry;
class MacroFunction;
class ScalarMacroFunction;
class CatalogEntry;
class SimpleFunction;
class HavingBinder;
class ColumnAliasBinder;
class LogicalOperator;
class LogicalProjection;
class LogicalAggregate;

struct TrySubqueryPlanIndexes;
struct DummyBinding;
struct SelectBindState;

struct BoundColumnReferenceInfo {
	Identifier name;
	QueryLocation query_location;
};

//! The outcome of resolving a name against the chain of enclosing query scopes
struct ColumnResolution {
	//! Whether some scope in the chain resolves the name
	bool found = false;
	//! The index of the resolving scope, i.e. the depth a reference to it binds at
	idx_t depth = 0;
	//! The qualified replacement produced by the resolving scope
	unique_ptr<ParsedExpression> qualified;
	//! The errors of every probed scope, combined - only set when the name is not found
	ErrorData error;
};

struct BindResult {
	BindResult() {
	}
	explicit BindResult(const Exception &ex) : error(ex) {
	}
	explicit BindResult(const string &error_msg) : error(ExceptionType::BINDER, error_msg) {
	}
	explicit BindResult(ErrorData error) : error(std::move(error)) {
	}
	explicit BindResult(unique_ptr<Expression> expr) : expression(std::move(expr)) {
	}

	bool HasError() const {
		return error.HasError();
	}
	void SetError(const string &error_message) {
		error = ErrorData(ExceptionType::BINDER, error_message);
	}

	unique_ptr<Expression> expression;
	ErrorData error;
};

class ExpressionBinder {
	friend class StackChecker<ExpressionBinder>;

public:
	ExpressionBinder(Binder &binder, ClientContext &context);
	virtual ~ExpressionBinder();

	virtual bool TryResolveAliasReference(ColumnRefExpression &colref, idx_t depth, bool root_expression,
	                                      BindResult &result, unique_ptr<ParsedExpression> &expr_ptr) {
		return false;
	}

	virtual bool IsLateralBinder() const {
		return false;
	}

	Binder &GetBinder() const {
		return binder;
	}

	// Returns true if the ColumnRef could be an alias reference (unqualified or qualified with table name "alias")
	static bool IsPotentialAlias(const ColumnRefExpression &colref);

	//! The target type that should result from the binder. If the result is not of this type, a cast to this type will
	//! be added. Defaults to INVALID.
	LogicalType target_type;

	optional_ptr<DummyBinding> macro_binding;
	optional_ptr<vector<DummyBinding>> lambda_bindings;
	//! The number of (outer) lambda bindings whose parameters can not be referenced, e.g. inside an aggregate
	idx_t inaccessible_lambda_binding_count = 0;

public:
	unique_ptr<Expression> Bind(unique_ptr<ParsedExpression> &expr, optional_ptr<LogicalType> result_type = nullptr,
	                            bool root_expression = true);

	//! Returns whether or not any columns have been bound by the expression binder
	bool HasBoundColumns() {
		return !bound_columns.empty();
	}
	const vector<BoundColumnReferenceInfo> &GetBoundColumns() {
		return bound_columns;
	}
	void TruncateBoundColumns(idx_t count) {
		if (bound_columns.size() > count) {
			bound_columns.resize(count);
		}
	}

	void SetCatalogLookupCallback(catalog_entry_callback_t callback);
	//! Bind the expression at the given depth, returning the bound expression or the error
	[[nodiscard]] BindResult Bind(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression = false);

	//! Returns the STRUCT_EXTRACT operator expression
	unique_ptr<ParsedExpression> CreateStructExtract(unique_ptr<ParsedExpression> base, const Identifier &field_name);
	//! Returns a STRUCT_PACK function expression
	unique_ptr<ParsedExpression> CreateStructPack(ColumnRefExpression &col_ref);

	BindResult BindQualifiedColumnName(ColumnRefExpression &colref, const Identifier &table_name);

	//! Entry point for qualifying the column references of the expression
	static void QualifyColumnNames(Binder &binder, unique_ptr<ParsedExpression> &expr,
	                               optional_ptr<ColumnAliasBinder> alias_binder = nullptr);
	static void QualifyColumnNames(ExpressionBinder &binder, unique_ptr<ParsedExpression> &expr);
	static void QualifyColumnNames(HavingBinder &having_binder, unique_ptr<ParsedExpression> &expr);

	//! Create the qualifier that resolves names against this binder's scope, with this binder's hooks
	virtual unique_ptr<ColumnQualifier> CreateColumnQualifier();

	//! Whether this scope groups by the given expression, i.e. whether a reference to it resolves to a group
	virtual bool MatchesGroup(ParsedExpression &expr);
	//! Whether this scope groups by the given expression and the bound group contains no subquery - the scopes
	//! a subquery resolves in are only known once bound, e.g. when it is hidden in a macro
	virtual bool MatchesGroupWithoutSubquery(ParsedExpression &expr);
	//! Whether this scope groups by an expression other than a plain column reference
	virtual bool HasCompoundGroups();

	//! Whether the name matches a select-list alias of this scope. Such a scope owns the name even
	//! though qualification cannot resolve it, and it is the one that reports any error.
	virtual bool ClaimsAlias(ColumnRefExpression &colref);

	//! Unify the types of two bound operands and build the bound comparison over them
	unique_ptr<Expression> CreateBoundComparison(ExpressionType comparison_type, unique_ptr<Expression> left,
	                                             unique_ptr<Expression> right, ErrorData &error);

	static bool PushCollation(ClientContext &context, unique_ptr<Expression> &source, const LogicalType &sql_type,
	                          CollationType type = CollationType::ALL_COLLATIONS);
	static void TestCollation(ClientContext &context, const string &collation);

	//! The query scopes an expression can be resolved against, innermost first: this binder followed by
	//! its enclosing scopes. The index of a scope is the depth a reference bound against it receives.
	idx_t ScopeCount() const;
	ExpressionBinder &ScopeAt(idx_t depth);

	//! Resolve a column reference against the scopes at or beyond the given depth, without binding it.
	//! Resolution consults exactly what a real bind against a scope would, by reusing the qualifier
	//! that scope builds for itself.
	ColumnResolution ResolveColumn(ColumnRefExpression &colref, idx_t start);
	//! The scope that owns an aggregate: the innermost one at or beyond `start` in which any of its
	//! arguments resolves a column. Invalid when no argument resolves a column at or beyond `start` -
	//! at `start == 0` that pins a constant-only aggregate to the scope it appears in.
	optional_idx ResolveAggregateOwner(FunctionExpression &aggregate, idx_t start);
	//! The innermost scope at or beyond `start` in which any column of the expression resolves
	optional_idx ResolveExpressionOwner(ParsedExpression &expr, idx_t start);
	//! The innermost scope at or beyond `start` in which any of the columns resolves
	optional_idx ResolveOwner(vector<reference<ColumnRefExpression>> &columns, idx_t start);
	//! The enclosing scope that owns all of the expression's columns and groups by the expression, if any
	optional_idx ResolveOuterGroupOwner(ParsedExpression &expr);
	//! Bind an expression as a reference to the group of the enclosing scope `owner`
	BindResult BindGroupInEnclosingScope(unique_ptr<ParsedExpression> &expr_ptr, idx_t owner, idx_t depth);
	//! The innermost scope at or beyond `start` whose groups all of the expressions match
	optional_idx ResolveOuterGroup(vector<reference<ParsedExpression>> &expressions, idx_t start);
	//! Merge the error of a newly probed scope into the error accumulated over the previous ones,
	//! preferring a missing column over any other error and merging the candidate bindings
	static void CombineErrors(ErrorData &current, ErrorData new_error);

	//! Bind the expression in the scope at the given depth, so that the semantics of that scope apply,
	//! and register the correlated columns of the result on this binder
	BindResult DispatchToScope(idx_t scope, unique_ptr<ParsedExpression> &expr_ptr, idx_t base_depth);

	//! Bind a column that does not resolve in this scope against the innermost enclosing scope that does.
	//! A scope that resolves the name but cannot bind it is passed over, and the search continues outward.
	BindResult BindInEnclosingScope(ColumnRefExpression &col_ref, idx_t depth, unique_ptr<ParsedExpression> &expr_ptr,
	                                ErrorData local_error);

	//! Bind an aggregate owned by an enclosing scope, starting at the given scope. Ownership follows from
	//! column resolutions, which are only a lower bound, so a scope that owns the aggregate but cannot
	//! bind it is passed over and the search continues outward.
	BindResult BindAggregateInEnclosingScope(FunctionExpression &aggregate, idx_t owner, idx_t depth,
	                                         unique_ptr<ParsedExpression> &expr_ptr);

	//! Bind a GROUPING owned by an enclosing scope, starting at the given scope. A scope that groups by
	//! all of the arguments can still fail to bind them, so it is passed over like any other.
	BindResult BindGroupingInEnclosingScope(OperatorExpression &op, vector<reference<ParsedExpression>> &children,
	                                        idx_t owner, idx_t depth);

	//! Bind a child expression, returning it. A null child, or one that fails to bind, returns null;
	//! the first error encountered is recorded in the accumulator.
	[[nodiscard]] unique_ptr<Expression> BindChild(unique_ptr<ParsedExpression> &expr, idx_t depth, ErrorData &error);
	static void ExtractCorrelatedExpressions(Binder &binder, Expression &expr);

	static bool ContainsNullType(const LogicalType &type);
	static LogicalType ExchangeNullType(const LogicalType &type);
	static bool ContainsType(const LogicalType &type, LogicalTypeId target);
	static LogicalType ExchangeType(const LogicalType &type, LogicalTypeId target, LogicalType new_type);

	//! Bind the given expression. Unlike Bind(), this does *not* mute the given ParsedExpression.
	//! Exposed to be used from sub-binders that aren't subclasses of ExpressionBinder.
	virtual BindResult BindExpression(unique_ptr<ParsedExpression> &expr_ptr, idx_t depth,
	                                  bool root_expression = false);

	//! FIXME: Generalise this for extensibility.
	//! Recursively replaces macro parameters with the provided input parameters.
	void ReplaceMacroParameters(unique_ptr<ParsedExpression> &expr, vector<identifier_set_t> &lambda_params);
	//! Enables special-handling of lambda parameters during macro replacement by tracking them in the lambda_params
	//! vector.
	void ReplaceMacroParametersInLambda(FunctionExpression &function, vector<identifier_set_t> &lambda_params);

	static LogicalType GetExpressionReturnType(const Expression &expr);

	virtual unique_ptr<ParsedExpression> QualifyColumnName(ColumnRefExpression &col_ref, ErrorData &error);

	//! Returns true if the function name is an alias for the UNNEST function
	static bool IsUnnestFunction(const Identifier &function_name);

	//! Continues the expression depth of the binder this one binds a nested expression for
	void ContinueStackDepth(const ExpressionBinder &parent);

	//! While set, scalar subqueries bound by this binder are lowered with an exposed error flag, which
	//! the TRY subquery lowering of the enclosing select node consumes
	void SetTransportTryErrors(bool transport) {
		transport_try_errors = transport;
	}

private:
	//! Current stack depth
	idx_t stack_depth = DConstants::INVALID_INDEX;

	void InitializeStackCheck();

protected:
	StackChecker<ExpressionBinder> StackCheck(const ParsedExpression &expr, idx_t extra_stack = 1);

	BindResult BindExpression(BetweenExpression &expr, idx_t depth);
	BindResult BindExpression(CaseExpression &expr, idx_t depth);
	BindResult BindExpression(CollateExpression &expr, idx_t depth);
	BindResult BindExpression(CastExpression &expr, idx_t depth);
	BindResult BindExpression(ColumnRefExpression &expr, idx_t depth, bool root_expression,
	                          unique_ptr<ParsedExpression> &expr_ptr);
	BindResult BindExpression(LambdaRefExpression &expr, idx_t depth);
	BindResult BindExpression(ComparisonExpression &expr, idx_t depth);
	BindResult BindExpression(ConjunctionExpression &expr, idx_t depth);
	BindResult BindExpression(ConstantExpression &expr, idx_t depth);
	BindResult BindExpression(FunctionExpression &expr, idx_t depth, unique_ptr<ParsedExpression> &expr_ptr);
	BindResult BindExpression(TypeExpression &expr, idx_t depth);

	BindResult BindExpression(LambdaExpression &expr, idx_t depth, const vector<LogicalType> &function_child_types,
	                          optional_ptr<bind_lambda_function_t> bind_lambda_function,
	                          optional_ptr<BindLambdaContext> bind_lambda_context);
	BindResult BindExpression(OperatorExpression &expr, idx_t depth);
	BindResult BindOperatorAsFunction(OperatorExpression &op, const Identifier &function_name,
	                                  vector<unique_ptr<Expression>> children);
	BindResult BindExpression(ParameterExpression &expr, idx_t depth);
	BindResult BindExpression(SubqueryExpression &expr, idx_t depth);
	//! Rewrite a call that resolved to the built-in has_error into the HAS_ERROR operator and bind that
	BindResult BindHasErrorFunction(FunctionExpression &function, idx_t depth, unique_ptr<ParsedExpression> &expr_ptr);
	//! Bind TRY over a scalar subquery as an ordinary subquery, then lower the TRY onto its bound plan
	BindResult BindTrySubquery(OperatorExpression &op, idx_t depth);
	//! Throw when the subquery is not a plain SELECT node, the only shape the TRY lowering covers
	void CheckTrySubqueryNode(const SubqueryExpression &subquery_expr, const ParsedExpression &location);
	//! Lower TRY onto the maskable positions of a bound subquery plan, before the subquery is flattened.
	//! With expose_error_flag the output column becomes a struct of the masked value and the error flag,
	//! so the use site can re-raise captured errors. Returns the resulting output type, or an empty
	//! optional when the shape is not coverable and tolerant allows falling back to the plain subquery.
	optional<LogicalType> LowerTrySubquery(const ParsedExpression &location, const TrySubqueryPlanIndexes &indexes,
	                                       LogicalOperator &root, bool expose_error_flag, bool tolerant);
	//! Wrap the inputs of the gated aggregates in TRY and aggregate HAS_ERROR probes over them.
	//! Sets gate to a column reference to the probe aggregate, or to nullptr when the aggregates have no
	//! inputs. Returns false when a shape cannot be gated and strict is unset (throws otherwise).
	bool GateTryAggregates(const ParsedExpression &location, LogicalAggregate &aggregate,
	                       const vector<idx_t> &aggregate_positions, bool strict, unique_ptr<Expression> &gate);
	BindResult BindPositionalReference(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression);

	void TransformCapturedLambdaColumn(unique_ptr<Expression> &original, unique_ptr<Expression> &replacement,
	                                   BoundLambdaExpression &bound_lambda_expr,
	                                   const optional_ptr<bind_lambda_function_t> bind_lambda_function,
	                                   const optional_ptr<BindLambdaContext> bind_lambda_context,
	                                   const vector<LogicalType> &function_child_types);

	void CaptureLambdaColumns(BoundLambdaExpression &bound_lambda_expr, unique_ptr<Expression> &expr,
	                          const optional_ptr<bind_lambda_function_t> bind_lambda_function,
	                          const optional_ptr<BindLambdaContext> bind_lambda_context,
	                          const vector<LogicalType> &function_child_types);

	unique_ptr<ParsedExpression> GetSQLValueFunction(const Identifier &column_name);

	LogicalType ResolveOperatorType(OperatorExpression &op, vector<unique_ptr<Expression>> &children);
	LogicalType ResolveCoalesceType(OperatorExpression &op, vector<unique_ptr<Expression>> &children);
	LogicalType ResolveNotType(OperatorExpression &op, vector<unique_ptr<Expression>> &children);

	BindResult BindUnsupportedExpression(ParsedExpression &expr, idx_t depth, const string &message);

	CatalogEntry &BindFunction(FunctionExpression &function);

protected:
	virtual BindResult BindGroupingFunction(OperatorExpression &op, idx_t depth);
	virtual BindResult BindFunction(FunctionExpression &expr, ScalarFunctionCatalogEntry &function, idx_t depth);
	virtual BindResult BindLambdaFunction(FunctionExpression &expr, ScalarFunctionCatalogEntry &function, idx_t depth);
	virtual BindResult BindAggregate(FunctionExpression &expr, AggregateFunctionCatalogEntry &function, idx_t depth);
	virtual BindResult BindWindow(FunctionExpression &expr, WindowFunctionCatalogEntry &function, idx_t depth);
	virtual BindResult BindUnnest(FunctionExpression &expr, idx_t depth, bool root_expression);
	virtual BindResult BindMacro(FunctionExpression &expr, ScalarMacroCatalogEntry &macro, idx_t depth,
	                             unique_ptr<ParsedExpression> &expr_ptr);
	void FindAggregateExprs(unique_ptr<ParsedExpression> &expr, vector<reference<unique_ptr<ParsedExpression>>> &exprs);
	void UnfoldWindowMacroExpression(unique_ptr<ParsedExpression> &expr, ScalarMacroFunction &macro_def);
	MacroFunction &UnfoldMacroExpression(FunctionExpression &function, ScalarMacroCatalogEntry &macro_func,
	                                     unique_ptr<ParsedExpression> &expr, idx_t depth);

	virtual string UnsupportedAggregateMessage();
	virtual string UnsupportedWindowMessage();
	virtual string UnsupportedUnnestMessage();
	optional_ptr<CatalogEntry> GetCatalogEntry(const Identifier &catalog, const Identifier &schema,
	                                           const EntryLookupInfo &lookup_info, OnEntryNotFound on_entry_not_found);
	//! Look up an entry using the (possibly nested) qualification carried in the lookup itself
	optional_ptr<CatalogEntry> GetCatalogEntry(const EntryLookupInfo &lookup_info, OnEntryNotFound on_entry_not_found);

	Binder &binder;
	ClientContext &context;
	vector<BoundColumnReferenceInfo> bound_columns;
	bool inside_try = false;
	//! Set while binding the subquery of a TRY, so the subquery binder knows TRY is lowered onto its plan
	bool bind_try_subquery = false;
	//! Set while binding the select item of a TRY subquery, see SetTransportTryErrors
	bool transport_try_errors = false;

	BindResult TryBindLambdaOrJson(FunctionExpression &function, idx_t depth, CatalogEntry &func,
	                               const LambdaSyntaxType syntax_type);

	virtual void ThrowIfUnnestInLambda(const ColumnBinding &column_binding);
};

} // namespace duckdb
