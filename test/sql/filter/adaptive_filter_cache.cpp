#include "catch.hpp"
#include "test_helpers.hpp"
#include "duckdb/common/vector/vector_writer.hpp"
#include "duckdb/planner/expression/bound_conjunction_expression.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/shared_ptr_ipp.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/planner/filter/expression_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/planner/table_filter_state.hpp"
#include "duckdb/storage/table/column_segment.hpp"

using namespace duckdb;

static unique_ptr<Expression> CacheFunction(ScalarFunction function, unique_ptr<FunctionData> data) {
	vector<unique_ptr<Expression>> arguments;
	arguments.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0));
	return make_uniq<BoundFunctionExpression>(BoundScalarFunction(function), std::move(arguments), std::move(data));
}

TEST_CASE("Published pure runtime-filter children support adaptive caches", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector keys(Value::BIGINT(3000), count_t(1));
	Vector hashes(LogicalType::HASH);
	VectorOperations::Hash(keys, hashes, 1);
	auto bloom = make_shared_ptr<BloomFilter>();
	bloom->Initialize(*con.context, 1);
	bloom->InsertHashes(hashes);
	bloom->Seal(*con.context);
	auto prefix = PrefixRangeFilter::CreatePrefixRangeFilter(LogicalType::BIGINT);
	prefix->Initialize(*con.context, 1, Value::BIGINT(3000), Value::BIGINT(3000), 1);
	auto build_state = prefix->InitializeBuildState(*con.context);
	prefix->InsertKeys(keys, *build_state);
	prefix->MergeBuildState(*build_state);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::BF, threshold, vectors);
	for (bool use_bloom : {false, true}) {
		for (bool published : {false, true}) {
			CAPTURE(use_bloom, published);
			if (published) {
				if (use_bloom) {
					bloom->MarkPublished();
				} else {
					prefix->MarkPublished();
				}
			}
			auto child =
			    use_bloom ? CacheFunction(BloomFilterScalarFun::GetFunction(LogicalType::BIGINT),
			                              make_uniq<BloomFilterFunctionData>(bloom, true, "k", LogicalType::BIGINT,
			                                                                 threshold, vectors))
			              : CacheFunction(PrefixRangeScalarFun::GetFunction(LogicalType::BIGINT),
			                              make_uniq<PrefixRangeFunctionData>(prefix.get(), true, "k",
			                                                                 LogicalType::BIGINT, threshold, vectors));
			ExpressionFilter independently_adaptive(child->Copy());
			REQUIRE_FALSE(TableFilterState::Initialize(*con.context, independently_adaptive)->can_cache);
			ExpressionFilter wrapped(
			    CreateSelectivityOptionalFilterExpression(std::move(child), LogicalType::BIGINT, threshold, vectors));
			auto state = TableFilterState::Initialize(*con.context, wrapped);
			REQUIRE(state->can_cache == published);
			if (published) {
				auto cache = state->CreateCache(keys, 1);
				SelectionVector codes, selected;
				idx_t count = 1;
				cache->Filter(codes, selected, count);
				REQUIRE(count == 1);
			}
		}
	}
}

