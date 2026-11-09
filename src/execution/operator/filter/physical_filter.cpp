#include "duckdb/execution/operator/filter/physical_filter.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/parallel/thread_context.hpp"
namespace duckdb {

PhysicalFilter::PhysicalFilter(PhysicalPlan &physical_plan, vector<LogicalType> types,
                               vector<unique_ptr<Expression>> select_list, idx_t estimated_cardinality)
    : CachingPhysicalOperator(physical_plan, PhysicalOperatorType::FILTER, std::move(types), estimated_cardinality),
      expression(BoundConjunctionExpression::Create(ExpressionType::CONJUNCTION_AND, std::move(select_list))) {
}

class FilterState : public CachingOperatorState {
public:
	explicit FilterState(ExecutionContext &context, Expression &expr)
	    : executor(context.client, expr), sel(STANDARD_VECTOR_SIZE) {
	}

	ExpressionExecutor executor;
	SelectionVector sel;

public:
	void Finalize(const PhysicalOperator &op, ExecutionContext &context) override {
		context.thread.profiler.Flush(op);
	}

	bool SupportsReuse() const override {
		return true;
	}

	void Reset() override {
		ResetCachingState();
	}
};

void PhysicalFilter::InitializeOutputChunk(Allocator &allocator, DataChunk &chunk) const {
	// every output column only references or slices the input
	chunk.Initialize(allocator, types, vector<bool>(types.size(), false));
}

unique_ptr<OperatorState> PhysicalFilter::GetOperatorState(ExecutionContext &context) const {
	return make_uniq<FilterState>(context, *expression);
}

OperatorResultType PhysicalFilter::ExecuteInternal(ExecutionContext &context, DataChunk &input, DataChunk &chunk,
                                                   GlobalOperatorState &gstate, OperatorState &state_p) const {
	auto &state = state_p.Cast<FilterState>();
	idx_t result_count = state.executor.SelectExpression(input, state.sel);
	if (result_count == input.size()) {
		// nothing was filtered: skip adding any selection vectors
		chunk.Reference(input);
	} else if (result_count > 0) {
		chunk.Slice(input, state.sel, result_count);
	}
	return OperatorResultType::NEED_MORE_INPUT;
}

InsertionOrderPreservingMap<string> PhysicalFilter::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["__expression__"] = expression->GetName().GetIdentifierName();
	SetEstimatedCardinality(result, estimated_cardinality);
	return result;
}

} // namespace duckdb
