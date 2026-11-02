#include "duckdb/catalog/catalog_entry/aggregate_function_catalog_entry.hpp"
#include "duckdb/function/builtin_function_lookup.hpp"
#include "duckdb/function/scalar/struct_functions.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_expanded_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_parameter_expression.hpp"
#include "duckdb/planner/expression_binder/group_binder.hpp"
#include "duckdb/planner/expression_binder/select_binder.hpp"
#include "duckdb/planner/query_node/bound_select_node.hpp"
#include "duckdb/planner/expression/bound_unnest_expression.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/main/settings.hpp"

namespace duckdb {

static unique_ptr<Expression> BindStructExtract(ClientContext &context, vector<unique_ptr<Expression>> arguments,
                                                const LogicalType &child_type) {
	auto result = BindBuiltinScalarExpression(context, StructExtractFun::Name, std::move(arguments));
	if (result->GetReturnType() != child_type) {
		// a NULL input is folded into an untyped NULL constant
		result = BoundCastExpression::AddCastToType(context, std::move(result), child_type);
	}
	return result;
}

static unique_ptr<Expression> CreateBoundStructExtract(ClientContext &context, unique_ptr<Expression> expr,
                                                       const LogicalType &child_type, const vector<string> &key_path,
                                                       bool keep_parent_names) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(expr));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value(key_path.back())));
	auto result = BindStructExtract(context, std::move(arguments), child_type);

	if (keep_parent_names) {
		auto alias = StringUtil::Join(key_path, ".");
		if (!alias.empty() && alias[0] == '.') {
			alias = alias.substr(1);
		}
		result->SetAlias(Identifier(alias));
	} else {
		result->SetAlias(Identifier(key_path[0]));
	}
	return result;
}

static unique_ptr<Expression> CreateBoundStructExtractIndex(ClientContext &context, unique_ptr<Expression> expr,
                                                            const LogicalType &child_type, idx_t key) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(std::move(expr));
	arguments.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(int64_t(key))));
	auto result = BindStructExtract(context, std::move(arguments), child_type);

	result->SetAlias(Identifier("element" + to_string(key)));
	return result;
}

void SelectBinder::ThrowIfUnnestInLambda(const ColumnBinding &column_binding) {
	// Extract the unnests and check if any match the column index.
	auto check_unnests = [&](const BoundUnnestMap &unnests) {
		for (auto &node_pair : unnests) {
			auto &unnest_node = node_pair.second;

			if (unnest_node.index == column_binding.table_index) {
				if (column_binding.column_index < unnest_node.expressions.size()) {
					throw BinderException("UNNEST in lambda expressions is not supported");
				}
			}
		}
	};
	check_unnests(node.unnests.SelectList());
	check_unnests(node.unnests.GroupBy());
}

static void AddUnnestExpression(Binder &binder, BoundUnnestMap &unnests, idx_t current_level,
                                unique_ptr<BoundUnnestExpression> result, TableIndex &unnest_table_index,
                                ProjectionIndex &unnest_column_index) {
	auto entry = unnests.find(current_level);
	if (entry == unnests.end()) {
		BoundUnnestNode unnest_node;
		unnest_node.index = binder.GenerateTableIndex();
		unnest_table_index = unnest_node.index;
		unnest_column_index = ColumnBinding::PushExpression(unnest_node.expressions, std::move(result));
		unnests.insert(make_pair(current_level, std::move(unnest_node)));
	} else {
		unnest_table_index = entry->second.index;
		unnest_column_index = ColumnBinding::PushExpression(entry->second.expressions, std::move(result));
	}
}

enum class StructUnnestHandling : uint8_t { ALLOW_ROOT, REJECT };

class UnnestLevelGuard {
public:
	explicit UnnestLevelGuard(idx_t &unnest_level) : unnest_level(unnest_level) {
		unnest_level++;
	}
	~UnnestLevelGuard() {
		unnest_level--;
	}

private:
	idx_t &unnest_level;
};

class UnnestBinder {
public:
	UnnestBinder(ExpressionBinder &expression_binder, Binder &binder, ClientContext &context, BoundUnnestMap &unnests,
	             idx_t &unnest_level, StructUnnestHandling struct_handling, string struct_error)
	    : expression_binder(expression_binder), binder(binder), context(context), unnests(unnests),
	      unnest_level(unnest_level), struct_handling(struct_handling), struct_error(std::move(struct_error)) {
	}

	BindResult Bind(FunctionExpression &function, idx_t depth, bool root_expression);

private:
	ExpressionBinder &expression_binder;
	Binder &binder;
	ClientContext &context;
	BoundUnnestMap &unnests;
	idx_t &unnest_level;
	StructUnnestHandling struct_handling;
	string struct_error;
};

