#include "duckdb/common/vector/map_vector.hpp"
#include "core_functions/scalar/map_functions.hpp"
#include "duckdb/planner/expression/bound_function_expression.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/types/value_map.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"

namespace duckdb {

static void MapFunctionEmptyInput(Vector &result, const idx_t row_count) {
	// If no chunk is set in ExpressionExecutor::ExecuteExpression (args.data.empty(), e.g., in SELECT MAP()),
	// then we always pass a row_count of 1.
	result.SetVectorType(VectorType::CONSTANT_VECTOR);
	ListVector::SetListSize(result, 0);

	auto result_data = ConstantVector::GetData<list_entry_t>(result);
	result_data[0] = list_entry_t();
	result.Verify();
}

static bool MapIsNull(DataChunk &chunk) {
	if (chunk.data.empty()) {
		return false;
	}
	D_ASSERT(chunk.data.size() == 2);
	const auto &keys = chunk.data[0];
	const auto &values = chunk.data[1];

	if (keys.GetType().id() == LogicalTypeId::SQLNULL) {
		return true;
	}
	if (values.GetType().id() == LogicalTypeId::SQLNULL) {
		return true;
	}
	return false;
}

static void MapFunction(DataChunk &args, ExpressionState &, Vector &result) {
	// internal MAP representation
	// - LIST-vector that contains STRUCTs as child entries
	// - STRUCTs have exactly two fields, a key-field, and a value-field
	// - key names are unique
	D_ASSERT(result.GetType().id() == LogicalTypeId::MAP);
	if (MapIsNull(args)) {
		auto &validity = FlatVector::ValidityMutable(result);
		validity.SetInvalid(0);
		result.SetVectorType(VectorType::CONSTANT_VECTOR);
		return;
	}

	auto row_count = args.size();

	// early-out, if no data
	if (args.data.empty()) {
		return MapFunctionEmptyInput(result, row_count);
	}

	auto &keys = args.data[0];
	auto &values = args.data[1];

	// a LIST vector, where each row contains a LIST of KEYS
	UnifiedVectorFormat keys_data;
	keys.ToUnifiedFormat(keys_data);
	auto keys_entries = UnifiedVectorFormat::GetData<list_entry_t>(keys_data);

	// the KEYs child vector
	auto &keys_child_vector = ListVector::GetChildMutable(keys);
	UnifiedVectorFormat keys_child_data;
	keys_child_vector.ToUnifiedFormat(keys_child_data);

	// a LIST vector, where each row contains a LIST of VALUES
	UnifiedVectorFormat values_data;
	values.ToUnifiedFormat(values_data);
	auto values_entries = UnifiedVectorFormat::GetData<list_entry_t>(values_data);

	// the VALUEs child vector
	auto &values_child_vector = ListVector::GetChildMutable(values);
	UnifiedVectorFormat values_child_data;
	values_child_vector.ToUnifiedFormat(values_child_data);

	// a LIST vector, where each row contains a MAP (LIST of STRUCTs)
	auto result_entries = FlatVector::Writer<list_entry_t>(result, row_count);

	// get the resulting size of the key/value child lists
	idx_t result_child_size = 0;
	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto keys_idx = keys_data.sel->get_index(row_idx);
		auto values_idx = values_data.sel->get_index(row_idx);
		if (!keys_data.validity.RowIsValid(keys_idx) || !values_data.validity.RowIsValid(values_idx)) {
			continue;
		}
		auto keys_entry = keys_entries[keys_idx];
		result_child_size += keys_entry.length;
	}

	// we need to slice potential non-flat vectors
	SelectionVector sel_keys(result_child_size);
	SelectionVector sel_values(result_child_size);
	idx_t offset = 0;

	for (idx_t row_idx = 0; row_idx < row_count; row_idx++) {
		auto keys_idx = keys_data.sel->get_index(row_idx);
		auto values_idx = values_data.sel->get_index(row_idx);

		// NULL MAP
		if (!keys_data.validity.RowIsValid(keys_idx) || !values_data.validity.RowIsValid(values_idx)) {
			result_entries.WriteNull();
			continue;
		}

		auto keys_entry = keys_entries[keys_idx];
		auto values_entry = values_entries[values_idx];

		if (keys_entry.length != values_entry.length) {
			MapVector::EvalMapInvalidReason(MapInvalidReason::NOT_ALIGNED);
		}

		// set the selection vectors and perform a duplicate key check
		value_set_t unique_keys;
		for (idx_t child_idx = 0; child_idx < keys_entry.length; child_idx++) {
			auto key_idx = keys_child_data.sel->get_index(keys_entry.offset + child_idx);
			auto value_idx = values_child_data.sel->get_index(values_entry.offset + child_idx);

			// NULL check
			if (!keys_child_data.validity.RowIsValid(key_idx)) {
				MapVector::EvalMapInvalidReason(MapInvalidReason::NULL_KEY);
			}

			// unique check
			auto value = keys_child_vector.GetValue(key_idx);
			auto unique = unique_keys.insert(value).second;
			if (!unique) {
				MapVector::EvalMapInvalidReason(MapInvalidReason::DUPLICATE_KEY);
			}

			// set selection vectors
			sel_keys.set_index(offset + child_idx, key_idx);
			sel_values.set_index(offset + child_idx, value_idx);
		}

		// keys_entry and values_entry have the same length
		result_entries.WriteValue(list_entry_t(offset, keys_entry.length));
		offset += keys_entry.length;
	}
	D_ASSERT(offset == result_child_size);

	auto &result_key_vector = MapVector::GetKeys(result);
	auto &result_value_vector = MapVector::GetValues(result);

	ListVector::SetListSize(result, offset);
	result_key_vector.Slice(keys_child_vector, sel_keys, offset);
	result_key_vector.Flatten();
	result_value_vector.Slice(values_child_vector, sel_values, offset);
	result_value_vector.Flatten();
	FlatVector::ValidityMutable(ListVector::GetChildMutable(result)).Resize(result_child_size);
	result.Verify();
}

ScalarFunctionSet MapFun::GetFunctions() {
	ScalarFunction empty_func({}, LogicalType::MAP(LogicalType::SQLNULL, LogicalType::SQLNULL), MapFunction);
	empty_func.SetFallible();

	auto key_type = LogicalType::TEMPLATE("K");
	auto val_type = LogicalType::TEMPLATE("V");
	ScalarFunction value_func({}, LogicalType::MAP(key_type, val_type), MapFunction);
	value_func.GetSignature()
	    .AddParameter("keys", LogicalType::LIST(key_type))
	    .AddParameter("values", LogicalType::LIST(val_type));
	value_func.SetFallible();
	value_func.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);

	ScalarFunctionSet set;

	set.AddFunction(empty_func);
	set.AddFunction(value_func);

	return set;
}

} // namespace duckdb
