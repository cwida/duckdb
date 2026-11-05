//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node256.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

//! Node256 holds up to 256 children. They are indexed by their key byte.
class Node256 {
	friend class Node48;

public:
	static constexpr NType NODE_256 = NType::NODE_256;
	static constexpr NType TYPE = NType::NODE_256;
	static constexpr uint16_t CAPACITY = 256;
	static constexpr uint8_t SHRINK_THRESHOLD = 36;

public:
	Node256() = delete;
	Node256(const Node256 &) = delete;
	Node256 &operator=(const Node256 &) = delete;

private:
	uint16_t count;
	NodePtr children[CAPACITY];

public:
	//! Get a new Node256 handle and initialize the Node256.
	static NodeHandle New(ART &art, NodePtr &node_ptr) {
		node_ptr = NodePtr::GetAllocator(art, NODE_256).New();
		node_ptr.SetMetadata(static_cast<uint8_t>(NODE_256));

		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node256>();

		// Reset the node (count and children).
		n.count = 0;
		for (uint16_t i = 0; i < CAPACITY; i++) {
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
		D_ASSERT(count > SHRINK_THRESHOLD);
		auto status = children[byte].GetGateStatus();
		children[byte] = child_ptr;
		if (status == GateStatus::GATE_SET && child_ptr.HasMetadata()) {
			children[byte].SetGateStatus(status);
		}
	}

public:
	template <class F, class NODE>
	static void Iterator(NODE &n, F &&lambda) {
		D_ASSERT(n.count);
		for (idx_t i = 0; i < CAPACITY; i++) {
			if (n.children[i].HasMetadata()) {
				lambda(n.children[i]);
			}
		}
	}

	//! Get the child node at byte, if it exists.
	static OptionalNodePtr GetChildNode(const Node256 &n, const uint8_t byte) {
		if (n.children[byte].HasMetadata()) {
			return n.children[byte];
		}
		return OptionalNodePtr();
	}

	//! Get the first child node >= byte, if it exists, and update byte.
	static OptionalNodePtr GetNextChildNode(const Node256 &n, uint8_t &byte) {
		for (idx_t i = byte; i < CAPACITY; i++) {
			if (n.children[i].HasMetadata()) {
				byte = UnsafeNumericCast<uint8_t>(i);
				return n.children[i];
			}
		}
		return OptionalNodePtr();
	}

	template <class NODE>
	static unsafe_optional_ptr<NodePtr> GetChild(NODE &n, const uint8_t byte, const bool unsafe = false) {
		if (unsafe) {
			return &n.children[byte];
		}
		if (n.children[byte].HasMetadata()) {
			return &n.children[byte];
		}
		return nullptr;
	}

	template <class NODE>
	static unsafe_optional_ptr<NodePtr> GetNextChild(NODE &n, uint8_t &byte) {
		for (idx_t i = byte; i < CAPACITY; i++) {
			if (n.children[i].HasMetadata()) {
				byte = UnsafeNumericCast<uint8_t>(i);
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

		uint16_t ptr_idx = 0;
		for (idx_t i = 0; i < CAPACITY; i++) {
			if (children[i].HasMetadata()) {
				bytes[ptr_idx] = UnsafeNumericCast<uint8_t>(i);
				children_ptr[ptr_idx] = children[i];
				ptr_idx++;
			}
		}

		return NodeChildren(bytes, children_ptr);
	}

private:
	static void GrowNode48(ART &art, NodePtr &node256_ptr, NodePtr &node48_ptr);
};
} // namespace duckdb