TEST_CASE("Run-weighted cached selections match expanded rows", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	Vector dictionary(LogicalType::BIGINT, 3);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 3);
		writer.WriteValue(1);
		writer.WriteValue(2500);
		writer.WriteValue(3000);
	}
	for (int32_t order = 0; order < 3; order++) {
		auto lower =
		    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
		auto required =
		    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2000));
		auto optional =
		    CreateSelectivityOptionalFilterExpression(lower->expr->Copy(), LogicalType::BIGINT, threshold, vectors);
		unique_ptr<Expression> expression;
		if (order == 0) {
			expression = std::move(optional);
		} else {
			expression = make_uniq<BoundConjunctionExpression>(
			    ExpressionType::CONJUNCTION_AND, order == 1 ? required->expr->Copy() : std::move(optional),
			    order == 1 ? std::move(optional) : required->expr->Copy());
		}
		ExpressionFilter filter(std::move(expression));
		auto state = TableFilterState::Initialize(*con.context, filter);
		auto oracle = TableFilterState::Initialize(*con.context, filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		for (idx_t batch = 0; batch < 6 * vectors; batch++) {
			CAPTURE(order, batch);
			vector<idx_t> lengths {9, 1};
			SelectionVector run_codes(2), row_codes(10), selected_runs, expected_rows;
			const auto first = batch < vectors ? 2 : 0;
			run_codes.set_index(0, first);
			run_codes.set_index(1, 1);
			for (idx_t row = 0; row < 10; row++) {
				row_codes.set_index(row, row < lengths[0] ? first : 1);
			}
			idx_t run_count = lengths.size(), expected_count = 10;
			auto expected_cache = oracle->CreateCache(dictionary, dictionary.size());
			cache->FilterRuns(run_codes, array_ptr<const idx_t>(lengths.data(), lengths.size()), selected_runs,
			                  run_count);
			expected_cache->Filter(row_codes, expected_rows, expected_count);
			vector<idx_t> actual_rows;
			for (idx_t idx = 0; idx < run_count; idx++) {
				const auto run = selected_runs.get_index(idx);
				const auto offset = run == 0 ? 0 : lengths[0];
				for (idx_t row = 0; row < lengths[run]; row++) {
					actual_rows.push_back(offset + row);
				}
			}
			REQUIRE(actual_rows.size() == expected_count);
			for (idx_t row = 0; row < expected_count; row++) {
				REQUIRE(actual_rows[row] == expected_rows.get_index(row));
			}
			if (batch == vectors) {
				cache = state->CreateCache(dictionary, dictionary.size());
			}
		}
	}
}

TEST_CASE("Run cache accounting survives selections partial batches and row fallback", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	auto comparison =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
	ExpressionFilter filter(
	    CreateSelectivityOptionalFilterExpression(comparison->expr->Copy(), LogicalType::BIGINT, threshold, vectors));
	Vector dictionary(LogicalType::BIGINT, 2);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 2);
		writer.WriteValue(1);
		writer.WriteValue(3000);
	}
	for (const bool incoming : {false, true}) {
		auto state = TableFilterState::Initialize(*con.context, filter);
		auto oracle = TableFilterState::Initialize(*con.context, filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		for (idx_t batch = 0; batch < vectors * 6; batch++) {
			const idx_t count = STANDARD_VECTOR_SIZE - batch % 2;
			vector<idx_t> lengths {count - 1, 1};
			if (batch % 2) {
				std::swap(lengths[0], lengths[1]);
			}
			const idx_t first_code = batch < vectors ? 1 : 0;
			SelectionVector run_codes(2), codes(count), selected_runs, selected_rows, expected;
			run_codes.set_index(0, first_code);
			run_codes.set_index(1, 1 - first_code);
			Vector input(LogicalType::BIGINT, count);
			{
				auto writer = FlatVector::Writer<int64_t>(input, count);
				for (idx_t row = 0; row < count; row++) {
					const auto code = row < lengths[0] ? first_code : 1 - first_code;
					codes.set_index(row, code);
					writer.WriteValue(code == 0 ? 1 : 3000);
				}
			}
			idx_t run_count = 2, expected_count = count;
			if (incoming) {
				selected_runs.Initialize(1);
				selected_runs.set_index(0, 0);
				run_count = 1;
				expected_count = lengths[0];
				expected.Initialize(count);
				selected_rows.Initialize(count);
				for (idx_t row = 0; row < expected_count; row++) {
					expected.set_index(row, row);
					selected_rows.set_index(row, row);
				}
			}
			const auto before_count = expected_count;
			ColumnSegment::FilterSelection(expected, input, *oracle, count, expected_count);
			vector<idx_t> actual;
			if (batch % 2) {
				idx_t selected_count = before_count;
				ColumnSegment::FilterSelection(selected_rows, input, *state, count, selected_count);
				for (idx_t row = 0; row < selected_count; row++) {
					actual.push_back(selected_rows.get_index(row));
				}
			} else {
				cache->FilterRuns(run_codes, array_ptr<const idx_t>(lengths.data(), lengths.size()), selected_runs,
				                  run_count);
				for (idx_t i = 0; i < run_count; i++) {
					const auto run = selected_runs.get_index(i);
					const auto offset = run == 0 ? 0 : lengths[0];
					for (idx_t row = 0; row < lengths[run]; row++) {
						actual.push_back(offset + row);
					}
				}
			}
			CAPTURE(incoming, batch);
			REQUIRE(actual.size() == expected_count);
			for (idx_t row = 0; row < expected_count; row++) {
				REQUIRE(actual[row] == expected.get_index(row));
			}
			if (batch == vectors) {
				cache = state->CreateCache(dictionary, dictionary.size());
			}
		}
		auto replacement = TableFilterState::Initialize(*con.context, filter);
		REQUIRE_FALSE(cache->IsValidFor(*replacement));
		state.reset();
		REQUIRE_FALSE(cache->CanAcceptAll());
		REQUIRE_FALSE(cache->CanSkipAll());
#ifndef DUCKDB_CRASH_ON_ASSERT
		SelectionVector values, selected;
		idx_t count = 0;
		REQUIRE_THROWS_AS(cache->FilterRuns(values, array_ptr<const idx_t>(nullptr, 0), selected, count),
		                  InternalException);
#endif
	}
}

