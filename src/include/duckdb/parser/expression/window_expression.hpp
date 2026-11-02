//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/window_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/query_node.hpp"
#include "duckdb/parser/expression/function_expression.hpp"

namespace duckdb {

enum class WindowBoundary : uint8_t {
	INVALID = 0,
	UNBOUNDED_PRECEDING = 1,
	UNBOUNDED_FOLLOWING = 2,
	CURRENT_ROW_RANGE = 3,
	CURRENT_ROW_ROWS = 4,
	EXPR_PRECEDING_ROWS = 5,
	EXPR_FOLLOWING_ROWS = 6,
	EXPR_PRECEDING_RANGE = 7,
	EXPR_FOLLOWING_RANGE = 8,
	CURRENT_ROW_GROUPS = 9,
	EXPR_PRECEDING_GROUPS = 10,
	EXPR_FOLLOWING_GROUPS = 11
};

//! Represents the window exclusion mode
enum class WindowExcludeMode : uint8_t { NO_OTHER = 0, CURRENT_ROW = 1, GROUP = 2, TIES = 3 };

const char *ToString(WindowBoundary value);

//! The WindowExpression represents a window function in the query.
class WindowExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::WINDOW;

public:
	WindowExpression(const string &catalog_name, const string &schema, const string &function_name);

public:
	bool IsWindow() const override {
		return true;
	}

	//! Convert the Expression to a String
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;
	hash_t Hash() const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

	static string ExpressionTypeToWindow(ExpressionType expression_type);
	void SetFunctionName(const string &function_name);

	static ExpressionType WindowToExpressionType(const string &fun_name);

public:
	const QualifiedName &GetQualifiedName() const {
		return qualified_name;
	}
	QualifiedName &GetQualifiedNameMutable() {
		return qualified_name;
	}
	void SetQualifiedName(QualifiedName name) {
		qualified_name = std::move(name);
		type = WindowToExpressionType(qualified_name.Name().GetIdentifierName());
	}
	void SetQualifiedName(Identifier catalog, Identifier schema, Identifier name) {
		SetQualifiedName(QualifiedName(std::move(catalog), std::move(schema), std::move(name)));
	}
	const Identifier &FunctionName() const {
		return qualified_name.Name();
	}
	const vector<unique_ptr<ParsedExpression>> &Partitions() const {
		return partitions;
	}
	vector<unique_ptr<ParsedExpression>> &PartitionsMutable() {
		return partitions;
	}
	const vector<OrderByNode> &OrderBy() const {
		return orders;
	}
	vector<OrderByNode> &OrderByMutable() {
		return orders;
	}
	const unique_ptr<ParsedExpression> &Filter() const {
		return filter_expr;
	}
	unique_ptr<ParsedExpression> &FilterMutable() {
		return filter_expr;
	}
	bool HasIgnoreNulls() const {
		return has_ignore_nulls;
	}
	bool &HasIgnoreNullsMutable() {
		return has_ignore_nulls;
	}
	bool IgnoreNulls() const {
		return ignore_nulls;
	}
	bool &IgnoreNullsMutable() {
		return ignore_nulls;
	}
	bool Distinct() const {
		return distinct;
	}
	bool &DistinctMutable() {
		return distinct;
	}
	WindowBoundary WindowStart() const {
		return start;
	}
	WindowBoundary &WindowStartMutable() {
		return start;
	}
	WindowBoundary WindowEnd() const {
		return end;
	}
	WindowBoundary &WindowEndMutable() {
		return end;
	}
	WindowExcludeMode WindowExclude() const {
		return exclude_clause;
	}
	WindowExcludeMode &WindowExcludeMutable() {
		return exclude_clause;
	}
	const unique_ptr<ParsedExpression> &StartExpr() const {
		return start_expr;
	}
	unique_ptr<ParsedExpression> &StartExprMutable() {
		return start_expr;
	}
	const unique_ptr<ParsedExpression> &EndExpr() const {
		return end_expr;
	}
	unique_ptr<ParsedExpression> &EndExprMutable() {
		return end_expr;
	}
	const vector<OrderByNode> &ArgOrders() const {
		return arg_orders;
	}
	vector<OrderByNode> &ArgOrdersMutable() {
		return arg_orders;
	}

	const vector<FunctionArgument> &GetArguments() const {
		return arguments;
	}

	vector<FunctionArgument> &GetArgumentsMutable() {
		return arguments;
	}

	static inline string ToUnits(const WindowBoundary boundary, const WindowBoundary rows, const WindowBoundary range,
	                             const WindowBoundary groups) {
		if (boundary == rows) {
			return "ROWS";
		} else if (boundary == range) {
			return "RANGE";
		} else {
			return "GROUPS";
		}
	}