BindResult UnnestBinder::Bind(FunctionExpression &function, idx_t depth, bool root_expression) {
	// bind the children of the function expression
	if (depth > 0) {
		return BindResult(BinderException(function, "UNNEST() for correlated expressions is not supported yet"));
	}

	ErrorData error;
	if (function.GetArguments().empty()) {
		return BindResult(BinderException(function, "UNNEST() requires at least one argument"));
	}

	if (function.Distinct() || function.Filter() || !function.OrderBy()->orders.empty()) {
		throw InvalidInputException("\"DISTINCT\", \"FILTER\", and \"ORDER BY\" are not "
		                            "applicable to \"UNNEST\"");
	}

	idx_t max_depth = 1;
	bool keep_parent_names = false;
	auto &args = function.GetArgumentsMutable();

	if (args.size() != 1) {
		bool supported_argument = false;
		for (idx_t i = 1; i < args.size(); i++) {
			if (args[i].GetExpression().HasParameter()) {
				throw ParameterNotAllowedException("Parameter not allowed in unnest parameter");
			}
			if (!args[i].GetExpression().IsScalar()) {
				break;
			}
			auto alias = args[i].GetExpression().GetAlias();
			auto const_child = expression_binder.BindChild(args[i].GetExpressionMutable(), depth, error);
			if (error.HasError()) {
				return BindResult(std::move(error));
			}
			if (!const_child->IsFoldable()) {
				throw BinderException(function, "UNNEST parameter %s must be a constant", alias);
			}
			auto value = ExpressionExecutor::EvaluateScalar(context, *const_child, true);
			if (value.IsNull()) {
				throw BinderException(function, "UNNEST parameter %s cannot be NULL", alias);
			}
			if (alias == "recursive") {
				auto recursive = value.GetValue<bool>();
				if (recursive) {
					max_depth = NumericLimits<idx_t>::Maximum();
				}
			} else if (alias == "max_depth") {
				max_depth = value.GetValue<uint32_t>();
				if (max_depth == 0) {
					throw BinderException("UNNEST cannot have a max depth of 0");
				}
			} else if (alias == "keep_parent_names") {
				keep_parent_names = value.GetValue<bool>();
			} else if (!alias.empty()) {
				throw BinderException("Unsupported parameter %s for unnest", alias);
			} else {
				break;
			}
			supported_argument = true;
		}
		if (!supported_argument) {
			return BindResult(BinderException(
			    function, "UNNEST - unsupported extra argument, unnest only supports "
			              "recursive := [true/false], max_depth := # or keep_parent_names := [true/false]"));
		}
	}
	auto outer_unnest_level = unnest_level;
	UnnestLevelGuard unnest_level_guard(unnest_level);
	auto child = expression_binder.BindChild(args[0].GetExpressionMutable(), depth, error);
	if (error.HasError()) {
		return BindResult(std::move(error));
	}
	child = BoundCastExpression::AddArrayCastToList(context, std::move(child));
	auto &child_type = child->GetReturnType();

	if (outer_unnest_level > 0) {
		throw BinderException(
		    function,
		    "Nested UNNEST calls are not supported - use UNNEST(x, recursive := true) to unnest multiple levels");
	}

	switch (child_type.id()) {
	case LogicalTypeId::UNKNOWN:
		throw ParameterNotResolvedException();
	case LogicalTypeId::LIST:
	case LogicalTypeId::STRUCT:
	case LogicalTypeId::TUPLE:
	case LogicalTypeId::SQLNULL:
		break;
	default:
		return BindResult(BinderException(function, "UNNEST() can only be applied to lists, structs and NULL, not %s",
		                                  child_type.ToString()));
	}

	idx_t list_unnests;
	idx_t struct_unnests = 0;

	auto unnest_expr = std::move(child);
	if (child_type.id() == LogicalTypeId::SQLNULL) {
		list_unnests = 1;
	} else {
		// perform all LIST/ARRAY unnests
		auto type = child_type;
		list_unnests = 0;
		while (type.id() == LogicalTypeId::LIST || type.id() == LogicalTypeId::ARRAY) {
			if (type.id() == LogicalTypeId::LIST) {
				type = ListType::GetChildType(type);
			} else {
				type = ArrayType::GetChildType(type);
			}
			list_unnests++;
			if (list_unnests >= max_depth) {
				break;
			}
		}
		// unnest structs
		if (StructType::IsStruct(type)) {
			struct_unnests = max_depth - list_unnests;
		}
	}
	if (struct_unnests > 0 && struct_handling == StructUnnestHandling::REJECT) {
		child = std::move(unnest_expr);
		return BindResult(BinderException(function, struct_error));
	}
	if (struct_unnests > 0 && !root_expression) {
		child = std::move(unnest_expr);
		return BindResult(BinderException(
		    function, "UNNEST() on a struct column can only be applied as the root element of a SELECT expression"));
	}
	// perform all LIST unnests
	auto return_type = child_type;
	for (idx_t current_depth = 0; current_depth < list_unnests; current_depth++) {
		if (return_type.id() == LogicalTypeId::LIST) {
			return_type = ListType::GetChildType(return_type);
		} else if (return_type.id() == LogicalTypeId::ARRAY) {
			return_type = ArrayType::GetChildType(return_type);
		}

		if (unnest_expr->GetReturnType().id() == LogicalTypeId::ARRAY) {
			unnest_expr = BoundCastExpression::AddArrayCastToList(context, std::move(unnest_expr));
		}

		auto result = make_uniq<BoundUnnestExpression>(return_type);
		result->ChildMutable() = std::move(unnest_expr);
		Identifier alias = function.GetAlias().empty() ? Identifier(result->ToString()) : function.GetAlias();

		auto current_level = outer_unnest_level + list_unnests - current_depth - 1;
		TableIndex unnest_table_index;
		ProjectionIndex unnest_column_index;
		AddUnnestExpression(binder, unnests, current_level, std::move(result), unnest_table_index, unnest_column_index);
		// now create a column reference referring to the unnest
		unnest_expr = make_uniq<BoundColumnRefExpression>(
		    std::move(alias), return_type, ColumnBinding(unnest_table_index, unnest_column_index), depth);
	}
	// now perform struct unnests, if any
	if (struct_unnests > 0) {
		vector<unique_ptr<Expression>> struct_expressions;
		struct_expressions.push_back(std::move(unnest_expr));
		for (idx_t i = 0; i < struct_unnests; i++) {
			vector<unique_ptr<Expression>> new_expressions;
			// check if there are any structs left
			bool has_structs = false;
			for (auto &expr : struct_expressions) {
				if (StructType::IsStruct(expr->GetReturnType())) {
					// struct! push a struct_extract
					auto &child_types = StructType::GetChildTypes(expr->GetReturnType());
					if (expr->GetReturnType().id() == LogicalTypeId::TUPLE) {
						for (idx_t child_index = 0; child_index < child_types.size(); child_index++) {
							new_expressions.push_back(CreateBoundStructExtractIndex(
							    context, expr->Copy(), child_types[child_index].second, child_index + 1));
						}
					} else {
						for (auto &entry : child_types) {
							vector<string> current_key_path;
							// During recursive expansion, not all expressions are BoundFunctionExpression
							if (keep_parent_names && expr->GetExpressionClass() == ExpressionClass::BOUND_FUNCTION) {
								current_key_path.emplace_back(expr->GetAlias());
							}
							current_key_path.emplace_back(entry.first);
							new_expressions.push_back(CreateBoundStructExtract(context, expr->Copy(), entry.second,
							                                                   current_key_path, keep_parent_names));
						}
					}
					has_structs = true;
				} else {
					// not a struct - push as-is
					new_expressions.push_back(std::move(expr));
				}
			}
			struct_expressions = std::move(new_expressions);
			if (!has_structs) {
				break;
			}
		}
		unnest_expr = make_uniq<BoundExpandedExpression>(std::move(struct_expressions));
	}
	return BindResult(std::move(unnest_expr));
}

BindResult SelectBinder::BindUnnest(FunctionExpression &function, idx_t depth, bool root_expression) {
	if (inside_window || inside_aggregate || inside_try) {
		return BindResult(BinderException(function, UnsupportedUnnestMessage()));
	}
	UnnestBinder unnest_binder(*this, binder, context, node.unnests.SelectList(), unnest_level,
	                           StructUnnestHandling::ALLOW_ROOT, "UNNEST of struct cannot be used in GROUP BY clause");
	return unnest_binder.Bind(function, depth, root_expression);
}

BindResult GroupBinder::BindUnnest(FunctionExpression &function, idx_t depth, bool root_expression) {
	if (inside_try) {
		return BindResult(BinderException(function, UnsupportedUnnestMessage()));
	}
	UnnestBinder unnest_binder(*this, binder, context, node.unnests.GroupBy(), unnest_level,
	                           StructUnnestHandling::REJECT, "UNNEST of struct cannot be used in GROUP BY clause");
	return unnest_binder.Bind(function, depth, root_expression);
}

} // namespace duckdb
