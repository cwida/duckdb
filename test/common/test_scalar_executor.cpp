#include "catch.hpp"
#include "duckdb/common/operator/comparison_operators.hpp"
#include "duckdb/common/types/string_heap.hpp"
#include "duckdb/common/vector/flat_vector.hpp"
#include "duckdb/common/vector_operations/binary_executor.hpp"
#include "duckdb/common/vector_operations/ternary_executor.hpp"
#include "duckdb/common/vector_operations/unary_executor.hpp"
#include "duckdb/common/vector_operations/variadic_executor.hpp"

using namespace duckdb; // NOLINT

namespace {

static Vector MakeFlatVector(idx_t count, int64_t offset = 0) {
	Vector result(LogicalType::BIGINT, count);
	auto data = FlatVector::GetDataMutable<int64_t>(result);
	for (idx_t i = 0; i < count; i++) {
		data[i] = NumericCast<int64_t>(i) + offset;
	}
	FlatVector::SetSize(result, count);
	return result;
}

static Vector MakeConstantVector(idx_t count, int64_t value) {
	return Vector(Value::BIGINT(value), count_t(count));
}

static Vector MakeNullConstantVector(idx_t count) {
	return Vector(Value(LogicalType::BIGINT), count_t(count));
}

static void RequireValue(const Vector &vector, idx_t row, int64_t expected) {
	auto value = vector.GetValue(row);
	REQUIRE(!value.IsNull());
	REQUIRE(value.GetValue<int64_t>() == expected);
}

static void RequireNull(const Vector &vector, idx_t row) {
	REQUIRE(vector.GetValue(row).IsNull());
}

struct UnaryAddOne {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		return input + 1;
	}
};

struct UnaryGenericAdd {
	template <class INPUT_TYPE, class RESULT_TYPE, class DATA_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, ValidityMask &mask, idx_t row, DATA_TYPE &state) {
		if (input == state.null_value) {
			mask.SetInvalid(row);
			return RESULT_TYPE();
		}
		return input + state.offset;
	}
};

struct UnaryGenericState {
	int64_t offset;
	int64_t null_value;
};

struct UnaryStringCopy {
	template <class INPUT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input, StringHeap &heap) {
		return heap.AddString(input);
	}
};

struct BinaryAdd {
	template <class INPUT_TYPE>
	static INPUT_TYPE Operation(INPUT_TYPE left, INPUT_TYPE right) {
		return left + right;
	}
};

struct BinaryTypedAdd {
	template <class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(LEFT_TYPE left, RIGHT_TYPE right) {
		return left + right;
	}
};

struct RuntimeNullBinaryWrapper {
	template <class FUNC, class OP, class LEFT_TYPE, class RIGHT_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(FUNC &fun, LEFT_TYPE left, RIGHT_TYPE right, ValidityMask &mask, idx_t row) {
		if (right == 2) {
			mask.SetInvalid(row);
			return RESULT_TYPE();
		}
		return left + right;
	}

	static bool AddsNulls() {
		return true;
	}
};

struct TernaryAdd {
	template <class A_TYPE, class B_TYPE, class C_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(A_TYPE a, B_TYPE b, C_TYPE c) {
		return a + b + c;
	}
};

struct TernaryLessThanSum {
	template <class A_TYPE, class B_TYPE, class C_TYPE>
	static bool Operation(A_TYPE a, B_TYPE b, C_TYPE c) {
		return a < b + c;
	}
};

struct QuaternaryAdd {
	template <class A_TYPE, class B_TYPE, class C_TYPE, class D_TYPE, class RESULT_TYPE>
	static RESULT_TYPE Operation(A_TYPE a, B_TYPE b, C_TYPE c, D_TYPE d) {
		return a + b + c + d;
	}
};

static bool Contains(const SelectionVector &sel, idx_t count, idx_t value) {
	for (idx_t i = 0; i < count; i++) {
		if (sel.get_index(i) == value) {
			return true;
		}
	}
	return false;
}

} // namespace

