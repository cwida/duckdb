#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/execution/column_binding_resolver.hpp"
#include "duckdb/optimizer/mark_join_simplifier.hpp"
#include "duckdb/optimizer/optimizer.hpp"
#include "duckdb/parser/statement/logical_plan_statement.hpp"
#include "duckdb/planner/binder.hpp"
#include "duckdb/planner/expression/bound_columnref_expression.hpp"
#include "duckdb/planner/expression/bound_constant_expression.hpp"
#include "duckdb/planner/expression/bound_operator_expression.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/operator/logical_dummy_scan.hpp"
#include "duckdb/planner/operator/logical_filter.hpp"
#include "duckdb/planner/operator/logical_projection.hpp"

using namespace duckdb;

static unique_ptr<LogicalProjection> MarkerBindingInput(Binder &binder, vector<Value> values) {
	vector<unique_ptr<Expression>> expressions;
	for (auto &value : values) {
		expressions.push_back(make_uniq<BoundConstantExpression>(value));
	}
	auto projection = make_uniq<LogicalProjection>(binder.GenerateTableIndex(), std::move(expressions));
	projection->children.push_back(make_uniq<LogicalDummyScan>(binder.GenerateTableIndex()));
	return projection;
}

static unique_ptr<Expression> MarkerBindingRef(LogicalType type, ColumnBinding binding) {
	return make_uniq<BoundColumnRefExpression>(std::move(type), binding);
}

class BindingCountingFilter : public LogicalFilter {
public:
	explicit BindingCountingFilter(idx_t &binding_requests) : binding_requests(binding_requests) {
	}

	vector<ColumnBinding> GetColumnBindings() override {
		binding_requests++;
		return LogicalFilter::GetColumnBindings();
	}

private:
	idx_t &binding_requests;
};

TEST_CASE("MARK simplification skips output enumeration without candidates", "[optimizer][mark_join_simplifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto binder = Binder::CreateBinder(*con.context);
	Optimizer optimizer(*binder, *con.context);
	idx_t binding_requests = 0;
	unique_ptr<LogicalOperator> plan = MarkerBindingInput(*binder, {Value::INTEGER(42)});
	for (idx_t depth = 0; depth < 256; depth++) {
		auto filter = make_uniq<BindingCountingFilter>(binding_requests);
		filter->children.push_back(std::move(plan));
		plan = std::move(filter);
	}
	MarkJoinSimplifier simplifier(optimizer);
	simplifier.Optimize(plan);
	REQUIRE(binding_requests == 0);
}

TEST_CASE("Stacked MARK conversions retain original output consumers", "[optimizer][bindings][mark_join_simplifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto binder = Binder::CreateBinder(*con.context);
	Optimizer optimizer(*binder, *con.context);
	unique_ptr<LogicalOperator> plan = MarkerBindingInput(*binder, {Value::INTEGER(42)});
	auto probe_binding = plan->GetColumnBindings()[0];
	for (idx_t depth = 0; depth < 16; depth++) {
		auto build = MarkerBindingInput(*binder, {Value::INTEGER(42)});
		auto build_binding = build->GetColumnBindings()[0];
		auto mark = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
		mark->mark_index = binder->GenerateTableIndex();
		auto marker = ColumnBinding(mark->mark_index, ProjectionIndex(0));
		mark->conditions.emplace_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding),
		                              MarkerBindingRef(LogicalType::INTEGER, build_binding),
		                              ExpressionType::COMPARE_EQUAL);
		mark->children.push_back(std::move(plan));
		mark->children.push_back(std::move(build));
		auto filter = make_uniq<LogicalFilter>(MarkerBindingRef(LogicalType::BOOLEAN, marker));
		filter->children.push_back(std::move(mark));
		plan = std::move(filter);
	}
	plan->ResolveOperatorTypes();
	auto bindings = plan->GetColumnBindings();
	vector<unique_ptr<Expression>> outputs;
	for (idx_t i = 0; i < bindings.size(); i++) {
		outputs.push_back(MarkerBindingRef(plan->types[i], bindings[i]));
	}
	auto projection = make_uniq<LogicalProjection>(binder->GenerateTableIndex(), std::move(outputs));
	projection->children.push_back(std::move(plan));
	plan = std::move(projection);
	MarkJoinSimplifier simplifier(optimizer);
	simplifier.Optimize(plan);
	plan->ResolveOperatorTypes();
	REQUIRE(plan->types.size() == 17);
	auto child_bindings = plan->children[0]->GetColumnBindings();
	for (idx_t i = 0; i < plan->expressions.size(); i++) {
		REQUIRE(plan->expressions[i]->Cast<BoundColumnRefExpression>().Binding() == child_bindings[i]);
	}
	ColumnBindingResolver resolver(true);
	REQUIRE_NOTHROW(resolver.VisitOperator(*plan));
	REQUIRE_NO_FAIL(con.Query("SET debug_disable_optimizer=true"));
	auto result = con.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	for (idx_t i = 1; i < result->ColumnCount(); i++) {
		REQUIRE(CHECK_COLUMN(result, i, {true}));
	}
	con.Rollback();
}

