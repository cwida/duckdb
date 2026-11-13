#include "duckdb/execution/column_binding_resolver.hpp"

#include "duckdb/catalog/catalog_entry/table_catalog_entry.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/operator/logical_any_join.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_create_index.hpp"
#include "duckdb/planner/operator/logical_extension_operator.hpp"
#include "duckdb/planner/operator/logical_insert.hpp"

namespace duckdb {

ColumnBindingResolver::ColumnBindingResolver(bool verify_only) : verify_only(verify_only) {
}

ColumnBindingResolver::ColumnBindingResolver(vector<ColumnBinding> bindings_p, vector<LogicalType> types_p)
    : bindings(std::move(bindings_p)), types(std::move(types_p)), verify_only(false) {
}

//! Combine the types of both join sides for resolving expressions against the combined bindings.
//! An empty type vector for a non-empty binding set means type verification was skipped for that side (e.g., legacy
//! extension operators), in which case we skip it for the combined set too.
static vector<LogicalType> CombineJoinTypes(const vector<ColumnBinding> &left_bindings,
                                            const vector<LogicalType> &left_types,
                                            const vector<ColumnBinding> &right_bindings,
                                            const vector<LogicalType> &right_types) {
	if ((left_types.empty() && !left_bindings.empty()) || (right_types.empty() && !right_bindings.empty())) {
		return vector<LogicalType>();
	}
	auto result = left_types;
	result.insert(result.end(), right_types.begin(), right_types.end());
	return result;
}

void ColumnBindingResolver::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();

