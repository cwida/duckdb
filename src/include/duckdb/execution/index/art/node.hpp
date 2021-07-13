//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/common/common.hpp"

namespace duckdb {
enum class NodeType : uint8_t { N4 = 0, N16 = 1, N48 = 2, N256 = 3, NLeaf = 4 };

class ART;

class Node {
public:
	static const uint8_t EMPTY_MARKER = 48;

public:
	Node(ART &art, NodeType type, size_t compressed_prefix_size);
	virtual ~Node() {
	}

	//! length of the compressed path (prefix)
	uint32_t prefix_length;
	//! number of non-null children
	uint16_t count;
	//! node type
	NodeType type;
	//! compressed path (prefix)
	unique_ptr<uint8_t[]> prefix;

public:
	//! Get the position of a child corresponding exactly to the specific byte, returns INVALID_INDEX if not exists
	virtual idx_t GetChildPos(uint8_t k) {
		return INVALID_INDEX;
	}
	//! Get the position of the first child that is greater or equal to the specific byte, or INVALID_INDEX if there are
	//! no children matching the criteria
	virtual idx_t GetChildGreaterEqual(uint8_t k, bool &equal) {
		throw InternalException("Unimplemented GetChildGreaterEqual for ARTNode");
	}
	//! Get the position of the biggest element in node
	virtual idx_t GetMin();
	//! Get the next position in the node, or INVALID_INDEX if there is no next position. if pos == INVALID_INDEX, then
	//! the first valid position in the node will be returned.
	virtual idx_t GetNextPos(idx_t pos) {
		return INVALID_INDEX;
	}
	//! Get the child at the specified position in the node. pos should be between [0, count). Throws an assertion if
	//! the element is not found.
	virtual unique_ptr<Node> *GetChild(idx_t pos);

	//! Compare the key with the prefix of the node, return the number matching bytes
	static uint32_t PrefixMismatch(ART &art, Node *node, Key &key, uint64_t depth);
	//! Insert leaf into inner node
	static void InsertLeaf(ART &art, unique_ptr<Node> &node, uint8_t key, unique_ptr<Node> &new_node);
	//! Erase entry from node
	static void Erase(ART &art, unique_ptr<Node> &node, idx_t pos);

protected:
	//! Copies the prefix from the source to the destination node
	static void CopyPrefix(ART &art, Node *src, Node *dst);
};

} // namespace duckdb
