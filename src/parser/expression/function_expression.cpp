#include "duckdb/parser/expression/function_expression.hpp"

#include <utility>
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

FunctionExpression::FunctionExpression() : ParsedExpression(ExpressionType::FUNCTION, ExpressionClass::FUNCTION) {
}

FunctionExpression::FunctionExpression(const QualifiedName &function_name,
                                       vector<unique_ptr<ParsedExpression>> children_p,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys_p,
                                       bool distinct, bool is_operator, bool export_state_p)
    : ParsedExpression(ExpressionType::FUNCTION, ExpressionClass::FUNCTION),
      qualified_name(function_name.WithName(Identifier(StringUtil::Lower(function_name.Name().GetIdentifierName())))),
      is_operator(is_operator), distinct(distinct), filter(std::move(filter)), order_bys(std::move(order_bys_p)),
      export_state(export_state_p) {
	arguments.reserve(children_p.size());
	for (auto &child : children_p) {
		arguments.emplace_back(std::move(child));
	}
	D_ASSERT(!function_name.Name().empty());
	if (!order_bys) {
		order_bys = make_uniq<OrderModifier>();
	}
}

FunctionExpression::FunctionExpression(const Identifier &function_name, vector<unique_ptr<ParsedExpression>> children_p,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys,
                                       bool distinct, bool is_operator, bool export_state_p)
    : FunctionExpression(QualifiedName(function_name), std::move(children_p), std::move(filter), std::move(order_bys),
                         distinct, is_operator, export_state_p) {
}

FunctionExpression::FunctionExpression(const QualifiedName &function_name, vector<FunctionArgument> children,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys_p,
                                       bool distinct, bool is_operator, bool export_state)
    : ParsedExpression(ExpressionType::FUNCTION, ExpressionClass::FUNCTION),
      qualified_name(function_name.WithName(Identifier(StringUtil::Lower(function_name.Name().GetIdentifierName())))),
      is_operator(is_operator), arguments(std::move(children)), distinct(distinct), filter(std::move(filter)),
      order_bys(std::move(order_bys_p)), export_state(export_state) {
	D_ASSERT(!function_name.Name().empty());
	if (!order_bys) {
		this->order_bys = make_uniq<OrderModifier>();
	}
}

FunctionExpression::FunctionExpression(const Identifier &function_name, vector<FunctionArgument> children,
                                       unique_ptr<ParsedExpression> filter, unique_ptr<OrderModifier> order_bys,
                                       bool distinct, bool is_operator, bool export_state)
    : FunctionExpression(QualifiedName(function_name), std::move(children), std::move(filter), std::move(order_bys),
                         distinct, is_operator, export_state) {
}

string FunctionExpression::ToString() const {
	if (is_operator) {
		// built-in operator
		D_ASSERT(!distinct);
		if (arguments.size() == 1) {
			if (StringUtil::Contains(qualified_name.Name().GetIdentifierName(), "__postfix")) {
				return "((" + arguments[0].ToString() + ")" +
				       StringUtil::Replace(qualified_name.Name().GetIdentifierName(), "__postfix", "") + ")";
			}
			return "(" + qualified_name.Name().GetIdentifierName() + "(" + arguments[0].ToString() + "))";
		}
		if (arguments.size() == 2) {
			return StringUtil::Format("(%s %s %s)", arguments[0].ToString(), qualified_name.Name().GetIdentifierName(),
			                          arguments[1].ToString());
		}
	}
	// standard function call - render the full qualification, the schema path can be nested (e.g. s1.s2.s3.my_macro)
	string result = qualified_name.QualificationToString();
	result += SQLIdentifier(qualified_name.Name());
	result += "(";
	if (distinct) {
		result += "DISTINCT ";
	}
	result += StringUtil::Join(arguments, arguments.size(), ", ",
	                           [&](const FunctionArgument &child) { return child.ToString(); });
	// ordered aggregate - WITHIN GROUP is only accepted for mode, other aggregates take the ORDER BY as an argument
	if (order_bys && !order_bys->orders.empty()) {
		if (arguments.empty() && qualified_name.Name() == Identifier("mode")) {
			result += ") WITHIN GROUP (";
		}
		result += " ORDER BY ";
		for (idx_t i = 0; i < order_bys->orders.size(); i++) {
			if (i > 0) {
				result += ", ";
			}
			result += order_bys->orders[i].ToString();
		}
	}
	result += ")";

	// filtered aggregate
	if (filter) {
		result += " FILTER (WHERE " + filter->ToString() + ")";
	}

	if (export_state) {
		result += " EXPORT_STATE";
	}

	return result;
}

