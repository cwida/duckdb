//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/optional_idx.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/exception.hpp"
#include "duckdb/common/optional.hpp"

namespace duckdb {

class optional_idx {
public:
	constexpr optional_idx() {
	}
	// NOLINTNEXTLINE: allow implicit conversion from idx_t
	constexpr optional_idx(idx_t index_p) : index(index_p) {
	}

	static optional_idx Invalid() {
		return optional_idx();
	}

	bool IsValid() const {
		return index.has_value();
	}

	void SetInvalid() {
		index.reset();
	}

	idx_t GetIndex() const {
		if (!index.has_value()) {
			ThrowNotSet();
		}
		return *index;
	}

	inline bool operator==(const optional_idx &rhs) const {
		return index == rhs.index;
	}

	inline bool operator!=(const optional_idx &rhs) const {
		return index != rhs.index;
	}

private:
	//! Kept out-of-line so that the throwing path does not block inlining of the accessors
	[[noreturn]] DUCKDB_API static void ThrowNotSet();

private:
	optional<idx_t> index;
};

} // namespace duckdb
