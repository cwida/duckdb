#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/execution/join_hashtable.hpp"
#include "duckdb/execution/operator/aggregate/ungrouped_aggregate_state.hpp"
#include "duckdb/execution/operator/join/physical_hash_join.hpp"
#include "duckdb/execution/operator/join/join_filter_pushdown.hpp"
#include "duckdb/execution/operator/scan/physical_dummy_scan.hpp"
#include "duckdb/execution/operator/scan/physical_table_scan.hpp"
#include "duckdb/execution/physical_plan_generator.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression_iterator.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/operator/logical_comparison_join.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

using namespace duckdb;

TEST_CASE("Disjunctive scan filters preserve selection, validity and repeated execution", "[dynamic_filter_groups]") {
	DuckDB db(nullptr);
	Connection con(db);
	const auto strings = GENERATE(false, true);
	const auto optional = GENERATE(false, true);
	const auto nested = GENERATE(false, true);
	const auto in_filter = GENERATE(false, true);
	const auto type = strings ? LogicalType::VARCHAR : LogicalType::INTEGER;
	auto value = [&](int32_t number) {
		return strings ? Value(std::to_string(number)) : Value::INTEGER(number);
	};
	auto comparison = [&](ExpressionType comparison_type, int32_t number) {
		return ExpressionFilter::CreateComparisonFilter(comparison_type, value(number))->expr->Copy();
	};
	auto disjunction = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
	for (int32_t lower : {2, 4}) {
		if (in_filter) {
			disjunction->GetChildrenMutable().push_back(ExpressionFilter::CreateInExpression(
			    make_uniq<BoundReferenceExpression>(type, 0),
			    vector<Value> {value(lower), value(lower + 2), value(lower + 4), Value(type)}));
			continue;
		}
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
	explicit FilterGroupTest(const LogicalType &type = LogicalType::INTEGER)
	    : db(nullptr), con(db), plan(Allocator::Get(*con.context)),
	      first(plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1)),
	      second(plan.Make<PhysicalDummyScan>(vector<LogicalType> {}, 1)),
	      scan(plan.Make<PhysicalTableScan>(
	                   vector<LogicalType> {type, type},
	                   BoundTableFunction(TableFunction("filter_group", vector<LogicalType> {}, nullptr)), nullptr,
	                   vector<LogicalType> {type, type}, vector<ColumnIndex> {ColumnIndex(0), ColumnIndex(1)},
	                   vector<idx_t> {0, 1}, vector<string> {"a", "b"}, nullptr, 100, ExtraOperatorInfo(),
	                   vector<Value> {}, virtual_column_map_t {})
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

TEST_CASE("Runtime IN row policy follows each target's registered PRF", "[dynamic_filter_groups]") {
	const auto strings = GENERATE(false, true);
	const auto register_prf = GENERATE(false, true);
	const auto build_count = GENERATE(idx_t(2), MinValue<idx_t>(102, STANDARD_VECTOR_SIZE));
	const auto type = strings ? LogicalType::VARCHAR : LogicalType::INTEGER;
	if (strings && !PrefixRangeFilter::SupportedType(type)) {
		return;
	}
	FilterGroupTest test(type);
	auto &context = *test.con.context;
	auto &allocator = Allocator::Get(context);
	const vector<Value> values = strings ? vector<Value> {Value("same-a"), Value("same-z")}
	                                     : vector<Value> {Value::INTEGER(2), Value::INTEGER(8)};
	auto &probe = test.plan.Make<PhysicalDummyScan>(vector<LogicalType> {type}, 1);
	auto &build = test.plan.Make<PhysicalDummyScan>(vector<LogicalType> {type}, build_count);
	LogicalComparisonJoin logical(JoinType::SEMI);
	logical.types = {type};
	vector<JoinCondition> conditions;
	conditions.emplace_back(make_uniq<BoundReferenceExpression>(type, 0), make_uniq<BoundReferenceExpression>(type, 0),
	                        ExpressionType::COMPARE_EQUAL);
	auto &join = test.plan.Make<PhysicalHashJoin>(logical, probe, build, std::move(conditions), JoinType::SEMI, 2)
	                 .Cast<PhysicalHashJoin>();
	auto ht = join.InitializeHashTable(context, 0);
	auto layout = make_shared_ptr<TupleDataLayout>();
	REQUIRE(ht->build_types.empty());
	layout->Initialize(vector<LogicalType> {type, LogicalType::HASH}, TupleDataValidityType::CAN_HAVE_NULL_VALUES);
	ht->FinishInitWithLayout(std::move(layout));
	DataChunk keys;
	keys.Initialize(allocator, {type});
	keys.SetChildCardinality(build_count);
	if (strings) {
		auto writer = FlatVector::Writer<string_t>(keys.data[0], build_count);
		for (idx_t i = 0; i < build_count; i++) {
			writer.WriteValue(StringValue::Get(values[i + 1 == build_count ? 1 : 0]));
		}
	} else {
		auto writer = FlatVector::Writer<int32_t>(keys.data[0], build_count);
		for (idx_t i = 0; i < build_count; i++) {
			writer.WriteValue(values[i + 1 == build_count ? 1 : 0].GetValue<int32_t>());
		}
	}
	DataChunk payload;
	payload.Initialize(allocator, {});
	payload.SetChildCardinality(build_count);
	PartitionedTupleDataAppendState append_state;
	ht->GetSinkCollection().InitializeAppendState(append_state);
	ht->Build(append_state, keys, payload);
	ht->GetSinkCollection().FlushAppendState(append_state);
	ht->Unpartition();
	REQUIRE(ht->Count() == build_count);

	JoinFilterPushdownInfo pushdown;
	pushdown.join_condition = {0};
	pushdown.build_side_has_filter = true;
	for (idx_t target = 0; target < 4; target++) {
		JoinFilterPushdownColumn column;
		column.join_filter_idx = 0;
		column.probe_column_index = ColumnBinding(TableIndex(target), ProjectionIndex(0));
		column.storage_type = type;
		if (target == 2) {
			column.mode = JoinFilterPushdownMode::STORAGE_ONLY;
		}
		JoinFilterPushdownFilter info;
		info.dynamic_filters = make_shared_ptr<DynamicTableFilterSet>();
		info.columns.push_back(std::move(column));
		// The second target fails PRF's ratio gate even if the first target registered it.
		info.estimated_scan_cardinality = (target == 0 || target == 3) && register_prf ? 10000 : 1;
		pushdown.probe_info.push_back(std::move(info));
	}
	auto min_max = make_uniq<DataChunk>();
	min_max->Initialize(allocator, {type, type});
	min_max->SetChildCardinality(1);
	min_max->data[0].SetValue(0, values[0]);
	min_max->data[1].SetValue(0, values[1]);
	JoinFilterGlobalState global_state;
	pushdown.FinalizeFilters(context, join, std::move(min_max), *ht, false, true, global_state);
	REQUIRE(global_state.deferred_runtime_filters.size() == (register_prf ? 2 : 0));
	if (register_prf) {
		REQUIRE(ht->GetPrefixRangeFilter());
		REQUIRE(ht->GetPrefixRangeFilter()->IsExact() == !strings);
		REQUIRE_FALSE(ht->GetPrefixRangeFilter()->IsInitialized());
	} else {
		REQUIRE_FALSE(ht->GetPrefixRangeFilter());
	}
	for (idx_t target = 0; target < pushdown.probe_info.size(); target++) {
		auto snapshot = pushdown.probe_info[target].dynamic_filters->GetFinalTableFilters(test.scan, nullptr);
		REQUIRE(snapshot);
		auto filter = snapshot->TryGetFilterByColumnIndex(ProjectionIndex(0));
		REQUIRE(filter);
		const auto pruning_only = target == 2 || ((target == 0 || target == 3) && register_prf && !strings);
		REQUIRE(ExpressionFilter::IsRootNonSelectivityOptionalFilter(*filter) == pruning_only);
		auto &expression = *filter->Cast<ExpressionFilter>().expr;
		auto child = ExpressionFilter::GetOptionalFilterChild(expression);
		REQUIRE(child);
		REQUIRE(child->GetExpressionType() == ExpressionType::COMPARE_IN);

		ExpressionFilterState state(context, expression);
		REQUIRE(state.fast_executor);
		Vector input(type, 3);
		FlatVector::SetSize(input, count_t(3));
		input.SetValue(0, values[0]);
		input.SetValue(1, strings ? Value("same-b") : Value::INTEGER(5));
		input.SetValue(2, values[1]);
		SelectionVector selected;
		idx_t count = input.size();
		state.fast_executor->FilterSelection(selected, input, input.size(), count);
		REQUIRE(count == (pruning_only ? 3 : 2));
		if (!pruning_only) {
			REQUIRE(selected.get_index(0) == 0);
			REQUIRE(selected.get_index(1) == 2);
		}
	}
}

TEST_CASE("IN and other runtime filters from separate producers retain fast execution", "[dynamic_filter_groups]") {
	FilterGroupTest test;
	const auto bloom = GENERATE(false, true);
	auto type = LogicalType::INTEGER;
	auto filter = ExpressionFilter::CreateInExpression(make_uniq<BoundReferenceExpression>(type, 0),
	                                                   vector<Value> {Value::INTEGER(2), Value::INTEGER(8)});
	DynamicTableFilterSet filters;
	filters.PushFilter(
	    test.first, ProjectionIndex(0),
	    make_uniq<ExpressionFilter>(CreateSelectivityOptionalFilterExpression(std::move(filter), type, 0.5f, 6)));

	Vector keys(type, 2);
	{
		auto writer = FlatVector::Writer<int32_t>(keys, 2);
		writer.WriteValue(bloom ? 2 : 5);
		writer.WriteValue(8);
	}
	unique_ptr<Expression> other;
	unique_ptr<PrefixRangeFilter> prf;
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(make_uniq<BoundReferenceExpression>(type, 0));
	if (bloom) {
		auto bf = make_shared_ptr<BloomFilter>();
		bf->Initialize(*test.con.context, keys.size());
		Vector hashes(LogicalType::HASH, keys.size());
		VectorOperations::Hash(keys, hashes);
		bf->InsertHashes(hashes);
		bf->Seal(*test.con.context);
		bf->MarkPublished();
		other = make_uniq<BoundFunctionExpression>(BoundScalarFunction(BloomFilterScalarFun::GetFunction(type)),
		                                           std::move(arguments),
		                                           make_uniq<BloomFilterFunctionData>(bf, true, "a", type, 0.5f, 6));
	} else {
		prf = PrefixRangeFilter::CreatePrefixRangeFilter(type);
		prf->Initialize(*test.con.context, keys.size(), Value::INTEGER(5), Value::INTEGER(8), 64);
		auto state = prf->InitializeBuildState(*test.con.context);
		prf->InsertKeys(keys, *state);
		prf->MergeBuildState(*state);
		other = make_uniq<BoundFunctionExpression>(BoundScalarFunction(PrefixRangeScalarFun::GetFunction(type)),
		                                           std::move(arguments),
		                                           make_uniq<PrefixRangeFunctionData>(prf, true, "a", type, 0.5f, 6));
	}
	filters.PushFilter(
	    test.second, ProjectionIndex(0),
	    make_uniq<ExpressionFilter>(CreateSelectivityOptionalFilterExpression(std::move(other), type, 0.5f, 6)));
	auto combined = filters.GetFinalTableFilters(test.scan, nullptr);
	REQUIRE(combined);
	auto column = combined->TryGetFilterByColumnIndex(ProjectionIndex(0));
	REQUIRE(column);
	auto &expression = *column->Cast<ExpressionFilter>().expr;
	ExpressionFilterState state(*test.con.context, expression);
	REQUIRE(state.fast_executor);
	Vector input(type, 10);
	{
		auto writer = FlatVector::Writer<int32_t>(input, 10);
		for (int32_t i = 0; i < 10; i++) {
			writer.WriteValue(i);
		}
	}
	SelectionVector selected;
	idx_t count = input.size();
	state.fast_executor->FilterSelection(selected, input, input.size(), count);
	REQUIRE(count == (bloom ? 2 : 1));
	REQUIRE(selected.get_index(0) == (bloom ? 2 : 8));
	if (bloom) {
		REQUIRE(selected.get_index(1) == 8);
	}
}

TEST_CASE("Reconstructed IN consumers retain the pruning fallback", "[dynamic_filter_groups]") {
	const auto cast = GENERATE(false, true);
	auto type = LogicalType::INTEGER;
	unique_ptr<Expression> input = make_uniq<BoundReferenceExpression>(type, 0);
	if (cast) {
		input = BoundCastExpression::AddDefaultCastToType(std::move(input), LogicalType::BIGINT);
	}
	auto input_type = input->GetReturnType();
	auto filter = ExpressionFilter::CreateInExpression(
	    std::move(input),
	    vector<Value> {Value::INTEGER(2).DefaultCastAs(input_type), Value::INTEGER(8).DefaultCastAs(input_type)});
	if (!cast) {
		auto retained_nulls = make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_OR);
		retained_nulls->GetChildrenMutable().push_back(std::move(filter));
		retained_nulls->GetChildrenMutable().push_back(ExpressionFilter::CreateNullCheckExpression(
		    make_uniq<BoundReferenceExpression>(type, 0), ExpressionType::OPERATOR_IS_NULL));
		filter = std::move(retained_nulls);
	}
	auto row_filter = CreateSelectivityOptionalFilterExpression(filter->Copy(), type, 0.5f, 6);
	REQUIRE_FALSE(ExpressionFilterExecutor::TryCreate(*row_filter));
	auto pruning_filter = CreateOptionalFilterExpression(std::move(filter), type);
	REQUIRE(ExpressionFilterExecutor::TryCreate(*pruning_filter));
}

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
