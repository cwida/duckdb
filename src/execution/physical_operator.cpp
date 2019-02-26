#include "execution/physical_operator.hpp"

#include "main/client_context.hpp"

using namespace duckdb;
using namespace std;

string PhysicalOperator::ToString() const {
	string result = PhysicalOperatorToString(type);
	if (children.size() > 0) {
		result += "(";
		for (size_t i = 0; i < children.size(); i++) {
			auto &child = children[i];
			result += child->ToString();
			if (i < children.size() - 1) {
				result += ", ";
			}
		}
		result += ")";
	}

	return result;
}

PhysicalOperatorState::PhysicalOperatorState(PhysicalOperator *child) : finished(false) {
	if (child) {
		child->InitializeChunk(child_chunk);
		child_state = child->GetOperatorState();
	}
}

void PhysicalOperator::GetChunk(ClientContext &context, DataChunk &chunk, PhysicalOperatorState *state) {
	if (context.interrupted) {
		throw InterruptException();
	}
	// finished with this operator
	chunk.Reset();
	if (state->finished) {
		return;
	}

	context.profiler.StartOperator(this);
	_GetChunk(context, chunk, state);
	context.profiler.EndOperator(chunk);

	chunk.Verify();
}
