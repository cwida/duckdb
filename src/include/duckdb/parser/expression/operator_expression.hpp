//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/operator_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/string_util.hpp"

namespace duckdb {
//! Represents a built-in operator expression
class OperatorExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::OPERATOR;

public:
	DUCKDB_API explicit OperatorExpression(ExpressionType type, unique_ptr<ParsedExpression> left = nullptr,
	                                       unique_ptr<ParsedExpression> right = nullptr);
	DUCKDB_API OperatorExpression(ExpressionType type, vector<unique_ptr<ParsedExpression>> children);

	//! The placeholder for an omitted slice bound (arr[:n], arr[n:]): an empty list constructor
	DUCKDB_API static unique_ptr<ParsedExpression> EmptySliceBound();
	//! Whether the rendered slice bound is the placeholder (parsed: list_value(), bound and folded: [])
	static bool IsEmptySliceBound(const string &rendered_bound) {
		return rendered_bound == "list_value()" || rendered_bound == "[]";
	}

public:
	const vector<unique_ptr<ParsedExpression>> &GetChildren() const {
		return children;
	}
	vector<unique_ptr<ParsedExpression>> &GetChildrenMutable() {
		return children;
	}
	string ToString() const override;

	bool Equals(const ParsedExpression &other) const override;

	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);

private:
	OperatorExpression();

public:
	template <class T, class BASE>
	static string ToString(const T &entry) {
		auto &children = entry.GetChildren();
		auto op = ExpressionTypeToOperator(entry.GetExpressionType());
		if (!op.empty()) {
			// use the operator string to represent the operator
			D_ASSERT(children.size() == 2);
			return children[0]->ToString() + " " + op + " " + children[1]->ToString();
		}
		switch (entry.GetExpressionType()) {
		case ExpressionType::COMPARE_IN:
		case ExpressionType::COMPARE_NOT_IN: {
			string op_type = entry.GetExpressionType() == ExpressionType::COMPARE_IN ? " IN " : " NOT IN ";
			string in_child = children[0]->ToString();
			string child_list = "(";
			for (idx_t i = 1; i < children.size(); i++) {
				if (i > 1) {
					child_list += ", ";
				}
				child_list += children[i]->ToString();
			}
			child_list += ")";
			return "(" + in_child + op_type + child_list + ")";
		}
		case ExpressionType::OPERATOR_UNPACK: {
			return StringUtil::Format("UNPACK(%s)", children[0]->ToString());
		}
		case ExpressionType::OPERATOR_TRY: {
			return StringUtil::Format("TRY(%s)", children[0]->ToString());
		}
		case ExpressionType::OPERATOR_HAS_ERROR: {
			return StringUtil::Format("HAS_ERROR(%s)", children[0]->ToString());
		}
		case ExpressionType::OPERATOR_NOT: {
			string result = "(";
			result += ExpressionTypeToString(entry.GetExpressionType());
			result += " ";
			result += StringUtil::Join(children, children.size(), ", ",
			                           [](const unique_ptr<BASE> &child) { return child->ToString(); });
			result += ")";
			return result;
		}
		case ExpressionType::GROUPING_FUNCTION:
		case ExpressionType::OPERATOR_COALESCE: {
			string result = ExpressionTypeToString(entry.GetExpressionType());
			result += "(";
			result += StringUtil::Join(children, children.size(), ", ",
			                           [](const unique_ptr<BASE> &child) { return child->ToString(); });
			result += ")";
			return result;
		}
		case ExpressionType::OPERATOR_IS_NULL:
			return "(" + children[0]->ToString() + " IS NULL)";
		case ExpressionType::OPERATOR_IS_NOT_NULL:
			return "(" + children[0]->ToString() + " IS NOT NULL)";
		case ExpressionType::ARRAY_EXTRACT: {
			auto base = children[0]->ToString();
			if (children[0]->GetExpressionClass() == ExpressionClass::COLLATE ||
			    (children[0]->GetExpressionClass() == ExpressionClass::OPERATOR &&
			     children[0]->GetExpressionType() != ExpressionType::ARRAY_EXTRACT &&
			     children[0]->GetExpressionType() != ExpressionType::STRUCT_EXTRACT) ||
			    (children[0]->GetExpressionClass() == ExpressionClass::CONSTANT && !base.empty() && base[0] == '-')) {
				base = "(" + base + ")";
			}
			return base + "[" + children[1]->ToString() + "]";
		}
		case ExpressionType::ARRAY_SLICE: {
			auto base = children[0]->ToString();
			if (children[0]->GetExpressionClass() == ExpressionClass::COLLATE ||
			    (children[0]->GetExpressionClass() == ExpressionClass::OPERATOR &&
			     children[0]->GetExpressionType() != ExpressionType::ARRAY_EXTRACT &&
			     children[0]->GetExpressionType() != ExpressionType::STRUCT_EXTRACT) ||
			    (children[0]->GetExpressionClass() == ExpressionClass::CONSTANT && !base.empty() && base[0] == '-')) {
				base = "(" + base + ")";
			}
			string begin = children[1]->ToString();
			if (IsEmptySliceBound(begin)) {
				begin = "";
			}
			string end = children[2]->ToString();
			if (IsEmptySliceBound(end)) {
				if (children.size() == 4) {
					end = "-";
				} else {
					end = "";
				}
			}
			if (children.size() == 4) {
				return base + "[" + begin + ":" + end + ":" + children[3]->ToString() + "]";
			}
			return base + "[" + begin + ":" + end + "]";
		}
		case ExpressionType::STRUCT_EXTRACT: {
			if (children[1]->GetExpressionType() != ExpressionType::VALUE_CONSTANT) {
				return string();
			}
			auto child_string = children[1]->ToString();
			D_ASSERT(child_string.size() >= 3);
			D_ASSERT(child_string[0] == '\'' && child_string[child_string.size() - 1] == '\'');
			return StringUtil::Format("(%s).%s", children[0]->ToString(),
			                          SQLIdentifier(child_string.substr(1, child_string.size() - 2)));
		}
		case ExpressionType::ARRAY_CONSTRUCTOR: {
			string result = "(ARRAY[";
			result += StringUtil::Join(children, children.size(), ", ",
			                           [](const unique_ptr<BASE> &child) { return child->ToString(); });
			result += "])";
			return result;
		}
		default:
			throw InternalException("Unrecognized operator type");
		}
	}

private:
	vector<unique_ptr<ParsedExpression>> children;
};

} // namespace duckdb
