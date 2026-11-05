//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/planner/filter/bloom_filter.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/serializer/deserializer.hpp"
#include "duckdb/common/serializer/serializer.hpp"
#include "duckdb/planner/table_filter.hpp"

namespace duckdb {

class BloomFilter;

//! DEPRECATED - only preserved for backwards-compatible expression conversion
class LegacyBFTableFilter final : public TableFilter {
private:
	shared_ptr<const BloomFilter> filter;

	bool filters_null_values;
	string key_column_name;
	LogicalType key_type;

public:
	static constexpr auto TYPE = TableFilterType::LEGACY_BLOOM_FILTER;

public:
	explicit LegacyBFTableFilter(shared_ptr<const BloomFilter> filter_p, const bool filters_null_values_p,
	                             const string &key_column_name_p, const LogicalType &key_type_p)
	    : TableFilter(TYPE), filter(std::move(filter_p)), filters_null_values(filters_null_values_p),
	      key_column_name(key_column_name_p), key_type(key_type_p) {
	}

private:
	unique_ptr<Expression> ToExpression(const Expression &column) const override;

	void Serialize(Serializer &serializer) const override;
	static unique_ptr<TableFilter> Deserialize(Deserializer &deserializer);
};

} // namespace duckdb