TEST_CASE("Empty mandatory masks preserve feedback from preceding optional filters", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	auto lower =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
	auto mandatory = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_LESSTHAN, Value::BIGINT(0));
	ExpressionFilter filter(make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND,
	    CreateSelectivityOptionalFilterExpression(lower->expr->Copy(), LogicalType::BIGINT, threshold, vectors),
	    mandatory->expr->Copy()));
	auto state = TableFilterState::Initialize(*con.context, filter);
	auto oracle = TableFilterState::Initialize(*con.context, filter);
	Vector dictionary(Value::BIGINT(3000), count_t(1));
	auto cache = state->CreateCache(dictionary, 1);
	SelectionVector codes(STANDARD_VECTOR_SIZE);
	for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
		codes.set_index(row, 0);
	}
	Vector input(Value::BIGINT(3000), count_t(STANDARD_VECTOR_SIZE));
	for (idx_t batch = 0; batch < vectors; batch++) {
		SelectionVector selected;
		idx_t count = STANDARD_VECTOR_SIZE;
		if (cache->CanSkipAll()) {
			count = 0;
		} else {
			cache->Filter(codes, selected, count);
		}
		REQUIRE(count == 0);
		SelectionVector expected;
		idx_t expected_count = STANDARD_VECTOR_SIZE;
		ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
		REQUIRE(expected_count == 0);
	}
	Vector tail(Value::BIGINT(-1), count_t(STANDARD_VECTOR_SIZE));
	cache = state->CreateCache(tail, 1);
	SelectionVector selected, expected;
	idx_t count = STANDARD_VECTOR_SIZE, expected_count = STANDARD_VECTOR_SIZE;
	cache->Filter(codes, selected, count);
	ColumnSegment::FilterSelection(expected, tail, *oracle, STANDARD_VECTOR_SIZE, expected_count);
	REQUIRE(count == expected_count);
	REQUIRE(count == STANDARD_VECTOR_SIZE);
}

TEST_CASE("Expired filter caches cannot prune pages", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto filter =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
	auto state = TableFilterState::Initialize(*con.context, *filter);
	Vector input(Value::BIGINT(1), count_t(1));
	auto cache = state->CreateCache(input, 1);
	REQUIRE(cache->CanSkipAll());
	state.reset();
	REQUIRE_FALSE(cache->CanSkipAll());
}

static unique_ptr<ExpressionFilter> AdaptiveCacheFilter(bool mandatory) {
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	auto lower =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
	auto upper =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_LESSTHANOREQUALTO, Value::BIGINT(3000));
	auto optional = make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND,
	    CreateSelectivityOptionalFilterExpression(lower->expr->Copy(), LogicalType::BIGINT, threshold, vectors),
	    CreateSelectivityOptionalFilterExpression(upper->expr->Copy(), LogicalType::BIGINT, threshold, vectors));
	if (!mandatory) {
		return make_uniq<ExpressionFilter>(std::move(optional));
	}
	auto required =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2000));
	return make_uniq<ExpressionFilter>(make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_AND, required->expr->Copy(), std::move(optional)));
}

