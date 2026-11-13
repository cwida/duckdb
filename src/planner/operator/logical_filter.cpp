#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"

namespace duckdb {

LogicalFilter::LogicalFilter(unique_ptr<Expression> expression) : LogicalOperator(LogicalOperatorType::LOGICAL_FILTER) {
	expressions.push_back(std::move(expression));
	SplitPredicates(expressions);
}

LogicalFilter::LogicalFilter() : LogicalOperator(LogicalOperatorType::LOGICAL_FILTER) {
}

void LogicalFilter::ResolveTypes() {
	types = MapTypes(children[0]->types, projection_map);
}

vector<ColumnBinding> LogicalFilter::GetColumnBindings() {
	return MapBindings(children[0]->GetColumnBindings(), projection_map);
}

// Split the predicates separated by AND statements
// These are the predicates that are safe to push down because all of them MUST
// be true
bool LogicalFilter::SplitPredicates(vector<unique_ptr<Expression>> &expressions) {
	bool found_conjunction = false;
	for (idx_t i = 0; i < expressions.size(); i++) {
		if (expressions[i]->GetExpressionType() == ExpressionType::CONJUNCTION_AND) {
			auto &conjunction = expressions[i]->Cast<BoundConjunctionExpression>();
			found_conjunction = true;
			// AND expression, append the other children
			for (idx_t k = 1; k < conjunction.GetChildren().size(); k++) {
				expressions.push_back(std::move(conjunction.GetChildrenMutable()[k]));
			}
			// replace this expression with the first child of the conjunction
			expressions[i] = std::move(conjunction.GetChildrenMutable()[0]);
			// we move back by one so the right child is checked again
			// in case it is an AND expression as well
			i--;
		}
	}
	return found_conjunction;
}

} // namespace duckdb
