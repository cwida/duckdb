//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression/bound_case_expression.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/planner/expression.hpp"

namespace duckdb {

struct BoundCaseCheck {
	unique_ptr<Expression> when_expr;
	unique_ptr<Expression> then_expr;

	void Serialize(Serializer &serializer) const;
	static BoundCaseCheck Deserialize(Deserializer &deserializer);
};

class BoundCaseExpression : public Expression {
public:
	static constexpr const ExpressionClass TYPE = ExpressionClass::BOUND_CASE;

public:
	explicit BoundCaseExpression(LogicalType type);
	BoundCaseExpression(unique_ptr<Expression> when_expr, unique_ptr<Expression> then_expr,
	                    unique_ptr<Expression> else_expr);

public:
	const vector<BoundCaseCheck> &CaseChecks() const {
		return case_checks;
	}
	vector<BoundCaseCheck> &CaseChecksMutable() {
		return case_checks;
	}
	const Expression &Else() const {
		return *else_expr;
	}
	const unique_ptr<Expression> &ElseExpression() const {
		return else_expr;
	}
	unique_ptr<Expression> &ElseMutable() {
		return else_expr;
	}

public:
	string ToString() const override;

	bool Equals(const BaseExpression &other) const override;

	unique_ptr<Expression> Copy() const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<Expression> Deserialize(Deserializer &deserializer);

private:
	vector<BoundCaseCheck> case_checks;
	unique_ptr<Expression> else_expr;
};
} // namespace duckdb
