#include "duckdb/execution/index/art/leaf.hpp"

#include "duckdb/common/types.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/iterator.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/execution/index/art/prefix_handle.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"

namespace duckdb {

void Leaf::New(NodePtr &node_ptr, const row_t row_id) {
	D_ASSERT(row_id < MAX_ROW_ID_LOCAL);
	node_ptr.Clear();
	node_ptr.SetMetadata(static_cast<uint8_t>(INLINED));
	node_ptr.SetRowId(row_id);
}

void Leaf::MergeInlined(ArenaAllocator &arena, ART &art, NodePtr &left_ptr, NodePtr &right_ptr, GateStatus status,
                        idx_t depth) {
	D_ASSERT(left_ptr.GetType() == NType::LEAF_INLINED);
	D_ASSERT(right_ptr.GetType() == NType::LEAF_INLINED);

	status = status == GateStatus::GATE_NOT_SET ? GateStatus::GATE_SET : GateStatus::GATE_NOT_SET;
	if (status == GateStatus::GATE_SET) {
		// Case 1: We are outside a nested leaf,
		// so we create a nested leaf.
		depth = 0;
	}
	// Otherwise, case 2: we are in a nested leaf with two 'compressed' prefixes.
	// A 'compressed prefix' is an inlined leaf that could've been expanded to
	// a prefix with an inlined leaf as its only child.

	// Get the corresponding row IDs and their ART keys.
	auto left_row_id = left_ptr.GetRowId();
	auto right_row_id = right_ptr.GetRowId();
	auto left_key = ARTKey::CreateARTKey<row_t>(arena, left_row_id);
	auto right_key = ARTKey::CreateARTKey<row_t>(arena, right_row_id);

	auto pos = left_key.GetMismatchPos(right_key, depth);

	auto left_byte = left_key.data[pos];
	auto right_byte = right_key.data[pos];

	NodePtr merged_root_ptr;
	if (pos == Prefix::ROW_ID_COUNT) {
		// The row IDs differ on the last byte.
		Node7Leaf::New(art, merged_root_ptr);
		Node7Leaf::InsertByte(art, merged_root_ptr, left_byte);
		Node7Leaf::InsertByte(art, merged_root_ptr, right_byte);
	} else {
		// Create and insert the (compressed) children.
		// We inline directly into the node, instead of creating prefixes
		// with a single inlined leaf as their child.
		Node4::New(art, merged_root_ptr);

		NodePtr left_child_ptr;
		Leaf::New(left_child_ptr, left_row_id);
		Node4::InsertChild(art, merged_root_ptr, left_byte, left_child_ptr);

		NodePtr right_child_ptr;
		Leaf::New(right_child_ptr, right_row_id);
		Node4::InsertChild(art, merged_root_ptr, right_byte, right_child_ptr);
	}

	if (pos != depth) {
		// The row IDs share a prefix.
		auto chain = PrefixHandle::New(art, left_key, depth, pos - depth);
		chain.tail.Child(art) = merged_root_ptr;
		merged_root_ptr = chain.root_ptr;
	}

	merged_root_ptr.SetGateStatus(status);
	left_ptr = merged_root_ptr;
}

void Leaf::TransformToNested(ART &art, NodePtr &node_ptr) {
	D_ASSERT(node_ptr.GetType() == LEAF);

	ArenaAllocator arena(Allocator::Get(art.db));
	NodePtr root_ptr = NodePtr();

	// Move all row IDs into the nested leaf.
	NodePtr current_ptr = node_ptr;
	while (current_ptr.HasMetadata()) {
		ConstNodeHandle handle(art, current_ptr);
		auto &leaf = handle.Get<Leaf>();
		for (uint8_t i = 0; i < leaf.count; i++) {
			auto row_id = ARTKey::CreateARTKey<row_t>(arena, leaf.row_ids[i]);
			auto conflict_type = ARTOperator::Insert(arena, art, root_ptr, row_id, 0, row_id, GateStatus::GATE_SET,
			                                         DeleteIndexInfo(), IndexAppendMode::INSERT_DUPLICATES);
			if (conflict_type != ARTConflictType::NO_CONFLICT) {
				throw InternalException("invalid conflict type in Leaf::TransformToNested");
			}
		}
		current_ptr = leaf.next_leaf_ptr;
	}

	root_ptr.SetGateStatus(GateStatus::GATE_SET);
	DeprecatedFree(art, node_ptr);
	node_ptr = root_ptr;
}

void Leaf::TransformToDeprecated(ART &art, NodePtr &node_ptr) {
	D_ASSERT(node_ptr.GetGateStatus() == GateStatus::GATE_SET || node_ptr.GetType() == LEAF);

	// Early-out, if we never transformed this leaf.
	if (node_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET) {
		return;
	}

	// Collect all row IDs and free the nested leaf.
	set<row_t> row_ids;
	Iterator it(art);
	it.FindMinimum(node_ptr);
	ARTKey empty_key = ARTKey();
	RowIdSetOutput output(row_ids, NumericLimits<row_t>().Maximum());
	it.Scan(empty_key, output, false);
	NodePtr::FreeTree(art, node_ptr);
	D_ASSERT(row_ids.size() > 1);

	// Create the deprecated leaves.
	idx_t remaining = row_ids.size();
	auto row_ids_it = row_ids.begin();
	reference<NodePtr> leaf_ptr_ref(node_ptr);
	while (remaining) {
		leaf_ptr_ref.get() = NodePtr::GetAllocator(art, LEAF).New();
		leaf_ptr_ref.get().SetMetadata(static_cast<uint8_t>(LEAF));

		auto &leaf = NodePtr::Ref<Leaf>(art, leaf_ptr_ref, LEAF);
		auto min = MinValue(UnsafeNumericCast<idx_t>(LEAF_SIZE), remaining);
		leaf.count = UnsafeNumericCast<uint8_t>(min);

		for (uint8_t i = 0; i < leaf.count; i++) {
			leaf.row_ids[i] = *row_ids_it;
			row_ids_it++;
		}
		remaining -= leaf.count;

		leaf_ptr_ref = leaf.next_leaf_ptr;
		leaf.next_leaf_ptr.Clear();
	}
}

//===--------------------------------------------------------------------===//
// Deprecated code paths.
//===--------------------------------------------------------------------===//

void Leaf::DeprecatedFree(ART &art, NodePtr &node_ptr) {
	D_ASSERT(node_ptr.GetType() == LEAF);
	while (node_ptr.HasMetadata()) {
		NodePtr next_ptr;
		{
			ConstNodeHandle handle(art, node_ptr);
			next_ptr = handle.Get<Leaf>().next_leaf_ptr;
		}
		NodePtr::FreeNode(art, node_ptr);
		node_ptr = next_ptr;
	}
	node_ptr.Clear();
}

bool Leaf::DeprecatedGetRowIds(const ART &art, const NodePtr &node_ptr, set<row_t> &row_ids, const idx_t max_count) {
	D_ASSERT(node_ptr.GetType() == LEAF);

	NodePtr current_ptr = node_ptr;
	while (current_ptr.HasMetadata()) {
		ConstNodeHandle handle(art, current_ptr);
		auto &leaf = handle.Get<Leaf>();
		if (row_ids.size() + leaf.count > max_count) {
			return false;
		}
		for (uint8_t i = 0; i < leaf.count; i++) {
			row_ids.insert(leaf.row_ids[i]);
		}
		current_ptr = leaf.next_leaf_ptr;
	}
	return true;
}

void Leaf::DeprecatedVacuum(ART &art, NodePtr node_ptr) {
	D_ASSERT(node_ptr.HasMetadata());
	D_ASSERT(node_ptr.GetType() == LEAF);

	auto &allocator = NodePtr::GetAllocator(art, LEAF);
	while (node_ptr.HasMetadata()) {
		NodeHandle handle(art, node_ptr);
		auto &leaf = handle.Get<Leaf>();
		if (leaf.next_leaf_ptr.HasMetadata() && allocator.NeedsVacuum(leaf.next_leaf_ptr)) {
			leaf.next_leaf_ptr = allocator.VacuumPointer(leaf.next_leaf_ptr);
			leaf.next_leaf_ptr.SetMetadata(static_cast<uint8_t>(LEAF));
		}
		node_ptr = leaf.next_leaf_ptr;
	}
}

string Leaf::DeprecatedToString(ART &art, const NodePtr &node_ptr, const ToStringOptions &options) {
	string str = "";

	if (!options.print_deprecated_leaves) {
		str = options.tree_prefix + "[deprecated leaves]\n";
		return str;
	}

	reference<const NodePtr> leaf_ptr_ref(node_ptr);

	while (leaf_ptr_ref.get().HasMetadata()) {
		auto &leaf = NodePtr::Ref<const Leaf>(art, leaf_ptr_ref, LEAF);
		str += options.tree_prefix + "Leaf [count: " + to_string(leaf.count) + ", row IDs: ";
		for (uint8_t i = 0; i < leaf.count; i++) {
			str += to_string(leaf.row_ids[i]) + "-";
		}
		str += "]\n";
		leaf_ptr_ref = leaf.next_leaf_ptr;
	}

	return str;
}

void Leaf::DeprecatedVerify(ART &art, const NodePtr &node_ptr) {
	D_ASSERT(node_ptr.GetType() == LEAF);

	NodePtr current_ptr = node_ptr;
	while (current_ptr.HasMetadata()) {
		ConstNodeHandle handle(art, current_ptr);
		auto &leaf = handle.Get<Leaf>();
		D_ASSERT(leaf.count <= LEAF_SIZE);
		current_ptr = leaf.next_leaf_ptr;
	}
}

void Leaf::DeprecatedVerifyAllocations(ART &art, const NodePtr &node_ptr, unordered_map<uint8_t, idx_t> &node_counts) {
	D_ASSERT(node_ptr.GetType() == LEAF);

	auto idx = NodePtr::GetAllocatorIdx(LEAF);

	NodePtr current_ptr = node_ptr;
	while (current_ptr.HasMetadata()) {
		node_counts[idx]++;

		ConstNodeHandle handle(art, current_ptr);
		auto &leaf = handle.Get<Leaf>();
		current_ptr = leaf.next_leaf_ptr;
	}
}

} // namespace duckdb
