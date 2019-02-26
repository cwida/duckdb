#include "parser/statement/delete_statement.hpp"
#include "planner/logical_plan_generator.hpp"
#include "planner/operator/list.hpp"

using namespace duckdb;
using namespace std;

void LogicalPlanGenerator::CreatePlan(DeleteStatement &statement) {
	// we require row ids for the deletion
	require_row_id = true;
	// create the table scan
	AcceptChild(&statement.table);
	if (!root || root->type != LogicalOperatorType::GET) {
		throw Exception("Cannot create delete node without table scan!");
	}
	auto &get = (LogicalGet &)*root;
	// create the filter (if any)
	if (statement.condition) {
		VisitExpression(&statement.condition);
		auto filter = make_unique<LogicalFilter>(move(statement.condition));
		filter->AddChild(move(root));
		root = move(filter);
	}
	// create the delete node
	auto del = make_unique<LogicalDelete>(get.table);
	del->AddChild(move(root));
	// we push an BoundColumnRef binding to the row_id index
	del->expressions.push_back(make_unique<BoundColumnRefExpression>(
	    "", TypeId::POINTER, ColumnBinding(get.table_index, get.column_ids.size() - 1)));
	root = move(del);
}
