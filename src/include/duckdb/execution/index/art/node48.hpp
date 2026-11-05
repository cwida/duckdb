//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node48.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/node256.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"

namespace duckdb {

//! Node48 holds up to 48 children. The child_index array is indexed by the key byte.
//! It contains the position of the child node in the children array.
class Node48 {
	friend class Node16;
	friend class Node256;

public:
	static constexpr NType NODE_48 = NType::NODE_48;
	static constexpr NType TYPE = NType::NODE_48;
	static constexpr uint8_t CAPACITY = 48;
	static constexpr uint8_t EMPTY_MARKER = 48;
	static constexpr uint8_t SHRINK_THRESHOLD = 12;

public:
	Node48() = delete;
	Node48(const Node48 &) = delete;
	Node48 &operator=(const Node48 &) = delete;

private:
	uint8_t count;
	uint8_t child_index[Node256::CAPACITY];
	NodePtr children[CAPACITY];

public:
	//! Get a new Node48 handle and initialize the Node48.
	static NodeHandle New(ART &art, NodePtr &node_ptr) {
		node_ptr = NodePtr::GetAllocator(art, NODE_48).New();
		node_ptr.SetMetadata(static_cast<uint8_t>(NODE_48));

		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node48>();

		// Reset the node (count and child_index).
		n.count = 0;
		for (uint16_t i = 0; i < Node256::CAPACITY; i++) {
			n.child_index[i] = EMPTY_MARKER;
		}
		// Zero-initialize the node.
		for (uint8_t i = 0; i < CAPACITY; i++) {
			n.children[i].Clear();
		}

		return handle;
	}

	//! Insert a child at byte.
	static void InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr);
	//! Delete the child at byte.
	static void DeleteChild(ART &art, NodePtr &node_ptr, const uint8_t byte);
	//! Replace the child at byte.
	void ReplaceChild(const uint8_t byte, const NodePtr child_ptr) {
		D_ASSERT(count >= SHRINK_THRESHOLD);
		auto status = children[child_index[byte]].GetGateStatus();
		children[child_index[byte]] = child_ptr;
		if (status == GateStatus::GATE_SET && child_ptr.HasMetadata()) {
			children[child_index[byte]].SetGateStatus(status);
		}
	}

public:
	template <class F, class NODE>
	static void Iterator(NODE &n, F &&lambda) {
		D_ASSERT(n.count);
		for (idx_t i = 0; i < Node256::CAPACITY; i++) {
			if (n.child_index[i] != EMPTY_MARKER) {
				lambda(n.children[n.child_index[i]]);
			}
		}
	}

	//! Get the child node at byte, if it exists.
	static OptionalNodePtr GetChildNode(const Node48 &n, const uint8_t byte) {
		if (n.child_index[byte] != EMPTY_MARKER) {
			if (!n.children[n.child_index[byte]].HasMetadata()) {
				throw InternalException("empty child for byte %d in Node48::GetChildNode", byte);
			}
			return n.children[n.child_index[byte]];
		}
		return OptionalNodePtr();
	}

	//! Get the first child node >= byte, if it exists, and update byte.
	static OptionalNodePtr GetNextChildNode(const Node48 &n, uint8_t &byte) {
		for (idx_t i = byte; i < Node256::CAPACITY; i++) {
			if (n.child_index[i] != EMPTY_MARKER) {
				if (!n.children[n.child_index[i]].HasMetadata()) {
					throw InternalException("empty child for byte %d in Node48::GetNextChildNode",
					                        UnsafeNumericCast<uint8_t>(i));
				}
				byte = UnsafeNumericCast<uint8_t>(i);
				return n.children[n.child_index[i]];
			}
		}
		return OptionalNodePtr();
	}

	template <class NODE>
	static unsafe_optional_ptr<NodePtr> GetChild(NODE &n, const uint8_t byte, const bool unsafe = false) {
		if (n.child_index[byte] != Node48::EMPTY_MARKER) {
			if (!unsafe && !n.children[n.child_index[byte]].HasMetadata()) {
				throw InternalException("empty child for byte %d in Node48::GetChild", byte);
			}
			return &n.children[n.child_index[byte]];
		}
		return nullptr;
	}

	template <class NODE>
	static unsafe_optional_ptr<NodePtr> GetNextChild(NODE &n, uint8_t &byte) {
		for (idx_t i = byte; i < Node256::CAPACITY; i++) {
			if (n.child_index[i] != EMPTY_MARKER) {
				byte = UnsafeNumericCast<uint8_t>(i);
				return &n.children[n.child_index[i]];
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

		uint16_t ptr_idx = 0;
		for (idx_t i = 0; i < Node256::CAPACITY; i++) {
			if (child_index[i] != EMPTY_MARKER) {
				bytes[ptr_idx] = UnsafeNumericCast<uint8_t>(i);
				children_ptr[ptr_idx] = children[child_index[i]];
				ptr_idx++;
			}
		}

		return NodeChildren(bytes, children_ptr);
	}

private:
	static void GrowNode16(ART &art, NodePtr &node48_ptr, NodePtr &node16_ptr);
	//! We shrink at <= Node256::SHRINK_THRESHOLD.
	static void ShrinkNode256(ART &art, NodePtr &node48_ptr, NodePtr &node256_ptr);
};
} // namespace duckdb
