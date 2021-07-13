//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_relation.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/unordered_set.hpp"

namespace duckdb {
class LogicalOperator;

//! Represents a single relation and any metadata accompanying that relation
struct SingleJoinRelation {
	LogicalOperator *op;
	LogicalOperator *parent;

	SingleJoinRelation() {
	}
	SingleJoinRelation(LogicalOperator *op, LogicalOperator *parent) : op(op), parent(parent) {
	}
};

//! Set of relations, used in the join graph.
struct JoinRelationSet {
	JoinRelationSet(unique_ptr<idx_t[]> relations, idx_t count) : relations(move(relations)), count(count) {
	}

	string ToString() const;

	unique_ptr<idx_t[]> relations;
	idx_t count;

	static bool IsSubset(JoinRelationSet *super, JoinRelationSet *sub);
};

//! The JoinRelationTree is a structure holding all the created JoinRelationSet objects and allowing fast lookup on to
//! them
class JoinRelationSetManager {
public:
	//! Contains a node with a JoinRelationSet and child relations
	// FIXME: this structure is inefficient, could use a bitmap for lookup instead (todo: profile)
	struct JoinRelationTreeNode {
		unique_ptr<JoinRelationSet> relation;
		unordered_map<idx_t, unique_ptr<JoinRelationTreeNode>> children;
	};

public:
	//! Create or get a JoinRelationSet from a single node with the given index
	JoinRelationSet *GetJoinRelation(idx_t index);
	//! Create or get a JoinRelationSet from a set of relation bindings
	JoinRelationSet *GetJoinRelation(unordered_set<idx_t> &bindings);
	//! Create or get a JoinRelationSet from a (sorted, duplicate-free!) list of relations
	JoinRelationSet *GetJoinRelation(unique_ptr<idx_t[]> relations, idx_t count);
	//! Union two sets of relations together and create a new relation set
	JoinRelationSet *Union(JoinRelationSet *left, JoinRelationSet *right);
	// //! Create the set difference of left \ right (i.e. all elements in left that are not in right)
	// JoinRelationSet *Difference(JoinRelationSet *left, JoinRelationSet *right);

private:
	JoinRelationTreeNode root;
};

} // namespace duckdb