TEST_CASE("Pure filter caches expose borrowed masks with root lifetime checks", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector dictionary(LogicalType::BIGINT, 4);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 4);
		writer.WriteValue(1);
		writer.WriteValue(2500);
		writer.WriteValue(3000);
		writer.WriteNull();
	}
	auto lower =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2000));
	auto upper =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_LESSTHANOREQUALTO, Value::BIGINT(2700));
	ExpressionFilter filter(make_uniq<BoundConjunctionExpression>(ExpressionType::CONJUNCTION_AND, lower->expr->Copy(),
	                                                              upper->expr->Copy()));
	auto state = TableFilterState::Initialize(*con.context, filter);
	auto cache = state->CreateCache(dictionary, dictionary.size());
	auto mask = cache->GetPureMask();
	REQUIRE(mask.has_value());
	REQUIRE(mask->size() == dictionary.size());
	REQUIRE_FALSE((*mask)[0]);
	REQUIRE((*mask)[1]);
	REQUIRE_FALSE((*mask)[2]);
	REQUIRE_FALSE((*mask)[3]);
	REQUIRE(mask->data() == cache->GetPureMask()->data());
	auto replacement = state->CreateCache(dictionary, dictionary.size());
	REQUIRE(cache->GetPureMask()->data() != replacement->GetPureMask()->data());
	state.reset();
	REQUIRE_FALSE(cache->CanAcceptAll());
	REQUIRE_FALSE(cache->CanSkipAll());
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS_AS(cache->GetPureMask(), InternalException);
	REQUIRE_THROWS_AS(replacement->GetPureMask(), InternalException);
#endif
}

TEST_CASE("Adaptive roots never expose a static pure mask", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector dictionary(Value::BIGINT(3000), count_t(1));
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	for (bool mandatory : {false, true}) {
		auto filter = AdaptiveCacheFilter(mandatory);
		auto state = TableFilterState::Initialize(*con.context, *filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		for (idx_t batch = 0; batch <= vectors; batch++) {
			if (batch == vectors) {
				REQUIRE(cache->CanAcceptAll());
			}
			REQUIRE_FALSE(cache->GetPureMask().has_value());
			idx_t count = STANDARD_VECTOR_SIZE;
			cache->FilterConstant(0, count);
		}
	}
}

TEST_CASE("Accept-all cache checks preserve adaptive recovery and mandatory masks", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	Vector dictionary(LogicalType::BIGINT, 2);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 2);
		writer.WriteValue(1);
		writer.WriteValue(3000);
	}
	SelectionVector codes(STANDARD_VECTOR_SIZE);
	for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
		codes.set_index(row, 1);
	}
	for (bool mandatory : {false, true}) {
		auto filter = AdaptiveCacheFilter(mandatory);
		auto state = TableFilterState::Initialize(*con.context, *filter);
		auto oracle = TableFilterState::Initialize(*con.context, *filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		REQUIRE_FALSE(cache->CanAcceptAll());
		Vector input(Value::BIGINT(3000), count_t(STANDARD_VECTOR_SIZE));
		for (idx_t batch = 0; batch < vectors; batch++) {
			SelectionVector selected, expected;
			idx_t count = STANDARD_VECTOR_SIZE, expected_count = count;
			cache->Filter(codes, selected, count);
			ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			REQUIRE(count == expected_count);
		}
		// The mandatory mask distinguishes 1 from 3000 even while optional predicates are paused.
		REQUIRE(cache->CanAcceptAll() == !mandatory);
		for (idx_t batch = 0; batch < 2 * vectors; batch++) {
			CAPTURE(mandatory, batch);
			const auto accepts_all = cache->CanAcceptAll();
			REQUIRE(cache->CanAcceptAll() == accepts_all);
			SelectionVector selected, expected, no_codes;
			idx_t count = STANDARD_VECTOR_SIZE, expected_count = count;
			Vector tail(Value::BIGINT(1), count_t(STANDARD_VECTOR_SIZE));
			cache->Filter(accepts_all ? no_codes : codes, selected, count);
			// The encoded input must correspond to whichever code path is being tested.
			ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			REQUIRE(count == expected_count);
			// Exercise rejection and recovery using the lower dictionary entry next.
			for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
				codes.set_index(row, 0);
			}
			count = expected_count = STANDARD_VECTOR_SIZE;
			selected = SelectionVector();
			expected = SelectionVector();
			const auto tail_accepts_all = cache->CanAcceptAll();
			cache->Filter(tail_accepts_all ? no_codes : codes, selected, count);
			ColumnSegment::FilterSelection(expected, tail, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			REQUIRE(count == expected_count);
			for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
				codes.set_index(row, 1);
			}
		}
		state.reset();
		REQUIRE_FALSE(cache->CanAcceptAll());
	}
}

