#include "duckdb/execution/index/art/node256.hpp"

#include "duckdb/execution/index/art/node48.hpp"

namespace duckdb {

void Node256::InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr) {
	NodeHandle handle(art, node_ptr);
	auto &n = handle.Get<Node256>();
	n.count++;
	n.children[byte] = child_ptr;
}

void Node256::DeleteChild(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node256>();

		// Free the child and decrease the count.
		NodePtr::FreeTree(art, n.children[byte]);
		n.count--;

		if (n.count > SHRINK_THRESHOLD) {
			return;
		}
	}

	// Shrink to Node48.
	auto node256_ptr = node_ptr;
	Node48::ShrinkNode256(art, node_ptr, node256_ptr);
}

void Node256::GrowNode48(ART &art, NodePtr &node256_ptr, NodePtr &node48_ptr) {
	{
		NodeHandle n48_handle(art, node48_ptr);
		auto &n48 = n48_handle.Get<Node48>();

		auto n256_handle = New(art, node256_ptr);
		auto &n256 = n256_handle.Get<Node256>();
		node256_ptr.SetGateStatus(node48_ptr.GetGateStatus());

		n256.count = n48.count;
		for (uint16_t i = 0; i < CAPACITY; i++) {
			if (n48.child_index[i] != Node48::EMPTY_MARKER) {
				n256.children[i] = n48.children[n48.child_index[i]];
			}
		}
	}
	NodePtr::FreeNode(art, node48_ptr);
}

} // namespace duckdb
