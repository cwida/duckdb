#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

using namespace duckdb;

TEST_CASE("Disjunctive scan filters preserve selection, validity and repeated execution", "[dynamic_filter_groups]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto strings = GENERATE(false, true);
	const auto optional = GENERATE(false, true);
	const auto nested = GENERATE(false, true);
	const auto type = strings ? LogicalType::VARCHAR : LogicalType::INTEGER;
	auto value = [&](int32_t number) {
		return strings ? Value(std::to_string(number)) : Value::INTEGER(number);
	};
	auto comparison = [&](ExpressionType comparison_type, int32_t number) {
		return ExpressionFilter::CreateComparisonFilter(comparison_type, value(number))->expr->Copy();
	};
	auto disjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	for (int32_t lower : {2, 4}) {
		auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conjunction->GetChildrenMutable().push_back(comparison(ExpressionType::COMPARE_GREATERTHANOREQUALTO, lower));
		conjunction->GetChildrenMutable().push_back(
		    comparison(ExpressionType::COMPARE_LESSTHANOREQUALTO, lower == 2 ? 5 : 8));
		disjunction->GetChildrenMutable().push_back(std::move(conjunction));
	}
	if (optional) {
		disjunction->GetChildrenMutable().push_back(
		    CreateOptionalFilterExpression(comparison(ExpressionType::COMPARE_EQUAL, 0), type));
	}
	unique_ptr<Expression> expression = std::move(disjunction);
	if (nested) {
		auto conjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND);
		conjunction->GetChildrenMutable().push_back(std::move(expression));
		conjunction->GetChildrenMutable().push_back(comparison(ExpressionType::COMPARE_LESSTHAN, 7));
		expression = std::move(conjunction);
	}
	ExpressionFilterState state(*con.context, *expression);
	REQUIRE(state.fast_executor);
	ExpressionFilterState fallback(*con.context, *expression);
	fallback.fast_executor.reset();
	auto check = [&](Vector &input, SelectionVector &selection, idx_t &count) {
		SelectionVector expected(selection);
		auto expected_count = count;
		ColumnSegment::FilterSelection(expected, input, fallback, input.size(), expected_count);
		state.fast_executor->FilterSelection(selection, input, input.size(), count);
		REQUIRE(count == expected_count);
		for (idx_t i = 0; i < count; i++) {
			REQUIRE(selection.get_index(i) == expected.get_index(i));
		}
	};
	for (idx_t size : {idx_t(0), idx_t(1), idx_t(31), idx_t(STANDARD_VECTOR_SIZE), idx_t(2 * STANDARD_VECTOR_SIZE + 31),
	                   idx_t(31)}) {
		Vector source(type, size);
		if (strings) {
			auto writer = FlatVector::Writer<string_t>(source, size);
			for (idx_t i = 0; i < size; i++) {
				if (i % 7 == 0) {
					writer.WriteNull();
				} else {
					writer.WriteValue(std::to_string(i % 10));
				}
			}
		} else {
			auto writer = FlatVector::Writer<int32_t>(source, size);
			for (idx_t i = 0; i < size; i++) {
				if (i % 7 == 0) {
					writer.WriteNull();
				} else {
					writer.WriteValue(int32_t(i % 10));
				}
			}
		}
		for (idx_t representation = 0; representation < 4; representation++) {
			Vector input(Vector::Ref(source));
			if (representation == 1) {
				SelectionVector dictionary(size);
				for (idx_t i = 0; i < size; i++) {
					dictionary.set_index(i, (size - i - 1) / 2);
				}
				input.Slice(dictionary, size);
			} else if (representation >= 2) {
				Vector constant(representation == 2 ? value(4) : Value(type), count_t(size));
				input.Reference(constant);
			}
			for (bool sparse : {false, true}) {
				SelectionVector selected;
				idx_t count = size;
				if (sparse) {
					selected.Initialize(size);
					count = 0;
					for (idx_t i = 1; i < size; i += 3) {
						selected.set_index(count++, i);
					}
				}
				check(input, selected, count);
				check(input, selected, count);
				if (selected.IsSet() && count >= 4) {
					selected.ShiftLeft(count - 4, 4);
					count = 4;
					check(input, selected, count);
				}
			}
		}
	}
}

static unique_ptr<TableFilterSet> GroupTestStaticFilter(idx_t column, int32_t value) {
	auto result = make_uniq<TableFilterSet>();
	result->PushFilter(ProjectionIndex(column),
	                   ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, Value::INTEGER(value)));
	return result;
}

static bool EvaluateGroupTestFilter(ClientContext &context, const TableFilterSet &filters,
                                    const vector<Value> &values) {
	auto evaluate = [&](const ExpressionFilter &filter, const vector<ProjectionIndex> &columns) {
		auto expression = filter.expr->Copy();
		ExpressionIterator::VisitExpressionMutable<BoundReferenceExpression>(
		    expression, [&](BoundReferenceExpression &ref, unique_ptr<Expression> &owned) {
			    owned = make_uniq<BoundConstantExpression>(values.at(columns.at(ref.Index()).GetIndex()));
		    });
		auto result = ExpressionExecutor::EvaluateScalar(context, *expression);
		return !result.IsNull() && result.GetValue<bool>();
	};
	for (auto &entry : filters) {
		if (!evaluate(entry.Filter().Cast<ExpressionFilter>(), {entry.GetIndex()})) {
			return false;
		}
	}
	for (auto &filter : filters.GetMultiColumnFilters()) {
		auto &expression_filter = filter->Cast<ExpressionFilter>();
		if (!evaluate(expression_filter, expression_filter.column_indexes)) {
			return false;
		}
	}
	return true;
}

