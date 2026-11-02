#include "duckdb/planner/filter/dynamic_filter.hpp"
#include "duckdb/planner/filter/table_filter_functions.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/value_operations/value_operations.hpp"
#include "duckdb/storage/statistics/base_statistics.hpp"
#include "duckdb/storage/statistics/numeric_stats.hpp"
#include "duckdb/storage/statistics/string_stats.hpp"

namespace duckdb {

LegacyDynamicFilter::LegacyDynamicFilter() : TableFilter(TableFilterType::LEGACY_DYNAMIC_FILTER) {
}

LegacyDynamicFilter::LegacyDynamicFilter(shared_ptr<DynamicFilterData> filter_data_p)
    : TableFilter(TableFilterType::LEGACY_DYNAMIC_FILTER), filter_data(std::move(filter_data_p)) {
}

unique_ptr<Expression> LegacyDynamicFilter::ToExpression(const Expression &column) const {
	// emit the dynamic filter even without filter data (e.g. after deserialization), so it can be set later
	return CreateDynamicFilterExpression(filter_data, column.GetReturnType(), column.Copy());
}

void DynamicFilterData::SetValue(Value val) {
	if (val.IsNull()) {
		return;
	}
	annotated_lock_guard<annotated_shared_mutex> l(lock);
	constant = std::move(val);
	initialized = true;
}

void DynamicFilterData::Reset() {
	annotated_lock_guard<annotated_shared_mutex> l(lock);
	initialized = false;
}

} // namespace duckdb
