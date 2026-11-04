#include "duckdb/optimizer/aggregate_reuse.hpp"

#include "duckdb/optimizer/aggregate_reuse_internal.hpp"
#include "duckdb/optimizer/aggregate_rewrite_helper.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/expression/bound_aggregate_expression.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/operator/logical_aggregate.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_cteref.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_get.hpp"
#include "duckdb/planner/operator/logical_materialized_cte.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

namespace duckdb {

struct RelationEdge {
	TableColumnOrigin left;
	TableColumnOrigin right;
	ExpressionType comparison;
};

struct RelationGraph {
	vector<reference<LogicalGet>> sources;
	vector<RelationEdge> edges;
};

static bool ContainsSource(const RelationGraph &graph, TableCatalogEntry &table) {
	for (auto &source : graph.sources) {
		if (source.get().GetTable().get() == &table) {
			return true;
		}
	}
	return false;
}

static optional<TableColumnOrigin> FindBindingOrigin(LogicalOperator &op, const ColumnBinding &binding) {
	auto bindings = op.GetColumnBindings();
	auto entry = std::find(bindings.begin(), bindings.end(), binding);
	if (entry != bindings.end()) {
		auto result = GetTableColumnOrigin(op, NumericCast<idx_t>(entry - bindings.begin()), true);
		if (result) {
			return result;
		}
	}
	for (auto &child : op.children) {
		auto result = FindBindingOrigin(*child, binding);
		if (result) {
			return result;
		}
	}
	return nullopt;
}

static bool SameEdge(const RelationEdge &left, const RelationEdge &right) {
	if (left.comparison != right.comparison) {
		return false;
	}
	return (SameOrigin(left.left, right.left) && SameOrigin(left.right, right.right)) ||
	       (SameOrigin(left.left, right.right) && SameOrigin(left.right, right.left));
}

static bool CollectInnerGraph(LogicalOperator &op, optional_ptr<LogicalComparisonJoin> domain_semi,
                              RelationGraph &graph) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (projection.children.size() != 1) {
			return false;
		}
		for (auto &expression : projection.expressions) {
			if (expression->IsVolatile()) {
				return false;
			}
		}
		return CollectInnerGraph(*projection.children[0], domain_semi, graph);
	}
	if (op.type == LogicalOperatorType::LOGICAL_FILTER) {
		auto &filter = op.Cast<LogicalFilter>();
		if (filter.children.size() != 1) {
			return false;
		}
		for (auto &expression : filter.expressions) {
			if (expression->IsVolatile()) {
				return false;
			}
		}
		return filter.expressions.empty() && CollectInnerGraph(*filter.children[0], domain_semi, graph);
	}
	if (op.type == LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		auto &join = op.Cast<LogicalComparisonJoin>();
		if (join.HasProjectionMap() || join.HasArbitraryConditions() || join.children.size() != 2) {
			return false;
		}
		if (domain_semi && &join == domain_semi.get()) {
			const auto core_child = join.join_type == JoinType::RIGHT_SEMI ? idx_t(1) : idx_t(0);
			return CollectInnerGraph(*join.children[core_child], domain_semi, graph);
		}
		if (join.join_type != JoinType::INNER) {
			return false;
		}
		for (auto &condition : join.conditions) {
			if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
				return false;
			}
			auto left = GetTableColumnOrigin(*join.children[0], condition.GetLHS(), true);
			auto right = GetTableColumnOrigin(*join.children[1], condition.GetRHS(), true);
			if (!left || !right) {
				return false;
			}
			graph.edges.push_back({*left, *right, condition.GetComparisonType()});
		}
		return CollectInnerGraph(*join.children[0], domain_semi, graph) &&
		       CollectInnerGraph(*join.children[1], domain_semi, graph);
	}
	if (op.type != LogicalOperatorType::LOGICAL_GET) {
		return false;
	}
	auto &get = op.Cast<LogicalGet>();
	auto table = get.GetTable();
	if (!table || !get.children.empty() || ContainsSource(graph, *table)) {
		return false;
	}
	graph.sources.push_back(get);
	return true;
}

