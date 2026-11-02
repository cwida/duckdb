#include "duckdb/execution/index/art/iterator.hpp"

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/vector_size.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/const_prefix_handle.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/node_handle.hpp"
#include "duckdb/execution/index/art/prefix.hpp"

namespace duckdb {

RowIdVectorOutput::RowIdVectorOutput(const idx_t capacity_p) : capacity(capacity_p) {
	row_ids.reserve(MinValue<idx_t>(capacity, STANDARD_VECTOR_SIZE));
}

bool RowIdVectorOutput::TryAdd(const row_t row_id) {
	D_ASSERT(row_ids.size() <= capacity);
	if (row_ids.size() >= capacity) {
		Normalize();
		if (row_ids.size() >= capacity) {
			return false;
		}
	}
	row_ids.push_back(row_id);
	return true;
}

void RowIdVectorOutput::Reset() {
	row_ids.clear();
}

void RowIdVectorOutput::Normalize() {
	std::sort(row_ids.begin(), row_ids.end());
	row_ids.erase(std::unique(row_ids.begin(), row_ids.end()), row_ids.end());
}

unsafe_vector<row_t> RowIdVectorOutput::TakeRows() {
	Normalize();
	return std::move(row_ids);
}

//===--------------------------------------------------------------------===//
// IteratorKey
//===--------------------------------------------------------------------===//

bool IteratorKey::Contains(const ARTKey &key) const {
	if (Size() < key.len) {
		return false;
	}
	for (idx_t i = 0; i < key.len; i++) {
		if (key_bytes[i] != key.data[i]) {
			return false;
		}
	}
	return true;
}

bool IteratorKey::GreaterThan(const ARTKey &key, const bool equal, const uint8_t nested_depth) const {
	for (idx_t i = 0; i < MinValue<idx_t>(Size(), key.len); i++) {
		if (key_bytes[i] > key.data[i]) {
			return true;
		} else if (key_bytes[i] < key.data[i]) {
			return false;
		}
	}

	// Returns true, if current_key is greater than (or equal to) key.
	D_ASSERT(Size() >= nested_depth);
	auto this_len = Size() - nested_depth;
	return equal ? this_len > key.len : this_len >= key.len;
}

//===--------------------------------------------------------------------===//
// Iterator
//===--------------------------------------------------------------------===//

template <typename Output>
ARTScanProgress Iterator::Scan(const ARTKey &upper_bound, Output &output, bool equal) {
	bool has_next;
	do {
		// An empty upper bound indicates that no upper bound exists.
		if (!upper_bound.Empty()) {
			if (status == GateStatus::GATE_NOT_SET || entered_nested_leaf) {
				if (current_key.GreaterThan(upper_bound, equal, nested_depth)) {
					return ARTScanProgress::COMPLETED;
				}
			}
		}

		// Set the current key in the output policy.
		D_ASSERT(current_key.Size() >= nested_depth);
		auto key_len = current_key.Size() - nested_depth;
		output.SetKey(current_key, key_len);

		switch (last_leaf_ptr.GetType()) {
		case NType::LEAF_INLINED: {
			if (!output.TryAdd(last_leaf_ptr.GetRowId())) {
				return ARTScanProgress::PAUSED;
			}
			break;
		}
		case NType::LEAF: {
			D_ASSERT(nested_depth == 0);
			if (!resume_state.has_cached_row_ids) {
				resume_state.cached_row_ids.clear();
				Leaf::DeprecatedGetRowIds(art, last_leaf_ptr, resume_state.cached_row_ids,
				                          NumericLimits<idx_t>::Maximum());
				resume_state.cached_row_ids_it = resume_state.cached_row_ids.begin();
				resume_state.has_cached_row_ids = true;
			}
			// Try to output the next entry in the deprecated leaf chain.
			while (resume_state.cached_row_ids_it != resume_state.cached_row_ids.end()) {
				if (!output.TryAdd(*resume_state.cached_row_ids_it)) {
					// If we pause here, then scanning will resume at cached_row_ids_it.
					return ARTScanProgress::PAUSED;
				}
				++resume_state.cached_row_ids_it;
			}
			resume_state.has_cached_row_ids = false;
			break;
		}
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF: {
			// If we haven't traversed this leaf yet, set nested_started to true (allows us to pick up iteration again
			// in case we fill the output with capacity.
			if (!resume_state.nested_started) {
				resume_state.nested_byte = 0;
				resume_state.nested_started = true;
			}
			// Try to output the next inlined leaf.
			while (last_leaf_ptr.GetNextByte(art, resume_state.nested_byte)) {
				row_id[ROW_ID_SIZE - 1] = resume_state.nested_byte;
				ARTKey rid_key(&row_id[0], ROW_ID_SIZE);
				if (!output.TryAdd(rid_key.GetRowId())) {
					// If we pause here, then scanning will resume at nested_byte in the current leaf.
					return ARTScanProgress::PAUSED;
				}

				if (resume_state.nested_byte == NumericLimits<uint8_t>::Maximum()) {
					break;
				}
				resume_state.nested_byte++;
			}
			resume_state.nested_started = false;
			break;
		}
		default:
			throw InternalException("Invalid leaf type for index scan.");
		}

		entered_nested_leaf = false;
		has_next = Next();
	} while (has_next);
	return ARTScanProgress::COMPLETED;
}

// Explicit template instantiations for the output policies.
template ARTScanProgress Iterator::Scan<RowIdSetOutput>(const ARTKey &, RowIdSetOutput &, bool);
template ARTScanProgress Iterator::Scan<RowIdVectorOutput>(const ARTKey &, RowIdVectorOutput &, bool);
template ARTScanProgress Iterator::Scan<KeyRowIdOutput>(const ARTKey &, KeyRowIdOutput &, bool);

void Iterator::FindMinimum(NodePtr current_ptr) {
	while (current_ptr.HasMetadata()) {
		// Found the minimum.
		if (current_ptr.IsAnyLeaf()) {
			last_leaf_ptr = current_ptr;
			return;
		}

		// We are passing a gate node.
		if (current_ptr.GetGateStatus() == GateStatus::GATE_SET) {
			D_ASSERT(status == GateStatus::GATE_NOT_SET);
			status = GateStatus::GATE_SET;
			entered_nested_leaf = true;
			nested_depth = 0;
		}

		// Traverse the prefix.
		if (current_ptr.GetType() == NType::PREFIX) {
			NodePtr child_ptr;
			{
				ConstNodeHandle handle(art, current_ptr);
				auto data = handle.GetPtr();
				auto count = data[art.PrefixCount()];

				for (idx_t i = 0; i < count; i++) {
					current_key.Push(data[i]);
					if (status == GateStatus::GATE_SET) {
						row_id[nested_depth] = data[i];
						nested_depth++;
						D_ASSERT(nested_depth < Prefix::ROW_ID_SIZE);
					}
				}
				// Copy the child locator before releasing the prefix handle.
				child_ptr = ConstPrefixHandle::ChildRef(art, handle);
			}
			nodes.emplace(current_ptr, 0);
			current_ptr = child_ptr;
			continue;
		}

		// Go to the leftmost entry in the current node.
		uint8_t byte = 0;
		auto child_ptr = current_ptr.GetNextChildNode(art, byte);
		D_ASSERT(child_ptr);

		// Move to the leftmost node.
		current_key.Push(byte);
		if (status == GateStatus::GATE_SET) {
			row_id[nested_depth] = byte;
			nested_depth++;
			D_ASSERT(nested_depth < Prefix::ROW_ID_SIZE);
		}
		nodes.emplace(current_ptr, byte);
		current_ptr = child_ptr.Get();
	}
	// Should always have a node with metadata.
	throw InternalException("ART Iterator::FindMinimum: Reached node without metadata");
}

bool Iterator::LowerBound(NodePtr current_ptr, const ARTKey &key, const bool equal) {
	idx_t depth = 0;

	while (current_ptr.HasMetadata()) {
		// We found any leaf node, or a gate.
		if (current_ptr.IsAnyLeaf() || current_ptr.GetGateStatus() == GateStatus::GATE_SET) {
			D_ASSERT(status == GateStatus::GATE_NOT_SET);
			D_ASSERT(current_key.Size() == key.len);
			if (!equal && current_key.Contains(key)) {
				return Next();
			}

			if (current_ptr.GetGateStatus() == GateStatus::GATE_SET) {
				FindMinimum(current_ptr);
			} else {
				last_leaf_ptr = current_ptr;
			}
			return true;
		}

		D_ASSERT(current_ptr.GetGateStatus() == GateStatus::GATE_NOT_SET);
		if (current_ptr.GetType() != NType::PREFIX) {
			auto next_byte = key[depth];
			auto child_ptr = current_ptr.GetNextChildNode(art, next_byte);

			// The key is greater than any key in this subtree.
			if (!child_ptr) {
				return Next();
			}

			current_key.Push(next_byte);
			nodes.emplace(current_ptr, next_byte);

			// We return the minimum because all keys are greater than the lower bound.
			if (next_byte > key[depth]) {
				FindMinimum(child_ptr.Get());
				return true;
			}

			// Move to the child and increment depth.
			current_ptr = child_ptr.Get();
			depth++;
			continue;
		}

		// Copy the prefix bytes and child while the prefix is pinned.
		uint8_t prefix_count;
		NodePtr prefix_child_ptr;
		const auto prefix_offset = current_key.Size();
		{
			ConstNodeHandle handle(art, current_ptr);
			auto data = handle.GetPtr();
			prefix_count = data[art.PrefixCount()];
			// Copy the child locator before releasing the prefix handle.
			prefix_child_ptr = ConstPrefixHandle::ChildRef(art, handle);

			for (idx_t i = 0; i < prefix_count; i++) {
				current_key.Push(data[i]);
			}
			nodes.emplace(current_ptr, 0);
		}

		// Compare the copied prefix bytes with the key bytes.
		for (idx_t i = 0; i < prefix_count; i++) {
			// We found a prefix byte that is less than its corresponding key byte.
			// I.e., the subsequent node is lesser than the key. Thus, the next node
			// is the lower bound.
			if (current_key[prefix_offset + i] < key[depth + i]) {
				return Next();
			}

			// We found a prefix byte that is greater than its corresponding key byte.
			// I.e., the subsequent node is greater than the key. Thus, the minimum is
			// the lower bound.
			if (current_key[prefix_offset + i] > key[depth + i]) {
				FindMinimum(prefix_child_ptr);
				return true;
			}
		}

		// The prefix matches the key.
		depth += prefix_count;
		current_ptr = prefix_child_ptr;
	}
	// Should always have a node with metadata.
	throw InternalException("ART Iterator::LowerBound: Reached node without metadata");
}

bool Iterator::Next() {
	while (!nodes.empty()) {
		auto &top = nodes.top();
		D_ASSERT(!top.node_ptr.IsAnyLeaf());

		if (top.node_ptr.GetType() == NType::PREFIX) {
			PopNode();
			continue;
		}

		if (top.byte == NumericLimits<uint8_t>::Maximum()) {
			// No more children of this node.
			// Move up the tree by popping the key byte of the current node.
			PopNode();
			continue;
		}

		top.byte++;
		auto child_ptr = top.node_ptr.GetNextChildNode(art, top.byte);
		if (!child_ptr) {
			// No more children of this node.
			// Move up the tree by popping the key byte of the current node.
			PopNode();
			continue;
		}

		current_key.Pop(1);
		current_key.Push(top.byte);
		if (status == GateStatus::GATE_SET) {
			row_id[nested_depth - 1] = top.byte;
		}

		FindMinimum(child_ptr.Get());
		return true;
	}
	return false;
}

void Iterator::PopNode() {
	auto gate_status = nodes.top().node_ptr.GetGateStatus();

	// Pop the byte and the node.
	if (nodes.top().node_ptr.GetType() != NType::PREFIX) {
		current_key.Pop(1);
		if (status == GateStatus::GATE_SET) {
			nested_depth--;
			D_ASSERT(nested_depth < Prefix::ROW_ID_SIZE);
		}

	} else {
		// Pop all prefix bytes and the node.
		ConstNodeHandle handle(art, nodes.top().node_ptr);
		auto data = handle.GetPtr();
		auto prefix_byte_count = data[art.PrefixCount()];
		current_key.Pop(prefix_byte_count);

		if (status == GateStatus::GATE_SET) {
			nested_depth -= prefix_byte_count;
			D_ASSERT(nested_depth < Prefix::ROW_ID_SIZE);
		}
	}
	nodes.pop();

	// We are popping a gate node.
	if (gate_status == GateStatus::GATE_SET) {
		D_ASSERT(status == GateStatus::GATE_SET);
		status = GateStatus::GATE_NOT_SET;
	}
}

} // namespace duckdb
