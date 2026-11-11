//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/vector_operations/selection_helpers.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include <cstdint>
#include <type_traits>

namespace duckdb {

//! Limit boolean staging to narrow integers; wider inputs can lose to scalar selection.
template <class... ARGS>
constexpr bool UseBooleanSelectionBatch() {
	return ((std::is_integral<ARGS>::value && sizeof(ARGS) <= sizeof(uint32_t)) && ...);
}

} // namespace duckdb
