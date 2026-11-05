#include "catch.hpp"

#include "duckdb/optimizer/column_binding_replacer.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

static optional_ptr<const ReplacementBinding> FindEdge(const BindingReplacementGraph &graph, ColumnBinding binding) {
	for (auto &replacement : graph) {
		if (replacement.old_binding == binding) {
			return replacement;
		}
	}
	return nullptr;
}

TEST_CASE("Binding replacement graph preserves direct edges", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));

	for (bool reverse : {false, true}) {
		BindingReplacementGraph graph;
		if (reverse) {
			graph.Add(binding_b, binding_c);
			graph.Add(binding_a, binding_b);
		} else {
			graph.Add(binding_a, binding_b);
			graph.Add(binding_b, binding_c);
		}
		REQUIRE(graph.Resolve(binding_a) == binding_c);
		REQUIRE(FindEdge(graph, binding_a)->new_binding == binding_b);
		REQUIRE(FindEdge(graph, binding_b)->new_binding == binding_c);
	}

	BindingReplacementGraph first;
	first.Add(binding_a, binding_b);
	BindingReplacementGraph second;
	second.Add(binding_b, binding_c);
	for (bool reverse : {false, true}) {
		BindingReplacementGraph graph;
		if (reverse) {
			graph.Merge(second);
			graph.Merge(first);
		} else {
			graph.Merge(first);
			graph.Merge(second);
		}
		REQUIRE(graph.Resolve(binding_a) == binding_c);
		REQUIRE(FindEdge(graph, binding_a)->new_binding == binding_b);
	}
}

TEST_CASE("Binding replacement graph rejects ambiguous provenance", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));

	BindingReplacementGraph graph;
	REQUIRE(graph.TryAdd(ReplacementBinding(binding_a, binding_b)));
	REQUIRE(graph.TryAdd(ReplacementBinding(binding_a, binding_b, LogicalType::BIGINT)));
	auto edge = FindEdge(graph, binding_a);
	REQUIRE(edge);
	REQUIRE(edge->replace_type);
	REQUIRE(edge->new_type == LogicalType::BIGINT);
	REQUIRE_FALSE(graph.TryAdd(ReplacementBinding(binding_a, binding_b, LogicalType::VARCHAR)));

	graph.Add(binding_b, binding_c);
	REQUIRE_FALSE(graph.TryAdd(ReplacementBinding(binding_a, binding_c)));
	REQUIRE_FALSE(graph.TryAdd(ReplacementBinding(binding_c, binding_a)));

	BindingReplacementGraph reverse_cycle;
	reverse_cycle.Add(binding_c, binding_a);
	REQUIRE_FALSE(reverse_cycle.TryAdd(ReplacementBinding(binding_a, binding_c)));

	BindingReplacementGraph identity;
	REQUIRE(identity.TryAdd(ReplacementBinding(binding_a, binding_a)));
	REQUIRE(identity.Empty());
}

TEST_CASE("Binding replacement graph validates output boundaries", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));
	auto binding_d = ColumnBinding(TableIndex(3), ProjectionIndex(0));

	BindingReplacementGraph graph;
	graph.Add(binding_a, binding_b);
	graph.Add(binding_b, binding_c);

	// Validation stops at the current output boundary even if the complete graph continues beyond it.
	REQUIRE_NOTHROW(ColumnBindingRewrite::ValidateOutput({binding_a}, {binding_b}, graph));
	REQUIRE_NOTHROW(ColumnBindingRewrite::ValidateOutput({binding_b}, {binding_b}, graph));
	REQUIRE_NOTHROW(ColumnBindingRewrite::ValidateOutput({binding_a}, {binding_c}, graph));
	REQUIRE_NOTHROW(ColumnBindingRewrite::ValidateOutput({binding_d}, {binding_d}, graph));
}

static unique_ptr<LogicalOperator> CreateBoundaryTestPlan(TableIndex child_index, ColumnBinding filter_binding) {
	vector<unique_ptr<Expression>> child_expressions;
	child_expressions.push_back(make_uniq<BoundConstantExpression>(Value::INTEGER(10)));
	child_expressions.push_back(make_uniq<BoundConstantExpression>(Value::BIGINT(20)));
	auto child = make_uniq<LogicalProjection>(child_index, std::move(child_expressions));

	auto filter_expression = make_uniq<BoundColumnRefExpression>(LogicalType::INTEGER, filter_binding);
	auto filter = make_uniq<LogicalFilter>(std::move(filter_expression));
	filter->projection_map.emplace_back(ProjectionIndex(0));
	filter->children.push_back(std::move(child));
	return std::move(filter);
}

