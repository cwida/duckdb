#include "catch.hpp"
#include "test_helpers.hpp"

#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/planner/expression/bound_reference_expression.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"

using namespace duckdb;

TEST_CASE("Bloom filter selection preserves NULL policy and mapped rows", "[filter][bloom_filter]") {
	DuckDB db(nullptr);
	Connection con(db);
	auto &context = *con.context;

	Vector build_keys(LogicalType::BIGINT, 2);
	build_keys.SetValue(0, Value(LogicalType::BIGINT));
	build_keys.SetValue(1, Value::BIGINT(11));
	FlatVector::SetSize(build_keys, count_t(2));
	Vector hashes(LogicalType::HASH, 2);
	VectorOperations::Hash(build_keys, hashes, 2);
	const vector<Value> values = {Value::BIGINT(11), Value(LogicalType::BIGINT), Value::BIGINT(22),
	                              Value::BIGINT(11), Value(LogicalType::BIGINT), Value::BIGINT(22),
	                              Value::BIGINT(11), Value::BIGINT(22)};
	const auto count = values.size();
	Vector keys(LogicalType::BIGINT, count);
	for (idx_t i = 0; i < count; i++) {
		keys.SetValue(i, values[i]);
	}
	FlatVector::SetSize(keys, count_t(count));
	SelectionVector dictionary_sel(count);
	SelectionVector input_sel(count);
	for (idx_t i = 0; i < count; i++) {
		dictionary_sel.set_index(i, (3 * i + 1) % count);
		input_sel.set_index(i, count - i - 1);
	}

	for (bool contains_null_hash : {false, true}) {
		auto filter = make_shared_ptr<BloomFilter>();
		filter->Initialize(context, 2);
		SelectionVector build_sel(2);
		build_sel.set_index(0, 1);
		build_sel.set_index(1, 0);
		// With the NULL hash present, only the explicit NULL policy can reject it.
		filter->InsertHashes(hashes, build_sel, contains_null_hash ? 2 : 1);
		filter->Seal(context);
		REQUIRE(filter->LookupOne(hashes.GetValue(0).GetValue<hash_t>()) == contains_null_hash);
		REQUIRE_FALSE(filter->LookupOne(Hash(int64_t(22))));

		for (bool filters_null_values : {false, true}) {
			vector<unique_ptr<Expression>> children;
			children.push_back(make_uniq<BoundReferenceExpression>(LogicalType::BIGINT, 0ULL));
			BoundFunctionExpression expr(
			    BoundScalarFunction(BloomFilterScalarFun::GetFunction(LogicalType::BIGINT)), std::move(children),
			    make_uniq<BloomFilterFunctionData>(filter, filters_null_values, "k", LogicalType::BIGINT, 0.0f, 0ULL));
			ExpressionExecutor executor(context, expr);

			for (bool dictionary : {false, true}) {
				DataChunk input;
				input.Initialize(context, {LogicalType::BIGINT}, count);
				if (dictionary) {
					input.data[0].Slice(keys, dictionary_sel, count);
				} else {
					input.data[0].Reference(keys);
				}
				input.CheckCardinality(count);

				for (bool mapped : {false, true}) {
					CAPTURE(contains_null_hash, filters_null_values, dictionary, mapped);
					vector<idx_t> expected_true, expected_false;
					for (idx_t i = 0; i < count; i++) {
						const auto row = mapped ? input_sel.get_index(i) : i;
						const auto value = input.GetValue(0, row);
						const auto passed = value.IsNull() ? !filters_null_values : value.GetValue<int64_t>() == 11;
						(passed ? expected_true : expected_false).push_back(row);
					}
					SelectionVector true_sel(count), false_sel(count);
					const auto true_count =
					    executor.SelectExpression(input, &true_sel, &false_sel, mapped ? &input_sel : nullptr, count);
					REQUIRE(true_count == expected_true.size());
					for (idx_t i = 0; i < true_count; i++) {
						REQUIRE(true_sel.get_index(i) == expected_true[i]);
					}
					for (idx_t i = 0; i < count - true_count; i++) {
						REQUIRE(false_sel.get_index(i) == expected_false[i]);
					}
				}
			}
		}
	}
}
