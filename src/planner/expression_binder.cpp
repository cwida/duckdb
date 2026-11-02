#include "duckdb/planner/expression_binder.hpp"

#include "duckdb/function/match_recognize.hpp"

#include "duckdb/parser/expression/list.hpp"
#include "duckdb/parser/parsed_expression_iterator.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/common/operator/cast_operators.hpp"
#include "duckdb/main/client_config.hpp"
#include "duckdb/main/settings.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {

void ExpressionBinder::SetCatalogLookupCallback(catalog_entry_callback_t callback) {
	binder.SetCatalogLookupCallback(std::move(callback));
}

ExpressionBinder::ExpressionBinder(Binder &binder, ClientContext &context) : binder(binder), context(context) {
	InitializeStackCheck();
}

ExpressionBinder::~ExpressionBinder() {
}

void ExpressionBinder::InitializeStackCheck() {
	static constexpr idx_t INITIAL_DEPTH = 5;
	if (binder.HasEnclosingScope()) {
		stack_depth = binder.GetInnermostScope().stack_depth + INITIAL_DEPTH;
	} else {
		stack_depth = INITIAL_DEPTH;
	}
}

void ExpressionBinder::ContinueStackDepth(const ExpressionBinder &parent) {
	stack_depth = parent.stack_depth;
}

StackChecker<ExpressionBinder> ExpressionBinder::StackCheck(const ParsedExpression &expr, idx_t extra_stack) {
	D_ASSERT(stack_depth != DConstants::INVALID_INDEX);
	auto max_expression_depth = Settings::Get<MaxExpressionDepthSetting>(context);
	if (stack_depth + extra_stack >= max_expression_depth) {
		throw BinderException("Max expression depth limit of %lld exceeded. Use \"SET max_expression_depth TO x\" to "
		                      "increase the maximum expression depth.",
		                      max_expression_depth);
	}
	return StackChecker<ExpressionBinder>(*this, extra_stack);
}

BindResult ExpressionBinder::BindExpression(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression) {
	auto stack_checker = StackCheck(*expr);

	if (binder.HasEnclosingScope()) {
		// an expression an enclosing query groups by is a reference to that group, e.g. `(SELECT x::DATE)`
		// inside a query that groups by `x::DATE`
		auto owner = ResolveOuterGroupOwner(*expr);
		if (owner.IsValid()) {
			return BindGroupInEnclosingScope(expr, owner.GetIndex(), depth);
		}
	}
	auto &expr_ref = *expr;
	switch (expr_ref.GetExpressionClass()) {
	case ExpressionClass::BETWEEN:
		return BindExpression(expr_ref.Cast<BetweenExpression>(), depth);
	case ExpressionClass::CASE:
		return BindExpression(expr_ref.Cast<CaseExpression>(), depth);
	case ExpressionClass::CAST:
		return BindExpression(expr_ref.Cast<CastExpression>(), depth);
	case ExpressionClass::COLLATE:
		return BindExpression(expr_ref.Cast<CollateExpression>(), depth);
	case ExpressionClass::COLUMN_REF:
		return BindExpression(expr_ref.Cast<ColumnRefExpression>(), depth, root_expression, expr);
	case ExpressionClass::LAMBDA_REF:
		return BindExpression(expr_ref.Cast<LambdaRefExpression>(), depth);
	case ExpressionClass::COMPARISON:
		return BindExpression(expr_ref.Cast<ComparisonExpression>(), depth);
	case ExpressionClass::CONJUNCTION:
		return BindExpression(expr_ref.Cast<ConjunctionExpression>(), depth);
	case ExpressionClass::CONSTANT:
		return BindExpression(expr_ref.Cast<ConstantExpression>(), depth);
	case ExpressionClass::TYPE:
		return BindExpression(expr_ref.Cast<TypeExpression>(), depth);
	case ExpressionClass::FUNCTION: {
		auto &function = expr_ref.Cast<FunctionExpression>();
		if (IsUnnestFunction(function.FunctionName())) {
			// special case, not in catalog
			return BindUnnest(function, depth, root_expression);
		}
		if (function.FunctionName() == MATCH_RECOGNIZE_RUNNING_MARKER ||
		    function.FunctionName() == MATCH_RECOGNIZE_FINAL_MARKER) {
			// RUNNING and FINAL say how much of a match the call below them reads, and only the
			// MEASURES binder knows what a match is
			return BindResult(BinderException::Unsupported(
			    function, "RUNNING and FINAL are only meaningful in the MEASURES of a MATCH_RECOGNIZE"));
		}
		// binding a function expression requires an extra parameter for macros
		return BindExpression(function, depth, expr);
	}
	case ExpressionClass::LAMBDA: {
		const vector<LogicalType> function_child_types;
		return BindExpression(expr_ref.Cast<LambdaExpression>(), depth, function_child_types, nullptr, nullptr);
	}
	case ExpressionClass::PATTERN:
		// a row pattern is matched by MATCH_RECOGNIZE rather than evaluated, so it is not an expression
		// anywhere an expression is expected
		return BindResult(BinderException::Unsupported(expr_ref, "A row pattern is only meaningful in the PATTERN "
		                                                         "clause of MATCH_RECOGNIZE"));
	case ExpressionClass::OPERATOR:
		return BindExpression(expr_ref.Cast<OperatorExpression>(), depth);
	case ExpressionClass::SUBQUERY:
		return BindExpression(expr_ref.Cast<SubqueryExpression>(), depth);
	case ExpressionClass::PARAMETER:
		return BindExpression(expr_ref.Cast<ParameterExpression>(), depth);
	case ExpressionClass::POSITIONAL_REFERENCE:
		return BindPositionalReference(expr, depth, root_expression);
	case ExpressionClass::STAR:
		return BindResult(BinderException::Unsupported(expr_ref, "STAR expression is not supported here"));
	case ExpressionClass::WINDOW:
		// a binder that does not handle windows itself reports that they are unsupported here
		return BindUnsupportedExpression(expr_ref, depth, UnsupportedWindowMessage());
	default:
		return BindResult(
		    NotImplementedException("Unimplemented expression class in ExpressionBinder::BindExpression: %s",
		                            EnumUtil::ToString(expr_ref.GetExpressionClass())));
	}
}

