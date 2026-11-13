//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/main/query_parameters.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/identifier.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/shared_ptr.hpp"
#include "duckdb/planner/expression/bound_parameter_data.hpp"

namespace duckdb {

class ResultFormat;

struct QueryParameters {
	QueryParameters() = default;
	//! So a caller that wants only a format does not have to spell out the other fields
	explicit QueryParameters(shared_ptr<ResultFormat> format_p) : format(std::move(format_p)) {
	}

	//! Arguments for a parameterized statement (may be null)
	optional_ptr<identifier_map_t<BoundParameterData>> statement_args;
	shared_ptr<ResultFormat> format;
};

} // namespace duckdb
