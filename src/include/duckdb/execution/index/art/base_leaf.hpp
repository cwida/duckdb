//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/base_leaf.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

template <uint8_t CAPACITY, NType NODE_TYPE>
class BaseLeaf {
	friend class Node7Leaf;
	friend class Node15Leaf;
	friend class Node256Leaf;

public:
	static constexpr NType TYPE = NODE_TYPE;

	BaseLeaf() = delete;
	BaseLeaf(const BaseLeaf &) = delete;
	BaseLeaf &operator=(const BaseLeaf &) = delete;

private:
	uint8_t count;
	uint8_t key[CAPACITY];

public:
	//! Get a new BaseLeaf and initialize it.
	static NodeHandle New(ART &art, NodePtr &node_ptr) {
		node_ptr = NodePtr::GetAllocator(art, TYPE).New();
		node_ptr.SetMetadata(static_cast<uint8_t>(TYPE));

		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<BaseLeaf>();

		n.count = 0;
		return handle;
	}

	//! Returns true, if the byte exists, else false.
	bool HasByte(const uint8_t byte) const {
		for (uint8_t i = 0; i < count; i++) {
			if (key[i] == byte) {
				return true;
			}
		}
		return false;
	}

	//! Returns a pointer to the bytes in the leaf.
	//! The pointer's data is valid as long as the leaf is valid.
	array_ptr<uint8_t> GetBytes() {
		return array_ptr<uint8_t>(key, count);
	}

	//! Get the first byte greater than or equal to the byte.
	//! Returns true, if such a byte exists, else false.
	bool GetNextByte(uint8_t &byte) const {
		for (uint8_t i = 0; i < count; i++) {
			if (key[i] >= byte) {
				byte = key[i];
				return true;
			}
		}
		return false;
	}

private:
	static void InsertByteInternal(BaseLeaf &n, const uint8_t byte);
	static NodeHandle DeleteByteInternal(ART &art, NodePtr &node_ptr, const uint8_t byte);
};

//! Node7Leaf holds up to seven sorted bytes.
class Node7Leaf : public BaseLeaf<7, NType::NODE_7_LEAF> {
	friend class Node15Leaf;

public:
	static constexpr NType NODE_7_LEAF = NType::NODE_7_LEAF;
	static constexpr uint8_t CAPACITY = 7;
	static constexpr idx_t AND_LAST_BYTE = 0xFFFFFFFFFFFFFF00;

public:
	//! Insert a byte.
	static void InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte);
	//! Delete a byte.
	static void DeleteByte(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte, const ARTKey &row_id);

private:
	static void ShrinkNode15Leaf(ART &art, NodePtr &node7_leaf_ptr, NodePtr &node15_leaf_ptr);
};

//! Node15Leaf holds up to 15 sorted bytes.
class Node15Leaf : public BaseLeaf<15, NType::NODE_15_LEAF> {
	friend class Node7Leaf;
	friend class Node256Leaf;

public:
	static constexpr NType NODE_15_LEAF = NType::NODE_15_LEAF;
	static constexpr uint8_t CAPACITY = 15;

public:
	//! Insert a byte.
	static void InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte);
	//! Delete a byte.
	static void DeleteByte(ART &art, NodePtr &node_ptr, const uint8_t byte);

private:
	static void GrowNode7Leaf(ART &art, NodePtr &node15_leaf_ptr, NodePtr &node7_leaf_ptr);
	//! We shrink at <= Node48::SHRINK_THRESHOLD.
	static void ShrinkNode256Leaf(ART &art, NodePtr &node15_leaf_ptr, NodePtr &node256_leaf_ptr);
};

} // namespace duckdb