		VisitOperator(*comp_join.children[0]);
		auto left_bindings = bindings;
		auto left_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				VisitExpression(&cond.LeftReference());
			}
		}

		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}

		VisitOperator(*comp_join.children[1]);
		auto right_bindings = bindings;
		auto right_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				VisitExpression(&cond.RightReference());
			}
		}

		// combine bindings to resolve predicate
		auto combined_bindings = left_bindings;
		combined_bindings.insert(combined_bindings.end(), right_bindings.begin(), right_bindings.end());
		auto combined_types = CombineJoinTypes(left_bindings, left_types, right_bindings, right_types);

		bindings = combined_bindings;
		types = combined_types;
		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison()) {
				VisitExpression(&cond.JoinExpressionReference());
			}
		}

		// update to join output bindings
		bindings = op.GetColumnBindings();
		types = op.types;

		return;
	}
	case LogicalOperatorType::LOGICAL_DELIM_JOIN: {
		auto &comp_join = op.Cast<LogicalComparisonJoin>();
		// get bindings from the duplicate-eliminated side
		auto &delim_side = comp_join.delim_flipped ? *comp_join.children[1] : *comp_join.children[0];
		VisitOperator(delim_side);
		auto delim_bindings = bindings;
		auto delim_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				auto &expr = comp_join.delim_flipped ? cond.RightReference() : cond.LeftReference();
				VisitExpression(&expr);
			}
		}
		// visit the duplicate eliminated columns
		for (auto &expr : comp_join.duplicate_eliminated_columns) {
			VisitExpression(&expr);
		}
		// now the other side
		auto &other_side = comp_join.delim_flipped ? *comp_join.children[0] : *comp_join.children[1];
		VisitOperator(other_side);
		auto other_bindings = bindings;
		auto other_types = types;
		for (auto &cond : comp_join.conditions) {
			if (cond.IsComparison()) {
				auto &expr = comp_join.delim_flipped ? cond.LeftReference() : cond.RightReference();
				VisitExpression(&expr);
			}
		}

		// arbitrary expressions are resolved against both join sides in logical left/right order
		auto &left_bindings = comp_join.delim_flipped ? other_bindings : delim_bindings;
		auto &right_bindings = comp_join.delim_flipped ? delim_bindings : other_bindings;
		auto combined_bindings = left_bindings;
		combined_bindings.insert(combined_bindings.end(), right_bindings.begin(), right_bindings.end());
		auto &left_types = comp_join.delim_flipped ? other_types : delim_types;
		auto &right_types = comp_join.delim_flipped ? delim_types : other_types;
		auto combined_types = CombineJoinTypes(left_bindings, left_types, right_bindings, right_types);
		bindings = std::move(combined_bindings);
		types = std::move(combined_types);
		for (auto &cond : comp_join.conditions) {
			if (!cond.IsComparison()) {
				VisitExpression(&cond.JoinExpressionReference());
			}
		}
		// finally update the bindings with the result bindings of the join
		bindings = op.GetColumnBindings();
		types = op.types;
		return;
	}
	case LogicalOperatorType::LOGICAL_ANY_JOIN: {
		// ANY join, this join is different because we evaluate the expression on the bindings of BOTH join sides at
		// once i.e. we set the bindings first to the bindings of the entire join, and then resolve the expressions of
		// this operator
		VisitOperatorChildren(op);
		bindings = op.GetColumnBindings();
		types = op.types;
		auto &any_join = op.Cast<LogicalAnyJoin>();
		if (any_join.join_type == JoinType::SEMI || any_join.join_type == JoinType::ANTI) {
			auto right_bindings = op.children[1]->GetColumnBindings();
			bindings.insert(bindings.end(), right_bindings.begin(), right_bindings.end());
			auto &right_types = op.children[1]->types;
			types.insert(types.end(), right_types.begin(), right_types.end());
		}
		if (any_join.join_type == JoinType::RIGHT_SEMI || any_join.join_type == JoinType::RIGHT_ANTI) {
			throw InternalException("RIGHT SEMI/ANTI any join not supported yet");
		}
		VisitOperatorExpressions(op);

		//	Restore bindings for the caller
		bindings = op.GetColumnBindings();
		types = op.types;
		return;
	}
	case LogicalOperatorType::LOGICAL_CREATE_INDEX: {
		// CREATE INDEX statement, add the columns of the table with table index 0 to the binding set
		// afterwards bind the expressions of the CREATE INDEX statement
		auto &create_index = op.Cast<LogicalCreateIndex>();
		bindings = LogicalOperator::GenerateColumnBindings(TableIndex(0),
		                                                   create_index.table.GetColumns().LogicalColumnCount());
		// TODO: fill types in too (clearing skips type checks)
		types.clear();
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_GET: {
		//! We first need to update the current set of bindings and then visit operator expressions
		bindings = op.GetColumnBindings();
		types = op.types;
		VisitOperatorExpressions(op);
		return;
	}
	case LogicalOperatorType::LOGICAL_INSERT: {
		//! We want to execute the normal path, but also add a dummy 'excluded' binding if there is a
		// ON CONFLICT DO UPDATE clause
		auto &insert_op = op.Cast<LogicalInsert>();
		if (insert_op.on_conflict_info.action_type != OnConflictAction::THROW) {
			// Get the bindings from the children
			VisitOperatorChildren(op);
			auto column_count = insert_op.table.GetColumns().PhysicalColumnCount();
			auto dummy_bindings =
			    LogicalOperator::GenerateColumnBindings(insert_op.on_conflict_info.excluded_table_index, column_count);
			// Now insert our dummy bindings at the start of the bindings,
			// so the first 'column_count' indices of the chunk are reserved for our 'excluded' columns
			bindings.insert(bindings.begin(), dummy_bindings.begin(), dummy_bindings.end());
			// TODO: fill types in too (clearing skips type checks)
			types.clear();
			if (insert_op.on_conflict_info.on_conflict_condition) {
				VisitExpression(&insert_op.on_conflict_info.on_conflict_condition);
			}
			if (insert_op.on_conflict_info.do_update_condition) {
				VisitExpression(&insert_op.on_conflict_info.do_update_condition);
			}
			VisitOperatorExpressions(op);
			bindings = op.GetColumnBindings();
			types = op.types;
			return;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_EXTENSION_OPERATOR: {
		auto &ext_op = op.Cast<LogicalExtensionOperator>();
		if (ext_op.GetTypeBindingVerificationIdentifier()) {
			bindings.clear();
			types.clear();
			VisitOperatorChildren(op);
			VisitOperatorExpressions(op);
			bindings = op.GetColumnBindings();
			types = op.types;
			return;
		}
		// Just to be very sure, we clear before and after resolving extension operator column bindings
		// This skips checks, but makes sure we don't break any extension operators with type verification
		types.clear();
		ext_op.ResolveColumnBindings(*this, bindings);
		types.clear();
		return;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE: {
		VisitOperatorChildren(op);
		bindings = op.GetColumnBindings();

		types.clear();
		VisitOperatorExpressions(op);
		types = op.types;
		return;
	}
	default:
		break;
	}

	// general case
	// first visit the children of this operator
	VisitOperatorChildren(op);
	// now visit the expressions of this operator to resolve any bound column references
	VisitOperatorExpressions(op);
	// finally update the current set of bindings to the current set of column bindings
	bindings = op.GetColumnBindings();
	types = op.types;
}

unique_ptr<Expression> ColumnBindingResolver::VisitReplace(BoundColumnRefExpression &expr,
                                                           unique_ptr<Expression> *expr_ptr) {
	D_ASSERT(expr.Depth() == 0);
	// check the current set of column bindings to see which index corresponds to the column reference
	for (idx_t i = 0; i < bindings.size(); i++) {
		if (expr.Binding() == bindings[i]) {
			if (!types.empty()) {
				// LCOV_EXCL_START
				if (bindings.size() != types.size()) {
					throw InternalException(
					    "Failed to bind column reference %s [%d.%d]: inequal num bindings/types (%llu != %llu)",
					    expr.GetAlias(), expr.Binding().table_index.index, expr.Binding().column_index, bindings.size(),
					    types.size());
				}
				if (expr.GetReturnType() != types[i]) {
					throw InternalException("Failed to bind column reference %s [%d.%d]: inequal types (%s != %s)",
					                        expr.GetAlias(), expr.Binding().table_index.index,
					                        expr.Binding().column_index, expr.GetReturnType().ToString(),
					                        types[i].ToString());
				}
				// LCOV_EXCL_STOP
			}
			if (verify_only) {
				// in verification mode
				return nullptr;
			}
			return make_uniq<BoundReferenceExpression>(expr.GetAlias(), expr.GetReturnType(), i);
		}
	}
	// LCOV_EXCL_START
	// could not bind the column reference, this should never happen and indicates a bug in the code
	// generate an error message
	throw InternalException("Failed to bind column reference %s [%d.%d] (bindings: %s)", expr.GetAlias(),
	                        expr.Binding().table_index.index, expr.Binding().column_index,
	                        LogicalOperator::ColumnBindingsToString(bindings));
	// LCOV_EXCL_STOP
}

} // namespace duckdb
