#include "duckdb/execution/index/art/base_node.hpp"

#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/prefix.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// BaseNode
//===--------------------------------------------------------------------===//

template <uint8_t CAPACITY, NType NODE_TYPE>
void BaseNode<CAPACITY, NODE_TYPE>::InsertChildInternal(BaseNode &n, const uint8_t byte, const NodePtr child_ptr) {
	// Still space. Insert the child.
	uint8_t child_pos = 0;
	while (child_pos < n.count && n.key[child_pos] < byte) {
		child_pos++;
	}

	// Move children backwards to make space.
	for (uint8_t i = n.count; i > child_pos; i--) {
		n.key[i] = n.key[i - 1];
		n.children[i] = n.children[i - 1];
	}

	n.key[child_pos] = byte;
	n.children[child_pos] = child_ptr;
	n.count++;
}

template <uint8_t CAPACITY, NType NODE_TYPE>
NodeHandle BaseNode<CAPACITY, NODE_TYPE>::DeleteChildInternal(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	NodeHandle handle(art, node_ptr);
	auto &n = handle.Get<BaseNode<CAPACITY, NODE_TYPE>>();

	uint8_t child_pos = 0;
	for (; child_pos < n.count; child_pos++) {
		if (n.key[child_pos] == byte) {
			break;
		}
	}

	// Free the child and decrease the count.
	NodePtr::FreeTree(art, n.children[child_pos]);
	n.count--;

	// Possibly move children backwards.
	for (uint8_t i = child_pos; i < n.count; i++) {
		n.key[i] = n.key[i + 1];
		n.children[i] = n.children[i + 1];
	}

	return handle;
}

//===--------------------------------------------------------------------===//
// Node4
//===--------------------------------------------------------------------===//

void Node4::InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node4>();

		if (n.count != CAPACITY) {
			InsertChildInternal(n, byte, child_ptr);
			return;
		}
	}
	// The node is full.
	// Grow to Node16.
	auto node4_ptr = node_ptr;
	Node16::GrowNode4(art, node_ptr, node4_ptr);
	Node16::InsertChild(art, node_ptr, byte, child_ptr);
}

void Node4::DeleteChild(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte, const GateStatus status) {
	NodePtr child_ptr;
	uint8_t remaining_byte;

	{
		auto handle = DeleteChildInternal(art, node_ptr, byte);
		auto &n = handle.Get<Node4>();

		if (n.count != 1) {
			return;
		}

		// Compress one-way nodes.
		child_ptr = n.children[0];
		remaining_byte = n.key[0];
	}

	auto prev_node4_status = node_ptr.GetGateStatus();
	NodePtr::FreeNode(art, node_ptr);
	// Propagate both the prev_node_4 status and the general gate status (if the gate was earlier on),
	// since the concatenation logic depends on both.
	Prefix::Concat(art, parent_ptr, node_ptr, child_ptr, remaining_byte, prev_node4_status, status);
}

void Node4::ShrinkNode16(ART &art, NodePtr &node4_ptr, NodePtr &node16_ptr) {
	{
		auto n4_handle = New(art, node4_ptr);
		auto &n4 = n4_handle.Get<Node4>();
		node4_ptr.SetGateStatus(node16_ptr.GetGateStatus());

		NodeHandle n16_handle(art, node16_ptr);
		auto &n16 = n16_handle.Get<Node16>();

		n4.count = n16.count;
		for (uint8_t i = 0; i < n16.count; i++) {
			n4.key[i] = n16.key[i];
			n4.children[i] = n16.children[i];
		}
	}
	NodePtr::FreeNode(art, node16_ptr);
}

//===--------------------------------------------------------------------===//
// Node16
//===--------------------------------------------------------------------===//

void Node16::InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node16>();
		if (n.count != CAPACITY) {
			InsertChildInternal(n, byte, child_ptr);
			return;
		}
	}
	// The node is full.
	// Grow to Node48.
	auto node16_ptr = node_ptr;
	Node48::GrowNode16(art, node_ptr, node16_ptr);
	Node48::InsertChild(art, node_ptr, byte, child_ptr);
}

void Node16::DeleteChild(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		auto handle = DeleteChildInternal(art, node_ptr, byte);
		auto &n = handle.Get<Node16>();
		if (n.count >= Node4::CAPACITY) {
			return;
		}
	}
	// Shrink node to Node4.
	auto node16_ptr = node_ptr;
	Node4::ShrinkNode16(art, node_ptr, node16_ptr);
}

void Node16::GrowNode4(ART &art, NodePtr &node16_ptr, NodePtr &node4_ptr) {
	{
		NodeHandle n4_handle(art, node4_ptr);
		auto &n4 = n4_handle.Get<Node4>();

		auto n16_handle = New(art, node16_ptr);
		auto &n16 = n16_handle.Get<Node16>();
		node16_ptr.SetGateStatus(node4_ptr.GetGateStatus());

		n16.count = n4.count;
		for (uint8_t i = 0; i < n4.count; i++) {
			n16.key[i] = n4.key[i];
			n16.children[i] = n4.children[i];
		}
	}
	NodePtr::FreeNode(art, node4_ptr);
}

void Node16::ShrinkNode48(ART &art, NodePtr &node16_ptr, NodePtr &node48_ptr) {
	{
		auto n16_handle = New(art, node16_ptr);
		auto &n16 = n16_handle.Get<Node16>();
		node16_ptr.SetGateStatus(node48_ptr.GetGateStatus());

		NodeHandle n48_handle(art, node48_ptr);
		auto &n48 = n48_handle.Get<Node48>();

		n16.count = 0;
		for (uint16_t i = 0; i < Node256::CAPACITY; i++) {
			if (n48.child_index[i] != Node48::EMPTY_MARKER) {
				n16.key[n16.count] = UnsafeNumericCast<uint8_t>(i);
				n16.children[n16.count] = n48.children[n48.child_index[i]];
				n16.count++;
			}
		}
	}
	NodePtr::FreeNode(art, node48_ptr);
}

} // namespace duckdb