static optional_ptr<LogicalCTERef> GetDomainCTERef(LogicalOperator &op) {
	reference<LogicalOperator> current(op);
	while (current.get().type == LogicalOperatorType::LOGICAL_PROJECTION && current.get().children.size() == 1) {
		auto &projection = current.get().Cast<LogicalProjection>();
		for (auto &expression : projection.expressions) {
			if (expression->IsVolatile()) {
				return nullptr;
			}
		}
		current = *projection.children[0];
	}
	return current.get().type == LogicalOperatorType::LOGICAL_CTE_REF ? current.get().Cast<LogicalCTERef>()
	                                                                  : optional_ptr<LogicalCTERef>();
}

static optional_idx TraceDomainOutput(LogicalOperator &op, idx_t output_idx, TableIndex cte_index) {
	if (op.type == LogicalOperatorType::LOGICAL_CTE_REF) {
		auto &ref = op.Cast<LogicalCTERef>();
		return ref.cte_index == cte_index ? optional_idx(output_idx) : optional_idx();
	}
	if (op.children.size() != 1) {
		return optional_idx();
	}
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION) {
		auto &projection = op.Cast<LogicalProjection>();
		if (output_idx >= projection.expressions.size()) {
			return optional_idx();
		}
		auto child_idx = GetInvertibleReferenceIndex(*projection.expressions[output_idx], *op.children[0]);
		return child_idx.IsValid() ? TraceDomainOutput(*op.children[0], child_idx.GetIndex(), cte_index)
		                           : optional_idx();
	}
	if (op.type == LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		auto &aggregate = op.Cast<LogicalAggregate>();
		if (output_idx >= aggregate.groups.size()) {
			return optional_idx();
		}
		auto child_idx = GetInvertibleReferenceIndex(*aggregate.groups[output_idx], *op.children[0]);
		return child_idx.IsValid() ? TraceDomainOutput(*op.children[0], child_idx.GetIndex(), cte_index)
		                           : optional_idx();
	}
	return optional_idx();
}

struct DomainSemiInfo {
	reference<LogicalComparisonJoin> join;
	reference<LogicalCTERef> cte_ref;
	vector<TableColumnOrigin> core_keys;
	vector<idx_t> cte_columns;
};

static optional<DomainSemiInfo> FindDomainSemi(LogicalOperator &op) {
	if (op.type == LogicalOperatorType::LOGICAL_PROJECTION || op.type == LogicalOperatorType::LOGICAL_FILTER) {
		return op.children.size() == 1 ? FindDomainSemi(*op.children[0]) : nullopt;
	}
	if (op.type != LogicalOperatorType::LOGICAL_COMPARISON_JOIN) {
		return nullopt;
	}
	auto &join = op.Cast<LogicalComparisonJoin>();
	if (join.join_type == JoinType::INNER) {
		auto left = FindDomainSemi(*join.children[0]);
		auto right = FindDomainSemi(*join.children[1]);
		return left && !right ? left : right && !left ? right : nullopt;
	}
	if ((join.join_type != JoinType::SEMI && join.join_type != JoinType::RIGHT_SEMI) || join.HasProjectionMap() ||
	    join.HasArbitraryConditions() || join.conditions.empty()) {
		return nullopt;
	}
	const auto core_child = join.join_type == JoinType::RIGHT_SEMI ? idx_t(1) : idx_t(0);
	const auto domain_child = 1 - core_child;
	auto grouped = FindUnaryAggregate(*join.children[domain_child]);
	optional_ptr<LogicalOperator> domain_input;
	if (grouped) {
		if (!grouped->expressions.empty() || grouped->groups.size() != join.conditions.size() ||
		    grouped->children.size() != 1 || grouped->grouping_sets.size() > 1 ||
		    !grouped->grouping_functions.empty()) {
			return nullopt;
		}
		domain_input = grouped->children[0].get();
	} else {
		domain_input = join.children[domain_child].get();
	}
	auto cte_ref = GetDomainCTERef(*domain_input);
	if (!cte_ref) {
		return nullopt;
	}
	DomainSemiInfo result {join, *cte_ref, {}, {}};
	for (auto &condition : join.conditions) {
		if (!condition.IsComparison() || condition.GetComparisonType() != ExpressionType::COMPARE_EQUAL) {
			return nullopt;
		}
		auto &core_expression = core_child == 0 ? condition.GetLHS() : condition.GetRHS();
		auto &domain_expression = domain_child == 0 ? condition.GetLHS() : condition.GetRHS();
		auto core_key = GetTableColumnOrigin(*join.children[core_child], core_expression, true);
		auto domain_idx = GetDirectReferenceIndex(domain_expression, *join.children[domain_child]);
		if (!core_key || !domain_idx.IsValid()) {
			return nullopt;
		}
		auto cte_col = TraceDomainOutput(*join.children[domain_child], domain_idx.GetIndex(), cte_ref->cte_index);
		if (!cte_col.IsValid()) {
			return nullopt;
		}
		result.core_keys.push_back(*core_key);
		result.cte_columns.push_back(cte_col.GetIndex());
	}
	return result;
}

