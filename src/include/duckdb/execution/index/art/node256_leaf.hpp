//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node256_leaf.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/node256.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"

namespace duckdb {

//! Node256Leaf is a bitmask containing 256 bits.
class Node256Leaf {
	friend class Node15Leaf;

public:
	static constexpr NType NODE_256_LEAF = NType::NODE_256_LEAF;
	static constexpr NType TYPE = NType::NODE_256_LEAF;
	static constexpr uint16_t CAPACITY = Node256::CAPACITY;

public:
	Node256Leaf() = delete;
	Node256Leaf(const Node256Leaf &) = delete;
	Node256Leaf &operator=(const Node256Leaf &) = delete;

private:
	uint16_t count;
	validity_t mask[CAPACITY / ValidityMask::BITS_PER_VALUE];

public:
	//! Get a new Node256Leaf handle and initialize the leaf.
	static NodeHandle New(ART &art, NodePtr &node_ptr);

	//! Insert a byte.
	static void InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte);
	//! Delete a byte.
	static void DeleteByte(ART &art, NodePtr &node_ptr, const uint8_t byte);

	//! Returns true, if the byte exists, else false.
	bool HasByte(const uint8_t byte) const;

	//! Returns a pointer to the bytes in the leaf.
	//! The pointer data is valid as long as the arena is valid.
	array_ptr<uint8_t> GetBytes(ArenaAllocator &arena);

	//! Get the first byte greater or equal to the byte.
	//! Returns true, if such a byte exists, else false.
	bool GetNextByte(uint8_t &byte) const;

private:
	static void GrowNode15Leaf(ART &art, NodePtr &node256_leaf_ptr, NodePtr &node15_leaf_ptr);
};

} // namespace duckdb
