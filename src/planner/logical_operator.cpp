#include "duckdb/planner/logical_operator.hpp"

#include "duckdb/common/printer.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/tree_renderer.hpp"

namespace duckdb {

string LogicalOperator::GetName() const {
	return LogicalOperatorToString(type);
}

string LogicalOperator::ParamsToString() const {
	string result;
	for (idx_t i = 0; i < expressions.size(); i++) {
		if (i > 0) {
			result += "\n";
		}
		result += expressions[i]->GetName();
	}
	return result;
}

void LogicalOperator::ResolveOperatorTypes() {
	// if (types.size() > 0) {
	// 	// types already resolved for this node
	// 	return;
	// }
	types.clear();
	// first resolve child types
	for (auto &child : children) {
		child->ResolveOperatorTypes();
	}
	// now resolve the types for this operator
	ResolveTypes();
	D_ASSERT(types.size() == GetColumnBindings().size());
}

vector<ColumnBinding> LogicalOperator::GenerateColumnBindings(idx_t table_idx, idx_t column_count) {
	vector<ColumnBinding> result;
	for (idx_t i = 0; i < column_count; i++) {
		result.emplace_back(table_idx, i);
	}
	return result;
}

vector<LogicalType> LogicalOperator::MapTypes(const vector<LogicalType> &types, const vector<idx_t> &projection_map) {
	if (projection_map.empty()) {
		return types;
	} else {
		vector<LogicalType> result_types;
		result_types.reserve(projection_map.size());
		for (auto index : projection_map) {
			result_types.push_back(types[index]);
		}
		return result_types;
	}
}

vector<ColumnBinding> LogicalOperator::MapBindings(const vector<ColumnBinding> &bindings,
                                                   const vector<idx_t> &projection_map) {
	if (projection_map.empty()) {
		return bindings;
	} else {
		vector<ColumnBinding> result_bindings;
		result_bindings.reserve(projection_map.size());
		for (auto index : projection_map) {
			result_bindings.push_back(bindings[index]);
		}
		return result_bindings;
	}
}

string LogicalOperator::ToString(idx_t depth) const {
	TreeRenderer renderer;
	return renderer.ToString(*this);
}

void LogicalOperator::Verify() {
#ifdef DEBUG
	// verify expressions
	for (idx_t expr_idx = 0; expr_idx < expressions.size(); expr_idx++) {
		// verify that we can (correctly) copy this expression
		auto copy = expressions[expr_idx]->Copy();
		auto original_hash = expressions[expr_idx]->Hash();
		auto copy_hash = copy->Hash();
		// copy should be identical to original
		D_ASSERT(expressions[expr_idx]->ToString() == copy->ToString());
		D_ASSERT(original_hash == copy_hash);
		D_ASSERT(Expression::Equals(expressions[expr_idx].get(), copy.get()));

		D_ASSERT(!Expression::Equals(expressions[expr_idx].get(), nullptr));
		for (idx_t other_idx = 0; other_idx < expr_idx; other_idx++) {
			// comparison with other expressions
			auto other_hash = expressions[other_idx]->Hash();
			bool expr_equal = Expression::Equals(expressions[expr_idx].get(), expressions[other_idx].get());
			if (original_hash != other_hash) {
				// if the hashes are not equal the expressions should not be equal either
				D_ASSERT(!expr_equal);
			}
		}
	}
	D_ASSERT(!ToString().empty());
	for (auto &child : children) {
		child->Verify();
	}
#endif
}

void LogicalOperator::Print() {
	Printer::Print(ToString());
}

} // namespace duckdb