struct FilterGroupTest {
	FilterGroupTest()
	    : db(nullptr), con(db), plan(Allocator::Get(*con.context)),
	      first(plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1)),
	      second(plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1)),
	      scan(plan.Make<PhysicalTableScan>(
	                   vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                   BoundTableFunction(TableFunction("filter_group", vector<LogicalType> {}, nullptr)), nullptr,
	                   vector<LogicalType> {LogicalType::INTEGER, LogicalType::INTEGER},
	                   vector<ColumnIndex> {ColumnIndex(0), ColumnIndex(1)}, vector<idx_t> {0, 1},
	                   vector<string> {"a", "b"}, nullptr, 100, ExtraOperatorInfo(), vector<Value> {},
	                   virtual_column_map_t {})
	               .Cast<PhysicalTableScan>()) {
	}

	void Push(DynamicTableFilterSet &filters, const PhysicalOperator &producer, idx_t column, int32_t value) {
		filters.PushFilter(
		    producer, ProjectionIndex(column),
		    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, Value::INTEGER(value)));
	}

	DuckDB db;
	Connection con;
	PhysicalPlan plan;
	PhysicalOperator &first;
	PhysicalOperator &second;
	PhysicalTableScan &scan;
};

TEST_CASE("Runtime disjunctions require every alternative and retain immutable snapshots", "[dynamic_filter_groups]") {
	FilterGroupTest test;
	test.scan.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
	auto &root = *test.scan.dynamic_filters;
	auto alternatives = root.AddDisjunction(ProjectionIndex(0), 2);
	REQUIRE(root.GetDisjunctionFilters().size() == 2);
	test.Push(*alternatives[0], test.first, 0, 1);
	REQUIRE_FALSE(root.HasFilters());
	REQUIRE_FALSE(root.GetFinalTableFilters(test.scan, nullptr));
	test.Push(*alternatives[1], test.second, 0, 3);
	REQUIRE(root.HasFilters());
	auto snapshot = root.GetFinalTableFilters(test.scan, nullptr);
	REQUIRE(snapshot);
	for (int32_t value = 0; value <= 4; value++) {
		REQUIRE(EvaluateGroupTestFilter(*test.con.context, *snapshot, {Value::INTEGER(value)}) ==
		        (value == 1 || value == 3));
	}

	alternatives[0]->ClearFilters(test.first);
	alternatives[1]->ClearFilters(test.second);
	REQUIRE_FALSE(root.HasFilters());
	REQUIRE_FALSE(alternatives[0]->HasFilters());
	REQUIRE_FALSE(alternatives[1]->HasFilters());
	// The previous execution's value predicates are immutable snapshots.
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *snapshot, {Value::INTEGER(1)}));
	test.Push(*alternatives[0], test.first, 0, 2);
	REQUIRE_FALSE(root.GetFinalTableFilters(test.scan, nullptr));
	test.Push(*alternatives[1], test.second, 0, 4);
	auto next = root.GetFinalTableFilters(test.scan, nullptr);
	REQUIRE(next);
	REQUIRE_FALSE(EvaluateGroupTestFilter(*test.con.context, *next, {Value::INTEGER(1)}));
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *next, {Value::INTEGER(2)}));
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *next, {Value::INTEGER(4)}));
}

TEST_CASE("Incomplete runtime disjunctions preserve independent scan filters", "[dynamic_filter_groups]") {
	FilterGroupTest test;
	DynamicTableFilterSet root;
	auto alternatives = root.AddDisjunction(ProjectionIndex(0), 2);
	auto existing = GroupTestStaticFilter(1, 7);
	test.Push(*alternatives[0], test.first, 0, 1);
	auto partial = root.GetFinalTableFilters(test.scan, existing.get());
	REQUIRE(partial);
	REQUIRE_FALSE(partial->TryGetFilterByColumnIndex(ProjectionIndex(0)));
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *partial, {Value::INTEGER(2), Value::INTEGER(7)}));
	test.Push(*alternatives[1], test.second, 0, 3);
	auto complete = root.GetFinalTableFilters(test.scan, existing.get());
	REQUIRE(complete);
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *complete, {Value::INTEGER(3), Value::INTEGER(7)}));
	REQUIRE_FALSE(EvaluateGroupTestFilter(*test.con.context, *complete, {Value::INTEGER(3), Value::INTEGER(8)}));
	alternatives[1]->ClearFilters(test.second);
	REQUIRE_FALSE(root.HasFilters());
	REQUIRE(EvaluateGroupTestFilter(*test.con.context, *complete, {Value::INTEGER(3), Value::INTEGER(7)}));
}