static optional_ptr<LogicalGet> FindSource(const RelationGraph &graph, TableCatalogEntry &table) {
	for (auto &source : graph.sources) {
		if (source.get().GetTable().get() == &table) {
			return source.get();
		}
	}
	return nullptr;
}

static optional_ptr<const TableFilter> FindSourceFilter(LogicalGet &get, const ColumnIndex &column) {
	for (auto &entry : get.table_filters) {
		if (get.GetColumnIndex(entry.GetIndex()) == column) {
			return entry.Filter();
		}
	}
	return nullptr;
}

static bool SameSourceFilter(const TableFilter &left, const TableFilter &right) {
	if (left.filter_type != TableFilterType::EXPRESSION_FILTER ||
	    right.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return false;
	}
	return Expression::Equals(*left.Cast<ExpressionFilter>().expr, *right.Cast<ExpressionFilter>().expr);
}

static bool IsDomainCorrelationFilter(const TableFilter &filter, const TableColumnOrigin &subject,
                                      LogicalOperator &producer, const vector<TableColumnOrigin> &core_keys,
                                      const vector<TableColumnOrigin> &domain_keys) {
	if (filter.filter_type != TableFilterType::EXPRESSION_FILTER) {
		return false;
	}
	auto &expression = *filter.Cast<ExpressionFilter>().expr;
	if (expression.IsVolatile()) {
		return false;
	}
	vector<reference<const Expression>> children;
	ExpressionIterator::EnumerateChildren(expression, [&](const Expression &child) { children.push_back(child); });
	// The ordinary equality in the domain SEMI already rejects NULL core keys.
	if (expression.GetExpressionType() == ExpressionType::OPERATOR_IS_NOT_NULL && children.size() == 1 &&
	    children[0].get().GetExpressionClass() == ExpressionClass::BOUND_REF) {
		return true;
	}
	if (expression.GetExpressionType() != ExpressionType::COMPARE_NOT_DISTINCT_FROM) {
		return false;
	}
	if (children.size() == 2 && Expression::Equals(children[0].get(), children[1].get())) {
		return true;
	}
	if (children.size() == 2 && children[0].get().GetExpressionClass() == ExpressionClass::BOUND_REF &&
	    children[1].get().GetExpressionClass() == ExpressionClass::BOUND_REF &&
	    children[0].get().Cast<BoundReferenceExpression>().Index() ==
	        children[1].get().Cast<BoundReferenceExpression>().Index()) {
		return true;
	}
	optional<ColumnBinding> external_binding;
	idx_t reference_count = 0;
	ExpressionIterator::VisitExpression<BoundColumnRefExpression>(expression, [&](const BoundColumnRefExpression &ref) {
		external_binding = ref.Binding();
		reference_count++;
	});
	if (reference_count != 1 || !external_binding) {
		return false;
	}
	auto external = FindBindingOrigin(producer, *external_binding);
	if (!external) {
		return false;
	}
	for (idx_t idx = 0; idx < domain_keys.size(); idx++) {
		if (SameOrigin(subject, core_keys[idx]) && SameOrigin(*external, domain_keys[idx])) {
			return true;
		}
	}
	return false;
}

