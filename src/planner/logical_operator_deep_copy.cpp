#include "duckdb/planner/logical_operator_deep_copy.hpp"

#include "duckdb/planner/expression/list.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/list.hpp"

namespace duckdb {

LogicalOperatorDeepCopy::LogicalOperatorDeepCopy(Binder &binder, optional_ptr<bound_parameter_map_t> parameter_data)
    : binder(binder), parameter_data(parameter_data) {
}

unique_ptr<LogicalOperator> LogicalOperatorDeepCopy::DeepCopy(unique_ptr<duckdb::LogicalOperator> &op) {
	auto copy = op->Copy(binder.context);
	VisitOperator(*copy);
	TableBindingReplacer replacer(table_idx_replacements, parameter_data);
	replacer.VisitOperator(*copy);
	return copy;
}

// The following templates of TableIndexAccessor with ReplaceTableIndex and ReplaceTableIndexMulti
// reduces the amount of code significantly.
template <typename T>
struct TableIndexAccessor {
	static TableIndex &Get(T &plan) {
		return plan.table_index; // default
	}
};

template <>
struct TableIndexAccessor<LogicalAggregate> {
	static vector<reference<TableIndex>> Get(LogicalAggregate &plan) {
		return {std::ref(plan.group_index), std::ref(plan.aggregate_index), std::ref(plan.groupings_index)};
	}
};

template <>
struct TableIndexAccessor<LogicalWindow> {
	static TableIndex &Get(LogicalWindow &plan) {
		return plan.window_index;
	}
};

template <>
struct TableIndexAccessor<LogicalUnnest> {
	static TableIndex &Get(LogicalUnnest &plan) {
		return plan.unnest_index;
	}
};

template <>
struct TableIndexAccessor<LogicalPivot> {
	static TableIndex &Get(LogicalPivot &plan) {
		return plan.pivot_index;
	}
};

template <>
struct TableIndexAccessor<LogicalJoin> {
	static TableIndex &Get(LogicalJoin &plan) {
		return plan.mark_index;
	}
};

// Single-field version
template <typename T>
void LogicalOperatorDeepCopy::ReplaceTableIndex(LogicalOperator &op) {
	auto &plan = op.Cast<T>();
	auto &field = TableIndexAccessor<T>::Get(plan);
	auto new_idx = binder.GenerateTableIndex();
	table_idx_replacements[field] = new_idx;
	field = new_idx;
}

// Multi-field version
template <typename T>
void LogicalOperatorDeepCopy::ReplaceTableIndexMulti(LogicalOperator &op) {
	auto &plan = op.Cast<T>();
	for (auto &field_ref : TableIndexAccessor<T>::Get(plan)) {
		auto &field = field_ref.get();
		auto new_idx = binder.GenerateTableIndex();
		table_idx_replacements[field] = new_idx;
		field = new_idx;
	}
}

void LogicalOperatorDeepCopy::VisitOperator(LogicalOperator &op) {
	// Because we are changing table bindings, VisitOperatorChildren will clear projection maps
	// Therefore, the recursion is implemented here to avoid that
	for (auto &child : op.children) {
		VisitOperator(*child);
	}

	switch (op.type) {
	case LogicalOperatorType::LOGICAL_PROJECTION:
		ReplaceTableIndex<LogicalProjection>(op);
		break;

	case LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY:
		ReplaceTableIndexMulti<LogicalAggregate>(op);
		break;

	case LogicalOperatorType::LOGICAL_WINDOW:
		ReplaceTableIndex<LogicalWindow>(op);
		break;
	case LogicalOperatorType::LOGICAL_UNNEST: {
		ReplaceTableIndex<LogicalUnnest>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_PIVOT: {
		ReplaceTableIndex<LogicalPivot>(op);
		break;
	}

	// -----------------------------
	// Data sources
	// -----------------------------
	case LogicalOperatorType::LOGICAL_GET: {
		ReplaceTableIndex<LogicalGet>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_CHUNK_GET: {
		ReplaceTableIndex<LogicalColumnDataGet>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_DELIM_GET: {
		ReplaceTableIndex<LogicalDelimGet>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET: {
		ReplaceTableIndex<LogicalExpressionGet>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN: {
		ReplaceTableIndex<LogicalDummyScan>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT: {
		auto &empty_result = op.Cast<LogicalEmptyResult>();
		for (auto &binding : empty_result.bindings) {
			if (table_idx_replacements.find(binding.table_index) == table_idx_replacements.end()) {
				table_idx_replacements[binding.table_index] = binder.GenerateTableIndex();
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		ReplaceTableIndex<LogicalCTERef>(op);
		break;
	}
	// -----------------------------
	// Joins
	// -----------------------------
	case LogicalOperatorType::LOGICAL_DELIM_JOIN:
	case LogicalOperatorType::LOGICAL_COMPARISON_JOIN:
	case LogicalOperatorType::LOGICAL_ANY_JOIN:
	case LogicalOperatorType::LOGICAL_ASOF_JOIN:
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN:
	case LogicalOperatorType::LOGICAL_JOIN: {
		auto &join = op.Cast<LogicalJoin>();
		// Only replace the mark index for JOINs that have a valid mark index in the first place
		if (join.join_type == JoinType::MARK) {
			ReplaceTableIndex<LogicalJoin>(op);
		}
		break;
	}
	// -----------------------------
	// SetOps
	// -----------------------------
	case LogicalOperatorType::LOGICAL_UNION:
	case LogicalOperatorType::LOGICAL_EXCEPT:
	case LogicalOperatorType::LOGICAL_INTERSECT: {
		ReplaceTableIndex<LogicalSetOperation>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		ReplaceTableIndex<LogicalCTE>(op);
		break;
	}

	case LogicalOperatorType::LOGICAL_INSERT: {
		ReplaceTableIndex<LogicalInsert>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_DELETE: {
		ReplaceTableIndex<LogicalDelete>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_UPDATE: {
		ReplaceTableIndex<LogicalUpdate>(op);
		break;
	}
	case LogicalOperatorType::LOGICAL_MERGE_INTO: {
		ReplaceTableIndex<LogicalMergeInto>(op);
		break;
	}

	default:
		break;
	}
}

TableBindingReplacer::TableBindingReplacer(unordered_map<TableIndex, TableIndex> &table_idx_replacements,
                                           optional_ptr<bound_parameter_map_t> parameter_data)
    : table_idx_replacements(table_idx_replacements), parameter_data(parameter_data) {
}

void TableBindingReplacer::VisitOperator(LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_DEPENDENT_JOIN: {
		// The visitor does not catch the correlated_column vector of DEPENDENT_JOINs,
		// therefore we need to special case it.
		auto &join = op.Cast<LogicalDependentJoin>();
		for (auto &col : join.correlated_columns) {
			auto entry = table_idx_replacements.find(col.binding.table_index);
			if (entry != table_idx_replacements.end()) {
				col.binding.table_index = entry->second;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_RECURSIVE_CTE:
	case LogicalOperatorType::LOGICAL_MATERIALIZED_CTE: {
		// The visitor does not catch the correlated_column vector,
		// therefore we need to special case it.
		auto &cte = op.Cast<LogicalCTE>();
		for (auto &col : cte.correlated_columns) {
			auto entry = table_idx_replacements.find(col.binding.table_index);
			if (entry != table_idx_replacements.end()) {
				col.binding.table_index = entry->second;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_CTE_REF: {
		// Similarly, the visitor is unable to replace the cte reference's cte_index,
		// which may have changed. We have to check that and adapt accordingly.
		auto &cteref = op.Cast<LogicalCTERef>();
		auto entry = table_idx_replacements.find(cteref.cte_index);
		if (entry != table_idx_replacements.end()) {
			cteref.cte_index = entry->second;
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_SECURE_VIEW: {
		auto &view = op.Cast<LogicalSecureView>();
		for (auto &binding : view.output_bindings) {
			auto entry = table_idx_replacements.find(binding.table_index);
			if (entry != table_idx_replacements.end()) {
				binding.table_index = entry->second;
			}
		}
		break;
	}
	case LogicalOperatorType::LOGICAL_EMPTY_RESULT: {
		auto &empty_result = op.Cast<LogicalEmptyResult>();
		for (auto &binding : empty_result.bindings) {
			auto entry = table_idx_replacements.find(binding.table_index);
			if (entry != table_idx_replacements.end()) {
				binding.table_index = entry->second;
			}
		}
		break;
	}
	default:
		break;
	}

	// Because we are changing table bindings, VisitOperatorChildren will clear projection maps
	// Therefore, the recursion is implemented here to avoid that
	for (auto &child : op.children) {
		VisitOperator(*child);
	}
	VisitOperatorExpressions(op);
}

void TableBindingReplacer::VisitExpression(unique_ptr<Expression> *expression) {
	auto &expr = *expression;
	if (expr->GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF) {
		auto &bound_column_ref = expr->Cast<BoundColumnRefExpression>();
		auto entry = table_idx_replacements.find(bound_column_ref.Binding().table_index);
		if (entry != table_idx_replacements.end()) {
			bound_column_ref.BindingMutable().table_index = entry->second;
		}
	} else if (expr->GetExpressionClass() == ExpressionClass::BOUND_PARAMETER) {
		// we have to replace the parameter data if it is a bound parameter
		// because while the parameter data is copied, it does not contain the
		// correct pointer to the parameter data
		auto &bound_parameter = expr->Cast<BoundParameterExpression>();
		if (parameter_data) {
			auto entry = parameter_data->find(bound_parameter.Identifier());
			if (entry != parameter_data->end()) {
				bound_parameter.ParameterDataMutable() = entry->second;
			}
		}
	}

	VisitExpressionChildren(**expression);
}

} // namespace duckdb