	template <class T, class BASE, class ORDER_NODE>
	static string ToString(const T &entry, const string &qualification, const string &function_name) {
		// Start with function call - the qualification (if any) already ends in a "."
		string result = qualification + function_name;
		result += "(";

		if constexpr (std::is_same_v<T, WindowExpression>) {
			auto &children = entry.GetArguments();
			if (children.size()) {
				//	Only one DISTINCT is allowed (on the first argument)
				int distincts = entry.Distinct() ? 0 : 1;
				result += StringUtil::Join(children, children.size(), ", ", [&](const FunctionArgument &child) {
					return (distincts++ ? "" : "DISTINCT ") + child.ToString();
				});
			}
		} else {
			auto &children = entry.GetChildren();
			if (children.size()) {
				//	Only one DISTINCT is allowed (on the first argument)
				int distincts = entry.Distinct() ? 0 : 1;
				result += StringUtil::Join(children, children.size(), ", ", [&](const unique_ptr<BASE> &child) {
					return (distincts++ ? "" : "DISTINCT ") + child->ToString();
				});
			}
		}

		// ORDER BY arguments
		auto &arg_orders = entry.ArgOrders();
		if (!arg_orders.empty()) {
			result += " ORDER BY ";
			result += StringUtil::Join(arg_orders, arg_orders.size(), ", ",
			                           [](const ORDER_NODE &order) { return order.ToString(); });
		}

		// IGNORE NULLS
		if (entry.IgnoreNulls()) {
			result += " IGNORE NULLS";
		}
		// FILTER
		if (entry.Filter()) {
			result += ") FILTER (WHERE " + entry.Filter()->ToString();
		}

		// Over clause
		result += ") OVER (";
		string sep;

		// Partitions
		auto &partitions = entry.Partitions();
		if (!partitions.empty()) {
			result += "PARTITION BY ";
			result += StringUtil::Join(partitions, partitions.size(), ", ",
			                           [](const unique_ptr<BASE> &partition) { return partition->ToString(); });
			sep = " ";
		}

		// Orders
		auto &orders = entry.OrderBy();
		if (!orders.empty()) {
			result += sep;
			result += "ORDER BY ";
			result +=
			    StringUtil::Join(orders, orders.size(), ", ", [](const ORDER_NODE &order) { return order.ToString(); });
			sep = " ";
		}

		// Rows/Range
		string units = "ROWS";
		string from;
		auto window_start = entry.WindowStart();
		auto window_end = entry.WindowEnd();
		auto &start_expr = entry.StartExpr();
		auto &end_expr = entry.EndExpr();
		switch (window_start) {
		case WindowBoundary::CURRENT_ROW_RANGE:
		case WindowBoundary::CURRENT_ROW_ROWS:
		case WindowBoundary::CURRENT_ROW_GROUPS:
			from = "CURRENT ROW";
			units = ToUnits(window_start, WindowBoundary::CURRENT_ROW_ROWS, WindowBoundary::CURRENT_ROW_RANGE,
			                WindowBoundary::CURRENT_ROW_GROUPS);
			break;
		case WindowBoundary::UNBOUNDED_PRECEDING:
			if (window_end != WindowBoundary::CURRENT_ROW_RANGE) {
				from = "UNBOUNDED PRECEDING";
			}
			break;
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_PRECEDING_RANGE:
		case WindowBoundary::EXPR_PRECEDING_GROUPS:
			from = start_expr->ToString() + " PRECEDING";
			units = ToUnits(window_start, WindowBoundary::EXPR_PRECEDING_ROWS, WindowBoundary::EXPR_PRECEDING_RANGE,
			                WindowBoundary::EXPR_PRECEDING_GROUPS);
			break;
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_RANGE:
		case WindowBoundary::EXPR_FOLLOWING_GROUPS:
			from = start_expr->ToString() + " FOLLOWING";
			units = ToUnits(window_start, WindowBoundary::EXPR_FOLLOWING_ROWS, WindowBoundary::EXPR_FOLLOWING_RANGE,
			                WindowBoundary::EXPR_FOLLOWING_GROUPS);
			break;
		case WindowBoundary::UNBOUNDED_FOLLOWING:
		case WindowBoundary::INVALID:
			throw InternalException("Unrecognized FROM in WindowExpression");
		}

		string to;
		switch (window_end) {
		case WindowBoundary::CURRENT_ROW_RANGE:
			if (window_start != WindowBoundary::UNBOUNDED_PRECEDING) {
				to = "CURRENT ROW";
				units = "RANGE";
			}
			break;
		case WindowBoundary::CURRENT_ROW_ROWS:
		case WindowBoundary::CURRENT_ROW_GROUPS:
			to = "CURRENT ROW";
			units = ToUnits(window_end, WindowBoundary::CURRENT_ROW_ROWS, WindowBoundary::CURRENT_ROW_RANGE,
			                WindowBoundary::CURRENT_ROW_GROUPS);
			break;
		case WindowBoundary::UNBOUNDED_PRECEDING:
			to = "UNBOUNDED PRECEDING";
			break;
		case WindowBoundary::UNBOUNDED_FOLLOWING:
			to = "UNBOUNDED FOLLOWING";
			break;
		case WindowBoundary::EXPR_PRECEDING_ROWS:
		case WindowBoundary::EXPR_PRECEDING_RANGE:
		case WindowBoundary::EXPR_PRECEDING_GROUPS:
			to = end_expr->ToString() + " PRECEDING";
			units = ToUnits(window_end, WindowBoundary::EXPR_PRECEDING_ROWS, WindowBoundary::EXPR_PRECEDING_RANGE,
			                WindowBoundary::EXPR_PRECEDING_GROUPS);
			break;
		case WindowBoundary::EXPR_FOLLOWING_ROWS:
		case WindowBoundary::EXPR_FOLLOWING_RANGE:
		case WindowBoundary::EXPR_FOLLOWING_GROUPS:
			to = end_expr->ToString() + " FOLLOWING";
			units = ToUnits(window_end, WindowBoundary::EXPR_FOLLOWING_ROWS, WindowBoundary::EXPR_FOLLOWING_RANGE,
			                WindowBoundary::EXPR_FOLLOWING_GROUPS);
			break;
		case WindowBoundary::INVALID:
			throw InternalException("Unrecognized TO in WindowExpression");
		}
		auto exclude_clause = entry.WindowExclude();
		if (exclude_clause != WindowExcludeMode::NO_OTHER) {
			// if we have an explicit EXCLUDE we always need to fill in from/to
			if (from.empty()) {
				from = "UNBOUNDED PRECEDING";
			}
			if (to.empty()) {
				to = "CURRENT ROW";
				units = "RANGE";
			}
		}

		if (!from.empty() || !to.empty()) {
			result += sep + units;
		}
		if (!from.empty() && !to.empty()) {
			result += " BETWEEN ";
			result += from;
			result += " AND ";
			result += to;
		} else if (!from.empty()) {
			result += " ";
			result += from;
		} else if (!to.empty()) {
			result += " ";
			result += to;
		}

		if (exclude_clause != WindowExcludeMode::NO_OTHER) {
			result += " EXCLUDE ";
		}
		switch (exclude_clause) {
		case WindowExcludeMode::CURRENT_ROW:
			result += "CURRENT ROW";
			break;
		case WindowExcludeMode::GROUP:
			result += "GROUP";
			break;
		case WindowExcludeMode::TIES:
			result += "TIES";
			break;
		default:
			break;
		}

		result += ")";

		return result;
	}

