#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/parser/statement/select_statement.hpp"

namespace duckdb {

//! One parsed BRANCH clause of a trampoline CTE
struct TrampolineBranch {
	idx_t label;
	bool distinct;
	bool tee;
	unique_ptr<SelectStatement> statement;
};

} // namespace duckdb
