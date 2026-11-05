#include "duckdb/parser/expression/window_expression.hpp"
#include "duckdb/common/sql_identifier.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/serializer/deserializer.hpp"

namespace duckdb {

WindowExpression::WindowExpression() : ParsedExpression(ExpressionType::INVALID, ExpressionClass::WINDOW) {
}

vector<unique_ptr<ParsedExpression>> WindowExpression::SerializedChildren(Serializer &serializer) const {
	vector<unique_ptr<ParsedExpression>> result;
	idx_t nargs = arguments.size();
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0) &&
	    (qualified_name.Name() == "lead" || qualified_name.Name() == "lag")) {
		nargs = 1;
	}

	for (idx_t i = 0; i < nargs; ++i) {
		result.emplace_back(arguments[i].GetExpression().Copy());
	}

	return result;
}

unique_ptr<ParsedExpression> WindowExpression::SerializedOffset(Serializer &serializer) const {
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0) && arguments.size() > 1 &&
	    (qualified_name.Name() == "lead" || qualified_name.Name() == "lag")) {
		return arguments[1].GetExpression().Copy();
	}

	return nullptr;
}

unique_ptr<ParsedExpression> WindowExpression::SerializedDefault(Serializer &serializer) const {
	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0) && arguments.size() > 2 &&
	    (qualified_name.Name() == "lead" || qualified_name.Name() == "lag")) {
		return arguments[2].GetExpression().Copy();
	}

	return nullptr;
}

WindowExpression::WindowExpression(const string &catalog_name, const string &schema, const string &function_name)
    : ParsedExpression(WindowToExpressionType(function_name), ExpressionClass::WINDOW),
      qualified_name {Identifier(catalog_name), Identifier(schema), Identifier(StringUtil::Lower(function_name))},
      ignore_nulls(false), distinct(false) {
}

struct WindowFunctionDefinition {
	const char *name;
	ExpressionType expression_type;
};

static const WindowFunctionDefinition internal_window_functions[] = {
    {"rank", ExpressionType::WINDOW_RANK},
    {"rank_dense", ExpressionType::WINDOW_RANK_DENSE},
    {"dense_rank", ExpressionType::WINDOW_RANK_DENSE},
    {"percent_rank", ExpressionType::WINDOW_PERCENT_RANK},
    {"row_number", ExpressionType::WINDOW_ROW_NUMBER},
    {"first_value", ExpressionType::WINDOW_FIRST_VALUE},
    {"last_value", ExpressionType::WINDOW_LAST_VALUE},
    {"nth_value", ExpressionType::WINDOW_NTH_VALUE},
    {"cume_dist", ExpressionType::WINDOW_CUME_DIST},
    {"lead", ExpressionType::WINDOW_LEAD},
    {"lag", ExpressionType::WINDOW_LAG},
    {"ntile", ExpressionType::WINDOW_NTILE},
    {"fill", ExpressionType::WINDOW_FILL},
    {nullptr, ExpressionType::INVALID}};

ExpressionType WindowExpression::WindowToExpressionType(const string &fun_name) {
	D_ASSERT(StringUtil::IsLower(fun_name));
	auto functions = internal_window_functions;
	for (idx_t i = 0; functions[i].name != nullptr; i++) {
		if (fun_name == functions[i].name) {
			return functions[i].expression_type;
		}
	}
	return ExpressionType::WINDOW_AGGREGATE;
}

string WindowExpression::ExpressionTypeToWindow(ExpressionType expression_type) {
	auto functions = internal_window_functions;
	for (idx_t i = 0; functions[i].name != nullptr; i++) {
		if (expression_type == functions[i].expression_type) {
			return functions[i].name;
		}
	}
	return "";
}

void WindowExpression::SetFunctionName(const string &function_name_p) {
	qualified_name = qualified_name.WithName(Identifier(function_name_p));
	type = WindowToExpressionType(qualified_name.Name().GetIdentifierName());
}

string WindowExpression::ToString() const {
	// the name of the function can be a keyword, which only reads back as a name if it is quoted
	return ToString<WindowExpression, ParsedExpression, OrderByNode>(*this, qualified_name.QualificationToString(),
	                                                                 SQLIdentifier::ToString(qualified_name.Name()));
}

