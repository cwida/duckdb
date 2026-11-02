#include "duckdb/execution/index/art/art_builder.hpp"

#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/art_operator.hpp"

namespace duckdb {

ARTConflictType ARTBuilder::Build() {
	while (!s.empty()) {
		// Copy the entry so we can pop it.
		auto entry = s.top();
		s.pop();

		D_ASSERT(entry.start < keys.size());
		D_ASSERT(entry.end < keys.size());
		D_ASSERT(entry.start <= entry.end);

		auto &start = keys[entry.start];
		auto &end = keys[entry.end];
		D_ASSERT(start.len != 0);

		// Increment the depth until we reach a leaf or find a mismatching byte.
		auto prefix_depth = entry.depth;
		while (start.len != entry.depth && start.ByteMatches(end, entry.depth)) {
			entry.depth++;
		}

		// True, if we reached a leaf: all bytes of start_key and end_key match.
		if (start.len == entry.depth) {
			// Get the number of row IDs in the leaf.
			auto row_id_count = entry.end - entry.start + 1;
			if (art.IsUnique() && row_id_count != 1) {
				return ARTConflictType::CONSTRAINT;
			}

			reference<NodePtr> node_ptr_ref(entry.node_ptr);
			auto count = UnsafeNumericCast<idx_t>(start.len - prefix_depth);
			Prefix::New(art, node_ptr_ref, start, prefix_depth, count);

			// Inline the row ID.
			if (row_id_count == 1) {
				Leaf::New(node_ptr_ref, row_ids[entry.start].GetRowId());
				continue;
			}

			// Loop and insert the row IDs.
			// We cannot iterate into the nested leaf with the builder
			// because row IDs are not sorted.
			for (idx_t i = entry.start; i < entry.start + row_id_count; i++) {
				ARTOperator::Insert(arena, art, node_ptr_ref, row_ids[i], 0, row_ids[i], GateStatus::GATE_SET,
				                    DeleteIndexInfo(), IndexAppendMode::DEFAULT);
			}
			node_ptr_ref.get().SetGateStatus(GateStatus::GATE_SET);
			continue;
		}

		// Create the prefix. Returns early, if the prefix_length is zero.
		reference<NodePtr> node_ptr_ref(entry.node_ptr);
		auto prefix_length = entry.depth - prefix_depth;
		Prefix::New(art, node_ptr_ref, start, prefix_depth, prefix_length);

		vector<idx_t> child_offsets;
		child_offsets.emplace_back(entry.start);
		for (idx_t i = entry.start + 1; i <= entry.end; i++) {
			if (keys[i - 1].data[entry.depth] != keys[i].data[entry.depth]) {
				child_offsets.emplace_back(i);
			}
		}

		// Create a new node containing the children.
		NodePtr::New(art, node_ptr_ref, NodePtr::GetInternalNodeType(child_offsets.size()));
		auto start_offset = child_offsets[0];
		for (idx_t i = 1; i <= child_offsets.size(); i++) {
			auto child_byte = keys[start_offset].data[entry.depth];
			// FIXME: Improve performance by either returning a reference to the child directly,
			// FIXME: or by calling InsertChild after processing the child (at the end of the stack loop).
			NodePtr::InsertChild(art, node_ptr_ref, child_byte);
			auto child_ptr_ref = node_ptr_ref.get().NodePtr::GetChildMutableUnsafe(art, child_byte, true);
			auto end_offset = i != child_offsets.size() ? child_offsets[i] - 1 : entry.end;
			s.emplace(*child_ptr_ref, start_offset, end_offset, entry.depth + 1);
			start_offset = end_offset + 1;
		}
	}

	// We exhausted the stack.
	return ARTConflictType::NO_CONFLICT;
}

} // namespace duckdb