TEST_CASE("Dense optional batches avoid selections while feedback measures the predicate", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	auto filter = AdaptiveCacheFilter(false);
	auto state = TableFilterState::Initialize(*con.context, *filter);
	auto oracle = TableFilterState::Initialize(*con.context, *filter);
	Vector dictionary(LogicalType::BIGINT, 2);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 2);
		writer.WriteValue(1);
		writer.WriteValue(3000);
	}
	auto cache = state->CreateCache(dictionary, dictionary.size());
	SelectionVector codes(STANDARD_VECTOR_SIZE);
	// These two batches distinguish predicate feedback from the number of rows returned after bypassing.
	const auto dense_count = idx_t(STANDARD_VECTOR_SIZE * double(threshold)) + 1;
	const auto selective_count = idx_t(STANDARD_VECTOR_SIZE * (2 * double(threshold) - 1)) + 1;
	if (dense_count > STANDARD_VECTOR_SIZE || selective_count >= dense_count ||
	    double(dense_count + selective_count) / (2 * STANDARD_VECTOR_SIZE) >= threshold) {
		// Small vectors cannot contain this near-full counterexample.
		return;
	}
	REQUIRE(double(dense_count) / STANDARD_VECTOR_SIZE >= threshold);
	REQUIRE(double(selective_count) / STANDARD_VECTOR_SIZE < threshold);
	REQUIRE(double(dense_count + selective_count) / (2 * STANDARD_VECTOR_SIZE) < threshold);
	REQUIRE(double(STANDARD_VECTOR_SIZE + selective_count) / (2 * STANDARD_VECTOR_SIZE) >= threshold);
	for (idx_t batch = 0; batch < vectors; batch++) {
		const auto accepted = batch % 2 == 0 ? dense_count : selective_count;
		Vector input(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
		{
			auto writer = FlatVector::Writer<int64_t>(input, STANDARD_VECTOR_SIZE);
			for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
				codes.set_index(row, row < accepted ? 1 : 0);
				writer.WriteValue(row < accepted ? 3000 : 1);
			}
		}
		SelectionVector selected, expected;
		idx_t count = STANDARD_VECTOR_SIZE, expected_count = count;
		cache->Filter(codes, selected, count);
		ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
		REQUIRE(count == (batch % 2 == 0 ? STANDARD_VECTOR_SIZE : selective_count));
		REQUIRE(count == expected_count);
		if (batch % 2 == 0) {
			REQUIRE_FALSE(selected.IsSet());
			REQUIRE_FALSE(expected.IsSet());
		}
	}
	REQUIRE_FALSE(cache->CanAcceptAll());
	for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
		codes.set_index(row, row == 0 ? 0 : 1);
	}
	state = TableFilterState::Initialize(*con.context, *filter);
	cache = state->CreateCache(dictionary, dictionary.size());
	SelectionVector selected;
	idx_t prefix_count = STANDARD_VECTOR_SIZE - 1;
	if (double(prefix_count - 1) / prefix_count < threshold) {
		return;
	}
	cache->Filter(codes, selected, prefix_count);
	REQUIRE(prefix_count == STANDARD_VECTOR_SIZE - 1);
	REQUIRE_FALSE(selected.IsSet());
}