TEST_CASE("MARK simplification skips output enumeration for ineligible markers", "[optimizer][mark_join_simplifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto binder = Binder::CreateBinder(*con.context);
	Optimizer optimizer(*binder, *con.context);
	idx_t binding_requests = 0;
	auto probe = MarkerBindingInput(*binder, {Value(LogicalType::INTEGER)});
	auto build = MarkerBindingInput(*binder, {Value(LogicalType::INTEGER)});
	auto mark = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
	auto &retained_mark = *mark;
	mark->mark_index = binder->GenerateTableIndex();
	auto marker = ColumnBinding(mark->mark_index, ProjectionIndex(0));
	mark->conditions.emplace_back(MarkerBindingRef(LogicalType::INTEGER, probe->GetColumnBindings()[0]),
	                              MarkerBindingRef(LogicalType::INTEGER, build->GetColumnBindings()[0]),
	                              ExpressionType::COMPARE_EQUAL);
	mark->children.push_back(std::move(probe));
	mark->children.push_back(std::move(build));
	auto predicate = make_uniq<BoundOperatorExpression>(ExpressionType::OPERATOR_NOT, LogicalType::BOOLEAN);
	predicate->GetChildrenMutable().push_back(MarkerBindingRef(LogicalType::BOOLEAN, marker));
	auto filter = make_uniq<BindingCountingFilter>(binding_requests);
	filter->expressions.push_back(std::move(predicate));
	filter->children.push_back(std::move(mark));
	unique_ptr<LogicalOperator> plan = std::move(filter);
	for (idx_t depth = 0; depth < 256; depth++) {
		auto passthrough = make_uniq<BindingCountingFilter>(binding_requests);
		passthrough->children.push_back(std::move(plan));
		plan = std::move(passthrough);
	}
	auto expected_bindings = plan->GetColumnBindings();
	binding_requests = 0;
	MarkJoinSimplifier simplifier(optimizer);
	simplifier.Optimize(plan);
	REQUIRE(binding_requests == 0);
	REQUIRE(retained_mark.join_type == JoinType::MARK);
	REQUIRE(plan->GetColumnBindings() == expected_bindings);
}

