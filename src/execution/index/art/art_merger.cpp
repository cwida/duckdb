#include "duckdb/execution/index/art/art_merger.hpp"

#include "duckdb/common/swap.hpp"
#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/node256_leaf.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/execution/index/art/prefix_handle.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"

namespace duckdb {

void ARTMerger::Init(NodePtr &left_ptr, NodePtr &right_ptr) {
	Emplace(left_ptr, right_ptr, GateStatus::GATE_NOT_SET, 0);
}

ARTConflictType ARTMerger::Merge() {
	while (!s.empty()) {
		// Copy the entry so we can pop it.
		auto entry = s.top();
		s.pop();

		const auto left_type = entry.left_ptr.GetType();
		const auto right_type = entry.right_ptr.GetType();

		// Early-out due to a constraint violation.
		// If right is LEAF_INLINED, then left is also LEAF_INLINED.
		const auto duplicate_key =
		    right_type == NType::LEAF_INLINED || entry.right_ptr.GetGateStatus() == GateStatus::GATE_SET;
		if (art.IsUnique() && duplicate_key) {
			return ARTConflictType::CONSTRAINT;
		}

		if (left_type == NType::LEAF_INLINED) {
			// Both left and right are inlined leaves.
			D_ASSERT(right_type == NType::LEAF_INLINED);
			Leaf::MergeInlined(arena, art, entry.left_ptr, entry.right_ptr, entry.status, entry.depth);
			continue;
		}

		if (right_type == NType::LEAF_INLINED) {
			// Left is any node except LEAF_INLINED, right is LEAF_INLINED.
			auto result = MergeNodeAndInlined(entry);
			if (result != ARTConflictType::NO_CONFLICT) {
				return result;
			}
			continue;
		}

		if (entry.right_ptr.IsNestedLeaf()) {
			// Both left and right are nested leaves.
			D_ASSERT(entry.left_ptr.IsNestedLeaf());
			MergeLeaves(entry);
			continue;
		}

		if (entry.left_ptr.IsInternalNode() && entry.right_ptr.IsInternalNode()) {
			// Both left and right are internal nodes.
			MergeNodes(entry);
			continue;
		}

		D_ASSERT(right_type == NType::PREFIX);
		if (left_type == NType::PREFIX) {
			// Both left and right are prefixes.
			MergePrefixes(entry);
			continue;
		}
		// Left is a node, right is a PREFIX.
		MergeNodeAndPrefix(entry.left_ptr, entry.right_ptr, entry.status, entry.depth);
	}

	// We exhausted the stack.
	return ARTConflictType::NO_CONFLICT;
}

void ARTMerger::Emplace(NodePtr &left_ptr, NodePtr &right_ptr, const GateStatus parent_status, const idx_t depth) {
	const auto left_type = left_ptr.GetType();
	const auto right_type = right_ptr.GetType();

	if (left_type == NType::LEAF_INLINED) {
		swap(left_ptr, right_ptr);
	} else if (left_type == NType::PREFIX && right_type != NType::LEAF_INLINED) {
		swap(left_ptr, right_ptr);
	}

	// left ONLY has GATE_SET, if it is the gate node.
	// When outside the gate, we propagate the parent_status (GATE_NOT_SET) and the depth.
	// When inside the gate, we already reset the depth, and we propagate the parent_status (GATE_SET).
	if (left_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET) {
		s.emplace(left_ptr, right_ptr, parent_status, depth);
		return;
	}

	// Enter a gate.
	// Reset the depth.
	D_ASSERT(parent_status == GateStatus::GATE_NOT_SET);
	s.emplace(left_ptr, right_ptr, GateStatus::GATE_SET, 0);
}

ARTConflictType ARTMerger::MergeNodeAndInlined(NodeEntry &entry) {
	D_ASSERT(entry.right_ptr.GetType() == NType::LEAF_INLINED);
	D_ASSERT(entry.status == GateStatus::GATE_SET);

	// We fall back to the ART insertion code.
	auto row_id_key = ARTKey::CreateARTKey<row_t>(arena, entry.right_ptr.GetRowId());
	return ARTOperator::Insert(arena, art, entry.left_ptr, row_id_key, entry.depth, row_id_key, GateStatus::GATE_SET,
	                           DeleteIndexInfo(), IndexAppendMode::DEFAULT);
}

array_ptr<uint8_t> ARTMerger::GetBytes(NodePtr &leaf_ptr) {
	const auto type = leaf_ptr.GetType();
	switch (type) {
	case NType::NODE_7_LEAF:
		return NodePtr::Ref<Node7Leaf>(art, leaf_ptr, type).GetBytes();
	case NType::NODE_15_LEAF:
		return NodePtr::Ref<Node15Leaf>(art, leaf_ptr, type).GetBytes();
	case NType::NODE_256_LEAF:
		return NodePtr::Ref<Node256Leaf>(art, leaf_ptr, type).GetBytes(arena);
	default:
		throw InternalException("invalid node type for ARTMerger::GetBytes: %s", EnumUtil::ToString(type));
	}
}

void ARTMerger::MergeLeaves(NodeEntry &entry) {
	D_ASSERT(entry.left_ptr.IsNestedLeaf());
	D_ASSERT(entry.right_ptr.IsNestedLeaf());
	D_ASSERT(entry.left_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);
	D_ASSERT(entry.right_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);

	// Merge the smaller leaf into the bigger leaf.
	if (entry.left_ptr.GetType() < entry.right_ptr.GetType()) {
		swap(entry.left_ptr, entry.right_ptr);
	}

	// Get the bytes of the right node.
	// Then, copy them into left.
	auto bytes = GetBytes(entry.right_ptr);

	// FIXME: Obtain a reference to left once and
	// FIXME: handle the different node type combinations.
	for (idx_t i = 0; i < bytes.size(); i++) {
		NodePtr::InsertChild(art, entry.left_ptr, bytes[i]);
	}
	NodePtr::FreeNode(art, entry.right_ptr);
}

NodeChildren ARTMerger::ExtractChildren(NodePtr &node_ptr) {
	const auto type = node_ptr.GetType();
	switch (type) {
	case NType::NODE_4:
		return NodePtr::Ref<Node4>(art, node_ptr, type).ExtractChildren(arena);
	case NType::NODE_16:
		return NodePtr::Ref<Node16>(art, node_ptr, type).ExtractChildren(arena);
	case NType::NODE_48:
		return NodePtr::Ref<Node48>(art, node_ptr, type).ExtractChildren(arena);
	case NType::NODE_256:
		return NodePtr::Ref<Node256>(art, node_ptr, type).ExtractChildren(arena);
	default:
		throw InternalException("invalid node type for ARTMerger::GetChildren: %s", EnumUtil::ToString(type));
	}
}

void ARTMerger::MergeNodes(NodeEntry &entry) {
	D_ASSERT(entry.left_ptr.IsInternalNode());
	D_ASSERT(entry.right_ptr.IsInternalNode());

	// Merge the smaller node into the bigger node.
	if (entry.left_ptr.GetType() < entry.right_ptr.GetType()) {
		swap(entry.left_ptr, entry.right_ptr);
	}

	// Get the children of the right node.
	// Then, copy them into left.
	auto children = ExtractChildren(entry.right_ptr);
	// As long as the arena is valid,
	// the copied-out nodes (and their references) are valid.
	NodePtr::FreeNode(art, entry.right_ptr);

	// First, we iterate and insert children.
	// This might grow the node, so we need to do it prior to Emplace.
	vector<idx_t> remaining;
	for (idx_t i = 0; i < children.bytes.size(); i++) {
		const auto byte = children.bytes[i];
		auto child_ptr_ref = entry.left_ptr.GetChildMutableUnsafe(art, byte);

		if (!child_ptr_ref) {
			// There is no child at this byte.
			// We can insert the right node's child at byte and are done.
			auto &right_child_ptr = children.children[i];
			NodePtr::InsertChild(art, entry.left_ptr, byte, right_child_ptr);
			continue;
		}
		// There is a left and a right child at this byte.
		// We remember to emplace the two children.
		remaining.emplace_back(i);
	}

	// Emplace all remaining children.
	for (idx_t i = 0; i < remaining.size(); i++) {
		const auto byte = children.bytes[remaining[i]];
		auto &right_child_ptr = children.children[remaining[i]];
		auto child_ptr_ref = entry.left_ptr.GetChildMutableUnsafe(art, byte);
		Emplace(*child_ptr_ref, right_child_ptr, entry.status, entry.depth + 1);
	}
}

void ARTMerger::MergeNodeAndPrefix(NodePtr &node_ptr, NodePtr &prefix_ptr, const GateStatus parent_status,
                                   const idx_t parent_depth, const uint8_t pos) {
	D_ASSERT(node_ptr.IsInternalNode());
	D_ASSERT(prefix_ptr.GetType() == NType::PREFIX);

	// Get the child at the prefix byte, or nullptr, if there is no child.
	const auto byte = Prefix::GetByte(art, prefix_ptr, pos);
	auto child_ptr_ref = node_ptr.GetChildMutableUnsafe(art, byte);

	// Reduce the prefix to the bytes after pos.
	Prefix::Reduce(art, prefix_ptr, pos);

	if (child_ptr_ref) {
		// Iterate on the child and the remaining prefix.
		Emplace(*child_ptr_ref, prefix_ptr, parent_status, parent_depth + 1);
		return;
	}

	// There is no child at this prefix byte,
	// so we can insert the remaining prefix and are done.
	NodePtr::InsertChild(art, node_ptr, byte, prefix_ptr);
	prefix_ptr.Clear();
}

void ARTMerger::MergeNodeAndPrefix(NodePtr &node_ptr, NodePtr &prefix_ptr, const GateStatus parent_status,
                                   const idx_t parent_depth) {
	D_ASSERT(node_ptr.IsInternalNode());
	D_ASSERT(prefix_ptr.GetType() == NType::PREFIX);

	MergeNodeAndPrefix(node_ptr, prefix_ptr, parent_status, parent_depth, 0);
}

void ARTMerger::MergePrefixes(NodeEntry &entry) {
	D_ASSERT(entry.left_ptr.GetType() == NType::PREFIX);
	D_ASSERT(entry.right_ptr.GetType() == NType::PREFIX);

	// We traverse prefixes until we
	// Case 1: find a position where they differ.
	// Case 2: find that they are the same.
	// Case 3: find that one prefix contains the other.

	// Until we reach one of these cases, we keep reducing
	// the right prefix (and freeing the fully reduced nodes).
	// We can do so because up to any of these three cases,
	// the prefixes are the same. That means, we only need to keep
	// one of them around.

	Prefix l_prefix(art, entry.left_ptr, true);
	Prefix r_prefix(art, entry.right_ptr, true);
	const auto count = art.PrefixCount();

	// Find a byte at pos where the prefixes differ.
	// If they match up to max_count, then pos stays invalid.
	const auto max_count = MinValue(l_prefix.data[count], r_prefix.data[count]);
	optional_idx pos;
	for (idx_t i = 0; i < max_count; i++) {
		if (l_prefix.data[i] != r_prefix.data[i]) {
			pos = i;
			break;
		}
	}

	if (pos.IsValid()) {
		// The prefixes differ at pos.
		// We split the left prefix, and reduce the right prefix.
		// Then, we insert both remainders into a new Node4.
		// Then, we are done.
		const auto split_pos = UnsafeNumericCast<uint8_t>(pos.GetIndex());
		const auto l_byte = Prefix::GetByte(art, entry.left_ptr, split_pos);
		const auto r_byte = Prefix::GetByte(art, entry.right_ptr, split_pos);

		// Split and reduce.
		NodePtr branching_node4_ptr;
		Node4::New(art, branching_node4_ptr);
		auto l_child_ptr = PrefixHandle::Split(art, entry.left_ptr, branching_node4_ptr, split_pos);
		Prefix::Reduce(art, entry.right_ptr, split_pos);

		Node4::InsertChild(art, branching_node4_ptr, l_byte, l_child_ptr);
		Node4::InsertChild(art, branching_node4_ptr, r_byte, entry.right_ptr);
		entry.right_ptr.Clear();
		return;
	}

	if (l_prefix.data[count] == r_prefix.data[count]) {
		// The prefixes match.
		// Free the right prefix, but keep the reference to its child alive.
		// Then, iterate on the left and right (reduced) child.
		auto r_child_ptr = *r_prefix.child_slot;
		NodePtr::FreeNode(art, entry.right_ptr);
		entry.right_ptr = r_child_ptr;

		auto depth = entry.depth + l_prefix.data[count];
		Emplace(*l_prefix.child_slot, entry.right_ptr, entry.status, depth);
		return;
	}

	// max_count indexes the byte after the exhausted prefix in a child node.
	if (r_prefix.data[count] == max_count) {
		// We exhausted the right prefix.
		// Ensure that we continue merging into left.
		swap(entry.left_ptr, entry.right_ptr);
		MergeNodeAndPrefix(*r_prefix.child_slot, entry.right_ptr, entry.status, entry.depth + max_count, max_count);
		return;
	}

	// We exhausted the left prefix.
	MergeNodeAndPrefix(*l_prefix.child_slot, entry.right_ptr, entry.status, entry.depth + max_count, max_count);
}

} // namespace duckdb
