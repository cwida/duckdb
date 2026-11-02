#include "core_functions/scalar/random_functions.hpp"
#include "duckdb/common/vector_operations/vector_operations.hpp"
#include "duckdb/execution/expression_executor.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/types/uuid.hpp"
#include "duckdb/common/types/timestamp.hpp"

namespace duckdb {

namespace {

struct ExtractVersionUuidOperator {
	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		char uuid[36]; // Intentionally no initialize.
		BaseUUID::ToString(input, uuid);
		// UUIDv4 and UUIDv7 stores version as the 15-th uint8_t.
		return uuid[14] - '0';
	}
};

struct ExtractTimestampUuidOperator {
	template <typename INPUT_TYPE, typename RESULT_TYPE>
	static RESULT_TYPE Operation(INPUT_TYPE input) {
		// Validate whether the given UUID is v7.
		const uint8_t version = (static_cast<uint8_t>((input.upper) >> 8) & 0xf0) >> 4;
		if (version != 7) {
			throw InvalidInputException("Given UUID is with version %u, not version 7.", version);
		}

		// UUID v7 begins with a 48 bit big-endian Unix Epoch timestamp with millisecond granularity.
		int64_t upper = input.upper;
		// flip the top byte
		upper ^= NumericLimits<int64_t>::Minimum();
		int64_t unix_ts_milli = upper;
		unix_ts_milli = unix_ts_milli >> 16;

		static constexpr int64_t kMilliToMicro = 1000;
		const int64_t unix_ts_ms = kMilliToMicro * unix_ts_milli;
		return timestamp_t {unix_ts_ms};
	}
};

template <typename INPUT, typename OP>
void ExtractVersionFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);
	const auto &input = args.data[0];
	UnaryExecutor::Execute<INPUT, uint32_t, OP>(input, result);
}

template <typename INPUT, typename OP>
void ExtractTimestampFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 1);
	const auto &input = args.data[0];
	UnaryExecutor::Execute<INPUT, timestamp_t, OP>(input, result);
}

struct RandomLocalState : public FunctionLocalState {
	explicit RandomLocalState(uint64_t seed) : random_engine(0) {
		random_engine.SetSeed(seed);
	}

	RandomEngine random_engine;
};

void RandomFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 0);
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RandomLocalState>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<double>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		result_data.WriteValue(lstate.random_engine.NextRandom());
	}
}

unique_ptr<FunctionLocalState> RandomInitLocalState(ExpressionState &state, const BoundFunctionExpression &expr,
                                                    FunctionData *bind_data) {
	auto &random_engine = RandomEngine::Get(state.GetContext());
	lock_guard<mutex> guard(random_engine.lock);
	return make_uniq<RandomLocalState>(random_engine.NextRandomInteger64());
}

void GenerateUUIDv4Function(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 0);
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RandomLocalState>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<hugeint_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		result_data.WriteValue(UUIDv4::GenerateRandomUUID(lstate.random_engine));
	}
}

void GenerateUUIDv7Function(DataChunk &args, ExpressionState &state, Vector &result) {
	D_ASSERT(args.ColumnCount() == 0);
	auto &lstate = ExecuteFunctionState::GetFunctionState(state)->Cast<RandomLocalState>();

	result.SetVectorType(VectorType::FLAT_VECTOR);
	auto result_data = FlatVector::Writer<hugeint_t>(result, args.size());
	for (idx_t i = 0; i < args.size(); i++) {
		result_data.WriteValue(UUIDv7::GenerateRandomUUID(lstate.random_engine));
	}
}

} // namespace

ScalarFunction RandomFun::GetFunction() {
	ScalarFunction random("random", {}, LogicalType::DOUBLE, RandomFunction, nullptr, nullptr, RandomInitLocalState);
	random.SetStability(FunctionStability::VOLATILE);
	return random;
}

ScalarFunction UUIDFun::GetFunction() {
	return UUIDv4Fun::GetFunction();
}

ScalarFunction UUIDv4Fun::GetFunction() {
	ScalarFunction uuid_v4_function({}, LogicalType::UUID, GenerateUUIDv4Function, nullptr, nullptr,
	                                RandomInitLocalState);
	// generate a random uuid v4
	uuid_v4_function.SetStability(FunctionStability::VOLATILE);
	return uuid_v4_function;
}

ScalarFunction UUIDv7Fun::GetFunction() {
	ScalarFunction uuid_v7_function({}, LogicalType::UUID, GenerateUUIDv7Function, nullptr, nullptr,
	                                RandomInitLocalState);
	// generate a random uuid v7
	uuid_v7_function.SetStability(FunctionStability::VOLATILE);
	return uuid_v7_function;
}

ScalarFunction UUIDExtractVersionFun::GetFunction() {
	ScalarFunction function({}, LogicalType::UINTEGER, ExtractVersionFunction<hugeint_t, ExtractVersionUuidOperator>);
	function.GetSignature().AddParameter("uuid", LogicalType::UUID);
	return function;
}

ScalarFunction UUIDExtractTimestampFun::GetFunction() {
	ScalarFunction function({}, LogicalType::TIMESTAMP_TZ,
	                        ExtractTimestampFunction<hugeint_t, ExtractTimestampUuidOperator>);
	function.GetSignature().AddParameter("uuid", LogicalType::UUID);
	// throws if the UUID is not a version 7 UUID
	function.SetFallible();
	return function;
}

} // namespace duckdb
