//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/art_builder.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/common/stack.hpp"

namespace duckdb {

class ARTBuilder {
public:
	ARTBuilder() = delete;
	ARTBuilder(ArenaAllocator &arena, ART &art, const unsafe_vector<ARTKey> &keys, const unsafe_vector<ARTKey> &row_ids)
	    : arena(arena), art(art), keys(keys), row_ids(row_ids) {
	}

public:
	//! Initialize the ART builder by passing a reference to the root node.
	void Init(NodePtr &node_ptr, const idx_t end) {
		s.emplace(node_ptr, 0, end, 0);
	}
	//! Build the ART starting at the first entry in the stack.
	ARTConflictType Build();

private:
	struct NodeEntry {
		NodeEntry() = delete;
		NodeEntry(NodePtr &node_ptr, const idx_t start, const idx_t end, const idx_t depth)
		    : node_ptr(node_ptr), start(start), end(end), depth(depth) {};

		NodePtr &node_ptr;
		idx_t start;
		idx_t end;
		idx_t depth;
	};

	//! The arena holds any temporary memory allocated during the Build phase.
	ArenaAllocator &arena;
	//! The ART holding the node memory.
	ART &art;
	//! The keys to build the ART from.
	const unsafe_vector<ARTKey> &keys;
	//! The row IDs matching the keys.
	const unsafe_vector<ARTKey> &row_ids;
	//! The stack. While merging, NodeEntry elements are pushed onto of the stack.
	stack<NodeEntry> s;
};

} // namespace duckdb
