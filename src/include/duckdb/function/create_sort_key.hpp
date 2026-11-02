//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/function/create_sort_key.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/function_set.hpp"

namespace duckdb {

class Expression;

struct OrderModifiers {
	OrderModifiers(OrderType order_type, OrderByNullType null_type) : order_type(order_type), null_type(null_type) {
	}

	OrderType order_type;
	OrderByNullType null_type;

	bool operator==(const OrderModifiers &other) const {
		return order_type == other.order_type && null_type == other.null_type;
	}

	static OrderModifiers Parse(const string &val) {
		auto lcase = StringUtil::Replace(StringUtil::Lower(val), "_", " ");
		OrderType order_type;
		if (StringUtil::StartsWith(lcase, "asc")) {
			order_type = OrderType::ASCENDING;
		} else if (StringUtil::StartsWith(lcase, "desc")) {
			order_type = OrderType::DESCENDING;
		} else {
			throw BinderException("create_sort_key modifier must start with either ASC or DESC");
		}
		OrderByNullType null_type;
		if (StringUtil::EndsWith(lcase, "nulls first")) {
			null_type = OrderByNullType::NULLS_FIRST;
		} else if (StringUtil::EndsWith(lcase, "nulls last")) {
			null_type = OrderByNullType::NULLS_LAST;
		} else {
			throw BinderException("create_sort_key modifier must end with either NULLS FIRST or NULLS LAST");
		}
		return OrderModifiers(order_type, null_type);
	}
};

struct CreateSortKeyHelpers {
	static void CreateSortKey(DataChunk &input, const vector<OrderModifiers> &modifiers, Vector &result);
	static void CreateSortKey(const Vector &input, OrderModifiers modifiers, Vector &result);
	static void CreateSortKey(const Vector &input, idx_t count, OrderModifiers modifiers, Vector &result);
	static bool IsNullSortKey(const_data_ptr_t data, OrderByNullType null_order);
	static idx_t DecodeSortKey(string_t sort_key, Vector &result, idx_t result_idx, OrderModifiers modifiers);
	static void DecodeSortKey(string_t sort_key, DataChunk &result, idx_t result_idx,
	                          const vector<OrderModifiers> &modifiers);
	static void CreateSortKeyWithValidity(const Vector &input, Vector &result, const OrderModifiers &modifiers);
	static void CreateSortKeyWithValidity(const Vector &input, Vector &result, const OrderModifiers &modifiers,
	                                      idx_t count);
};

//! We don't add this function to the catalog, for internal use only
//! Therefore, it's not defined in src/function/scalar/generic/functions.json
struct DecodeSortKeyFun {
	static constexpr const char *Name = "decode_sort_key";
	static constexpr const char *Parameters = "parameters...";
	static constexpr const char *Description =
	    "Decodes a sort key created with create_sort_key into a STRUCT based on a set of sort qualifiers";
	static constexpr const char *Example = "decode_sort_key(sort_key, 'A INTEGER', 'DESC')";
	static constexpr const char *Categories = "";

	static ScalarFunction GetFunction();
	//! Binds decode_sort_key from bound column types, bypassing the VARCHAR column definitions the SQL form parses
	static unique_ptr<Expression> Bind(unique_ptr<Expression> sort_key, child_list_t<LogicalType> columns,
	                                   vector<OrderModifiers> modifiers);
};

} // namespace duckdb