TEST_CASE("Boolean selection compacts bounded stable output", "[selection][scalar_executor]") {
	constexpr sel_t SENTINEL = NumericLimits<sel_t>::Maximum();
	for (idx_t count : {0, 1, 7, 8, 9, 63, 64, 65, 127, 129}) {
		for (idx_t pattern = 0; pattern < 258; pattern++) {
			auto matches = make_unsafe_uniq_array<bool>(count);
			duckdb::vector<sel_t> expected, expected_false;
			for (idx_t i = 0; i < count; i++) {
				matches[i] = pattern >= 256 ? pattern - 256 : ((pattern + 17 * (i / 8)) >> (i % 8)) & 1;
				(matches[i] ? expected : expected_false).push_back(UnsafeNumericCast<sel_t>(97 + i));
			}
			for (bool select_true : {false, true}) {
				for (bool select_false : {false, true}) {
					if (!select_true && !select_false) {
						continue;
					}
					duckdb::vector<sel_t> output(count + 2, SENTINEL), false_output(count + 2, SENTINEL);
					optional<array_ptr<sel_t>> true_sel, false_sel;
					if (select_true) {
						true_sel = array_ptr<sel_t>(output.data() + 1, count);
					}
					if (select_false) {
						false_sel = array_ptr<sel_t>(false_output.data() + 1, count);
					}
					auto true_count =
					    SelectionVector::SelectFromBoolean(matches.get(), nullptr, count, true_sel, false_sel, 97);
					REQUIRE(true_count == expected.size());
					if (select_true) {
						for (idx_t i = 0; i < expected.size(); i++) {
							REQUIRE(output[i + 1] == expected[i]);
						}
					}
					if (select_false) {
						for (idx_t i = 0; i < expected_false.size(); i++) {
							REQUIRE(false_output[i + 1] == expected_false[i]);
						}
					}
					REQUIRE(output.front() == SENTINEL);
					REQUIRE(output.back() == SENTINEL);
					REQUIRE(false_output.front() == SENTINEL);
					REQUIRE(false_output.back() == SENTINEL);
				}
			}
		}
	}

	REQUIRE(SelectionVector::SelectFromBoolean(nullptr, nullptr, 0, {}) == 0);
#ifndef DUCKDB_CRASH_ON_ASSERT
	bool matches[8] {};
	REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, nullptr, 8, {}), InternalException);
	// Even an empty selection needs scratch space for every input row.
	sel_t output[8], false_output[8];
	std::fill(output, output + 8, SENTINEL);
	std::fill(false_output, false_output + 8, SENTINEL);
	for (idx_t capacity : {0, 7}) {
		REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, nullptr, 8, array_ptr<sel_t>(output, capacity)),
		                  InternalException);
		REQUIRE_THROWS_AS(
		    SelectionVector::SelectFromBoolean(matches, nullptr, 8, {}, array_ptr<sel_t>(false_output, capacity)),
		    InternalException);
		for (bool short_true : {false, true}) {
			REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(
			                      matches, nullptr, 8, array_ptr<sel_t>(output, short_true ? capacity : 8),
			                      array_ptr<sel_t>(false_output, short_true ? 8 : capacity)),
			                  InternalException);
		}
		for (idx_t i = 0; i < 8; i++) {
			REQUIRE(output[i] == SENTINEL);
			REQUIRE(false_output[i] == SENTINEL);
		}
	}
#endif
}

TEST_CASE("Boolean selection reads mapped matches and emits logical positions", "[selection][scalar_executor]") {
	bool matches[257];
	for (idx_t i = 0; i < 257; i++) {
		matches[i] = i % 7 < 3;
	}
	const sel_t canary = 999999;
	for (idx_t count : {0, 1, 7, 8, 9, 63, 64, 65, 2048, 4099}) {
		SelectionVector match_sel(count);
		duckdb::vector<sel_t> expected, expected_false;
		for (idx_t i = 0; i < count; i++) {
			auto index = (i * 97 + i / 5) % 257;
			match_sel.set_index(i, index);
			(matches[index] ? expected : expected_false).push_back(NumericCast<sel_t>(i + 13));
		}
		for (bool select_true : {false, true}) {
			for (bool select_false : {false, true}) {
				if (!select_true && !select_false) {
					continue;
				}
				duckdb::vector<sel_t> output(count + 2, canary), false_output(count + 2, canary);
				optional<array_ptr<sel_t>> true_sel, false_sel;
				if (select_true) {
					true_sel = array_ptr<sel_t>(output.data() + 1, count);
				}
				if (select_false) {
					false_sel = array_ptr<sel_t>(false_output.data() + 1, count);
				}
				auto true_count =
				    SelectionVector::SelectFromBoolean(matches, &match_sel, count, true_sel, false_sel, 13);
				REQUIRE(true_count == expected.size());
				if (select_true) {
					for (idx_t i = 0; i < expected.size(); i++) {
						REQUIRE(output[i + 1] == expected[i]);
					}
				}
				if (select_false) {
					for (idx_t i = 0; i < expected_false.size(); i++) {
						REQUIRE(false_output[i + 1] == expected_false[i]);
					}
				}
				REQUIRE(output.front() == canary);
				REQUIRE(output.back() == canary);
				REQUIRE(false_output.front() == canary);
				REQUIRE(false_output.back() == canary);
			}
		}
	}

	SelectionVector identity;
	sel_t output[257], false_output[257];
	auto selected = SelectionVector::SelectFromBoolean(matches, &identity, 257, array_ptr<sel_t>(output, 257),
	                                                   array_ptr<sel_t>(false_output, 257));
	idx_t expected_count = 0, expected_false_count = 0;
	for (idx_t i = 0; i < 257; i++) {
		if (matches[i]) {
			REQUIRE(output[expected_count++] == i);
		} else {
			REQUIRE(false_output[expected_false_count++] == i);
		}
	}
	REQUIRE(selected == expected_count);

#ifndef DUCKDB_CRASH_ON_ASSERT
	SelectionVector short_selection(7);
	std::fill(output, output + 257, canary);
	std::fill(false_output, false_output + 257, canary);
	REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, &short_selection, 8, array_ptr<sel_t>(output, 257)),
	                  InternalException);
	REQUIRE_THROWS_AS(
	    SelectionVector::SelectFromBoolean(matches, &short_selection, 8, {}, array_ptr<sel_t>(false_output, 257)),
	    InternalException);
	REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, &short_selection, 8, array_ptr<sel_t>(output, 257),
	                                                     array_ptr<sel_t>(false_output, 257)),
	                  InternalException);
	SelectionVector valid_selection = SelectionVector::Incremental(8);
	REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, &valid_selection, 8, array_ptr<sel_t>(output, 7), {}),
	                  InternalException);
	REQUIRE_THROWS_AS(SelectionVector::SelectFromBoolean(matches, &valid_selection, 8, array_ptr<sel_t>(output, 257),
	                                                     array_ptr<sel_t>(false_output, 7)),
	                  InternalException);
	for (idx_t i = 0; i < 257; i++) {
		REQUIRE(output[i] == canary);
		REQUIRE(false_output[i] == canary);
	}
