#include "duckdb/execution/operator/projection/physical_projection.hpp"
#include "duckdb/parallel/thread_context.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"

namespace duckdb {

class ProjectionState : public OperatorState {
public:
	explicit ProjectionState(ExecutionContext &context, const vector<unique_ptr<Expression>> &expressions)
	    : executor(context.client, expressions) {
	}

	ExpressionExecutor executor;

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}

	bool SupportsReuse() const override {
		return true;
	}
};

PhysicalProjection::PhysicalProjection(PhysicalPlan &physical_plan, vector<LogicalType> types,
                                       vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality)
    : PhysicalOperator(physical_plan, PhysicalOperatorType::PROJECTION, std::move(types), estimated_cardinality),
      select_list(std::move(select_list)) {
}

void PhysicalProjection::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// column references only reference their input vector. Only computed columns need a buffer
	vector<bool> initialize;
	bool any_computed = false;
	for (auto &expression : select_list) {
		const bool computed = expression->GetExpressionClass() != ExpressionClass::BOUND_REF;
		initialize.push_back(computed);
		any_computed = any_computed || computed;
	}
	if (!any_computed) {
		chunk.InitializeEmpty(types);
		return;
	}
	chunk.Initialize(allocator, types, initialize);
}

OperatorResultType PhysicalProjection::Execute(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                               GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<ProjectionState>();
	state.executor.Execute(input, chunk);
	return OperatorResultType::NEED_MORE_INPUT;
}

unique_ptr<OperatorState> PhysicalProjection::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<ProjectionState>(context, select_list);
}

bool PhysicalProjection::ParallelOperator() const {
	for (auto &expression : select_list) {
		bool requires_ordered_execution = false;
		ExpressionIterator::VisitExpression<BoundFunctionExpression>(*expression, [&](const auto &function) {
			if (function.RequiresOrderedExecution()) {
				requires_ordered_execution = true;
			}
		});
		if (requires_ordered_execution) {
			return false;
		}
	}
	return true;
}

InsertionOrderPreservingMap<string> PhysicalProjection::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	string projections;
	for (idx_t i = 0; i < select_list.size(); i++) {
		if (i > 0) {
			projections += "\n";
		}
		auto &expr = select_list[i];
		projections += expr->GetName();
	}
	result["__projections__"] = projections;
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
