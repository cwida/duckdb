#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

static void MapKeyValueFunction(DataChunk &args, ExpressionState &state, Vector &result,
                                Vector &(*get_child_vector)(Vector &)) {
	auto &map = args.data[0];

	D_ASSERT(result.GetType().id() == LogicalTypeId::LIST);
	if (map.GetType().id() == LogicalTypeId::SQLNULL) {
		ConstantVector::SetNull(result, count_t(args.size()));
		return;
	}
	map.Flatten();

	D_ASSERT(map.GetType().id() == LogicalTypeId::MAP);
	auto &child = get_child_vector(map);

	auto &entries = ListVector::GetChildMutable(result);
	entries.Reference(child);

	FlatVector::SetData(result, FlatVector::GetDataMutable(map), count_t(args.size()));
	FlatVector::SetValidity(result, FlatVector::ValidityMutable(map));
	auto list_size = ListVector::GetListSize(map);
	ListVector::SetListSize(result, list_size);
	result.Verify();
}

static void MapKeysFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	MapKeyValueFunction(args, state, result, MapVector::GetKeys);
}

static void MapValuesFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	MapKeyValueFunction(args, state, result, MapVector::GetValues);
}

static unique_ptr<BaseStatistics> MapValuesStats(ClientContext &, FunctionStatisticsInput &input) {
	if (input.child_stats.size() != 1 || input.child_stats[0].GetStatsType() != StatisticsType::LIST_STATS) {
		return nullptr;
	}
	auto &entry_stats = ListStats::GetChildStats(input.child_stats[0]);
	if (entry_stats.GetStatsType() != StatisticsType::STRUCT_STATS) {
		return nullptr;
	}
	auto result = ListStats::CreateEmpty(input.expr.GetReturnType());
	result.CopyValidity(input.child_stats[0]);
	ListStats::GetChildStats(result).Copy(StructStats::GetChildStats(entry_stats, 1));
	return result.ToUnique();
}

ScalarFunction MapKeysFun::GetFunction() {
	//! the arguments and return types are actually set in the binder function
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction function({}, LogicalType::LIST(key_type), MapKeysFunction);
	function.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type));
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);

	function.SetFallible();
	return function;
}

ScalarFunction MapValuesFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction function({}, LogicalType::LIST(val_type), MapValuesFunction);
	function.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type));
	function.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	function.SetStatisticsCallback(MapValuesStats);

	function.SetFallible();
	return function;
}

} // namespace duckdb