#endif
}

TEST_CASE("Scalar executor handles every ternary flat and constant profile", "[scalar_executor]") {
	static constexpr idx_t COUNT = 131;
	for (uint8_t constant_mask = 0; constant_mask < 8; constant_mask++) {
		auto a = (constant_mask & 1) ? MakeConstantVector(COUNT, 3) : MakeFlatVector(COUNT, 1);
		auto b = (constant_mask & 2) ? MakeConstantVector(COUNT, 5) : MakeFlatVector(COUNT, 2);
		auto c = (constant_mask & 4) ? MakeConstantVector(COUNT, 7) : MakeFlatVector(COUNT, 3);
		Vector result(LogicalType::BIGINT, COUNT);

		if (!(constant_mask & 1)) {
			auto &validity = FlatVector::ValidityMutable(a);
			validity.SetInvalid(0);
			validity.SetInvalid(63);
			validity.SetInvalid(65);
			validity.SetInvalid(129);
		}
		if (!(constant_mask & 2)) {
			auto &validity = FlatVector::ValidityMutable(b);
			validity.SetInvalid(64);
			validity.SetInvalid(65);
			validity.SetInvalid(130);
		}
		if (!(constant_mask & 4)) {
			auto &validity = FlatVector::ValidityMutable(c);
			validity.SetInvalid(1);
			validity.SetInvalid(128);
		}

		idx_t invocation_count = 0;
		TernaryExecutor::Execute<int64_t, int64_t, int64_t, int64_t>(a, b, c, result,
		                                                             [&](int64_t left, int64_t middle, int64_t right) {
			                                                             invocation_count++;
			                                                             return left + middle * right;
		                                                             });

		idx_t expected_invocations = 0;
		for (idx_t row = 0; row < COUNT; row++) {
			bool valid = ((constant_mask & 1) || (row != 0 && row != 63 && row != 65 && row != 129)) &&
			             ((constant_mask & 2) || (row != 64 && row != 65 && row != 130)) &&
			             ((constant_mask & 4) || (row != 1 && row != 128));
			if (!valid) {
				RequireNull(result, row);
				continue;
			}
			expected_invocations++;
			auto left = (constant_mask & 1) ? 3 : NumericCast<int64_t>(row) + 1;
			auto middle = (constant_mask & 2) ? 5 : NumericCast<int64_t>(row) + 2;
			auto right = (constant_mask & 4) ? 7 : NumericCast<int64_t>(row) + 3;
			RequireValue(result, row, left + middle * right);
		}
		REQUIRE(invocation_count == (constant_mask == 7 ? 1 : expected_invocations));
		REQUIRE(result.size() == COUNT);
		REQUIRE(result.GetVectorType() == (constant_mask == 7 ? VectorType::CONSTANT_VECTOR : VectorType::FLAT_VECTOR));
	}
}

TEST_CASE("Scalar executor clears stale validity in reused results", "[scalar_executor]") {
	static constexpr idx_t COUNT = 130;
	auto valid = MakeConstantVector(COUNT, 4);
	auto null = MakeNullConstantVector(COUNT);
	Vector result(LogicalType::BIGINT, COUNT);
	idx_t invocation_count = 0;
	auto operation = [&](int64_t input) {
		invocation_count++;
		return input + 1;
	};

	UnaryExecutor::Execute<int64_t, int64_t>(valid, result, operation);
	RequireValue(result, 0, 5);
	UnaryExecutor::Execute<int64_t, int64_t>(null, result, operation);
	RequireNull(result, 0);
	UnaryExecutor::Execute<int64_t, int64_t>(valid, result, operation);
	RequireValue(result, 0, 5);
	REQUIRE(invocation_count == 2);

	auto flat = MakeFlatVector(COUNT, 10);
	FlatVector::ValidityMutable(flat).SetInvalid(64);
	UnaryExecutor::Execute<int64_t, int64_t>(flat, result, [](int64_t input) { return input; });
	RequireNull(result, 64);
	FlatVector::ValidityMutable(flat).SetValid(64);
	UnaryExecutor::Execute<int64_t, int64_t>(flat, result, [](int64_t input) { return input; });
	RequireValue(result, 64, 74);

	UnaryExecutor::Execute<int64_t, int64_t>(flat, result, [](int64_t input) -> optional<int64_t> {
		if (input == 75) {
			return optional<int64_t>();
		}
		return input;
	});
	RequireNull(result, 65);
	RequireValue(flat, 65, 75);
	UnaryExecutor::Execute<int64_t, int64_t>(flat, result, [](int64_t input) -> optional<int64_t> { return input; });
	RequireValue(result, 65, 75);
}

