#include "duckdb/planner/operator/logical_secure_view.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/tableref/bound_at_clause.hpp"

#include "duckdb/common/string_util.hpp"
#include "duckdb/planner/operator/logical_get.hpp"

namespace duckdb {

//! Statistics gathered inside a view describe the data it reads - they may only be exposed to the caller if
//! every row that is read is also emitted. Any operator that removes rows means the statistics below it also cover
//! rows that the view hides, so only row-preserving operators are accepted here.
static bool EmitsAllRows(const LogicalOperator &op) {
	switch (op.type) {
	case LogicalOperatorType::LOGICAL_SECURE_VIEW:
		// a nested secure view applies this same rule to its own contents - nothing beyond what it exposes itself
		// can reach this boundary, so there is no need to look inside it
		return true;
	case LogicalOperatorType::LOGICAL_PROJECTION:
	case LogicalOperatorType::LOGICAL_ORDER_BY:
	case LogicalOperatorType::LOGICAL_EXPRESSION_GET:
	case LogicalOperatorType::LOGICAL_DUMMY_SCAN:
		break;
	case LogicalOperatorType::LOGICAL_GET: {
		auto &get = op.Cast<LogicalGet>();
		if (get.table_filters.HasFilters() || get.extra_info.sample_options || get.dynamic_filters) {
			return false;
		}
		if (!get.children.empty()) {
			// an in-out function decides itself which of the rows it reads it emits
			return false;
		}
		break;
	}
	default:
		return false;
	}
	for (auto &child : op.children) {
		if (!EmitsAllRows(*child)) {
			return false;
		}
	}
	return true;
}

LogicalSecureView::LogicalSecureView() : LogicalOperator(LogicalOperatorType::LOGICAL_SECURE_VIEW) {
}

LogicalSecureView::LogicalSecureView(string view_name_p, unique_ptr<LogicalOperator> child)
    : LogicalOperator(LogicalOperatorType::LOGICAL_SECURE_VIEW), view_name(std::move(view_name_p)) {
	children.push_back(std::move(child));
}

LogicalSecureView::LogicalSecureView(string view_name_p, QualifiedName source_name_p,
                                     vector<LogicalType> source_types_p, optional_ptr<BoundAtClause> at_clause,
                                     unique_ptr<LogicalOperator> child)
    : LogicalOperator(LogicalOperatorType::LOGICAL_SECURE_VIEW), view_name(std::move(view_name_p)), has_source(true),
      source_name(std::move(source_name_p)), source_types(std::move(source_types_p)),
      has_at_clause(at_clause != nullptr) {
	if (at_clause) {
		at_unit = at_clause->Unit();
		at_value = at_clause->GetValue();
	}
	output_bindings = child->GetColumnBindings();
	D_ASSERT(output_bindings.size() == source_types.size());
	for (idx_t i = 0; i < source_types.size(); i++) {
		output_expressions.push_back(
		    make_uniq<BoundColumnRefExpression>(source_types[i], ColumnBinding(TableIndex(0), ProjectionIndex(i))));
	}
	children.push_back(std::move(child));
}

void LogicalSecureView::AnalyzeStatistics(LogicalOperator &plan) {
	if (plan.type == LogicalOperatorType::LOGICAL_SECURE_VIEW) {
		// the statistics of a view may only be exposed if the view emits every row it reads - otherwise they would
		// describe values from rows that the view does not return
		plan.Cast<LogicalSecureView>().propagate_statistics = EmitsAllRows(*plan.children[0]);
	}
	for (auto &child : plan.children) {
		AnalyzeStatistics(*child);
	}
}

vector<ColumnBinding> LogicalSecureView::GetColumnBindings() {
	return children[0]->GetColumnBindings();
}

idx_t LogicalSecureView::EstimateCardinality(ClientContext &context) {
	if (has_estimated_cardinality) {
		// the estimate is frozen before filters are pushed into the view - what the optimizer derives from those
		// filters using the statistics of the view contents must not escape the boundary
		return estimated_cardinality;
	}
	if (!propagate_statistics) {
		// the estimate of the child changes as filters are pushed into the view - never remember it here
		return children[0]->EstimateCardinality(context);
	}
	return LogicalOperator::EstimateCardinality(context);
}

InsertionOrderPreservingMap<string> LogicalSecureView::ParamsToString() const {
	InsertionOrderPreservingMap<string> result;
	result["View"] = view_name;
	if (!pushed_filters.empty()) {
		result["Filters"] = StringUtil::Join(pushed_filters, "\n");
	}
	SetParamsEstimatedCardinality(result);
	return result;
}

void LogicalSecureView::ResolveTypes() {
	types = children[0]->types;
}

} // namespace duckdb
