#include "duckdb/execution/index/art/node256_leaf.hpp"

#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/node48.hpp"

namespace duckdb {

NodeHandle Node256Leaf::New(ART &art, NodePtr &node_ptr) {
	node_ptr = NodePtr::GetAllocator(art, NODE_256_LEAF).New();
	node_ptr.SetMetadata(static_cast<uint8_t>(NODE_256_LEAF));

	NodeHandle handle(art, node_ptr);
	auto &n = handle.Get<Node256Leaf>();

	n.count = 0;
	ValidityMask mask(&n.mask[0], Node256::CAPACITY);
	mask.SetAllInvalid(CAPACITY);

	return handle;
}

void Node256Leaf::InsertByte(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	NodeHandle handle(art, node_ptr);
	auto &n = handle.Get<Node256Leaf>();

	n.count++;
	ValidityMask mask(&n.mask[0], Node256::CAPACITY);
	mask.SetValid(byte);
}

void Node256Leaf::DeleteByte(ART &art, NodePtr &node_ptr, const uint8_t byte) {
	{
		NodeHandle handle(art, node_ptr);
		auto &n = handle.Get<Node256Leaf>();

		n.count--;
		ValidityMask mask(&n.mask[0], Node256::CAPACITY);
		mask.SetInvalid(byte);

		if (n.count > Node48::SHRINK_THRESHOLD) {
			return;
		}
	}
	// Shrink node to Node15.
	auto node256_leaf_ptr = node_ptr;
	Node15Leaf::ShrinkNode256Leaf(art, node_ptr, node256_leaf_ptr);
}

bool Node256Leaf::HasByte(const uint8_t byte) const {
	idx_t entry_idx = 0;
	idx_t idx_in_entry = 0;
	ValidityMask::GetEntryIndex(byte, entry_idx, idx_in_entry);
	return ValidityMask::RowIsValid(mask[entry_idx], idx_in_entry);
}

array_ptr<uint8_t> Node256Leaf::GetBytes(ArenaAllocator &arena) {
	auto mem = arena.AllocateAligned(sizeof(uint8_t) * count);
	array_ptr<uint8_t> bytes(mem, count);

	ValidityMask v_mask(&mask[0], Node256::CAPACITY);
	uint16_t ptr_idx = 0;
	for (uint16_t i = 0; i < CAPACITY; i++) {
		if (v_mask.RowIsValid(i)) {
			bytes[ptr_idx++] = UnsafeNumericCast<uint8_t>(i);
		}
	}

	return bytes;
}

bool Node256Leaf::GetNextByte(uint8_t &byte) const {
	for (uint16_t i = byte; i < CAPACITY; i++) {
		idx_t entry_idx = 0;
		idx_t idx_in_entry = 0;
		ValidityMask::GetEntryIndex(i, entry_idx, idx_in_entry);
		if (ValidityMask::RowIsValid(mask[entry_idx], idx_in_entry)) {
			byte = UnsafeNumericCast<uint8_t>(i);
			return true;
		}
	}

	return false;
}

void Node256Leaf::GrowNode15Leaf(ART &art, NodePtr &node256_leaf_ptr, NodePtr &node15_leaf_ptr) {
	{
		NodeHandle n15_handle(art, node15_leaf_ptr);
		auto &n15 = n15_handle.Get<Node15Leaf>();

		auto n256_handle = New(art, node256_leaf_ptr);
		auto &n256 = n256_handle.Get<Node256Leaf>();
		node256_leaf_ptr.SetGateStatus(node15_leaf_ptr.GetGateStatus());

		n256.count = n15.count;
		ValidityMask mask(&n256.mask[0], Node256::CAPACITY);
		for (uint8_t i = 0; i < n15.count; i++) {
			mask.SetValid(n15.key[i]);
		}
	}
	NodePtr::FreeNode(art, node15_leaf_ptr);
}

} // namespace duckdb