TEST_CASE("Scalar executor preserves pre-seeded and aliased unary validity", "[scalar_executor]") {
	static constexpr idx_t COUNT = 8;
	auto flat_input = MakeFlatVector(COUNT, 20);
	Vector preseeded_result(LogicalType::BIGINT, COUNT);
	FlatVector::ValidityMutable(preseeded_result).SetInvalid(4);
	UnaryExecutor::Execute<int64_t, int64_t>(flat_input, preseeded_result, [](int64_t value) { return value + 1; });
	RequireNull(preseeded_result, 4);
	RequireValue(preseeded_result, 3, 24);
	auto validity_owner = MakeFlatVector(COUNT);
	FlatVector::ValidityMutable(validity_owner).SetInvalid(4);
	FlatVector::ValidityMutable(preseeded_result).Initialize(FlatVector::Validity(validity_owner));
	UnaryExecutor::Execute<int64_t, int64_t>(flat_input, preseeded_result, [](int64_t value) -> optional<int64_t> {
		return value == 22 ? optional<int64_t>() : value;
	});
	REQUIRE(!validity_owner.GetValue(2).IsNull());
	RequireNull(preseeded_result, 2);
	RequireNull(preseeded_result, 4);

	auto child = MakeFlatVector(COUNT, 10);
	FlatVector::ValidityMutable(child).SetInvalid(3);
	SelectionVector sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		sel.set_index(row, row);
	}
	Vector input(LogicalType::BIGINT, COUNT);
	input.Slice(child, sel, COUNT);
	Vector result(LogicalType::BIGINT, COUNT);
	FlatVector::ValidityMutable(result).Initialize(FlatVector::Validity(child));

	UnaryExecutor::Execute<int64_t, int64_t>(input, result, [](int64_t value) { return value + 1; });
	RequireNull(input, 3);
	RequireNull(result, 3);
	RequireValue(result, 2, 13);
}

TEST_CASE("Scalar executor generic validity stays isolated across mapped result reuse", "[scalar_executor]") {
	static constexpr idx_t COUNT = 12;
	auto child = MakeFlatVector(COUNT, 10);
	FlatVector::ValidityMutable(child).SetInvalid(4);
	SelectionVector sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		sel.set_index(row, (row * 5 + 3) % COUNT);
	}
	Vector dictionary(LogicalType::BIGINT, COUNT);
	dictionary.Slice(child, sel, COUNT);
	auto other = MakeFlatVector(COUNT, 100);
	Vector result(LogicalType::BIGINT, COUNT);
	FlatVector::ValidityMutable(result).Initialize(FlatVector::Validity(child));

	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(dictionary, other, result,
	                                                   [](int64_t left, int64_t right) { return left + right; });
	RequireNull(result, 5);
	RequireValue(result, 0, 113);
	FlatVector::ValidityMutable(result).SetInvalid(0);
	RequireValue(child, 0, 10);

	FlatVector::ValidityMutable(child).SetValid(4);
	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(dictionary, other, result,
	                                                   [](int64_t left, int64_t right) { return left + right; });
	RequireValue(result, 0, 113);
	RequireValue(result, 5, 119);

	FlatVector::ValidityMutable(child).SetInvalid(4);
	FlatVector::ValidityMutable(result).Initialize(FlatVector::Validity(child));
	UnaryExecutor::Execute<int64_t, int64_t>(dictionary, result, [](int64_t input) -> optional<int64_t> {
		return input == 17 ? optional<int64_t>() : input + 1;
	});
	RequireNull(result, 5);
	RequireNull(result, 8);
	RequireValue(child, 7, 17);
}