TEST_CASE("MARK conversion does not enumerate unchanged enclosing filters",
          "[optimizer][bindings][mark_join_simplifier]") {
	DuckDB db(nullptr);
	Connection con(db);
	con.BeginTransaction();
	auto binder = Binder::CreateBinder(*con.context);
	Optimizer optimizer(*binder, *con.context);
	idx_t binding_requests = 0;
	auto probe = MarkerBindingInput(*binder, {Value::INTEGER(42)});
	auto build = MarkerBindingInput(*binder, {Value::INTEGER(42)});
	auto probe_binding = probe->GetColumnBindings()[0];
	auto mark = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
	auto &converted_mark = *mark;
	mark->mark_index = binder->GenerateTableIndex();
	auto marker = ColumnBinding(mark->mark_index, ProjectionIndex(0));
	mark->conditions.emplace_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding),
	                              MarkerBindingRef(LogicalType::INTEGER, build->GetColumnBindings()[0]),
	                              ExpressionType::COMPARE_EQUAL);
	mark->children.push_back(std::move(probe));
	mark->children.push_back(std::move(build));
	auto filter = make_uniq<LogicalFilter>(MarkerBindingRef(LogicalType::BOOLEAN, marker));
	filter->children.push_back(std::move(mark));
	unique_ptr<LogicalOperator> plan = std::move(filter);
	for (idx_t depth = 0; depth < 256; depth++) {
		auto enclosing = make_uniq<BindingCountingFilter>(binding_requests);
		enclosing->expressions.push_back(MarkerBindingRef(LogicalType::BOOLEAN, marker));
		enclosing->children.push_back(std::move(plan));
		plan = std::move(enclosing);
	}
	vector<unique_ptr<Expression>> outputs;
	outputs.push_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding));
	outputs.push_back(MarkerBindingRef(LogicalType::BOOLEAN, marker));
	auto projection = make_uniq<LogicalProjection>(binder->GenerateTableIndex(), std::move(outputs));
	projection->children.push_back(std::move(plan));
	plan = std::move(projection);
	MarkJoinSimplifier simplifier(optimizer);
	simplifier.Optimize(plan);
	REQUIRE(binding_requests == 0);
	REQUIRE(converted_mark.join_type == JoinType::SEMI);
	plan->ResolveOperatorTypes();
	ColumnBindingResolver resolver(true);
	REQUIRE_NOTHROW(resolver.VisitOperator(*plan));
	REQUIRE_NO_FAIL(con.Query("SET debug_disable_optimizer=true"));
	auto result = con.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
	REQUIRE_NO_FAIL(*result);
	REQUIRE(CHECK_COLUMN(result, 0, {42}));
	REQUIRE(CHECK_COLUMN(result, 1, {true}));
	con.Rollback();
}

