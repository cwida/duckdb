#include "parser/statement/update_statement.hpp"
#include "planner/binder.hpp"

using namespace duckdb;
using namespace std;

void Binder::Bind(UpdateStatement &stmt) {
	// visit the table reference
	AcceptChild(&stmt.table);
	// project any additional columns required for the condition/expressions
	if (stmt.condition) {
		VisitExpression(&stmt.condition);
	}
	for (auto &expression : stmt.expressions) {
		VisitExpression(&expression);
		if (expression->type == ExpressionType::VALUE_DEFAULT) {
			// we resolve the type of the DEFAULT expression in the
			// LogicalPlanGenerator because that is where we resolve the
			// to-be-updated column
			continue;
		}
		expression->ResolveType();
		if (expression->return_type == TypeId::INVALID && !expression->HasParameter()) {
			throw BinderException("Could not resolve type of projection element!");
		}
	}
}
