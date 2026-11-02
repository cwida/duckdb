//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/optimizer/join_order/join_relation_set.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/unordered_set.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/optimizer/join_order/relation_index.hpp"

namespace duckdb {

//! Set of relations, used in the join graph.
struct JoinRelationSet {
public:
	JoinRelationSet(unsafe_unique_array<RelationIndex> relations, idx_t count);

public:
	string ToString() const;
	bool Empty() const;
	static bool IsSubset(const JoinRelationSet &super, const JoinRelationSet &sub);
	static bool Intersects(const JoinRelationSet &left, const JoinRelationSet &right);

public:
	unsafe_unique_array<RelationIndex> relations;
	idx_t count;
	//! Bitmask over the relation indexes for fast subset/intersection checks
	vector<idx_t> bitmask;
};

//! The JoinRelationTree is a structure holding all the created JoinRelationSet objects and allowing fast lookup on to
//! them
class JoinRelationSetManager {
public:
	//! Contains a node with a JoinRelationSet and child relations
	// FIXME: this structure is inefficient, could use a bitmap for lookup instead (todo: profile)
	struct JoinRelationTreeNode {
		unique_ptr<JoinRelationSet> relation;
		unordered_map<RelationIndex, unique_ptr<JoinRelationTreeNode>> children;
	};

public:
	//! Get an empty JoinRelationSet
	JoinRelationSet &GetEmptyJoinRelationSet();
	//! Create or get a JoinRelationSet from a single node with the given index
	JoinRelationSet &GetJoinRelation(RelationIndex index);
	//! Create or get a JoinRelationSet from a set of relation bindings
	JoinRelationSet &GetJoinRelation(const unordered_set<RelationIndex> &bindings);
	//! Create or get a JoinRelationSet from a (sorted, duplicate-free!) list of relations
	JoinRelationSet &GetJoinRelation(unsafe_unique_array<RelationIndex> relations, idx_t count);
	//! Union two sets of relations together and create a new relation set
	JoinRelationSet &Union(JoinRelationSet &left, JoinRelationSet &right);
	string ToString() const;
	void Print();

private:
	JoinRelationTreeNode root;
	optional_ptr<JoinRelationSet> empty_relation_set;
};

} // namespace duckdb
