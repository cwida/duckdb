//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/base_node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

template <uint8_t CAPACITY, NType NODE_TYPE>
class BaseNode {
	friend class Node4;
	friend class Node16;
	friend class Node48;

public:
	static constexpr NType TYPE = NODE_TYPE;

	BaseNode() = delete;
	BaseNode(const BaseNode &) = delete;
	BaseNode &operator=(const BaseNode &) = delete;

private:
	uint8_t count;
	uint8_t key[CAPACITY];
	NodePtr children[CAPACITY];

public:
	//! Get a new BaseNode handle and initialize the base node.
	static NodeHandle New(ART &art, NodePtr &node_ptr) {
		node_ptr = NodePtr::GetAllocator(art, TYPE).New();
		node_ptr.SetMetadata(static_cast<uint8_t>(TYPE));

		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<BaseNode>();

		// Reset the node (count).
		n.count = 0;
		// Zero-initialize the node.
		for (uint8_t i = 0; i < CAPACITY; i++) {
			n.key[i] = 0;
			n.children[i].Clear();
		}

		return handle;
	}

	//! Replace the child at byte.
	static void ReplaceChild(BaseNode &n, const uint8_t byte, const NodePtr child_ptr) {
		D_ASSERT(n.count != 0);
		for (uint8_t i = 0; i < n.count; i++) {
			if (n.key[i] == byte) {
				auto status = n.children[i].GetGateStatus();
				n.children[i] = child_ptr;

				if (status == GateStatus::GATE_SET && child_ptr.HasMetadata()) {
					n.children[i].SetGateStatus(status);
				}
				return;
			}
		}
	}

	//! Get the child node at byte, if it exists.
	static OptionalNodePtr GetChildNode(const BaseNode &n, const uint8_t byte) {
		for (uint8_t i = 0; i < n.count; i++) {
			if (n.key[i] == byte) {
				if (!n.children[i].HasMetadata()) {
					throw InternalException("empty child i = %d for byte %d in BaseNode::GetChildNode", i, byte);
				}
				return n.children[i];
			}
		}
		return OptionalNodePtr();
	}

	//! Get the first child node >= byte, if it exists, and update byte.
	static OptionalNodePtr GetNextChildNode(const BaseNode &n, uint8_t &byte) {
		for (uint8_t i = 0; i < n.count; i++) {
			if (n.key[i] >= byte) {
				if (!n.children[i].HasMetadata()) {
					throw InternalException("empty child i = %d for byte %d in BaseNode::GetNextChildNode", i,
					                        n.key[i]);
				}
				byte = n.key[i];
				return n.children[i];
			}
		}
		return OptionalNodePtr();
	}

	//! Get the child at byte.
	static unsafe_optional_ptr<NodePtr> GetChild(BaseNode &n, const uint8_t byte, const bool unsafe = false) {
		for (uint8_t i = 0; i < n.count; i++) {
			if (n.key[i] == byte) {
				if (!unsafe && !n.children[i].HasMetadata()) {
					throw InternalException("empty child i = %d for byte %d in BaseNode::GetChild", i, byte);
				}
				return &n.children[i];
			}
		}
		return nullptr;
	}

	//! Get the first child greater than or equal to the byte.
	static unsafe_optional_ptr<NodePtr> GetNextChild(BaseNode &n, uint8_t &byte) {
		for (uint8_t i = 0; i < n.count; i++) {
			if (n.key[i] >= byte) {
				byte = n.key[i];
				return &n.children[i];
			}
		}
		return nullptr;
	}

	//! Extracts the bytes and their respective children.
	//! The return value is valid as long as the arena is valid.
	//! The node must be freed after calling into this function.
	NodeChildren ExtractChildren(ArenaAllocator &arena) {
		auto mem_bytes = arena.AllocateAligned(sizeof(uint8_t) * count);
		array_ptr<uint8_t> bytes(mem_bytes, count);
		auto mem_children = arena.AllocateAligned(sizeof(NodePtr) * count);
		array_ptr<NodePtr> children_ptr(reinterpret_cast<NodePtr *>(mem_children), count);

		for (uint8_t i = 0; i < count; i++) {
			bytes[i] = key[i];
			children_ptr[i] = children[i];
		}

		return NodeChildren(bytes, children_ptr);
	}

public:
	template <class F>
	static void Iterator(BaseNode<CAPACITY, TYPE> &n, F &&lambda) {
		for (uint8_t i = 0; i < n.count; i++) {
			lambda(n.children[i]);
		}
	}

	template <class F>
	static void Iterator(const BaseNode<CAPACITY, TYPE> &n, F &&lambda) {
		for (uint8_t i = 0; i < n.count; i++) {
			lambda(n.children[i]);
		}
	}

private:
	static void InsertChildInternal(BaseNode &n, const uint8_t byte, const NodePtr child_ptr);
	static NodeHandle DeleteChildInternal(ART &art, NodePtr &node_ptr, const uint8_t byte);
};

//! Node4 holds up to four children sorted by their key byte.
class Node4 : public BaseNode<4, NType::NODE_4> {
	friend class Node16;

public:
	static constexpr NType NODE_4 = NType::NODE_4;
	static constexpr uint8_t CAPACITY = 4;

public:
	//! Insert a child at byte.
	static void InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr);
	//! Delete the child at byte.
	static void DeleteChild(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte,
	                        const GateStatus status);

private:
	static void ShrinkNode16(ART &art, NodePtr &node4_ptr, NodePtr &node16_ptr);
};

class Node16 : public BaseNode<16, NType::NODE_16> {
	friend class Node4;
	friend class Node48;

public:
	static constexpr NType NODE_16 = NType::NODE_16;
	static constexpr uint8_t CAPACITY = 16;

public:
	//! Insert a child at byte.
	static void InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr);
	//! Delete the child at byte.
	static void DeleteChild(ART &art, NodePtr &node_ptr, const uint8_t byte);

private:
	static void GrowNode4(ART &art, NodePtr &node16_ptr, NodePtr &node4_ptr);
	//! We shrink at < Node48::SHRINK_THRESHOLD.
	static void ShrinkNode48(ART &art, NodePtr &node16_ptr, NodePtr &node48_ptr);
};

} // namespace duckdb