TEST_CASE("Scalar executor generic fallback preserves dictionary mappings", "[scalar_executor]") {
	static constexpr idx_t COUNT = 12;
	auto left_base = MakeFlatVector(COUNT, 20);
	auto right_base = MakeFlatVector(COUNT, 200);
	SelectionVector left_sel(COUNT);
	SelectionVector right_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		left_sel.set_index(row, (row * 5 + 3) % COUNT);
		right_sel.set_index(row, COUNT - row - 1);
	}
	Vector left_dictionary(LogicalType::BIGINT, COUNT);
	Vector right_dictionary(LogicalType::BIGINT, COUNT);
	left_dictionary.Slice(left_base, left_sel, COUNT);
	right_dictionary.Slice(right_base, right_sel, COUNT);
	Vector result(LogicalType::BIGINT, COUNT);
	auto add = [](int64_t left, int64_t right) {
		return left + right;
	};

	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(left_dictionary, right_base, result, add);
	for (idx_t row = 0; row < COUNT; row++) {
		RequireValue(result, row, NumericCast<int64_t>(left_sel.get_index(row)) + 20 + NumericCast<int64_t>(row) + 200);
	}
	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(left_base, right_dictionary, result, add);
	for (idx_t row = 0; row < COUNT; row++) {
		RequireValue(result, row,
		             NumericCast<int64_t>(row) + 20 + NumericCast<int64_t>(right_sel.get_index(row)) + 200);
	}
	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(left_dictionary, right_dictionary, result, add);
	for (idx_t row = 0; row < COUNT; row++) {
		RequireValue(result, row,
		             NumericCast<int64_t>(left_sel.get_index(row)) + 20 +
		                 NumericCast<int64_t>(right_sel.get_index(row)) + 200);
	}

	auto base = MakeFlatVector(COUNT, 10);
	FlatVector::ValidityMutable(base).SetInvalid(4);
	SelectionVector first_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		first_sel.set_index(row, (row * 5 + 3) % COUNT);
	}
	Vector dictionary(LogicalType::BIGINT, COUNT);
	dictionary.Slice(base, first_sel, COUNT);

	SelectionVector second_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		second_sel.set_index(row, COUNT - row - 1);
	}
	dictionary.Slice(second_sel, COUNT);

	auto other = MakeFlatVector(COUNT, 100);
	BinaryExecutor::Execute<int64_t, int64_t, int64_t>(dictionary, other, result,
	                                                   [](int64_t left, int64_t right) { return left + right; });
	for (idx_t row = 0; row < COUNT; row++) {
		auto first_row = COUNT - row - 1;
		auto base_row = (first_row * 5 + 3) % COUNT;
		if (base_row == 4) {
			RequireNull(result, row);
		} else {
			RequireValue(result, row, NumericCast<int64_t>(base_row) + 10 + NumericCast<int64_t>(row) + 100);
		}
	}

	std::array<VariadicExecutor::VectorRef, 4> inputs = {{dictionary, other, base, other}};
	VariadicExecutor::Execute<int64_t, int64_t, int64_t, int64_t, int64_t>(
	    inputs, result, [](int64_t a, int64_t b, int64_t c, int64_t d) { return a + b + c + d; });
	for (idx_t row = 0; row < COUNT; row++) {
		auto base_row = ((COUNT - row - 1) * 5 + 3) % COUNT;
		if (base_row == 4 || row == 4) {
			RequireNull(result, row);
		} else {
			RequireValue(result, row,
			             NumericCast<int64_t>(base_row) + 10 + 2 * (NumericCast<int64_t>(row) + 100) +
			                 NumericCast<int64_t>(row) + 10);
		}
	}
}

TEST_CASE("Unary dictionary execution respects error and reuse guards", "[scalar_executor]") {
	static constexpr idx_t DICTIONARY_SIZE = 8;
	static constexpr idx_t COUNT = 32;
	auto child = MakeFlatVector(DICTIONARY_SIZE, 1);
	auto child_data = FlatVector::GetDataMutable<int64_t>(child);
	child_data[7] = -1;
	FlatVector::ValidityMutable(child).SetInvalid(6);
	SelectionVector sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		sel.set_index(row, row % 3);
	}
	Vector dictionary(LogicalType::BIGINT, COUNT);
	dictionary.Slice(child, sel, COUNT);
	Vector result(LogicalType::BIGINT, COUNT);

	idx_t domain_invocations = 0;
	UnaryExecutor::Execute<int64_t, int64_t>(
	    dictionary, result,
	    [&](int64_t input) {
		    domain_invocations++;
		    return input * 2;
	    },
	    FunctionErrors::CANNOT_ERROR);
#if !DUCKDB_SMALLER_BINARY(unary_executor_flat)
	REQUIRE(domain_invocations == DICTIONARY_SIZE - 1);
	REQUIRE(result.GetVectorType() == VectorType::DICTIONARY_VECTOR);
#else
	REQUIRE(domain_invocations == COUNT);
	REQUIRE(result.GetVectorType() == VectorType::FLAT_VECTOR);
#endif
	for (idx_t row = 0; row < COUNT; row++) {
		RequireValue(result, row, NumericCast<int64_t>((row % 3) + 1) * 2);
	}

	idx_t throwing_invocations = 0;
	Vector throwing_result(LogicalType::BIGINT, COUNT);
	REQUIRE_NOTHROW(UnaryExecutor::Execute<int64_t, int64_t>(dictionary, throwing_result, [&](int64_t input) {
		throwing_invocations++;
		if (input < 0) {
			throw InvalidInputException("unreferenced dictionary value");
		}
		return input;
	}));
	REQUIRE(throwing_invocations == COUNT);

	Vector optional_result(LogicalType::BIGINT, COUNT);
	UnaryExecutor::Execute<int64_t, int64_t>(
	    dictionary, optional_result,
	    [](int64_t input) -> optional<int64_t> {
		    if (input == 2) {
			    return optional<int64_t>();
		    }
		    return input;
	    },
	    FunctionErrors::CANNOT_ERROR);
	for (idx_t row = 0; row < COUNT; row++) {
		if (row % 3 == 1) {
			RequireNull(optional_result, row);
		} else {
			RequireValue(optional_result, row, NumericCast<int64_t>((row % 3) + 1));
		}
	}
}