static bool SourceFiltersMatch(const RelationGraph &core, const RelationGraph &producer, LogicalOperator &producer_op,
                               const vector<TableColumnOrigin> &core_keys,
                               const vector<TableColumnOrigin> &domain_keys) {
	for (auto &core_source_ref : core.sources) {
		auto &core_source = core_source_ref.get();
		auto table = core_source.GetTable();
		auto producer_source = table ? FindSource(producer, *table) : optional_ptr<LogicalGet>();
		if (!producer_source) {
			return false;
		}
		for (auto &entry : producer_source->table_filters) {
			auto column = producer_source->GetColumnIndex(entry.GetIndex());
			auto core_filter = FindSourceFilter(core_source, column);
			if (!core_filter || !SameSourceFilter(*core_filter, entry.Filter())) {
				return false;
			}
		}
		for (auto &entry : core_source.table_filters) {
			auto column = core_source.GetColumnIndex(entry.GetIndex());
			auto producer_filter = FindSourceFilter(*producer_source, column);
			if (producer_filter && SameSourceFilter(entry.Filter(), *producer_filter)) {
				continue;
			}
			TableColumnOrigin subject {*table, column};
			bool is_core_key = std::find_if(core_keys.begin(), core_keys.end(), [&](const TableColumnOrigin &key) {
				                   return SameOrigin(subject, key);
			                   }) != core_keys.end();
			if (!is_core_key ||
			    !IsDomainCorrelationFilter(entry.Filter(), subject, producer_op, core_keys, domain_keys)) {
				return false;
			}
		}
	}
	return true;
}

static optional_idx FindProducerOutput(LogicalOperator &producer, const TableColumnOrigin &origin) {
	auto bindings = producer.GetColumnBindings();
	for (idx_t idx = 0; idx < bindings.size(); idx++) {
		auto candidate = GetTableColumnOrigin(producer, idx, true);
		if (candidate && SameOrigin(*candidate, origin)) {
			return optional_idx(idx);
		}
	}
	return optional_idx();
}

static unique_ptr<Expression> RewriteToCTE(const Expression &expression, LogicalOperator &scope,
                                           LogicalOperator &producer, LogicalCTERef &cte_ref,
                                           const vector<pair<TableColumnOrigin, TableColumnOrigin>> &key_map,
                                           bool &success) {
	if (expression.GetExpressionClass() == ExpressionClass::BOUND_COLUMN_REF ||
	    expression.GetExpressionClass() == ExpressionClass::BOUND_REF) {
		auto output_idx = GetDirectReferenceIndex(expression, scope);
		if (!output_idx.IsValid()) {
			success = false;
			return nullptr;
		}
		if (scope.type == LogicalOperatorType::LOGICAL_PROJECTION) {
			auto &projection = scope.Cast<LogicalProjection>();
			if (projection.children.size() != 1 || output_idx.GetIndex() >= projection.expressions.size()) {
				success = false;
				return nullptr;
			}
			return RewriteToCTE(*projection.expressions[output_idx.GetIndex()], *projection.children[0], producer,
			                    cte_ref, key_map, success);
		}
		auto origin = GetTableColumnOrigin(scope, output_idx.GetIndex(), true);
		if (!origin) {
			success = false;
			return nullptr;
		}
		for (auto &entry : key_map) {
			if (SameOrigin(*origin, entry.first)) {
				origin = entry.second;
				break;
			}
		}
		auto producer_idx = FindProducerOutput(producer, *origin);
		if (!producer_idx.IsValid() || producer_idx.GetIndex() >= cte_ref.types.size()) {
			success = false;
			return nullptr;
		}
		return make_uniq<BoundColumnRefExpression>(
		    cte_ref.types[producer_idx.GetIndex()],
		    ColumnBinding(cte_ref.table_index, ProjectionIndex(producer_idx.GetIndex())));
	}
	auto result = expression.Copy();
	ExpressionIterator::EnumerateChildren(*result, [&](unique_ptr<Expression> &child) {
		if (success) {
			child = RewriteToCTE(*child, scope, producer, cte_ref, key_map, success);
		}
	});
	return success ? std::move(result) : nullptr;
}

