//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/bound_pragma_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/function/pragma_function.hpp"

namespace duckdb {

struct BoundPragmaInfo {
	BoundPragmaInfo(PragmaFunction function_p, vector<Value> parameters_p, named_argument_map_t named_parameters_p)
	    : function(std::move(function_p)), parameters(std::move(parameters_p)),
	      named_parameters(std::move(named_parameters_p)) {
	}

	PragmaFunction function;
	//! Parameter list (if any)
	vector<Value> parameters;
	//! Named parameter list (if any)
	named_argument_map_t named_parameters;
};

} // namespace duckdb