void WindowExpression::Serialize(Serializer &serializer) const {
	ParsedExpression::Serialize(serializer);
	serializer.WritePropertyWithDefault<Identifier>(200, "function_name", qualified_name.Name());
	serializer.WritePropertyWithDefault<Identifier>(201, "schema", qualified_name.Schema());
	serializer.WritePropertyWithDefault<Identifier>(202, "catalog", qualified_name.Catalog());

	if (!serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		// Legacy serialization.
		vector<unique_ptr<ParsedExpression>> children;
		for (auto &arg : arguments) {
			auto copy = arg.GetExpression().Copy();
			copy->SetAlias(arg.GetName());
			children.push_back(std::move(copy));
		}
		serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(203, "children", children);
	}

	serializer.WritePropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(204, "partitions", partitions);
	serializer.WritePropertyWithDefault<vector<OrderByNode>>(205, "orders", orders);
	serializer.WriteProperty<WindowBoundary>(206, "start", start);
	serializer.WriteProperty<WindowBoundary>(207, "end", end);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(208, "start_expr", start_expr);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(209, "end_expr", end_expr);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(210, "offset_expr", SerializedOffset(serializer));
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(211, "default_expr",
	                                                                  SerializedDefault(serializer));
	serializer.WritePropertyWithDefault<bool>(212, "ignore_nulls", ignore_nulls);
	serializer.WritePropertyWithDefault<unique_ptr<ParsedExpression>>(213, "filter_expr", filter_expr);
	serializer.WritePropertyWithDefault<WindowExcludeMode>(214, "exclude_clause", exclude_clause,
	                                                       WindowExcludeMode::NO_OTHER);
	serializer.WritePropertyWithDefault<bool>(215, "distinct", distinct);
	serializer.WritePropertyWithDefault<vector<OrderByNode>>(216, "arg_orders", arg_orders);
	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<bool>(217, "has_ignore_nulls", has_ignore_nulls);
	}

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<vector<FunctionArgument>>(218, "arguments", arguments);
	}

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0) || qualified_name.Path().size() > 3) {
		// the catalog/schema properties above cannot represent a nested schema path
		serializer.WriteProperty<QualifiedName>(219, "qualified_name", qualified_name);
	}

	if (serializer.ShouldSerialize(StorageVersion::V2_0_0)) {
		serializer.WritePropertyWithDefault<bool>(220, "is_legacy_function_call", is_legacy_function_call);
	}
}

unique_ptr<ParsedExpression> WindowExpression::Deserialize(Deserializer &deserializer) {
	auto result = duckdb::unique_ptr<WindowExpression>(new WindowExpression());
	auto function_name = deserializer.ReadPropertyWithDefault<Identifier>(200, "function_name");
	auto schema = deserializer.ReadPropertyWithDefault<Identifier>(201, "schema");
	auto catalog = deserializer.ReadPropertyWithDefault<Identifier>(202, "catalog");
	result->SetQualifiedName(std::move(catalog), std::move(schema), std::move(function_name));

	// Legacy children deserialization
	vector<unique_ptr<ParsedExpression>> children;
	deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(203, "children", children);
	if (!children.empty()) {
		result->arguments.reserve(children.size());
		for (auto &child : children) {
			auto alias = child->GetAlias();
			result->arguments.emplace_back(std::move(alias), std::move(child));
		}
		// Mark this function expression as a legacy function call, so that the binder can handle it accordingly.
		result->is_legacy_function_call = true;
	}

	deserializer.ReadPropertyWithDefault<vector<unique_ptr<ParsedExpression>>>(204, "partitions", result->partitions);
	deserializer.ReadPropertyWithDefault<vector<OrderByNode>>(205, "orders", result->orders);
	deserializer.ReadProperty<WindowBoundary>(206, "start", result->start);
	deserializer.ReadProperty<WindowBoundary>(207, "end", result->end);
	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(208, "start_expr", result->start_expr);
	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(209, "end_expr", result->end_expr);
	deserializer.ReadDeletedProperty<unique_ptr<ParsedExpression>>(210, "offset_expr");
	deserializer.ReadDeletedProperty<unique_ptr<ParsedExpression>>(211, "default_expr");
	deserializer.ReadPropertyWithDefault<bool>(212, "ignore_nulls", result->ignore_nulls);
	deserializer.ReadPropertyWithDefault<unique_ptr<ParsedExpression>>(213, "filter_expr", result->filter_expr);
	deserializer.ReadPropertyWithExplicitDefault<WindowExcludeMode>(214, "exclude_clause", result->exclude_clause,
	                                                                WindowExcludeMode::NO_OTHER);
	deserializer.ReadPropertyWithDefault<bool>(215, "distinct", result->distinct);
	deserializer.ReadPropertyWithDefault<vector<OrderByNode>>(216, "arg_orders", result->arg_orders);
	deserializer.ReadPropertyWithDefault<bool>(217, "has_ignore_nulls", result->has_ignore_nulls);

	// New children deserialization
	if (children.empty()) {
		deserializer.ReadPropertyWithDefault<vector<FunctionArgument>>(218, "arguments", result->arguments);
	}

	auto qualified_name =
	    deserializer.ReadPropertyWithExplicitDefault<QualifiedName>(219, "qualified_name", QualifiedName());
	if (!qualified_name.Path().empty()) {
		result->SetQualifiedName(std::move(qualified_name));
	}

	bool is_legacy_function_call = false;
	deserializer.ReadPropertyWithDefault<bool>(220, "is_legacy_function_call", is_legacy_function_call);
	if (is_legacy_function_call) {
		result->is_legacy_function_call = true;
	}

	return std::move(result);
}

} // namespace duckdb
