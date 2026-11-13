//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/lateral_row_function.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/table_index.hpp"
#include "duckdb/planner/logical_operator.hpp"

namespace duckdb {

class Optimizer;

//! Runs the right side of a decorrelated lateral join directly over the left rows when it is a function of the left
//! row alone.
//!
//!   MATERIALIZED_CTE d (P)                                 PROJECTION[d1 columns, R columns]
//!     INNER[d1.c = R.c, for every key c]            ->       (chain over P, the columns of P passed through)
//!       CTE_REF d1
//!       R = chain over AGGREGATE[group by keys](CTE_REF d2)
//!
//! This is the plan of a lateral join whose right side has no table of its own, e.g. FROM t, UNNEST([t.a, t.b]): the
//! left rows are materialized, the right side is computed once per distinct value of the correlated columns and
//! joined back to the left rows on those columns. When the chain consists of operators that map every input row on
//! its own (PROJECTION, FILTER, UNNEST) and has no volatile expression, what it produces for a key and the join
//! pairs with every left row of that key is what it produces from that left row.
class LateralRowFunction {
public:
	explicit LateralRowFunction(Optimizer &optimizer);

	void Optimize(unique_ptr<LogicalOperator> &plan);

private:
	//! The grouping-only aggregate over the domain reference
	struct Domain {
		//! Per group of the aggregate: the CTE column it reads
		vector<idx_t> group_columns;
		//! The table index of the domain reference
		TableIndex table_index;
		//! The filters below the aggregate, over the bindings of the domain reference
		vector<unique_ptr<Expression>> filters;
	};

	struct Candidate {
		//! The INNER join of the left rows with the chain
		optional_ptr<unique_ptr<LogicalOperator>> join;
		//! Which child of the join is the reference to the left rows
		idx_t rows_side = 0;
		//! The operators from the other child of the join down to the operator above the aggregate
		vector<reference<unique_ptr<LogicalOperator>>> chain;
		//! The grouping-only aggregate over the domain reference
		optional_ptr<unique_ptr<LogicalOperator>> aggregate;
		Domain domain;
		//! The CTE columns compared with =: left rows with a NULL there never join
		vector<idx_t> not_null_columns;
	};

private:
	void VisitOperator(unique_ptr<LogicalOperator> &op);
	bool FindCandidate(unique_ptr<LogicalOperator> &cte, Candidate &candidate);
	void Rewrite(unique_ptr<LogicalOperator> &cte, Candidate &candidate);
	static bool MatchDomain(LogicalOperator &op, TableIndex cte_index, Domain &domain);

private:
	Optimizer &optimizer;
};

} // namespace duckdb
