#include "duckdb/parser/expression/lambda_expression.hpp"

#include "duckdb/catalog/catalog.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/common/types/hash.hpp"
#include "duckdb/parser/expression/columnref_expression.hpp"
#include "duckdb/parser/expression/function_expression.hpp"
#include "duckdb/main/attached_database.hpp"
#include "duckdb/storage/storage_manager.hpp"

namespace duckdb {

LambdaExpression::LambdaExpression() : ParsedExpression(ExpressionType::LAMBDA, ExpressionClass::LAMBDA) {
}

LambdaExpression::LambdaExpression(vector<string> named_parameters_p, unique_ptr<ParsedExpression> expr)
    : ParsedExpression(ExpressionType::LAMBDA, ExpressionClass::LAMBDA), syntax_type(LambdaSyntaxType::LAMBDA_KEYWORD),
      expr(std::move(expr)) {
	if (named_parameters_p.size() == 1) {
		lhs = make_uniq<ColumnRefExpression>(Identifier(named_parameters_p.back()));
		return;
	}
	// Create a dummy row function and insert the children.
	vector<unique_ptr<ParsedExpression>> children;
	for (const auto &name : named_parameters_p) {
		auto child = make_uniq<ColumnRefExpression>(Identifier(name));
		children.push_back(std::move(child));
	}
	lhs = make_uniq<FunctionExpression>("row", std::move(children));
}

LambdaExpression::LambdaExpression(unique_ptr<ParsedExpression> lhs, unique_ptr<ParsedExpression> expr)
    : ParsedExpression(ExpressionType::LAMBDA, ExpressionClass::LAMBDA), syntax_type(LambdaSyntaxType::SINGLE_ARROW),
      lhs(std::move(lhs)), expr(std::move(expr)) {
}

vector<reference<const ParsedExpression>> LambdaExpression::ExtractColumnRefExpressions(string &error_message) const {
	// we return an error message because we can't throw a binder exception here,
	// since we can't distinguish between a lambda function and the JSON operator yet
	vector<reference<const ParsedExpression>> column_refs;

	if (Left().GetExpressionClass() == ExpressionClass::COLUMN_REF) {
		// single column reference
		column_refs.emplace_back(Left());
		return column_refs;
	}

	if (Left().GetExpressionClass() == ExpressionClass::FUNCTION) {
		// list of column references
		auto &func_expr = Left().Cast<FunctionExpression>();
		if (func_expr.FunctionName() != "row") {
			error_message = InvalidParametersErrorMessage();
			return column_refs;
		}

		for (auto &child : func_expr.GetArguments()) {
			if (child.GetExpression().GetExpressionClass() != ExpressionClass::COLUMN_REF) {
				error_message = InvalidParametersErrorMessage();
				return column_refs;
			}
			column_refs.emplace_back(child.GetExpression());
		}
	}

	if (column_refs.empty()) {
		error_message = InvalidParametersErrorMessage();
	}
	return column_refs;
}

string LambdaExpression::InvalidParametersErrorMessage() {
	// FIXME: remove this once we only support the new lambda syntax.
	return "Invalid lambda parameters! Parameters must be unqualified comma-separated names like x or (x, y).";
}

bool LambdaExpression::IsLambdaParameter(const vector<identifier_set_t> &lambda_params,
                                         const Identifier &parameter_name) {
	for (const auto &level : lambda_params) {
		if (level.find(parameter_name) != level.end()) {
			return true;
		}
	}
	return false;
}

void LambdaExpression::FinalizeDeserialization(Deserializer &deserializer) {
	if (syntax_type != LambdaSyntaxType::SINGLE_ARROW) {
		return;
	}
	// arrow lambdas stored before v2.0.0 keep binding, like those stored before v1.3.0
	auto catalog = deserializer.TryGet<Catalog>();
	if (!catalog) {
		return;
	}
	auto &attached = catalog->GetAttached();
	if (!attached.HasStorageManager()) {
		return;
	}
	auto &storage_manager = attached.GetStorageManager();
	if (storage_manager.HasStorageVersion() && storage_manager.GetStorageVersion() < StorageVersion::V2_0_0) {
		syntax_type = LambdaSyntaxType::SINGLE_ARROW_STORAGE;
	}
}

string LambdaExpression::ToString() const {
	if (GetLambdaSyntaxType() != LambdaSyntaxType::LAMBDA_KEYWORD) {
		return "(" + Left().ToString() + " -> " + Right().ToString() + ")";
	}

	string str = "";
	auto column_refs = ExtractColumnRefExpressions(str);

	str = "(lambda ";
	for (idx_t i = 0; i < column_refs.size(); i++) {
		auto &column_ref = column_refs[i].get();
		auto &cast_column_ref = column_ref.Cast<ColumnRefExpression>();

		if (i == column_refs.size() - 1) {
			str += cast_column_ref.ToString();
			continue;
		}
		str += cast_column_ref.ToString() + ", ";
	}
	return str + ": " + Right().ToString() + ")";
}

} // namespace duckdb
