//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/expression/lambda_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/common/enums/lambda_syntax.hpp"

namespace duckdb {

enum class LambdaSyntaxType : uint8_t { SINGLE_ARROW_STORAGE = 0, SINGLE_ARROW = 1, LAMBDA_KEYWORD = 2 };

//! DuckDB 1.3. introduced a new lambda syntax: lambda x, y: x + y.
//! The new syntax resolves any ambiguity with the JSON arrow operator.
//! Currently, we're still using a LambdaExpression for both cases.
class LambdaExpression : public ParsedExpression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::LAMBDA;

public:
	LambdaExpression(vector<string> named_parameters_p, unique_ptr<ParsedExpression> expr);
	LambdaExpression(unique_ptr<ParsedExpression> lhs, unique_ptr<ParsedExpression> expr);

public:
	LambdaSyntaxType GetLambdaSyntaxType() const {
		return syntax_type;
	}
	LambdaSyntaxType &GetLambdaSyntaxTypeMutable() {
		return syntax_type;
	}
	const ParsedExpression &Left() const {
		return *lhs;
	}
	unique_ptr<ParsedExpression> &LeftMutable() {
		return lhs;
	}
	const ParsedExpression &Right() const {
		return *expr;
	}
	unique_ptr<ParsedExpression> &RightMutable() {
		return expr;
	}
	unique_ptr<ParsedExpression> &CopiedExprMutable() {
		return copied_expr;
	}

	//! Returns a vector to the column references in the LHS expression, and fills the error message,
	//! if the LHS is not a valid lambda parameter list
	vector<reference<const ParsedExpression>> ExtractColumnRefExpressions(string &error_message) const;
	//! Returns the error message for an invalid lambda parameter list
	static string InvalidParametersErrorMessage();
	//! Returns true, if the column_name is a lambda parameter name
	static bool IsLambdaParameter(const vector<identifier_set_t> &lambda_params, const Identifier &column_name);

	string ToString() const override;
	bool Equals(const ParsedExpression &other) const override;
	unique_ptr<ParsedExpression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<ParsedExpression> Deserialize(Deserializer &deserializer);
	void FinalizeDeserialization(Deserializer &deserializer);

private:
	//! The syntax type.
	LambdaSyntaxType syntax_type;
	//! The LHS of a lambda expression or the JSON "->"-operator. We need the context
	//! to determine if the LHS is a list of column references (lambda parameters) or an expression (JSON)
	unique_ptr<ParsedExpression> lhs;
	//! The lambda or JSON expression (RHS)
	unique_ptr<ParsedExpression> expr;
	//! Band-aid for conflicts between lambda binding and JSON binding.
	unique_ptr<ParsedExpression> copied_expr;

private:
	LambdaExpression();
};

} // namespace duckdb
