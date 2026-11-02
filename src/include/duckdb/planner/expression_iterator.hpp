//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/expression_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/planner/expression.hpp"

#include <functional>

namespace duckdb {

class ExpressionIterator {
public:
	static void EnumerateChildren(const Expression &expression,
	                              const std::function<void(const Expression &child)> &callback);
	static void EnumerateChildren(Expression &expression, const std::function<void(Expression &child)> &callback);
	static void EnumerateChildren(Expression &expression,
	                              const std::function<void(unique_ptr<Expression> &child)> &callback);

	static void EnumerateExpression(unique_ptr<Expression> &expr,
	                                const std::function<void(Expression &child)> &callback);
	static void EnumerateExpression(unique_ptr<Expression> &expr,
	                                const std::function<void(unique_ptr<Expression> &child)> &callback);

	static void VisitExpressionClass(const Expression &expr, ExpressionClass expr_class,
	                                 const std::function<void(const Expression &child)> &callback);
	static void VisitExpressionClassMutable(unique_ptr<Expression> &expr, ExpressionClass expr_class,
	                                        const std::function<void(unique_ptr<Expression> &child)> &callback);
	static unique_ptr<Expression> ReplaceExpression(const Expression &expr, const Expression &target,
	                                                const Expression &replacement);

	template <class T>
	static void VisitExpressionMutable(unique_ptr<Expression> &expr,
	                                   const std::function<void(T &child, unique_ptr<Expression> &)> &callback) {
		VisitExpressionClassMutable(expr, T::TYPE,
		                            [&](unique_ptr<Expression> &child) { callback(child->Cast<T>(), child); });
	}
	template <class T>
	static void VisitExpression(const Expression &expr, const std::function<void(const T &child)> &callback) {
		VisitExpressionClass(expr, T::TYPE, [&](const Expression &child) { callback(child.Cast<T>()); });
	}
};

} // namespace duckdb