void FunctionExpression::Verify() const {
	D_ASSERT(!qualified_name.Name().empty());
}

optional_ptr<ParsedExpression> FunctionExpression::IsLambdaFunction() {
	// Ignore the ->> operator (JSON extension).
	if (qualified_name.Name() == "->>") {
		return nullptr;
	}
	// Check the children for lambda expressions.
	for (auto &child : arguments) {
		if (child.GetExpression().GetExpressionClass() == ExpressionClass::LAMBDA) {
			return *child.GetExpressionMutable();
		}
	}
	return nullptr;
}

void FunctionExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<Identifier>(200, "function_name", qualified_name.Name());
	serializer.WritePropertyWithDefault<Identifier>(201, "schema", qualified_name.Schema());

	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		// Legacy serialization.
		vector<unique_ptr<ParsedExpression>> children;
		for (auto &arg : arguments) {
			auto copy = arg.GetExpression().Copy();
			copy->SetAlias(arg.GetName());
			children.push_back(std::move(copy));
		}
		serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(202, "children", children);
	}

	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(203, "filter", filter);
	serializer.WritePropertyWithDefault<unique_ptr<OrderModifier>>(204, "order_bys", order_bys);
	serializer.WritePropertyWithDefault<bool>(205, "distinct", distinct);
	serializer.WritePropertyWithDefault<bool>(206, "is_operator", is_operator);
	serializer.WritePropertyWithDefault<bool>(207, "export_state", export_state);
	serializer.WritePropertyWithDefault<Identifier>(208, "catalog", qualified_name.Catalog());

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<vector<FunctionArgument>>(209, "arguments", arguments);
	}

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0) || qualified_name.Path().size() > 3) {
		// the catalog/schema properties above cannot represent a nested schema path
		serializer.WriteProperty<QualifiedName>(210, "qualified_name", qualified_name);
	}

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<bool>(211, "is_legacy_function_call", is_legacy_function_call);
	}
}

unique_ptr<ParsedExpression> FunctionExpression::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<FunctionExpression>(new FunctionExpression());
	auto function_name = deserializer.ReadPropertyWithDefault<Identifier>(200, "function_name");
	auto schema = deserializer.ReadPropertyWithDefault<Identifier>(201, "schema");

	// Legacy children deserialization
	vector<unique_ptr<ParsedExpression>> children;
	deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(202, "children", children);
	if (!children.empty()) {
		result->arguments.reserve(children.size());
		for (auto &child : children) {
			auto alias = child->GetAlias();
			result->arguments.emplace_back(alias, std::move(child));
		}

		// Mark this function expression as a legacy function call, so that the binder can handle it accordingly.
		result->is_legacy_function_call = true;
	}

	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(203, "filter", result->filter);
	auto order_bys = deserializer.ReadPropertyWithDefault<unique_ptr<ResultModifier>>(204, "order_bys");
	if (order_bys && order_bys->type != ResultModifierType::ORDER_MODIFIER) {
		throw SerializationException(
		    "Failed to deserialize FUNCTION expression: order_bys is not an ORDER BY modifier");
	}
	result->order_bys = unique_ptr_cast<ResultModifier, OrderModifier>(std::move(order_bys));
	deserializer.ReadPropertyWithDefault<bool>(205, "distinct", result->distinct);
	deserializer.ReadPropertyWithDefault<bool>(206, "is_operator", result->is_operator);
	deserializer.ReadPropertyWithDefault<bool>(207, "export_state", result->export_state);
	auto catalog = deserializer.ReadPropertyWithDefault<Identifier>(208, "catalog");
	result->SetQualifiedName(std::move(catalog), std::move(schema), std::move(function_name));

	// New children deserialization
	if (children.empty()) {
		deserializer.ReadPropertyWithDefault<vector<FunctionArgument>>(209, "arguments", result->arguments);
	}

	auto qualified_name =
	    deserializer.ReadPropertyWithExplicitDefault<QualifiedName>(210, "qualified_name", QualifiedName());
	if (!qualified_name.Path().empty()) {
		result->SetQualifiedName(std::move(qualified_name));
	}

	bool is_legacy_function_call = false;
	deserializer.ReadPropertyWithDefault<bool>(211, "is_legacy_function_call", is_legacy_function_call);
	if (is_legacy_function_call) {
		result->is_legacy_function_call = true;
	}

	return std::move(result);
}

hash_t FunctionArgument::Hash() const {
	hash_t result = duckdb::Hash<const char *>(name.c_str());
	if (expression) {
		result = CombineHash(result, expression->Hash());
	}
	return result;
}

} // namespace duckdb