TEST_CASE("Dense integral selection handles validity words and mapped fallbacks", "[scalar_executor]") {
	static constexpr idx_t COUNT = 193;
	for (bool nullable : {false, true}) {
		Vector base(LogicalType::INTEGER, COUNT);
		{
			auto writer = FlatVector::Writer<int32_t>(base, COUNT);
			for (idx_t row = 0; row < COUNT; row++) {
				if (nullable && ((row >= 64 && row < 128) || row == 132)) {
					writer.WriteNull();
				} else {
					writer.WriteValue(NumericCast<int32_t>((row * 37) % 101) - 50);
				}
			}
		}
		SelectionVector dictionary_sel(COUNT);
		SelectionVector labels(COUNT);
		for (idx_t row = 0; row < COUNT; row++) {
			dictionary_sel.set_index(row, (row * 17) % (COUNT / 2));
			labels.set_index(row, 1000 + (COUNT - row) / 2);
		}
		Vector dictionary(base, dictionary_sel, COUNT);
		Vector constant(Value::INTEGER(7), count_t(COUNT));
		for (bool use_dictionary : {false, true}) {
			auto &input = use_dictionary ? dictionary : base;
			for (bool mapped : {false, true}) {
				for (bool constant_left : {false, true}) {
					for (idx_t output_mode = 0; output_mode < 3; output_mode++) {
						SelectionVector true_sel(COUNT);
						SelectionVector false_sel(COUNT);
						auto true_ptr = output_mode == 1 ? nullptr : &true_sel;
						auto false_ptr = output_mode == 0 ? nullptr : &false_sel;
						auto count = BinaryExecutor::Select<int32_t, int32_t, LessThan>(
						    constant_left ? constant : input, constant_left ? input : constant,
						    mapped ? &labels : nullptr, COUNT, true_ptr, false_ptr);
						idx_t true_count = 0;
						idx_t false_count = 0;
						for (idx_t row = 0; row < COUNT; row++) {
							auto value = input.GetValue(row);
							auto label = mapped ? labels.get_index(row) : row;
							bool selected = !value.IsNull() && (constant_left ? 7 < value.GetValue<int32_t>()
							                                                  : value.GetValue<int32_t>() < 7);
							if (selected) {
								if (true_ptr) {
									REQUIRE(true_sel.get_index(true_count) == label);
								}
								true_count++;
							} else {
								if (false_ptr) {
									REQUIRE(false_sel.get_index(false_count) == label);
								}
								false_count++;
							}
						}
						REQUIRE(count == true_count);
					}
				}
			}
		}
	}
}

TEST_CASE("Scalar selection preserves result mappings and NULL semantics", "[scalar_executor]") {
	static constexpr idx_t COUNT = 10;
	auto left = MakeFlatVector(COUNT, 0);
	auto right = MakeConstantVector(COUNT, 5);
	FlatVector::ValidityMutable(left).SetInvalid(2);
	SelectionVector input_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		input_sel.set_index(row, 100 + ((row * 7) % COUNT));
	}
	SelectionVector true_sel(COUNT);
	SelectionVector false_sel(COUNT);
	for (idx_t output_mode = 0; output_mode < 3; output_mode++) {
		auto true_ptr = output_mode == 1 ? nullptr : &true_sel;
		auto false_ptr = output_mode == 0 ? nullptr : &false_sel;
		auto unary_true_count = UnaryExecutor::Select<int64_t>(
		    left, &input_sel, COUNT, [](int64_t value) { return value < 5; }, true_ptr, false_ptr);
		REQUIRE(unary_true_count == 4);
		idx_t true_index = 0;
		idx_t false_index = 0;
		for (idx_t row = 0; row < COUNT; row++) {
			bool selected = row < 5 && row != 2;
			if (selected) {
				if (true_ptr) {
					REQUIRE(true_sel.get_index(true_index) == input_sel.get_index(row));
				}
				true_index++;
			} else {
				if (false_ptr) {
					REQUIRE(false_sel.get_index(false_index) == input_sel.get_index(row));
				}
				false_index++;
			}
		}
	}

	auto true_count =
	    BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, right, &input_sel, COUNT, &true_sel, &false_sel);
	REQUIRE(true_count == 4);
	REQUIRE(Contains(true_sel, true_count, input_sel.get_index(0)));
	REQUIRE(Contains(true_sel, true_count, input_sel.get_index(4)));
	REQUIRE(Contains(false_sel, COUNT - true_count, input_sel.get_index(2)));
	REQUIRE(Contains(false_sel, COUNT - true_count, input_sel.get_index(9)));

	SelectionVector true_only(COUNT);
	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, right, &input_sel, COUNT, &true_only, nullptr) ==
	        true_count);
	for (idx_t row = 0; row < true_count; row++) {
		REQUIRE(true_only.get_index(row) == true_sel.get_index(row));
	}

	SelectionVector false_only(COUNT);
	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, right, &input_sel, COUNT, nullptr, &false_only) ==
	        true_count);
	for (idx_t row = 0; row < COUNT - true_count; row++) {
		REQUIRE(false_only.get_index(row) == false_sel.get_index(row));
	}

	auto constant_null = MakeNullConstantVector(COUNT);
	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(constant_null, right, &input_sel, COUNT, &true_sel,
	                                                           &false_sel) == 0);
	for (idx_t row = 0; row < COUNT; row++) {
		REQUIRE(false_sel.get_index(row) == input_sel.get_index(row));
	}
	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(constant_null, right, &input_sel, COUNT, &true_sel,
	                                                           nullptr) == 0);
	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, constant_null, &input_sel, COUNT, &true_sel,
	                                                           nullptr) == 0);

	REQUIRE(BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, right, nullptr, 0, &true_sel, &false_sel) == 0);
#ifndef DUCKDB_CRASH_ON_ASSERT
	REQUIRE_THROWS(BinaryExecutor::Select<int64_t, int64_t, LessThan>(left, right, nullptr, COUNT, nullptr, nullptr));
