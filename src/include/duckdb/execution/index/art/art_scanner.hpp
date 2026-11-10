//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/art_scanner.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/execution/index/art/const_prefix_handle.hpp"
#include "duckdb/execution/index/art/prefix_handle.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/node256.hpp"

namespace duckdb {

//! The result of an on_pop handler: scan the children of the popped node, or skip them.
enum class ARTScanNodeResult : uint8_t { SCAN_CHILDREN, SKIP };

//! Pins node and passes each of its child slots to child_handler.
//! child_handler runs under the pin, can update the slot in place,
//! and returns the NodePtr value to push, or an empty OptionalNodePtr.
template <class NODE, class NODE_HANDLE, class ART_TYPE, class CHILD_HANDLER, class PUSH>
void ARTScanChildrenInternal(ART_TYPE &art, const NodePtr node_ptr, CHILD_HANDLER &&child_handler, PUSH &&push) {
	NODE_HANDLE handle(art, node_ptr);
	auto &n = handle.template Get<NODE>();
	NODE::Iterator(n, [&](auto &child_ptr) {
		auto next_ptr = child_handler(child_ptr);
		if (next_ptr) {
			push(next_ptr.Get());
		}
	});
}

//! Dispatches on the node type and scans the children of current (see ARTScanChildrenInternal).
template <class NODE_HANDLE, class PREFIX, class ART_TYPE, class CHILD_HANDLER, class PUSH>
void ARTScanChildren(ART_TYPE &art, const NodePtr current_ptr, CHILD_HANDLER &&child_handler, PUSH &&push) {
	switch (current_ptr.GetType()) {
	case NType::LEAF_INLINED:
	case NType::LEAF:
	case NType::NODE_7_LEAF:
	case NType::NODE_15_LEAF:
	case NType::NODE_256_LEAF:
		break;
	case NType::PREFIX: {
		NODE_HANDLE handle(art, current_ptr);
		auto &child_ptr = PREFIX::ChildRef(art, handle);
		auto next_ptr = child_handler(child_ptr);
		if (next_ptr) {
			push(next_ptr.Get());
		}
		break;
	}
	case NType::NODE_4:
		ARTScanChildrenInternal<Node4, NODE_HANDLE>(art, current_ptr, child_handler, push);
		break;
	case NType::NODE_16:
		ARTScanChildrenInternal<Node16, NODE_HANDLE>(art, current_ptr, child_handler, push);
		break;
	case NType::NODE_48:
		ARTScanChildrenInternal<Node48, NODE_HANDLE>(art, current_ptr, child_handler, push);
		break;
	case NType::NODE_256:
		ARTScanChildrenInternal<Node256, NODE_HANDLE>(art, current_ptr, child_handler, push);
		break;
	default:
		throw InternalException("invalid node type for ARTScanChildren: %d", current_ptr.GetType());
	}
}

//===--------------------------------------------------------------------===//
// ARTScanPreorder
//===--------------------------------------------------------------------===//

template <class NODE_HANDLE, class PREFIX, class ART_TYPE, class NODE_REF, class CHILD_HANDLER, class ON_POP>
void ARTScanPreorderInternal(ART_TYPE &art, NODE_REF &root_ptr, CHILD_HANDLER &&child_handler, ON_POP &&on_pop) {
	vector<NodePtr> stack;
	auto push = [&stack](const NodePtr node_ptr) {
		stack.push_back(node_ptr);
	};

	// The caller must keep the root slot valid while child_handler runs. This is implicit for the
	// top-level ART root used by current callers; a subtree root requires its containing parent to remain pinned.
	auto next_ptr = child_handler(root_ptr);
	if (next_ptr) {
		push(next_ptr.Get());
	}

	while (!stack.empty()) {
		NodePtr current_ptr = stack.back();
		stack.pop_back();

		if (on_pop(current_ptr) == ARTScanNodeResult::SKIP) {
			continue;
		}
		ARTScanChildren<NODE_HANDLE, PREFIX>(art, current_ptr, child_handler, push);
	}
}

//! Pre-order scanner.
//! child_handler runs on the root and on each child slot. For child slots, it runs while the parent
//! is pinned and can update the slot in place. It returns the NodePtr value to continue the traversal with,
//! or an empty OptionalNodePtr to stop.
//! on_pop runs on each popped node with no scanner-owned handles held and decides whether to scan its children.
//! The caller may still hold a pin protecting the root slot.
template <class CHILD_HANDLER, class ON_POP>
void ARTScanPreorder(ART &art, NodePtr &root_ptr, CHILD_HANDLER &&child_handler, ON_POP &&on_pop) {
	ARTScanPreorderInternal<NodeHandle, PrefixHandle>(art, root_ptr, child_handler, on_pop);
}

//! Pre-order scanner over an immutable ART (see ARTScanPreorder).
template <class CHILD_HANDLER, class ON_POP>
void ARTConstScanPreorder(const ART &art, const NodePtr &root_ptr, CHILD_HANDLER &&child_handler, ON_POP &&on_pop) {
	ARTScanPreorderInternal<ConstNodeHandle, ConstPrefixHandle>(art, root_ptr, child_handler, on_pop);
}

//===--------------------------------------------------------------------===//
// ARTScanPostorder
//===--------------------------------------------------------------------===//

struct ARTPostOrderScanEntry {
	ARTPostOrderScanEntry(NodePtr node_ptr, bool children_visited_p)
	    : node_ptr(node_ptr), children_visited(children_visited_p) {
	}

	NodePtr node_ptr;
	bool children_visited;
};

//! Post-order scanner: each node is visited twice via the children_visited flag in ARTPostOrderScanEntry.
//! On the first visit, child_handler runs on each child slot (under the parent's pin) and returns
//! the NodePtr value to push, or an empty OptionalNodePtr.
//! On the second visit, after all descendants have been processed, post_handler runs with no scanner-owned handles
//! held. The caller may still hold a pin protecting the root slot.
template <class CHILD_HANDLER, class POST_HANDLER>
void ARTScanPostorder(ART &art, NodePtr &root_ptr, CHILD_HANDLER &&child_handler, POST_HANDLER &&post_handler) {
	vector<ARTPostOrderScanEntry> stack;
	auto push = [&stack](const NodePtr node_ptr) {
		stack.push_back(ARTPostOrderScanEntry {node_ptr, false});
	};

	D_ASSERT(root_ptr.HasMetadata());
	stack.push_back(ARTPostOrderScanEntry {root_ptr, false});

	while (!stack.empty()) {
		if (stack.back().children_visited) {
			auto current_ptr = stack.back().node_ptr;
			post_handler(current_ptr);
			stack.pop_back();
			continue;
		}

		auto current_ptr = stack.back().node_ptr;
		stack.back().children_visited = true;
		ARTScanChildren<NodeHandle, PrefixHandle>(art, current_ptr, child_handler, push);
	}
}

} // namespace duckdb