	bool IsLegacyFunctionCall() const {
		return is_legacy_function_call;
	}

private:
	//! Qualified name of the aggregate function (catalog.schema.name)
	QualifiedName qualified_name;
	//! The child expression of the main window function
	vector<FunctionArgument> arguments;
	//! The set of expressions to partition by
	vector<unique_ptr<ParsedExpression>> partitions;
	//! The set of ordering clauses
	vector<OrderByNode> orders;
	//! Expression representing a filter, only used for aggregates
	unique_ptr<ParsedExpression> filter_expr;
	//! True if we parsed IGNORE/RESPECT NULLS
	bool has_ignore_nulls = false;
	//! True to ignore NULL values
	bool ignore_nulls = false;
	//! Whether or not the aggregate function is distinct, only used for aggregates
	bool distinct = false;
	//! The window boundaries
	WindowBoundary start = WindowBoundary::INVALID;
	WindowBoundary end = WindowBoundary::INVALID;
	//! The EXCLUDE clause
	WindowExcludeMode exclude_clause = WindowExcludeMode::NO_OTHER;

	unique_ptr<ParsedExpression> start_expr;
	unique_ptr<ParsedExpression> end_expr;

	//! The set of argument ordering clauses
	//! These are distinct from the frame ordering clauses e.g., the "x" in
	//! FIRST_VALUE(a ORDER BY x) OVER (PARTITION BY p ORDER BY s)
	vector<OrderByNode> arg_orders;

	//! Whether this function is a legacy function call, which means it was parsed from a function call that does not
	//! use the new function argument syntax. This is used to determine how to handle named arguments during binding.
	bool is_legacy_function_call = false;

private:
	WindowExpression();

	//	Backwards-compatible serialization interface
	//	Remove LEAD/LAG offset/default
	vector<unique_ptr<ParsedExpression>> SerializedChildren(Serializer &serializer) const;
	unique_ptr<ParsedExpression> SerializedOffset(Serializer &serializer) const;
	unique_ptr<ParsedExpression> SerializedDefault(Serializer &serializer) const;
};

} // namespace duckdb