TEST_CASE("Dense optional selection bypass preserves mandatory filters in either order", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	if (STANDARD_VECTOR_SIZE <= 2 || double(STANDARD_VECTOR_SIZE - 2) / STANDARD_VECTOR_SIZE < threshold) {
		return;
	}
	auto lower =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2700));
	auto required =
	    ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_GREATERTHANOREQUALTO, Value::BIGINT(2000));
	auto optional =
	    CreateSelectivityOptionalFilterExpression(lower->expr->Copy(), LogicalType::BIGINT, threshold, vectors);
	Vector dictionary(LogicalType::BIGINT, 3), input(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 3);
		writer.WriteValue(1);
		writer.WriteValue(2500);
		writer.WriteValue(3000);
	}
	SelectionVector codes(STANDARD_VECTOR_SIZE);
	{
		auto writer = FlatVector::Writer<int64_t>(input, STANDARD_VECTOR_SIZE);
		for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
			codes.set_index(row, row < 2 ? row : 2);
			writer.WriteValue(row == 0 ? 1 : (row == 1 ? 2500 : 3000));
		}
	}
	for (bool mandatory_first : {false, true}) {
		ExpressionFilter filter(make_uniq<BoundConjunctionExpression>(
		    ExpressionType::CONJUNCTION_AND, mandatory_first ? required->expr->Copy() : optional->Copy(),
		    mandatory_first ? optional->Copy() : required->expr->Copy()));
		auto state = TableFilterState::Initialize(*con.context, filter);
		auto oracle = TableFilterState::Initialize(*con.context, filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		for (idx_t batch = 0; batch <= vectors; batch++) {
			CAPTURE(mandatory_first, batch);
			SelectionVector selected, expected;
			idx_t count = STANDARD_VECTOR_SIZE, expected_count = count;
			cache->Filter(codes, selected, count);
			ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			const auto rejected = mandatory_first && batch < vectors ? 2 : 1;
			REQUIRE(count == STANDARD_VECTOR_SIZE - rejected);
			REQUIRE(count == expected_count);
			for (idx_t row = 0; row < count; row++) {
				REQUIRE(selected.get_index(row) == expected.get_index(row));
				REQUIRE(selected.get_index(row) != 0);
			}
		}
	}
}

TEST_CASE("Filter cache capabilities reject mutable, throwing, and unsupported adaptive expressions",
          "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto dynamic_data = make_shared_ptr<DynamicFilterData>(ExpressionType::COMPARE_GREATERTHAN, Value::BIGINT(1));
	ExpressionFilter dynamic_filter(CreateDynamicFilterExpression(dynamic_data, LogicalType::BIGINT));
	REQUIRE_FALSE(TableFilterState::Initialize(*con.context, dynamic_filter)->can_cache);

	auto adaptive = AdaptiveCacheFilter(false);
	auto comparison = ExpressionFilter::CreateComparisonFilter(ExpressionType::COMPARE_EQUAL, Value::BIGINT(1));
	ExpressionFilter disjunction(make_uniq<BoundConjunctionExpression>(
	    ExpressionType::CONJUNCTION_OR, adaptive->expr->Copy(), comparison->expr->Copy()));
	auto disjunction_state = TableFilterState::Initialize(*con.context, disjunction);
	REQUIRE_FALSE(disjunction_state->Cast<ExpressionFilterState>().can_cache_result);
	REQUIRE_FALSE(disjunction_state->can_cache);

	float threshold;
	idx_t vectors;
	GetThresholdAndVectorsToCheck(SelectivityOptionalFilterType::MIN_MAX, threshold, vectors);
	auto throwing = BoundCastExpression::AddCastToType(
	    *con.context, make_uniq<BoundReferenceExpression>(LogicalType::VARCHAR, 0), LogicalType::BOOLEAN);
	REQUIRE(throwing->CanThrow());
	ExpressionFilter hidden_throw(
	    CreateSelectivityOptionalFilterExpression(std::move(throwing), LogicalType::VARCHAR, threshold, vectors));
	REQUIRE_FALSE(TableFilterState::Initialize(*con.context, hidden_throw)->can_cache);
}

