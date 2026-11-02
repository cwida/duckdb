#include "duckdb/common/types/data_chunk.hpp"
#include "duckdb/function/scalar/list_functions.hpp"
#include "duckdb/function/scalar/nested_functions.hpp"
#include "duckdb/planner/expression/bound_cast_expression.hpp"
#include "duckdb/function/create_sort_key.hpp"
#include "duckdb/common/string_map_set.hpp"
#include "duckdb/common/helper.hpp"

namespace duckdb {

static void ListIntersectFunction(DataChunk &args, ExpressionState &state, Vector &result) {
	auto row_count = args.size();

	// Handle NULL return type case
	if (result.GetType() == LogicalType::SQLNULL) {
		ConstantVector::SetNull(result, count_t(row_count));
		return;
	}

	auto &l_vec = args.data[0];
	auto &r_vec = args.data[1];

	const auto l_size = ListVector::GetListSize(l_vec);
	const auto r_size = ListVector::GetListSize(r_vec);

	auto &l_child = ListVector::GetChildMutable(l_vec);
	auto &r_child = ListVector::GetChildMutable(r_vec);

	auto l_entries = l_vec.Values<list_entry_t>();
	auto r_entries = r_vec.Values<list_entry_t>();

	Vector l_sortkey_vec(LogicalType::BLOB, l_size);
	Vector r_sortkey_vec(LogicalType::BLOB, r_size);

	const OrderModifiers order_modifiers(OrderType::ASCENDING, OrderByNullType::NULLS_LAST);

	CreateSortKeyHelpers::CreateSortKey(l_child, order_modifiers, l_sortkey_vec);
	CreateSortKeyHelpers::CreateSortKey(r_child, order_modifiers, r_sortkey_vec);

	const auto l_sortkey_ptr = l_sortkey_vec.Values<string_t>();
	const auto r_sortkey_ptr = r_sortkey_vec.Values<string_t>();
	auto l_validity = l_child.Validity();
	auto r_validity = r_child.Validity();

	auto result_data = FlatVector::Writer<list_entry_t>(result, row_count);
	for (idx_t i = 0; i < row_count; i++) {
		auto l_entry = l_entries[i];
		auto r_entry = r_entries[i];

		if (!l_entry.IsValid()) {
			result_data.WriteNull();
			continue;
		}

		auto list = result_data.WriteDynamicList();
		if (!r_entry.IsValid()) {
			continue;
		}

		const auto &l_list = l_entry.GetValue();
		const auto &r_list = r_entry.GetValue();

		if (l_list.length == 0 || r_list.length == 0) {
			continue;
		}

		string_set_t set;
		string_set_t result_set;
		string_map_t<idx_t> key_to_index_map;

		// Choose which side to hash and which to iterate
		const bool use_l_for_hash = l_list.length <= r_list.length;
		const auto &hash_list = use_l_for_hash ? l_list : r_list;
		const auto &iter_list = use_l_for_hash ? r_list : l_list;
		const auto &hash_fmt = use_l_for_hash ? l_validity : r_validity;
		const auto &iter_fmt = use_l_for_hash ? r_validity : l_validity;
		const auto &hash_keys = use_l_for_hash ? l_sortkey_ptr : r_sortkey_ptr;
		const auto &iter_keys = use_l_for_hash ? r_sortkey_ptr : l_sortkey_ptr;

		for (idx_t j = 0; j < hash_list.length; j++) {
			const idx_t h_idx = hash_list.offset + j;
			if (!hash_fmt.IsValid(h_idx)) {
				// value is NULL - skip
				continue;
			}
			const auto &key = hash_keys.GetValueUnsafe(h_idx);
			set.insert(key);
			if (use_l_for_hash) {
				key_to_index_map[key] = h_idx;
			}
		}

		// Iterate the chosen side, but ALWAYS emit a LEFT index
		const idx_t row_max_length = MinValue(l_list.length, r_list.length);
		SelectionVector row_sel(row_max_length);
		idx_t row_result_length = 0;
		for (idx_t j = 0; j < iter_list.length; j++) {
			const idx_t it_idx = iter_list.offset + j;
			if (!iter_fmt.IsValid(it_idx)) {
				// value is NULL - skip
				continue;
			}
			const auto &key = iter_keys.GetValueUnsafe(it_idx);
			if (set.find(key) == set.end() || result_set.find(key) != result_set.end()) {
				continue;
			}
			result_set.insert(key);

			const idx_t emit_left_idx = use_l_for_hash ? key_to_index_map[key] : it_idx;

			row_sel.set_index(row_result_length, emit_left_idx);
			row_result_length++;
		}

		list.Append(l_child, row_sel, l_size, 0, row_result_length);
	}
}
static unique_ptr<FunctionData> ListIntersectBind(BindScalarFunctionInput &input) {
	auto &context = input.GetClientContext();
	auto &arguments = input.GetArguments();
	D_ASSERT(input.GetBoundFunction().GetArguments().size() == 2);
	arguments[0] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[0]));
	arguments[1] = BoundCastExpression::AddArrayCastToList(context, std::move(arguments[1]));
	return nullptr;
}

ScalarFunction ListIntersectFun::GetFunction() {
	auto fun =
	    ScalarFunction({}, LogicalType::LIST(LogicalType::TEMPLATE("T")), ListIntersectFunction, ListIntersectBind);
	fun.GetSignature()
	    .AddParameter("list1", LogicalType::LIST(LogicalType::TEMPLATE("T")))
	    .AddParameter("list2", LogicalType::LIST(LogicalType::TEMPLATE("T")));
	fun.SetNullHandling(FunctionNullHandling::SPECIAL_HANDLING);
	return fun;
}

} // namespace duckdb