BindResult ExpressionBinder::DispatchToScope(idx_t scope, unique_ptr<ParsedExpression> &expr_ptr, idx_t base_depth) {
	auto result = ScopeAt(scope).BindExpression(expr_ptr, base_depth + scope, false);
	if (!result.HasError()) {
		// the reference reaches out of this scope: record it as a correlated column of this binder
		ExtractCorrelatedExpressions(binder, *result.expression);
	}
	return result;
}

BindResult ExpressionBinder::BindInEnclosingScope(ColumnRefExpression &col_ref, idx_t depth,
                                                  unique_ptr<ParsedExpression> &expr_ptr, ErrorData local_error) {
	auto bind_error = std::move(local_error);
	// the index of a scope is a depth, so a scope pushed or popped while the search is running would
	// shift every index underneath it
	const auto initial_scope_count = ScopeCount();
	idx_t scope = 1;
	while (scope < initial_scope_count) {
		D_ASSERT(ScopeCount() == initial_scope_count);
		auto resolution = ResolveColumn(col_ref, scope);
		if (!resolution.found) {
			// no scope reaches the name: report it against every scope that was searched
			CombineErrors(bind_error, std::move(resolution.error));
			break;
		}
		// bind the qualified form the scope produced, so that it is recognised by that scope, e.g. a reference to one
		// of its groups. The original is kept so the search can go on.
		auto original = std::move(expr_ptr);
		if (resolution.qualified) {
			expr_ptr = std::move(resolution.qualified);
		} else {
			expr_ptr = original->Copy();
		}
		auto result = DispatchToScope(resolution.depth, expr_ptr, depth);
		if (!result.HasError()) {
			return result;
		}
		// the scope reaches the name but cannot bind it, e.g. a column shadowing a table alias, so the scopes outside
		// it still get their turn
		expr_ptr = std::move(original);
		CombineErrors(bind_error, std::move(result.error));
		scope = resolution.depth + 1;
	}
	bind_error.AddQueryLocation(col_ref);
	return BindResult(std::move(bind_error));
}

unique_ptr<Expression> ExpressionBinder::BindChild(unique_ptr<ParsedExpression> &expr, idx_t depth, ErrorData &error) {
	if (!expr) {
		return nullptr;
	}
	auto result = Bind(expr, depth);
	if (result.HasError()) {
		if (!error.HasError()) {
			error = std::move(result.error);
		}
		return nullptr;
	}
	return std::move(result.expression);
}

void ExpressionBinder::ExtractCorrelatedExpressions(Binder &binder, Expression &expr) {
	if (expr.GetExpressionType() == ExpressionType::BOUND_COLUMN_REF) {
		auto &bound_colref = expr.Cast<BoundColumnRefExpression>();
		if (bound_colref.Depth() > 0) {
			binder.AddCorrelatedColumn(CorrelatedColumnInfo(bound_colref));
		}
	}
	ExpressionIterator::EnumerateChildren(expr,
	                                      [&](Expression &child) { ExtractCorrelatedExpressions(binder, child); });
}

