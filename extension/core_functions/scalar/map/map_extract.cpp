#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/list/contains_or_position.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/storage/statistics/list_stats.hpp"
#include "duckdb/storage/statistics/struct_stats.hpp"

namespace duckdb {

static void MapExtractValueFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();

	const auto &map_vec = args.data[0];
	const auto &arg_vec = args.data[1];

	const auto &key_vec = MapVector::GetKeys(map_vec);
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	ListSearchOp<int32_t>(map_vec, key_vec, arg_vec, pos_vec, args.size());

	UnifiedVectorFormat pos_format;
	UnifiedVectorFormat lst_format;

	pos_vec.ToUnifiedFormat(pos_format);
	map_vec.ToUnifiedFormat(lst_format);

	const auto pos_data = UnifiedVectorFormat::GetData<int32_t>(pos_format);
	const auto inc_list_data = UnifiedVectorFormat::GetData<list_entry_t>(lst_format);

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto lst_idx = lst_format.sel->get_index(row_idx);
		if (!lst_format.validity.RowIsValid(lst_idx)) {
			FlatVector::SetNull(result, row_idx, true);
			continue;
		}

		const auto pos_idx = pos_format.sel->get_index(row_idx);
		if (!pos_format.validity.RowIsValid(pos_idx)) {
			// We didnt find the key in the map, so return NULL
			FlatVector::SetNull(result, row_idx, true);
			continue;
		}

		// Compute the actual position of the value in the map value vector
		const auto pos = inc_list_data[lst_idx].offset + UnsafeNumericCast<idx_t>(pos_data[pos_idx] - 1);
		VectorOperations::Copy(val_vec, result, pos + 1, pos, row_idx);
	}

	if (args.size() == 1) {
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
	}

	result.Verify();
}

static void MapExtractListFunc(DataChunk &args, ExpressionState &state, Vector &result) {
	const auto count = args.size();

	const auto &map_vec = args.data[0];
	const auto &arg_vec = args.data[1];

	const auto &key_vec = MapVector::GetKeys(map_vec);
	const auto &val_vec = MapVector::GetValues(map_vec);

	// Collect the matching positions
	Vector pos_vec(LogicalType::INTEGER, count);
	ListSearchOp<int32_t>(map_vec, key_vec, arg_vec, pos_vec, args.size());

	auto pos_entries = pos_vec.Values<int32_t>();
	auto map_entries = map_vec.Values<list_entry_t>();
	const auto val_size = ListVector::GetListSize(map_vec);
	auto out_list_data = FlatVector::Writer<list_entry_t>(result, count);

	for (idx_t row_idx = 0; row_idx < count; row_idx++) {
		auto map_entry = map_entries[row_idx];
		if (!map_entry.IsValid()) {
			out_list_data.WriteNull();
			continue;
		}

		auto list = out_list_data.WriteDynamicList();
		auto pos_entry = pos_entries[row_idx];
		if (!pos_entry.IsValid()) {
			// key not found: return empty list
			continue;
		}

		const auto &inc_list = map_entry.GetValue();
		const auto pos = inc_list.offset + UnsafeNumericCast<idx_t>(pos_entry.GetValue() - 1);
		SelectionVector sel(1);
		sel.set_index(0, pos);
		list.Append(val_vec, sel, val_size, 0, 1);
	}
}

static unique_ptr<BaseStatistics> MapExtractValueStats(ClientContext &, FunctionStatisticsInput &input) {
	auto &entry_stats = ListStats::GetChildStats(input.child_stats[0]);
	auto value_copy = StructStats::GetChildStats(entry_stats, 1).Copy();
	// missing keys return NULL
	value_copy.Set(StatsInfo::CAN_HAVE_NULL_VALUES);
	if (!input.child_stats[0].CanHaveNoNull() || !input.child_stats[1].CanHaveNoNull()) {
		value_copy.Set(StatsInfo::CANNOT_HAVE_VALID_VALUES);
	}
	return value_copy.ToUnique();
}

ScalarFunction MapExtractValueFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, val_type, MapExtractValueFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetStatisticsCallback(MapExtractValueStats);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

ScalarFunction MapExtractFun::GetFunction() {
	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");

	ScalarFunction fun({}, LogicalType::LIST(val_type), MapExtractListFunc);
	fun.GetSignature().AddParameter("map", LogicalType::MAP(key_type, val_type)).AddParameter("key", key_type);
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
