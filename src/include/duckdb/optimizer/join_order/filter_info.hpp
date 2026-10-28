//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/filter_info.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/enums/join_type.hpp"
#include "duckdb/common/optional_idx.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/planner/column_binding.hpp"

namespace duckdb {

//! Filter info struct that is used by the cardinality estimator to set the initial cardinality
//! but is also eventually transformed into a query edge.
class FilterInfo {
public:
	FilterInfo(unique_ptr<Expression> filter, JoinRelationSet &set, idx_t filter_index,
	           JoinType join_type = JoinType::INNER);
	~FilterInfo();

public:
	void SetLeftSet(optional_ptr<JoinRelationSet> left_set_new);
	void SetRightSet(optional_ptr<JoinRelationSet> right_set_new);

public:
	unique_ptr<Expression> filter;
	reference<JoinRelationSet> set;
	idx_t filter_index;
	JoinType join_type;
	optional_ptr<JoinRelationSet> left_set;
	optional_ptr<JoinRelationSet> right_set;
	ColumnBinding left_binding;
	ColumnBinding right_binding;
	bool from_residual_predicate = false;
	//! Whether this predicate originated from a LogicalFilter rather than a join condition.
	bool from_logical_filter = false;
	//! Logical filters above a LEFT join can depend on NULL-extended rows and must remain filters.
	bool must_remain_filter = false;
	//! Filters carrying a barrier must not be pushed below any join in the reordered plan - they are only
	//! placed once the entire join tree has been built.
	bool must_remain_at_root = false;
	//! Index of the equivalence group for INNER equality/IS NOT DISTINCT FROM join filters.
	//! All filters transitively connected by equality (a=b, b=c -> a=c all share the same index).
	//! Used by cardinality estimation to skip redundant transitive conditions.
	optional_idx edge_equivalence_index;
	//! Original binary operator that supplied this costing predicate, if any.
	optional_idx source_operator_index;
	//! Semantic operator sides, including relations not referenced by this predicate.
	optional_ptr<JoinRelationSet> semantic_left_set;
	optional_ptr<JoinRelationSet> semantic_right_set;
};

} // namespace duckdb
