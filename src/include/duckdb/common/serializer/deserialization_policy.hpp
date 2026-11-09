//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/serializer/deserialization_policy.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bitset.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/types.hpp"
#include "duckdb/parser/literal.hpp"

namespace duckdb {

//! Restrictions on the type and literal identifiers a deserializer may reconstruct.
class DeserializationPolicy {
public:
	void Disallow(LogicalTypeId type) {
		disallowed_types.set(static_cast<idx_t>(type));
	}

	void Disallow(LiteralKind kind) {
		disallowed_literals.set(static_cast<idx_t>(kind));
	}

	bool IsAllowed(LogicalTypeId type) const {
		return !disallowed_types.test(static_cast<idx_t>(type));
	}

	bool IsAllowed(LiteralKind kind) const {
		return !disallowed_literals.test(static_cast<idx_t>(kind));
	}

private:
	using LogicalTypeStorage = std::underlying_type<LogicalTypeId>::type;
	using LiteralKindStorage = std::underlying_type<LiteralKind>::type;

	//! Size each bitset for the full range of its enum's underlying type.
	bitset<static_cast<idx_t>(NumericLimits<LogicalTypeStorage>::Maximum()) + 1> disallowed_types;
	bitset<static_cast<idx_t>(NumericLimits<LiteralKindStorage>::Maximum()) + 1> disallowed_literals;
};

} // namespace duckdb