#endif

	auto middle = MakeConstantVector(COUNT, 5);
	auto right_constant = MakeConstantVector(COUNT, 0);
	std::array<VariadicExecutor::VectorRef, 3> ternary_inputs = {{left, middle, right_constant}};
	for (idx_t output_mode = 0; output_mode < 3; output_mode++) {
		auto true_ptr = output_mode == 1 ? nullptr : &true_sel;
		auto false_ptr = output_mode == 0 ? nullptr : &false_sel;
		auto ternary_true_count = VariadicExecutor::Select<TernaryLessThanSum, int64_t, int64_t, int64_t>(
		    ternary_inputs, &input_sel, COUNT, true_ptr, false_ptr);
		REQUIRE(ternary_true_count == 4);
		idx_t true_index = 0;
		idx_t false_index = 0;
		for (idx_t row = 0; row < COUNT; row++) {
			bool selected = row < 5 && row != 2;
			if (selected) {
				if (true_ptr) {
					REQUIRE(true_sel.get_index(true_index) == input_sel.get_index(row));
				}
				true_index++;
			} else {
				if (false_ptr) {
					REQUIRE(false_sel.get_index(false_index) == input_sel.get_index(row));
				}
				false_index++;
			}
		}
	}

	auto null_bound = MakeNullConstantVector(COUNT);
	std::array<VariadicExecutor::VectorRef, 3> null_lower_inputs = {{left, null_bound, right_constant}};
	std::array<VariadicExecutor::VectorRef, 3> null_upper_inputs = {{left, middle, null_bound}};
	REQUIRE(VariadicExecutor::Select<TernaryLessThanSum, int64_t, int64_t, int64_t>(null_lower_inputs, &input_sel,
	                                                                                COUNT, &true_sel, nullptr) == 0);
	REQUIRE(VariadicExecutor::Select<TernaryLessThanSum, int64_t, int64_t, int64_t>(null_upper_inputs, &input_sel,
	                                                                                COUNT, &true_sel, nullptr) == 0);
}

TEST_CASE("Binary comparison folding remains correct on dictionaries", "[scalar_executor]") {
	static constexpr idx_t COUNT = 12;
	auto left_child = MakeFlatVector(COUNT, 0);
	auto right_child = MakeFlatVector(COUNT, 5);
	SelectionVector dictionary_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		dictionary_sel.set_index(row, COUNT - row - 1);
	}
	Vector left(LogicalType::BIGINT, COUNT);
	Vector right(LogicalType::BIGINT, COUNT);
	left.Slice(left_child, dictionary_sel, COUNT);
	right.Slice(right_child, dictionary_sel, COUNT);
	SelectionVector input_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		input_sel.set_index(row, row * 3 + 1);
	}

	for (bool nullable : {false, true}) {
		if (nullable) {
			FlatVector::ValidityMutable(left_child).SetInvalid(3);
			left.Slice(left_child, dictionary_sel, COUNT);
		}
		for (idx_t output_mode = 0; output_mode < 3; output_mode++) {
			SelectionVector true_sel(COUNT);
			SelectionVector false_sel(COUNT);
			auto true_ptr = output_mode == 1 ? nullptr : &true_sel;
			auto false_ptr = output_mode == 0 ? nullptr : &false_sel;
			auto not_equal_count = BinaryExecutor::Select<int64_t, int64_t, NotEquals>(left, right, &input_sel, COUNT,
			                                                                           true_ptr, false_ptr);
			auto greater_equal_count = BinaryExecutor::Select<int64_t, int64_t, GreaterThanEquals>(
			    left, right, &input_sel, COUNT, true_ptr, false_ptr);
			REQUIRE(not_equal_count == COUNT - (nullable ? 1 : 0));
			REQUIRE(greater_equal_count == 0);
		}
	}
}

TEST_CASE("Binary generic-constant selection preserves comparison folding", "[scalar_executor]") {
	static constexpr idx_t COUNT = 12;
	auto child = MakeFlatVector(COUNT, 0);
	SelectionVector dictionary_sel(COUNT);
	SelectionVector input_sel(COUNT);
	for (idx_t row = 0; row < COUNT; row++) {
		dictionary_sel.set_index(row, COUNT - row - 1);
		input_sel.set_index(row, row * 3 + 1);
	}
	Vector dictionary(LogicalType::BIGINT, COUNT);
	dictionary.Slice(child, dictionary_sel, COUNT);
	auto constant = MakeConstantVector(COUNT, 5);
	auto null_constant = MakeConstantVector(COUNT, 5);
	ConstantVector::SetNull(null_constant, true);

	for (bool nullable : {false, true}) {
		if (nullable) {
			FlatVector::ValidityMutable(child).SetInvalid(3);
			dictionary.Slice(child, dictionary_sel, COUNT);
		}
		for (bool constant_is_null : {false, true}) {
			auto &constant_input = constant_is_null ? null_constant : constant;
			for (bool constant_left : {false, true}) {
				for (idx_t output_mode = 0; output_mode < 3; output_mode++) {
					SelectionVector true_sel(COUNT);
					SelectionVector false_sel(COUNT);
					auto true_ptr = output_mode == 1 ? nullptr : &true_sel;
					auto false_ptr = output_mode == 0 ? nullptr : &false_sel;
					auto true_count = constant_left
					                      ? BinaryExecutor::Select<int64_t, int64_t, GreaterThanEquals>(
					                            constant_input, dictionary, &input_sel, COUNT, true_ptr, false_ptr)
					                      : BinaryExecutor::Select<int64_t, int64_t, GreaterThanEquals>(
					                            dictionary, constant_input, &input_sel, COUNT, true_ptr, false_ptr);
					idx_t expected_true_count = 0;
					idx_t expected_false_count = 0;
					for (idx_t row = 0; row < COUNT; row++) {
						auto child_index = dictionary_sel.get_index(row);
						bool valid = !constant_is_null && (!nullable || child_index != 3);
						bool selected = valid && (constant_left ? 5 >= child_index : child_index >= 5);
						if (selected) {
							if (true_ptr) {
								REQUIRE(true_sel.get_index(expected_true_count) == input_sel.get_index(row));
							}
							expected_true_count++;
						} else {
							if (false_ptr) {
								REQUIRE(false_sel.get_index(expected_false_count) == input_sel.get_index(row));
							}
							expected_false_count++;
						}
					}
					REQUIRE(true_count == expected_true_count);
					REQUIRE(expected_true_count + expected_false_count == COUNT);
				}
			}
		}
	}
}

