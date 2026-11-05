#include "duckdb/execution/index/art/prefix_handle.hpp"

#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

PrefixHandle PrefixHandle::NewInternal(ART &art, NodePtr &node_ptr, const_data_ptr_t data, const uint8_t count,
                                       const idx_t offset) {
	node_ptr = NodePtr::GetAllocator(art, PREFIX).New();
	node_ptr.SetMetadata(static_cast<uint8_t>(PREFIX));

	PrefixHandle prefix(NodeHandle(art, node_ptr));
	prefix.SetCount(art, count);
	if (data) {
		D_ASSERT(count);
		memcpy(prefix.Data(), data + offset, count);
	}
	prefix.Child(art).Clear();
	return prefix;
}

PrefixChain PrefixHandle::New(ART &art, const ARTKey &key, const idx_t depth, const idx_t count) {
	D_ASSERT(count > 0);

	NodePtr root_ptr;
	auto root_count = UnsafeNumericCast<uint8_t>(MinValue<idx_t>(art.PrefixCount(), count));
	auto prefix_handle = NewInternal(art, root_ptr, key.data, root_count, depth);
	auto tail_handle = std::move(prefix_handle);

	idx_t offset = root_count;
	while (offset < count) {
		auto this_count = UnsafeNumericCast<uint8_t>(MinValue<idx_t>(art.PrefixCount(), count - offset));
		auto next_handle = NewInternal(art, tail_handle.Child(art), key.data, this_count, depth + offset);
		tail_handle = std::move(next_handle);

		offset += this_count;
	}
	return {root_ptr, std::move(tail_handle)};
}

PrefixHandle PrefixHandle::AppendByte(ART &art, PrefixHandle prefix, const uint8_t byte) {
	const auto count = prefix.GetCount(art);
	if (count != art.PrefixCount()) {
		prefix.SetByte(count, byte);
		prefix.SetCount(art, UnsafeNumericCast<uint8_t>(count + 1));
		return prefix;
	}

	return NewInternal(art, prefix.Child(art), &byte, 1, 0);
}

void PrefixHandle::Append(ART &art, PrefixHandle prefix, NodePtr other_ptr) {
	D_ASSERT(other_ptr.HasMetadata());

	while (other_ptr.GetType() == PREFIX) {
		if (other_ptr.GetGateStatus() == GateStatus::GATE_SET) {
			prefix.Child(art) = other_ptr;
			return;
		}

		NodePtr next_ptr;
		{
			PrefixHandle other_prefix(NodeHandle(art, other_ptr));
			const auto count = other_prefix.GetCount(art);
			for (idx_t i = 0; i < count; i++) {
				prefix = AppendByte(art, std::move(prefix), other_prefix.GetByte(i));
			}
			next_ptr = other_prefix.Child(art);
			prefix.Child(art) = next_ptr;
		}

		NodePtr::FreeNode(art, other_ptr);
		other_ptr = next_ptr;
	}
	prefix.Child(art) = other_ptr;
}

NodePtr PrefixHandle::Split(ART &art, NodePtr &prefix_ptr, NodePtr &branching_node4_ptr, const uint8_t pos) {
	D_ASSERT(prefix_ptr.HasMetadata());
	D_ASSERT(prefix_ptr.GetType() == PREFIX);
	D_ASSERT(branching_node4_ptr.HasMetadata());

	NodePtr child_ptr;
	{
		PrefixHandle prefix(NodeHandle(art, prefix_ptr));
		const auto count = prefix.GetCount(art);
		D_ASSERT(pos < count);

		if (pos + 1 < count) {
			// The split is not at the last prefix byte.
			// After the caller attaches the returned child, we get:
			// [this prefix minus split byte, minus remaining bytes (omitted if pos == 0)] ->
			// [new node at split byte] --(split byte)-->
			// [child with remaining bytes, and possibly remaining prefix nodes].

			// Create a new prefix and
			// 1. copy the remaining bytes of this prefix.
			// 2. append remaining prefix nodes.
			const auto suffix_count = UnsafeNumericCast<uint8_t>(count - pos - 1);
			auto suffix = NewInternal(art, child_ptr, prefix.Data(), suffix_count, pos + 1);
			Append(art, std::move(suffix), prefix.Child(art));
		} else {
			// The split is at the last prefix byte, whether the prefix is full or not.
			// There are no bytes left in this prefix after the split.
			// After the caller attaches the returned child, we get:
			// [this prefix minus split byte (omitted if pos == 0)] ->
			// [new node at split byte] --(split byte)-->
			// [child at split byte: prefix.Child(art)].
			child_ptr = prefix.Child(art);
		}

		if (pos != 0) {
			// There are bytes left before the split.
			// The subsequent node replaces the split byte.
			// Any gate stays on this prefix.
			prefix.SetCount(art, pos);
			prefix.Child(art) = branching_node4_ptr;
			return child_ptr;
		}
		// No bytes left before the split, so branching_node4_ptr inherits the prefix's gate before we free it.
		branching_node4_ptr.SetGateStatus(prefix_ptr.GetGateStatus());
	}

	// Release the prefix handle before freeing its node, which may destroy the allocator buffer.
	NodePtr::FreeNode(art, prefix_ptr);
	prefix_ptr = branching_node4_ptr;
	return child_ptr;
}

