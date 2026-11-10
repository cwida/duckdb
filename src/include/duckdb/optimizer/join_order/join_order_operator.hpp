//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_order_operator.hpp
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/optimizer/join_order/join_relation_set.hpp"
#include "duckdb/planner/column_binding.hpp"
#include "duckdb/planner/joinside.hpp"

namespace duckdb {

enum class JoinOrderOperatorType : uint8_t { INNER, LEFT, SEMI, ANTI, CROSS_PRODUCT };

struct JoinOrderConflictRule {
	JoinOrderConflictRule(JoinRelationSet &trigger_p, JoinRelationSet &requirement_p)
	    : trigger(trigger_p), requirement(requirement_p) {
	}

	reference<JoinRelationSet> trigger;
	reference<JoinRelationSet> requirement;
};

//! Conflict descriptor for a binary operator occurrence in the original join tree.
//!
//! The representation follows CD-C from "On the Correct and Complete Enumeration of the Core Search Space":
//! syntactic eligibility is expanded into a total eligibility set, while conditional dependencies remain conflict
//! rules evaluated when the operator is considered for a candidate partition.
struct JoinOrderOperator {
	JoinOrderOperator(idx_t index_p, JoinOrderOperatorType type_p, JoinRelationSet &left_relations_p,
	                  JoinRelationSet &right_relations_p, JoinRelationSet &syntactic_set_p,
	                  vector<JoinCondition> conditions_p)
	    : index(index_p), type(type_p), left_relations(left_relations_p), right_relations(right_relations_p),
	      syntactic_set(syntactic_set_p), total_set(syntactic_set_p), left_total_set(syntactic_set_p),
	      right_total_set(syntactic_set_p), conditions(std::move(conditions_p)) {
	}

	idx_t index;
	JoinOrderOperatorType type;
	//! Tables below the operator's original left and right inputs.
	reference<JoinRelationSet> left_relations;
	reference<JoinRelationSet> right_relations;
	//! SES, TES, and their orientation-specific partitions.
	reference<JoinRelationSet> syntactic_set;
	reference<JoinRelationSet> total_set;
	reference<JoinRelationSet> left_total_set;
	reference<JoinRelationSet> right_total_set;
	vector<JoinOrderConflictRule> conflict_rules;
	//! Operators originally below the left and right inputs, used while constructing CD-C rules.
	vector<reference<JoinOrderOperator>> left_operators;
	vector<reference<JoinOrderOperator>> right_operators;
	//! Complete semantic conditions owned by this operator occurrence.
	vector<JoinCondition> conditions;
	//! Predicate copies used only for cardinality estimation and query-graph connectivity.
	vector<idx_t> costing_predicate_indices;
	//! LEFT ON residuals not covered by a costing predicate or an equivalent equality key.
	vector<reference<const Expression>> residual_predicates;
	//! Whether the complete conditions must be applied by this operator occurrence. This is decided before conflict
	//! rule simplification, which can erase conflicts already absorbed by the syntactic eligibility set.
	bool must_apply_as_operator = false;
};

class JoinOrderConflictDetector {
public:
	static void Build(vector<unique_ptr<JoinOrderOperator>> &operators, JoinRelationSetManager &set_manager,
	                  const unordered_map<TableIndex, RelationIndex> &relation_mapping);
	static bool IsApplicable(const JoinOrderOperator &op, const JoinRelationSet &left, const JoinRelationSet &right);
	static bool MustApplyAsOperator(const JoinOrderOperator &op);
	static bool IsCompletedBy(const JoinOrderOperator &op, const JoinRelationSet &relations);
	static bool IsCommutative(JoinOrderOperatorType type);
};

} // namespace duckdb