TEST_CASE("MARK conversion preserves duplicate output selections", "[optimizer][bindings][mark_join_simplifier]") {
	for (bool select_second : {false, true}) {
		CAPTURE(select_second);
		DuckDB db(nullptr);
		Connection con(db);
		con.BeginTransaction();
		auto binder = Binder::CreateBinder(*con.context);
		Optimizer optimizer(*binder, *con.context);

		auto probe = MarkerBindingInput(*binder, {Value::INTEGER(42)});
		auto build = MarkerBindingInput(*binder, {Value::INTEGER(42)});
		auto probe_binding = probe->GetColumnBindings()[0];
		auto build_binding = build->GetColumnBindings()[0];
		auto mark = make_uniq<LogicalComparisonJoin>(JoinType::MARK);
		mark->mark_index = binder->GenerateTableIndex();
		auto marker = ColumnBinding(mark->mark_index, ProjectionIndex(0));
		mark->left_projection_map = {ProjectionIndex(0), ProjectionIndex(0)};
		mark->conditions.emplace_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding),
		                              MarkerBindingRef(LogicalType::INTEGER, build_binding),
		                              ExpressionType::COMPARE_EQUAL);
		mark->children.push_back(std::move(probe));
		mark->children.push_back(std::move(build));

		auto filter = make_uniq<LogicalFilter>(MarkerBindingRef(LogicalType::BOOLEAN, marker));
		if (select_second) {
			filter->projection_map = {ProjectionIndex(1)};
		}
		filter->children.push_back(std::move(mark));

		auto other = MarkerBindingInput(*binder, {Value::INTEGER(42), Value("joined")});
		auto other_bindings = other->GetColumnBindings();
		auto join = make_uniq<LogicalComparisonJoin>(JoinType::INNER);
		auto &outer_join = *join;
		join->right_projection_map = {ProjectionIndex(1)};
		join->conditions.emplace_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding),
		                              MarkerBindingRef(LogicalType::INTEGER, other_bindings[0]),
		                              ExpressionType::COMPARE_EQUAL);
		join->children.push_back(std::move(filter));
		join->children.push_back(std::move(other));

		vector<unique_ptr<Expression>> outputs;
		outputs.push_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding));
		outputs.push_back(MarkerBindingRef(LogicalType::INTEGER, probe_binding));
		outputs.push_back(MarkerBindingRef(LogicalType::VARCHAR, other_bindings[1]));
		if (!select_second) {
			outputs.push_back(MarkerBindingRef(LogicalType::BOOLEAN, marker));
		}
		auto result_projection = make_uniq<LogicalProjection>(binder->GenerateTableIndex(), std::move(outputs));
		result_projection->children.push_back(std::move(join));
		unique_ptr<LogicalOperator> plan = std::move(result_projection);
		plan->ResolveOperatorTypes();
		auto expected_types = plan->types;
		ColumnBindingResolver initial_resolver(true);
		REQUIRE_NOTHROW(initial_resolver.VisitOperator(*plan));

		MarkJoinSimplifier simplifier(optimizer);
		simplifier.Optimize(plan);
		plan->ResolveOperatorTypes();
		REQUIRE(plan->types == expected_types);
		REQUIRE(outer_join.GetColumnBindings().size() == (select_second ? 2 : 4));
		auto &left = outer_join.children[0];
		if (select_second) {
			REQUIRE(left->type == LogicalOperatorType::LOGICAL_FILTER);
			auto &selection = left->Cast<LogicalFilter>();
			REQUIRE(selection.expressions.empty());
			REQUIRE(selection.projection_map == vector<ProjectionIndex> {ProjectionIndex(0)});
		} else {
			REQUIRE(left->type == LogicalOperatorType::LOGICAL_FILTER);
			vector<ProjectionIndex> expected_map {ProjectionIndex(0), ProjectionIndex(0), ProjectionIndex(2)};
			REQUIRE(left->Cast<LogicalFilter>().projection_map == expected_map);
			REQUIRE(outer_join.left_projection_map.empty());
		}
		auto &restoring = *left->children[0];
		REQUIRE(restoring.type == LogicalOperatorType::LOGICAL_PROJECTION);
		REQUIRE(restoring.expressions.size() == 3);
		REQUIRE(restoring.expressions[0]->Cast<BoundColumnRefExpression>().Binding() == probe_binding);
		REQUIRE(restoring.expressions[1]->Cast<BoundColumnRefExpression>().Binding() == probe_binding);
		REQUIRE(restoring.expressions[2]->Cast<BoundConstantExpression>().GetValue() == Value::BOOLEAN(true));
		REQUIRE(restoring.children[0]->Cast<LogicalComparisonJoin>().join_type == JoinType::SEMI);
		ColumnBindingResolver resolver(true);
		REQUIRE_NOTHROW(resolver.VisitOperator(*plan));

		REQUIRE_NO_FAIL(con.Query("SET debug_disable_optimizer=true"));
		auto result = con.Query(make_uniq<LogicalPlanStatement>(std::move(plan)));
		REQUIRE_NO_FAIL(*result);
		REQUIRE(CHECK_COLUMN(result, 0, {42}));
		REQUIRE(CHECK_COLUMN(result, 1, {42}));
		REQUIRE(CHECK_COLUMN(result, 2, {"joined"}));
		if (!select_second) {
			REQUIRE(CHECK_COLUMN(result, 3, {true}));
		}
		con.Rollback();
	}
}