NodeHandle PrefixHandle::NewDeprecated(FixedSizeAllocator &allocator, NodePtr &node_ptr) {
	node_ptr = allocator.New();
	node_ptr.SetMetadata(static_cast<uint8_t>(PREFIX));

	NodeHandle handle(allocator, node_ptr, PREFIX);
	auto data = handle.GetPtr();
	data[DEPRECATED_COUNT] = 0;
	return handle;
}

OptionalNodePtr PrefixHandle::TransformToDeprecated(ART &art, NodePtr &node_ptr, TransformToDeprecatedState &state) {
	// Early-out, if we do not need any transformations.
	if (!state.HasAllocator()) {
		NodePtr current_ptr = node_ptr;
		auto &allocator = NodePtr::GetAllocator(art, PREFIX);
		while (current_ptr.GetType() == PREFIX && current_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET) {
			if (!allocator.LoadedFromStorage(current_ptr)) {
				return OptionalNodePtr();
			}
			NodeHandle handle(art, current_ptr);
			auto &child_ptr = ChildRef(art, handle);
			current_ptr = child_ptr;
			// Handle gated endpoints while the parent of the prefix chain is still pinned.
			if (current_ptr.HasMetadata() && current_ptr.GetGateStatus() == GateStatus::GATE_SET) {
				Leaf::TransformToDeprecated(art, child_ptr);
				return OptionalNodePtr();
			}
		}
		return current_ptr;
	}

	// We need to create a new prefix (chain) in the deprecated format.
	auto &deprecated_allocator = state.GetAllocator();
	NodePtr rebuilt_prefix_ptr;
	auto tail_handle = NewDeprecated(deprecated_allocator, rebuilt_prefix_ptr);

	auto &allocator = NodePtr::GetAllocator(art, PREFIX);
	NodePtr source_prefix_ptr = node_ptr;
	while (source_prefix_ptr.GetType() == PREFIX && source_prefix_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET) {
		if (!allocator.LoadedFromStorage(source_prefix_ptr)) {
			return OptionalNodePtr();
		}
		{
			// Decrease the readers on source_handle after moving all data over.
			NodeHandle source_handle(art, source_prefix_ptr);
			auto source_data = source_handle.GetPtr();
			auto &source_child_ptr = ChildRef(art, source_handle);

			for (idx_t i = 0; i < source_data[art.PrefixCount()]; i++) {
				tail_handle =
				    TransformToDeprecatedAppend(std::move(tail_handle), art, deprecated_allocator, source_data[i]);
			}
			auto &tail_child_ptr = ChildRefWithCount(tail_handle, DEPRECATED_COUNT);
			tail_child_ptr = source_child_ptr;
		}

		// Freeing the node here can trigger a buffer removal (last segment on the buffer).
		// In that case, there cannot be any readers left on the buffer.
		NodePtr::FreeNode(art, source_prefix_ptr);
		auto &tail_child_ptr = ChildRefWithCount(tail_handle, DEPRECATED_COUNT);
		source_prefix_ptr = tail_child_ptr;
	}

	node_ptr = rebuilt_prefix_ptr;
	auto &tail_child_ptr = ChildRefWithCount(tail_handle, DEPRECATED_COUNT);
	// Handle gated endpoints while the new prefix is still pinned.
	NodePtr endpoint_ptr = tail_child_ptr;
	if (endpoint_ptr.HasMetadata() && endpoint_ptr.GetGateStatus() == GateStatus::GATE_SET) {
		Leaf::TransformToDeprecated(art, tail_child_ptr);
		return OptionalNodePtr();
	}
	return endpoint_ptr;
}

NodeHandle PrefixHandle::TransformToDeprecatedAppend(NodeHandle tail_handle, ART &art, FixedSizeAllocator &allocator,
                                                     const uint8_t byte) {
	auto tail_data = tail_handle.GetPtr();
	if (tail_data[DEPRECATED_COUNT] != DEPRECATED_COUNT) {
		tail_data[tail_data[DEPRECATED_COUNT]] = byte;
		tail_data[DEPRECATED_COUNT]++;
		return tail_handle;
	}

	auto &tail_child_ptr = ChildRefWithCount(tail_data, DEPRECATED_COUNT);
	auto new_tail_handle = NewDeprecated(allocator, tail_child_ptr);
	return TransformToDeprecatedAppend(std::move(new_tail_handle), art, allocator, byte);
}

} // namespace duckdb
