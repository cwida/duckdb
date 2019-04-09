#include "execution/operator/persistent/physical_insert.hpp"
#include "execution/physical_plan_generator.hpp"
#include "planner/operator/logical_insert.hpp"

using namespace duckdb;
using namespace std;

unique_ptr<PhysicalOperator> PhysicalPlanGenerator::CreatePlan(LogicalInsert &op) {
	unique_ptr<PhysicalOperator> plan;
	if (op.children.size() > 0) {
		assert(op.children.size() == 1);
		plan = CreatePlan(*op.children[0]);
	}

	auto insert = make_unique<PhysicalInsert>(op, op.table, move(op.insert_values), op.column_index_map);
	if (plan) {
		insert->children.push_back(move(plan));
	}
	return move(insert);
}
