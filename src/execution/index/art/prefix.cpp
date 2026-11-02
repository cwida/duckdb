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

void Prefix::Concat(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte,
                    const GateStatus node4_status, const GateStatus status) {
	// We have four situations from which we enter here:
	// 1: PREFIX (parent) - Node4 (prev_node4) - PREFIX (child) - INLINED_LEAF, or
	// 2: PREFIX (parent) - Node4 (prev_node4) - INLINED_LEAF (child), or
	// 3: INTERNAL_NODE (parent) - Node4 (prev_node4) - PREFIX (child) - INLINED_LEAF, or
	// 4: INTERNAL_NODE (parent) - Node4 (prev_node4) - INLINED_LEAF (child).

	D_ASSERT(!parent_ptr.IsAnyLeaf());
	D_ASSERT(child_ptr.HasMetadata());

	// The Node4 was a gate.
	if (node4_status == GateStatus::GATE_SET) {
		D_ASSERT(parent_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);
		D_ASSERT(child_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);
		ConcatNode4WasGate(art, node4_ptr, child_ptr, byte);
		return;
	}

	// The child is a gate.
	if (child_ptr.GetGateStatus() == GateStatus::GATE_SET) {
		D_ASSERT(node4_status == GateStatus::GATE_NOT_SET);
		ConcatChildIsGate(art, parent_ptr, node4_ptr, child_ptr, byte);
		return;
	}
	ConcatInternal(art, parent_ptr, node4_ptr, child_ptr, byte, status);
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

Prefix Prefix::GetTail(ART &art, const NodePtr &node_ptr) {
	Prefix prefix(art, node_ptr, true);
	while (prefix.child_slot->GetType() == PREFIX) {
		prefix = Prefix(art, *prefix.child_slot, true);
	}
	return prefix;
}

void Prefix::ConcatInternal(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte,
                            const GateStatus status) {
	if (child_ptr.GetType() == NType::LEAF_INLINED) {
		if (status == GateStatus::GATE_SET) {
			if (parent_ptr.GetType() == NType::PREFIX) {
				// The parent only contained the Node4, so we can now inline 'all the way up',
				// and the gate is no longer nested.
				while (parent_ptr.GetType() == NType::PREFIX) {
					Prefix prefix(art, parent_ptr, true);
					auto temp_ptr = *prefix.child_slot;
					NodePtr::FreeNode(art, parent_ptr);
					parent_ptr = temp_ptr;
				}
				parent_ptr = child_ptr;
				return;
			}
			// The parent is any node inside the gate.
			// Inside gates, inlined row IDs are not prefixed,
			// so we directly inline into the previous Node4.
			node4_ptr = child_ptr;
			return;
		}

		// Not inside a gate.
		if (parent_ptr.GetType() == NType::PREFIX) {
			// Append the byte to the prefix, and then inline the child.
			auto tail = GetTail(art, parent_ptr);
			tail = tail.Append(art, byte);
			*tail.child_slot = child_ptr;
			return;
		}

		auto prefix = NewInternal(art, node4_ptr, &byte, 1, 0);
		*prefix.child_slot = child_ptr;
		return;
	}

	// The child is not inlined.
	if (parent_ptr.GetType() == NType::PREFIX) {
		// Append the byte to the prefix.
		auto tail = GetTail(art, parent_ptr);
		tail = tail.Append(art, byte);

		// Append the child to the prefix.
		if (child_ptr.GetType() == NType::PREFIX) {
			tail.Append(art, child_ptr);
			return;
		}
		*tail.child_slot = child_ptr;
		return;
	}

	// The child is not inlined, and the parent is not a prefix.
	auto prefix = NewInternal(art, node4_ptr, &byte, 1, 0);
	if (child_ptr.GetType() == NType::PREFIX) {
		prefix.Append(art, child_ptr);
		return;
	}
	*prefix.child_slot = child_ptr;
}

void Prefix::ConcatNode4WasGate(ART &art, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte) {
	D_ASSERT(child_ptr.HasMetadata());

	if (child_ptr.GetType() == NType::LEAF_INLINED) {
		// Inside gates, inlined row IDs are not prefixed.
		// The child is inlined, so we inline into the previous Node4.
		// There is no longer a nested leaf.
		node4_ptr = child_ptr;
		return;
	}

	if (child_ptr.GetType() == PREFIX) {
		// At least one more row ID in this gate and the child is a prefix.
		// We create a new prefix of length one containing the remaining byte.
		// Then, we append the child prefix.
		auto prefix = NewInternal(art, node4_ptr, &byte, 1, 0);
		prefix.child_slot->Clear();
		prefix.Append(art, child_ptr);
		node4_ptr.SetGateStatus(GateStatus::GATE_SET);
		return;
	}
	// At least one more row ID in this gate and the child is not a prefix.
	// We create a new prefix of length one containing the remaining byte.
	// then, we append the child.
	auto prefix = NewInternal(art, node4_ptr, &byte, 1, 0);
	*prefix.child_slot = child_ptr;
	node4_ptr.SetGateStatus(GateStatus::GATE_SET);
}

void Prefix::ConcatChildIsGate(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr,
                               uint8_t byte) {
	if (parent_ptr.GetType() != PREFIX) {
		// Create a new prefix at the former position of the Node4,
		// and point it to the gate.
		auto prefix = NewInternal(art, node4_ptr, &byte, 1, 0);
		*prefix.child_slot = child_ptr;
		return;
	}

	// The parent is a prefix (chain), so we need to append the byte to its tail.
	auto tail = GetTail(art, parent_ptr);
	tail = tail.Append(art, byte);
	*tail.child_slot = child_ptr;
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