bool ExpressionBinder::ContainsType(const LogicalType &type, LogicalTypeId target) {
	if (type.id() == target) {
		return true;
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		auto child_count = StructType::GetChildCount(type);
		for (idx_t i = 0; i < child_count; i++) {
			if (ContainsType(StructType::GetChildType(type, i), target)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::UNION: {
		auto member_count = UnionType::GetMemberCount(type);
		for (idx_t i = 0; i < member_count; i++) {
			if (ContainsType(UnionType::GetMemberType(type, i), target)) {
				return true;
			}
		}
		return false;
	}
	case LogicalTypeId::LIST:
	case LogicalTypeId::MAP:
		return ContainsType(ListType::GetChildType(type), target);
	case LogicalTypeId::ARRAY:
		return ContainsType(ArrayType::GetChildType(type), target);
	default:
		return false;
	}
}

LogicalType ExpressionBinder::ExchangeType(const LogicalType &type, LogicalTypeId target, LogicalType new_type) {
	if (type.id() == target) {
		return new_type;
	}
	switch (type.id()) {
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE: {
		// we make a copy of the child types of the struct here
		auto child_types = StructType::GetChildTypes(type);
		for (auto &child_type : child_types) {
			child_type.second = ExchangeType(child_type.second, target, new_type);
		}
		return type.id() == LogicalTypeId::TUPLE ? LogicalType::TUPLE(std::move(child_types))
		                                         : LogicalType::STRUCT(std::move(child_types));
	}
	case LogicalTypeId::UNION: {
		auto member_types = UnionType::CopyMemberTypes(type);
		for (auto &member_type : member_types) {
			member_type.second = ExchangeType(member_type.second, target, new_type);
		}
		return LogicalType::UNION(std::move(member_types));
	}
	case LogicalTypeId::LIST:
		return LogicalType::LIST(ExchangeType(ListType::GetChildType(type), target, new_type));
	case LogicalTypeId::MAP:
		return LogicalType::MAP(ExchangeType(ListType::GetChildType(type), target, new_type));
	case LogicalTypeId::ARRAY:
		return LogicalType::ARRAY(ExchangeType(ArrayType::GetChildType(type), target, new_type),
		                          ArrayType::GetSize(type));
	default:
		return type;
	}
}

bool ExpressionBinder::ContainsNullType(const LogicalType &type) {
	return ContainsType(type, LogicalTypeId::SQLNULL);
}

LogicalType ExpressionBinder::ExchangeNullType(const LogicalType &type) {
	return ExchangeType(type, LogicalTypeId::SQLNULL, LogicalType::INTEGER);
}

unique_ptr<Expression> ExpressionBinder::Bind(unique_ptr<ParsedExpression> &expr, optional_ptr<LogicalType> result_type,
                                              bool root_expression) {
	// bind the main expression - a name that reaches out of this scope is resolved where it occurs
	auto bind_result = Bind(expr, 0, root_expression);
	if (bind_result.HasError()) {
		bind_result.error.Throw();
	}
	unique_ptr<Expression> result = std::move(bind_result.expression);
	if (target_type.id() != LogicalTypeId::INVALID) {
		// the binder has a specific target type: add a cast to that type
		result = BoundCastExpression::AddCastToType(context, std::move(result), target_type);
	} else {
		if (!binder.CanContainNulls()) {
			// SQL NULL type is only used internally in the binder
			// cast to INTEGER if we encounter it outside of the binder
			if (ContainsNullType(result->GetReturnType())) {
				auto exchanged_type = ExchangeNullType(result->GetReturnType());
				result = BoundCastExpression::AddCastToType(context, std::move(result), exchanged_type);
			}
		}
		if (ContainsType(result->GetReturnType(), LogicalTypeId::UNKNOWN)) {
			throw ParameterNotResolvedException();
		}
	}
	if (result_type) {
		*result_type = result->GetReturnType();
	}
	return result;
}

BindResult ExpressionBinder::Bind(unique_ptr<ParsedExpression> &expr, idx_t depth, bool root_expression) {
	auto query_location = expr->GetQueryLocation();
	auto alias = expr->GetAlias();
	BindResult result = BindExpression(expr, depth, root_expression);
	if (result.HasError()) {
		return result;
	}
	// carry the location and alias of the parsed node over to the bound expression
	result.expression->SetQueryLocation(query_location);
	if (!alias.empty()) {
		result.expression->SetAlias(alias);
	}
	return result;
}

BindResult ExpressionBinder::BindUnsupportedExpression(ParsedExpression &expr, idx_t depth, const string &message) {
	// we always prefer to throw an error if it occurs in a child expression
	// since that error might be more descriptive
	// bind all children
	ErrorData result;
	ParsedExpressionIterator::EnumerateChildren(expr, [&](unique_ptr<ParsedExpression> &child) {
		// the children are bound only to surface their errors - the expression itself is unsupported
		(void)BindChild(child, depth, result);
	});
	if (result.HasError()) {
		return BindResult(std::move(result));
	}
	return BindResult(BinderException::Unsupported(expr, message));
}

bool ExpressionBinder::IsUnnestFunction(const Identifier &function_name) {
	return function_name == "unnest" || function_name == "unlist";
}

bool ExpressionBinder::IsPotentialAlias(const ColumnRefExpression &colref) {
	// traditional alias (unqualified), or qualified with table name "alias"
	if (!colref.IsQualified()) {
		return true;
	}
	if (colref.ColumnNames().size() == 2) {
		return colref.ColumnNames()[0] == "alias";
	}
	return false;
}

} // namespace duckdb