TEST_CASE("Executor facade operation conventions remain compatible", "[scalar_executor]") {
	static constexpr idx_t COUNT = 4;
	auto a = MakeFlatVector(COUNT, 1);
	auto b = MakeFlatVector(COUNT, 2);
	auto c = MakeFlatVector(COUNT, 3);
	auto d = MakeFlatVector(COUNT, 4);
	Vector result(LogicalType::BIGINT, COUNT);

	UnaryExecutor::Execute<int64_t, int64_t, UnaryAddOne>(a, result);
	RequireValue(result, 0, 2);
	UnaryGenericState state {10, 3};
	UnaryExecutor::GenericExecute<int64_t, int64_t, UnaryGenericAdd>(a, result, state, true);
	RequireValue(result, 0, 11);
	RequireNull(result, 2);

	Vector string_input(Value("executor"), count_t(COUNT));
	Vector string_result(LogicalType::VARCHAR, COUNT);
	UnaryExecutor::ExecuteString<string_t, string_t, UnaryStringCopy>(string_input, string_result);
	REQUIRE(string_result.GetValue(0).ToString() == "executor");

	BinaryExecutor::Execute<int64_t, int64_t, int64_t, BinaryAdd>(a, b, result);
	RequireValue(result, 0, 3);
	BinaryExecutor::ExecuteStandard<int64_t, int64_t, int64_t, BinaryTypedAdd>(a, b, result);
	RequireValue(result, 0, 3);
	BinaryExecutor::Execute<int64_t, int64_t, int64_t, BinaryAdd, RuntimeNullBinaryWrapper>(a, b, result);
	RequireNull(result, 0);
	RequireValue(result, 1, 5);

	TernaryExecutor::ExecuteStandard<int64_t, int64_t, int64_t, int64_t, TernaryAdd>(a, b, c, result);
	RequireValue(result, 0, 6);
	TernaryExecutor::Execute<int64_t, int64_t, int64_t, int64_t>(
	    a, b, c, result, [](int64_t left, int64_t middle, int64_t right) -> optional<int64_t> {
		    if (left == 2) {
			    return optional<int64_t>();
		    }
		    return left + middle + right;
	    });
	RequireNull(result, 1);

	std::array<VariadicExecutor::VectorRef, 4> inputs = {{a, b, c, d}};
	VariadicExecutor::ExecuteStandard<int64_t, QuaternaryAdd, int64_t, int64_t, int64_t, int64_t>(inputs, result);
	RequireValue(result, 0, 10);
	VariadicExecutor::Execute<int64_t, int64_t, int64_t, int64_t, int64_t>(
	    inputs, result, [](int64_t w, int64_t x, int64_t y, int64_t z) { return w + x + y + z; });
	RequireValue(result, 3, 22);
}

TEST_CASE("Executor facades preserve vector-size errors", "[scalar_executor]") {
#ifdef DUCKDB_CRASH_ON_ASSERT
	return;
#endif
	auto short_vector = MakeFlatVector(3);
	auto long_vector = MakeFlatVector(4);
	Vector result(LogicalType::BIGINT, 4);
	REQUIRE_THROWS(BinaryExecutor::Execute<int64_t, int64_t, int64_t>(
	    short_vector, long_vector, result, [](int64_t left, int64_t right) { return left + right; }));

	auto third = MakeFlatVector(3);
	REQUIRE_THROWS(TernaryExecutor::Execute<int64_t, int64_t, int64_t, int64_t>(
	    short_vector, long_vector, third, result, [](int64_t a, int64_t b, int64_t c) { return a + b + c; }));

	std::array<VariadicExecutor::VectorRef, 4> inputs = {{short_vector, third, long_vector, third}};
	REQUIRE_THROWS(VariadicExecutor::Execute<int64_t, int64_t, int64_t, int64_t, int64_t>(
	    inputs, result, [](int64_t a, int64_t b, int64_t c, int64_t d) { return a + b + c + d; }));
}