bool AggregateReuseOptimizer::TryReuseMaterializedAggregate(unique_ptr<LogicalOperator> &op) {
	if (op->type != LogicalOperatorType::LOGICAL_AGGREGATE_AND_GROUP_BY) {
		return false;
	}
	auto &aggregate = op->Cast<LogicalAggregate>();
	if (aggregate.children.size() != 1 || aggregate.expressions.empty() || !HasCompleteGrouping(aggregate)) {
		return false;
	}
	for (auto &group : aggregate.groups) {
		if (group->IsVolatile()) {
			return false;
		}
	}
	for (auto &expression : aggregate.expressions) {
		if (expression->IsVolatile() || expression->GetExpressionClass() != ExpressionClass::BOUND_AGGREGATE) {
			return false;
		}
		auto &bound_aggregate = expression->Cast<BoundAggregateExpression>();
		if (!bound_aggregate.IsDistinct() &&
		    bound_aggregate.Function().GetDistinctDependent() != AggregateDistinctDependent::NOT_DISTINCT_DEPENDENT) {
			return false;
		}
	}

	auto domain = FindDomainSemi(*aggregate.children[0]);
	if (!domain || domain->core_keys.size() != aggregate.groups.size()) {
		return false;
	}
	auto cte_entry = cte_definitions.find(domain->cte_ref.get().cte_index.index);
	if (cte_entry == cte_definitions.end()) {
		return false;
	}
	auto &cte = cte_entry->second.get();
	if (cte.children.size() != 2 || cte.children[0]->types.size() < cte.column_count) {
		return false;
	}
	auto &producer_op = *cte.children[0];

	RelationGraph core_graph;
	RelationGraph producer_graph;
	auto core_ok = CollectInnerGraph(*aggregate.children[0], domain->join.get(), core_graph);
	auto producer_ok = CollectInnerGraph(producer_op, nullptr, producer_graph);
	if (!core_ok || !producer_ok || core_graph.sources.empty() ||
	    producer_graph.sources.size() != core_graph.sources.size() + 1) {
		return false;
	}
	for (auto &source : core_graph.sources) {
		auto table = source.get().GetTable();
		if (!table || !ContainsSource(producer_graph, *table)) {
			return false;
		}
	}
	optional_ptr<LogicalGet> extra_source;
	for (auto &source : producer_graph.sources) {
		auto table = source.get().GetTable();
		if (table && !ContainsSource(core_graph, *table)) {
			extra_source = source.get();
		}
	}
	if (!extra_source || !extra_source->GetTable()) {
		return false;
	}

	vector<bool> matched_core_edges(core_graph.edges.size(), false);
	vector<RelationEdge> cross_edges;
	for (auto &edge : producer_graph.edges) {
		const auto left_core = ContainsSource(core_graph, edge.left.table);
		const auto right_core = ContainsSource(core_graph, edge.right.table);
		if (left_core && right_core) {
			bool matched = false;
			for (idx_t idx = 0; idx < core_graph.edges.size(); idx++) {
				if (!matched_core_edges[idx] && SameEdge(edge, core_graph.edges[idx])) {
					matched_core_edges[idx] = true;
					matched = true;
					break;
				}
			}
			if (!matched) {
				return false;
			}
		} else if (left_core != right_core) {
			cross_edges.push_back(edge);
		} else {
			return false;
		}
	}
	if (std::find(matched_core_edges.begin(), matched_core_edges.end(), false) != matched_core_edges.end() ||
	    cross_edges.size() != domain->core_keys.size()) {
		return false;
	}

	vector<TableColumnOrigin> domain_keys;
	vector<pair<TableColumnOrigin, TableColumnOrigin>> key_map;
	vector<bool> matched_cross_edges(cross_edges.size(), false);
	for (idx_t key_idx = 0; key_idx < domain->core_keys.size(); key_idx++) {
		if (domain->cte_columns[key_idx] >= producer_op.GetColumnBindings().size()) {
			return false;
		}
		auto domain_key = GetTableColumnOrigin(producer_op, domain->cte_columns[key_idx], true);
		auto group_key = GetTableColumnOrigin(*aggregate.children[0], *aggregate.groups[key_idx], true);
		if (!domain_key || !group_key || !SameOrigin(*group_key, domain->core_keys[key_idx]) ||
		    &domain_key->table.get() != extra_source->GetTable().get()) {
			return false;
		}
		bool matched_cross = false;
		for (idx_t edge_idx = 0; edge_idx < cross_edges.size(); edge_idx++) {
			if (matched_cross_edges[edge_idx]) {
				continue;
			}
			auto &edge = cross_edges[edge_idx];
			if ((SameOrigin(edge.left, domain->core_keys[key_idx]) && SameOrigin(edge.right, *domain_key)) ||
			    (SameOrigin(edge.right, domain->core_keys[key_idx]) && SameOrigin(edge.left, *domain_key))) {
				matched_cross = true;
				matched_cross_edges[edge_idx] = true;
				break;
			}
		}
		if (!matched_cross) {
			return false;
		}
		domain_keys.push_back(*domain_key);
		key_map.emplace_back(domain->core_keys[key_idx], *domain_key);
	}
	auto filters_match = SourceFiltersMatch(core_graph, producer_graph, producer_op, domain->core_keys, domain_keys);
	if (!filters_match) {
		return false;
	}

	auto cte_ref_index = optimizer.binder.GenerateTableIndex();
	auto names = AggregateRewriteHelper::GenerateColumnNames("__aggregate_reuse", cte.column_count);
	vector<LogicalType> types(producer_op.types.begin(),
	                          producer_op.types.begin() +
	                              NumericCast<vector<LogicalType>::difference_type>(cte.column_count));
	auto replacement = make_uniq<LogicalCTERef>(cte_ref_index, cte.table_index, std::move(types), std::move(names));
	replacement->estimated_cardinality = producer_op.estimated_cardinality;
	replacement->has_estimated_cardinality = producer_op.has_estimated_cardinality;
	replacement->ResolveOperatorTypes();
	bool success = true;
	vector<unique_ptr<Expression>> groups;
	vector<unique_ptr<Expression>> expressions;
	for (auto &group : aggregate.groups) {
		groups.push_back(RewriteToCTE(*group, *aggregate.children[0], producer_op, *replacement, key_map, success));
	}
	for (auto &expression : aggregate.expressions) {
		expressions.push_back(
		    RewriteToCTE(*expression, *aggregate.children[0], producer_op, *replacement, key_map, success));
	}
	if (!success) {
		return false;
	}
	aggregate.groups = std::move(groups);
	aggregate.expressions = std::move(expressions);
	aggregate.children[0] = std::move(replacement);
	aggregate.ResolveOperatorTypes();
	return true;
}

} // namespace duckdb