TEST_CASE("Adaptive predicate caches share row feedback with fallback and preserve mandatory filters",
          "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	Vector dictionary(LogicalType::BIGINT, 6);
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, 6);
		for (auto value : {int64_t(1), int64_t(2500), int64_t(2900), int64_t(3000), int64_t(3100)}) {
			writer.WriteValue(value);
		}
		writer.WriteNull();
	}
	for (const bool mandatory : {false, true}) {
		auto filter = AdaptiveCacheFilter(mandatory);
		auto state = TableFilterState::Initialize(*con.context, *filter);
		auto oracle = TableFilterState::Initialize(*con.context, *filter);
		REQUIRE(state->can_cache);
		REQUIRE_FALSE(state->Cast<ExpressionFilterState>().can_cache_result);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		REQUIRE(cache->IsValidFor(*state));
		for (idx_t batch = 0; batch < 60; batch++) {
			CAPTURE(mandatory, batch);
			Vector input(LogicalType::BIGINT, STANDARD_VECTOR_SIZE);
			SelectionVector codes(STANDARD_VECTOR_SIZE), incoming(STANDARD_VECTOR_SIZE);
			idx_t count = 0;
			{
				auto writer = FlatVector::Writer<int64_t>(input, STANDARD_VECTOR_SIZE);
				auto values = dictionary.Values<int64_t>();
				for (idx_t row = 0; row < STANDARD_VECTOR_SIZE; row++) {
					const auto code = batch < 6 ? 3 : (batch + row) % dictionary.size();
					codes.set_index(row, code);
					const auto value = values[code];
					if (value.IsValid()) {
						writer.WriteValue(value.GetValue());
					} else {
						writer.WriteNull();
					}
					if (batch % 11 != 10 && (batch < 6 || row % 3 != 0)) {
						incoming.set_index(count++, row);
					}
				}
			}
			SelectionVector selected, expected;
			if (count != STANDARD_VECTOR_SIZE) {
				selected.Initialize(incoming);
				expected.Initialize(incoming);
			}
			auto actual_count = count;
			auto expected_count = count;
			if (batch % 7 == 0) {
				ColumnSegment::FilterSelection(selected, input, *state, STANDARD_VECTOR_SIZE, actual_count);
			} else {
				cache->Filter(codes, selected, actual_count);
			}
			ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			REQUIRE(actual_count == expected_count);
			for (idx_t row = 0; row < actual_count; row++) {
				REQUIRE(selected.get_index(row) == expected.get_index(row));
			}
			if (batch == 5 || batch == 6 || batch == 10) {
				cache = state->CreateCache(dictionary, dictionary.size());
			}
		}
		auto replacement = TableFilterState::Initialize(*con.context, *filter);
		REQUIRE_FALSE(cache->IsValidFor(*replacement));
		state.reset();
		REQUIRE_FALSE(cache->CanAcceptAll());
		REQUIRE_FALSE(cache->CanSkipAll());
#ifndef DUCKDB_CRASH_ON_ASSERT
		SelectionVector codes;
		SelectionVector selected;
		idx_t count = 1;
		REQUIRE_THROWS_AS(cache->Filter(codes, selected, count), InternalException);
#endif
	}
}

TEST_CASE("Constant cached batches preserve adaptive feedback and mandatory predicates", "[filter][adaptive_cache]") {
	DuckDB db(nullptr);
	Connection con(db);
	vector<Value> keys {Value::BIGINT(1),    Value::BIGINT(2500), Value::BIGINT(2900),
	                    Value::BIGINT(3000), Value::BIGINT(3100), Value(LogicalType::BIGINT)};
	Vector dictionary(LogicalType::BIGINT, keys.size());
	{
		auto writer = FlatVector::Writer<int64_t>(dictionary, keys.size());
		for (const auto &key : keys) {
			if (key.IsNull()) {
				writer.WriteNull();
			} else {
				writer.WriteValue(key.GetValue<int64_t>());
			}
		}
	}
	for (bool mandatory : {false, true}) {
		auto filter = AdaptiveCacheFilter(mandatory);
		auto state = TableFilterState::Initialize(*con.context, *filter);
		auto oracle = TableFilterState::Initialize(*con.context, *filter);
		auto cache = state->CreateCache(dictionary, dictionary.size());
		for (idx_t batch = 0; batch < 60; batch++) {
			CAPTURE(mandatory, batch);
			const auto code = batch < 6 ? 3 : batch % keys.size();
			Vector input(keys[code], count_t(STANDARD_VECTOR_SIZE));
			idx_t count = batch > 6 && batch % 11 == 0 ? 0 : STANDARD_VECTOR_SIZE;
			idx_t expected_count = count;
			SelectionVector expected;
			ColumnSegment::FilterSelection(expected, input, *oracle, STANDARD_VECTOR_SIZE, expected_count);
			cache->FilterConstant(code, count);
			REQUIRE(count == expected_count);
			if (batch == 5 || batch == 6) {
				cache = state->CreateCache(dictionary, dictionary.size());
			}
		}
		state.reset();
		REQUIRE_FALSE(cache->CanAcceptAll());
		REQUIRE_FALSE(cache->CanSkipAll());
#ifndef DUCKDB_CRASH_ON_ASSERT
		idx_t count = STANDARD_VECTOR_SIZE;
		REQUIRE_THROWS_AS(cache->FilterConstant(0, count), InternalException);
#endif
	}
}
