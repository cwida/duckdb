#include "duckdb/execution/index/art/node48.hpp"

#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/node256.hpp"

namespace duckdb {

void Node48::InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node48>();

		if (n.count != CAPACITY) {
			// Still space. Insert the child.
			// Find an empty position in the node list.
			auto child_pos = n.count;
			if (n.children[child_pos].HasMetadata()) {
				child_pos = 0;
				while (n.children[child_pos].HasMetadata()) {
					child_pos++;
				}
			}

			n.children[child_pos] = child_ptr;
			n.child_index[byte] = child_pos;
			n.count++;
			return;
		}
	}

	// The node is full.
	// Grow to Node256.
	auto node48_ptr = node_ptr;
	Node256::GrowNode48(art, node_ptr, node48_ptr);
	Node256::InsertChild(art, node_ptr, byte, child_ptr);
}

void Node48::DeleteChild(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node48>();

		// Free the child and decrease the count.
		NodePtr::FreeTree(art, n.children[n.child_index[byte]]);
		n.child_index[byte] = EMPTY_MARKER;
		n.count--;

		if (n.count >= SHRINK_THRESHOLD) {
			return;
		}
	}

	// Shrink to Node16.
	auto node48_ptr = node_ptr;
	Node16::ShrinkNode48(art, node_ptr, node48_ptr);
}

void Node48::GrowNode16(ART &art, NodePtr &node48_ptr, NodePtr &node16_ptr) {
	{
		NodeHandle n16_handle(art, node16_ptr);
		auto &n16 = n16_handle.Get<Node16>();

		auto n48_handle = New(art, node48_ptr);
		auto &n48 = n48_handle.Get<Node48>();
		node48_ptr.SetGateStatus(node16_ptr.GetGateStatus());

		n48.count = n16.count;
		for (uint8_t i = 0; i < n16.count; i++) {
			n48.child_index[n16.key[i]] = i;
			n48.children[i] = n16.children[i];
		}
	}
	NodePtr::FreeNode(art, node16_ptr);
}

void Node48::ShrinkNode256(ART &art, NodePtr &node48_ptr, NodePtr &node256_ptr) {
	{
		auto n48_handle = New(art, node48_ptr);
		auto &n48 = n48_handle.Get<Node48>();
		node48_ptr.SetGateStatus(node256_ptr.GetGateStatus());

		NodeHandle n256_handle(art, node256_ptr);
		auto &n256 = n256_handle.Get<Node256>();

		n48.count = 0;
		for (uint16_t i = 0; i < Node256::CAPACITY; i++) {
			if (!n256.children[i].HasMetadata()) {
				continue;
			}
			n48.child_index[i] = n48.count;
			n48.children[n48.count] = n256.children[i];
			n48.count++;
		}
	}
	NodePtr::FreeNode(art, node256_ptr);
}

} // namespace duckdb
