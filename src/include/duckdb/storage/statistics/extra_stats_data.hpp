//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/statistics/extra_stats_data.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/helper.hpp"

namespace duckdb {

//! Statistics data that does not fit the fixed-size statistics union
struct ExtraStatsData {
	virtual ~ExtraStatsData() = default;

	template <class TARGET>
	TARGET &Cast() {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<TARGET &>(*this);
	}
	template <class TARGET>
	const TARGET &Cast() const {
		DynamicCastCheck<TARGET>(this);
		return reinterpret_cast<const TARGET &>(*this);
	}
};

} // namespace duckdb
