//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_expression_iterator.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/parser/parsed_expression.hpp"
#include "duckdb/parser/tokens.hpp"

#include <functional>

namespace duckdb {

class ParsedExpressionIterator {
public:
	static void EnumerateChildren(const ParsedExpression &expression,
	                              const std::function<void(const ParsedExpression &child)> &callback);
	static void EnumerateChildren(ParsedExpression &expr, const std::function<void(ParsedExpression &child)> &callback);
	static void EnumerateChildren(ParsedExpression &expr,
	                              const std::function<void(unique_ptr<ParsedExpression> &child)> &callback);

	static void EnumerateTableRefChildren(TableRef &ref,
	                                      const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback,
	                                      const std::function<void(TableRef &ref)> &ref_callback = DefaultRefCallback);
	static void
	EnumerateQueryNodeChildren(QueryNode &node,
	                           const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback,
	                           const std::function<void(TableRef &ref)> &ref_callback = DefaultRefCallback);

	static void
	EnumerateQueryNodeModifiers(QueryNode &node,
	                            const std::function<void(unique_ptr<ParsedExpression> &child)> &expr_callback);

	static void VisitExpressionClass(const ParsedExpression &expr, ExpressionClass expr_class,
	                                 const std::function<void(const ParsedExpression &child)> &callback);
	static void VisitExpressionClassMutable(ParsedExpression &expr, ExpressionClass expr_class,
	                                        const std::function<void(ParsedExpression &child)> &callback);

	template <class T>
	static void VisitExpressionMutable(ParsedExpression &expr, const std::function<void(T &child)> &callback) {
		VisitExpressionClassMutable(expr, T::TYPE, [&](ParsedExpression &child) { callback(child.Cast<T>()); });
	}
	template <class T>
	static void VisitExpression(const ParsedExpression &expr, const std::function<void(const T &child)> &callback) {
		VisitExpressionClass(expr, T::TYPE, [&](const ParsedExpression &child) { callback(child.Cast<T>()); });
	}

private:
	static void DefaultRefCallback(TableRef &ref) {}; // NOP
};

} // namespace duckdb
