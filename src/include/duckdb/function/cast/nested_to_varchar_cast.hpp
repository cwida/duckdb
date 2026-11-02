#pragma once

#include "duckdb/function/cast/default_casts.hpp"

namespace duckdb {

struct NestedToVarcharCast {
public:
	static const bool LOOKUP_TABLE[256];

	//! Wraps the VARCHAR cast of a child of a nested type that is cast to VARCHAR. UNION and VARIANT children
	//! write their values as strings - these are quoted and escaped like VARCHAR children, unless the value is nested
	static void WrapChildCast(BoundCastInfo &child_cast, const LogicalType &child_type);
};

} // namespace duckdb