TEST_CASE("Binding rewrites stop at child output boundaries", "[optimizer][bindings]") {
	auto table_a = TableIndex(0);
	auto table_b = TableIndex(1);
	auto table_c = TableIndex(2);
	auto binding_a0 = ColumnBinding(table_a, ProjectionIndex(0));
	auto binding_a1 = ColumnBinding(table_a, ProjectionIndex(1));
	auto binding_b0 = ColumnBinding(table_b, ProjectionIndex(0));
	auto binding_b1 = ColumnBinding(table_b, ProjectionIndex(1));
	auto binding_c1 = ColumnBinding(table_c, ProjectionIndex(1));

	for (bool reverse : {false, true}) {
		BindingReplacementGraph graph;
		if (reverse) {
			graph.Add(ReplacementBinding(binding_b1, binding_c1, LogicalType::VARCHAR));
			graph.Add(ReplacementBinding(binding_a0, binding_b1, LogicalType::BIGINT));
		} else {
			graph.Add(ReplacementBinding(binding_a0, binding_b1, LogicalType::BIGINT));
			graph.Add(ReplacementBinding(binding_b1, binding_c1, LogicalType::VARCHAR));
		}
		graph.Add(binding_a1, binding_b0);

		auto plan = CreateBoundaryTestPlan(table_b, binding_a0);
		ColumnBindingRewrite::ApplyToChild(plan, 0, {binding_a0, binding_a1}, graph);

		auto &rewritten_filter = plan->Cast<LogicalFilter>();
		REQUIRE(rewritten_filter.projection_map.size() == 1);
		REQUIRE(rewritten_filter.projection_map[0] == ProjectionIndex(1));
		auto &rewritten_expression = rewritten_filter.expressions[0]->Cast<BoundColumnRefExpression>();
		REQUIRE(rewritten_expression.Binding() == binding_b1);
		REQUIRE(rewritten_expression.GetReturnType() == LogicalType::BIGINT);
	}

	BindingReplacementGraph downstream_type;
	downstream_type.Add(binding_a0, binding_b1);
	downstream_type.Add(ReplacementBinding(binding_b1, binding_c1, LogicalType::VARCHAR));
	auto plan = CreateBoundaryTestPlan(table_b, binding_a0);
	ColumnBindingRewrite::ApplyToChild(plan, 0, {binding_a0, binding_a1}, downstream_type);
	auto &rewritten_expression = plan->Cast<LogicalFilter>().expressions[0]->Cast<BoundColumnRefExpression>();
	REQUIRE(rewritten_expression.Binding() == binding_b1);
	REQUIRE(rewritten_expression.GetReturnType() == LogicalType::INTEGER);
}

TEST_CASE("Binding replacement graph finds direct edges by old binding", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));

	BindingReplacementGraph graph;
	graph.Add(binding_a, binding_b);
	graph.Add(ReplacementBinding(binding_b, binding_c, LogicalType::BIGINT));

	REQUIRE(graph.Find(binding_a));
	REQUIRE(graph.Find(binding_a)->new_binding == binding_b);
	REQUIRE(graph.Find(binding_b)->new_binding == binding_c);
	REQUIRE(graph.Find(binding_b)->replace_type);
	REQUIRE_FALSE(graph.Find(binding_c));
}

TEST_CASE("Binding replacement graph scopes to an output boundary", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));
	auto binding_d = ColumnBinding(TableIndex(3), ProjectionIndex(0));
	auto binding_e = ColumnBinding(TableIndex(4), ProjectionIndex(0));

	BindingReplacementGraph graph;
	graph.Add(ReplacementBinding(binding_a, binding_b, LogicalType::BIGINT));
	graph.Add(binding_b, binding_c);
	graph.Add(binding_d, binding_e);

	// Chains collapse to their first boundary hit, edges that cannot reach the boundary are dropped
	auto scoped = graph.ScopedToBoundary({binding_c});
	REQUIRE(scoped.Find(binding_a));
	REQUIRE(scoped.Find(binding_a)->new_binding == binding_c);
	REQUIRE(scoped.Find(binding_a)->replace_type);
	REQUIRE(scoped.Find(binding_a)->new_type == LogicalType::BIGINT);
	REQUIRE(scoped.Find(binding_b)->new_binding == binding_c);
	REQUIRE_FALSE(scoped.Find(binding_d));

	// Resolution stops at the first boundary binding on the chain
	auto scoped_mid = graph.ScopedToBoundary({binding_b, binding_c});
	REQUIRE(scoped_mid.Find(binding_a)->new_binding == binding_b);
	// Edges departing from a boundary binding keep their continuation for enclosing boundaries
	REQUIRE(scoped_mid.Find(binding_b));
	REQUIRE(scoped_mid.Find(binding_b)->new_binding == binding_c);
}

TEST_CASE("Indexed column binding replacer matches sequential application", "[optimizer][bindings]") {
	auto binding_a = ColumnBinding(TableIndex(0), ProjectionIndex(0));
	auto binding_b = ColumnBinding(TableIndex(1), ProjectionIndex(0));
	auto binding_c = ColumnBinding(TableIndex(2), ProjectionIndex(0));
	auto binding_d = ColumnBinding(TableIndex(3), ProjectionIndex(0));

	// Chain-free list: the index takes effect and single lookups match sequential application
	ColumnBindingReplacer replacer;
	replacer.replacement_bindings.emplace_back(binding_a, binding_c);
	replacer.replacement_bindings.emplace_back(binding_b, binding_d, LogicalType::BIGINT);
	replacer.BuildIndex();
	auto binding = binding_a;
	replacer.Replace(binding);
	REQUIRE(binding == binding_c);
	binding = binding_b;
	auto type = LogicalType(LogicalType::INTEGER);
	replacer.Replace(binding, type);
	REQUIRE(binding == binding_d);
	REQUIRE(type == LogicalType::BIGINT);
	binding = binding_c;
	replacer.Replace(binding);
	REQUIRE(binding == binding_c);

	// Chainable list: the index must not take effect, sequential application still chains a -> b -> c
	ColumnBindingReplacer chaining;
	chaining.replacement_bindings.emplace_back(binding_a, binding_b);
	chaining.replacement_bindings.emplace_back(binding_b, binding_c);
	chaining.BuildIndex();
	binding = binding_a;
	chaining.Replace(binding);
	REQUIRE(binding == binding_c);
}
