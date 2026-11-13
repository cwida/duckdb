#include "duckdb/execution/index/art/base_leaf.hpp"

#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/execution/index/art/node256_leaf.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// BaseLeaf
//===--------------------------------------------------------------------===//

template <uint8_t CAPACITY, NType NODE_TYPE>
void BaseLeaf<CAPACITY, NODE_TYPE>::InsertByteInternal(BaseLeaf &n, const uint8_t byte) {
	// Still space. Insert the child.
	uint8_t child_pos = 0;
	while (child_pos < n.count && n.key[child_pos] < byte) {
		child_pos++;
	}

	// Move children backwards to make space.
	for (uint8_t i = n.count; i > child_pos; i--) {
		n.key[i] = n.key[i - 1];
	}

	n.key[child_pos] = byte;
	n.count++;
}

template <uint8_t CAPACITY, NType NODE_TYPE>
NodeHandle BaseLeaf<CAPACITY, NODE_TYPE>::DeleteByteInternal(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	NodeHandle handle(art, node_ptr);
	auto &n = handle.Get<BaseLeaf<CAPACITY, NODE_TYPE>>();
	uint8_t child_pos = 0;

	for (; child_pos < n.count; child_pos++) {
		if (n.key[child_pos] == byte) {
			break;
		}
	}
	n.count--;

	// Possibly move children backwards.
	for (uint8_t i = child_pos; i < n.count; i++) {
		n.key[i] = n.key[i + 1];
	}
	return handle;
}

//===--------------------------------------------------------------------===//
// Node7Leaf
//===--------------------------------------------------------------------===//

void Node7Leaf::InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n7 = handle.Get<Node7Leaf>();

		if (n7.count != CAPACITY) {
			InsertByteInternal(n7, byte);
			return;
		}
	}
	// The node is full. Grow to Node15.
	auto node7_leaf_ptr = node_ptr;
	Node15Leaf::GrowNode7Leaf(art, node_ptr, node7_leaf_ptr);
	Node15Leaf::InsertByte(art, node_ptr, byte);
}

void Node7Leaf::DeleteByte(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte, const ARTKey &row_id) {
	idx_t remainder;
	{
		auto n7_handle = DeleteByteInternal(art, node_ptr, byte);
		auto &n7 = n7_handle.Get<Node7Leaf>();

		if (n7.count != 1) {
			return;
		}

		// Compress one-way nodes.
		D_ASSERT(node_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);

		// Get the remaining row ID.
		remainder = UnsafeNumericCast<idx_t>(row_id.GetRowId()) & AND_LAST_BYTE;
		remainder |= UnsafeNumericCast<idx_t>(n7.key[0]);
	}
	// Free the prefix (nodes) and inline the remainder.
	if (parent_ptr.GetType() == NType::PREFIX) {
		NodePtr::FreeTree(art, parent_ptr);
		Leaf::New(parent_ptr, UnsafeNumericCast<row_t>(remainder));
		return;
	}
	// Free the Node7Leaf and inline the remainder.
	NodePtr::FreeNode(art, node_ptr);
	Leaf::New(node_ptr, UnsafeNumericCast<row_t>(remainder));
}

void Node7Leaf::ShrinkNode15Leaf(ART &art, NodePtr &node7_leaf_ptr, NodePtr &node15_leaf_ptr) {
	{
		auto n7_handle = New(art, node7_leaf_ptr);
		auto &n7 = n7_handle.Get<Node7Leaf>();

		NodeHandle n15_handle(art, node15_leaf_ptr);
		auto &n15 = n15_handle.Get<Node15Leaf>();

		node7_leaf_ptr.SetGateStatus(node15_leaf_ptr.GetGateStatus());

		n7.count = n15.count;
		for (uint8_t i = 0; i < n15.count; i++) {
			n7.key[i] = n15.key[i];
		}
	}
	NodePtr::FreeNode(art, node15_leaf_ptr);
}

//===--------------------------------------------------------------------===//
// Node15Leaf
//===--------------------------------------------------------------------===//

void Node15Leaf::InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		NodeHandle n15_handle(art, node_ptr);
		auto &n15 = n15_handle.Get<Node15Leaf>();
		if (n15.count != CAPACITY) {
			InsertByteInternal(n15, byte);
			return;
		}
	}
	auto node15_leaf_ptr = node_ptr;
	Node256Leaf::GrowNode15Leaf(art, node_ptr, node15_leaf_ptr);
	Node256Leaf::InsertByte(art, node_ptr, byte);
}

void Node15Leaf::DeleteByte(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		auto n15_handle = DeleteByteInternal(art, node_ptr, byte);
		auto &n15 = n15_handle.Get<Node15Leaf>();
		if (n15.count >= Node7Leaf::CAPACITY) {
			return;
		}
	}
	auto node15_leaf_ptr = node_ptr;
	Node7Leaf::ShrinkNode15Leaf(art, node_ptr, node15_leaf_ptr);
}

void Node15Leaf::GrowNode7Leaf(ART &art, NodePtr &node15_leaf_ptr, NodePtr &node7_leaf_ptr) {
	{
		NodeHandle n7_handle(art, node7_leaf_ptr);
		auto &n7 = n7_handle.Get<Node7Leaf>();

		auto n15_handle = New(art, node15_leaf_ptr);
		auto &n15 = n15_handle.Get<Node15Leaf>();
		node15_leaf_ptr.SetGateStatus(node7_leaf_ptr.GetGateStatus());

		n15.count = n7.count;
		for (uint8_t i = 0; i < n7.count; i++) {
			n15.key[i] = n7.key[i];
		}
	}
	NodePtr::FreeNode(art, node7_leaf_ptr);
}

void Node15Leaf::ShrinkNode256Leaf(ART &art, NodePtr &node15_leaf_ptr, NodePtr &node256_leaf_ptr) {
	{
		auto n15_handle = New(art, node15_leaf_ptr);
		auto &n15 = n15_handle.Get<Node15Leaf>();

		NodeHandle n256_handle(art, node256_leaf_ptr);
		auto &n256 = n256_handle.Get<Node256Leaf>();

		node15_leaf_ptr.SetGateStatus(node256_leaf_ptr.GetGateStatus());

		ValidityMask mask(&n256.mask[0], Node256::CAPACITY);
		for (uint16_t i = 0; i < Node256::CAPACITY; i++) {
			if (mask.RowIsValid(i)) {
				n15.key[n15.count] = UnsafeNumericCast<uint8_t>(i);
				n15.count++;
			}
		}
	}
	NodePtr::FreeNode(art, node256_leaf_ptr);
}

} // namespace duckdb
