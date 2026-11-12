#include "duckdb/execution/index/art/prefix.hpp"

#include "duckdb/common/swap.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/const_prefix_handle.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/prefix_handle.hpp"

namespace duckdb {

Prefix::Prefix(const ART &art, const NodePtr node_ptr, const bool is_mutable, const bool set_in_memory) {
	if (!set_in_memory) {
		data = NodePtr::GetAllocator(art, PREFIX).Get(node_ptr, is_mutable);
	} else {
		data = NodePtr::GetAllocator(art, PREFIX).GetIfLoaded(node_ptr);
		if (!data) {
			child_slot = nullptr;
			in_memory = false;
			return;
		}
	}
	child_slot = &PrefixHandle::ChildRefWithCount(data, art.PrefixCount());
	in_memory = true;
}

Prefix::Prefix(FixedSizeAllocator &allocator, const NodePtr node_ptr, const idx_t count) {
	data = allocator.Get(node_ptr, true);
	child_slot = &PrefixHandle::ChildRefWithCount(data, count);
	in_memory = true;
}

uint8_t Prefix::GetByte(const ART &art, const NodePtr &node_ptr, const uint8_t pos) {
	D_ASSERT(node_ptr.GetType() == PREFIX);
	ConstPrefixHandle prefix(art, node_ptr);
	return prefix.GetByte(pos);
}

Prefix Prefix::NewInternal(ART &art, NodePtr &node_ptr, const data_ptr_t data, const uint8_t count,
                           const idx_t offset) {
	node_ptr = NodePtr::GetAllocator(art, PREFIX).New();
	node_ptr.SetMetadata(static_cast<uint8_t>(PREFIX));

	Prefix prefix(art, node_ptr, true);
	prefix.data[art.PrefixCount()] = count;
	if (data) {
		D_ASSERT(count);
		memcpy(prefix.data, data + offset, count);
	}
	prefix.child_slot->Clear();
	return prefix;
}

void Prefix::New(ART &art, reference<NodePtr> &node_ptr_ref, const ARTKey &key, const idx_t depth, idx_t count) {
	idx_t offset = 0;

	while (count) {
		auto min = MinValue(UnsafeNumericCast<idx_t>(art.PrefixCount()), count);
		auto this_count = UnsafeNumericCast<uint8_t>(min);
		auto prefix = NewInternal(art, node_ptr_ref, key.data, this_count, offset + depth);

		node_ptr_ref = *prefix.child_slot;
		offset += this_count;
		count -= this_count;
	}
}

void Prefix::Reduce(ART &art, NodePtr &node_ptr, const idx_t pos) {
	D_ASSERT(node_ptr.HasMetadata());
	D_ASSERT(pos < art.PrefixCount());

	// We always reduce by at least one byte,
	// thus, if the prefix was a gate, it no longer is.
	node_ptr.SetGateStatus(GateStatus::GATE_NOT_SET);

	Prefix prefix(art, node_ptr);
	if (pos == idx_t(prefix.data[art.PrefixCount()] - 1)) {
		auto next_ptr = *prefix.child_slot;
		NodePtr::FreeNode(art, node_ptr);
		node_ptr = next_ptr;
		return;
	}

	// FIXME: Copy into new prefix (chain) instead of shifting.
	for (idx_t i = 0; i < art.PrefixCount() - pos - 1; i++) {
		prefix.data[i] = prefix.data[pos + i + 1];
	}

	prefix.data[art.PrefixCount()] -= pos + 1;
	prefix.Append(art, *prefix.child_slot);
}

Prefix Prefix::Append(ART &art, const uint8_t byte) {
	if (data[art.PrefixCount()] != art.PrefixCount()) {
		data[data[art.PrefixCount()]] = byte;
		data[art.PrefixCount()]++;
		return *this;
	}

	auto prefix = NewInternal(art, *child_slot, nullptr, 0, 0);
	return prefix.Append(art, byte);
}

void Prefix::Append(ART &art, NodePtr other_ptr) {
	D_ASSERT(other_ptr.HasMetadata());

	Prefix prefix = *this;
	while (other_ptr.GetType() == PREFIX) {
		if (other_ptr.GetGateStatus() == GateStatus::GATE_SET) {
			*prefix.child_slot = other_ptr;
			return;
		}

		Prefix other_prefix(art, other_ptr, true);
		for (idx_t i = 0; i < other_prefix.data[art.PrefixCount()]; i++) {
			prefix = prefix.Append(art, other_prefix.data[i]);
		}

		*prefix.child_slot = *other_prefix.child_slot;
		NodePtr::FreeNode(art, other_ptr);
		other_ptr = *prefix.child_slot;
	}
}

Prefix Prefix::TransformToDeprecatedAppend(ART &art, FixedSizeAllocator &allocator, uint8_t byte) {
	if (data[DEPRECATED_COUNT] != DEPRECATED_COUNT) {
		data[data[DEPRECATED_COUNT]] = byte;
		data[DEPRECATED_COUNT]++;
		return *this;
	}

	*child_slot = allocator.New();
	child_slot->SetMetadata(static_cast<uint8_t>(PREFIX));
	Prefix prefix(allocator, *child_slot, DEPRECATED_COUNT);
	return prefix.TransformToDeprecatedAppend(art, allocator, byte);
}

} // namespace duckdb
