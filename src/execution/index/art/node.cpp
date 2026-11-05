#include "duckdb/execution/index/art/node.hpp"

#include "duckdb/common/limits.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/swap.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/art_scanner.hpp"
#include "duckdb/execution/index/art/base_leaf.hpp"
#include "duckdb/execution/index/art/base_node.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node256.hpp"
#include "duckdb/execution/index/art/node256_leaf.hpp"
#include "duckdb/execution/index/art/node48.hpp"
#include "duckdb/execution/index/art/const_prefix_handle.hpp"
#include "duckdb/execution/index/art/prefix.hpp"
#include "duckdb/execution/index/art/prefix_handle.hpp"
#include "duckdb/storage/table_io_manager.hpp"

namespace duckdb {

//===--------------------------------------------------------------------===//
// New and free
//===--------------------------------------------------------------------===//

void NodePtr::New(ART &art, NodePtr &node_ptr, NType type) {
	switch (type) {
	case NType::NODE_7_LEAF:
		Node7Leaf::New(art, node_ptr);
		break;
	case NType::NODE_15_LEAF:
		Node15Leaf::New(art, node_ptr);
		break;
	case NType::NODE_256_LEAF:
		Node256Leaf::New(art, node_ptr);
		break;
	case NType::NODE_4:
		Node4::New(art, node_ptr);
		break;
	case NType::NODE_16:
		Node16::New(art, node_ptr);
		break;
	case NType::NODE_48:
		Node48::New(art, node_ptr);
		break;
	case NType::NODE_256:
		Node256::New(art, node_ptr);
		break;
	default:
		throw InternalException("Invalid node type for New: %d.", type);
	}
}

void NodePtr::FreeNode(ART &art, NodePtr &node_ptr) {
	D_ASSERT(node_ptr.HasMetadata());
	GetAllocator(art, node_ptr.GetType()).Free(node_ptr);
	node_ptr.Clear();
}

void NodePtr::FreeTree(ART &art, NodePtr &subtree_root_ptr) {
	if (!subtree_root_ptr.HasMetadata()) {
		return;
	}
	// All nodes should be pushed onto the stack.
	auto child_handler = [](NodePtr &child_ptr) -> OptionalNodePtr {
		D_ASSERT(child_ptr.HasMetadata());
		return child_ptr;
	};
	// We freed the subtree pointed to by the current node. Free the node.
	auto post_handler = [&](NodePtr current_ptr) {
		D_ASSERT(current_ptr.HasMetadata());
		auto type = current_ptr.GetType();
		switch (type) {
		case NType::LEAF_INLINED:
			break;
		case NType::LEAF:
			Leaf::DeprecatedFree(art, current_ptr);
			break;
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF:
		case NType::PREFIX:
		case NType::NODE_4:
		case NType::NODE_16:
		case NType::NODE_48:
		case NType::NODE_256:
			FreeNode(art, current_ptr);
			break;
		default:
			throw InternalException("invalid node type for FreeTree: %d", type);
		}
	};
	ARTScanPostorder(art, subtree_root_ptr, child_handler, post_handler);
	subtree_root_ptr.Clear();
}

//===--------------------------------------------------------------------===//
// Allocators
//===--------------------------------------------------------------------===//

FixedSizeAllocator &NodePtr::GetAllocator(const ART &art, const NType type) {
	return *(*art.allocators)[GetAllocatorIdx(type)];
}

uint8_t NodePtr::GetAllocatorIdx(const NType type) {
	switch (type) {
	case NType::PREFIX:
		return 0;
	case NType::LEAF:
		return 1;
	case NType::NODE_4:
		return 2;
	case NType::NODE_16:
		return 3;
	case NType::NODE_48:
		return 4;
	case NType::NODE_256:
		return 5;
	case NType::NODE_7_LEAF:
		return 6;
	case NType::NODE_15_LEAF:
		return 7;
	case NType::NODE_256_LEAF:
		return 8;
	default:
		throw InternalException("Invalid node type for GetAllocatorIdx: %d.", type);
	}
}

//===--------------------------------------------------------------------===//
// Inserts
//===--------------------------------------------------------------------===//

void NodePtr::ReplaceChild(const ART &art, const uint8_t byte, const NodePtr child_ptr) const {
	D_ASSERT(HasMetadata());

	auto type = GetType();
	switch (type) {
	case NType::NODE_4:
		return Node4::ReplaceChild(Ref<Node4>(art, *this, type), byte, child_ptr);
	case NType::NODE_16:
		return Node16::ReplaceChild(Ref<Node16>(art, *this, type), byte, child_ptr);
	case NType::NODE_48:
		return Ref<Node48>(art, *this, type).ReplaceChild(byte, child_ptr);
	case NType::NODE_256:
		return Ref<Node256>(art, *this, type).ReplaceChild(byte, child_ptr);
	default:
		throw InternalException("Invalid node type for ReplaceChild: %d.", type);
	}
}

void NodePtr::InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr) {
	D_ASSERT(node_ptr.HasMetadata());

	auto type = node_ptr.GetType();
	switch (type) {
	case NType::NODE_4:
		return Node4::InsertChild(art, node_ptr, byte, child_ptr);
	case NType::NODE_16:
		return Node16::InsertChild(art, node_ptr, byte, child_ptr);
	case NType::NODE_48:
		return Node48::InsertChild(art, node_ptr, byte, child_ptr);
	case NType::NODE_256:
		return Node256::InsertChild(art, node_ptr, byte, child_ptr);
	case NType::NODE_7_LEAF:
		return Node7Leaf::InsertByte(art, node_ptr, byte);
	case NType::NODE_15_LEAF:
		return Node15Leaf::InsertByte(art, node_ptr, byte);
	case NType::NODE_256_LEAF:
		return Node256Leaf::InsertByte(art, node_ptr, byte);
	default:
		throw InternalException("Invalid node type for InsertChild: %d.", type);
	}
}

//===--------------------------------------------------------------------===//
// Delete
//===--------------------------------------------------------------------===//

void NodePtr::DeleteChild(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte, const GateStatus status,
                          const ARTKey &row_id) {
	D_ASSERT(node_ptr.HasMetadata());

	auto type = node_ptr.GetType();
	switch (type) {
	case NType::NODE_4:
		return Node4::DeleteChild(art, node_ptr, parent_ptr, byte, status);
	case NType::NODE_16:
		return Node16::DeleteChild(art, node_ptr, byte);
	case NType::NODE_48:
		return Node48::DeleteChild(art, node_ptr, byte);
	case NType::NODE_256:
		return Node256::DeleteChild(art, node_ptr, byte);
	case NType::NODE_7_LEAF:
		return Node7Leaf::DeleteByte(art, node_ptr, parent_ptr, byte, row_id);
	case NType::NODE_15_LEAF:
		return Node15Leaf::DeleteByte(art, node_ptr, byte);
	case NType::NODE_256_LEAF:
		return Node256Leaf::DeleteByte(art, node_ptr, byte);
	default:
		throw InternalException("Invalid node type for DeleteChild: %d.", type);
	}
}

//===--------------------------------------------------------------------===//
// Get child and byte.
//===--------------------------------------------------------------------===//

template <class NODE>
static unsafe_optional_ptr<NodePtr> GetChildInternal(ART &art, NODE &node_ptr, const uint8_t byte, const bool unsafe) {
	D_ASSERT(node_ptr.HasMetadata());

	auto type = node_ptr.GetType();
	switch (type) {
	case NType::NODE_4:
		return Node4::GetChild(NodePtr::Ref<Node4>(art, node_ptr, type), byte, unsafe);
	case NType::NODE_16:
		return Node16::GetChild(NodePtr::Ref<Node16>(art, node_ptr, type), byte, unsafe);
	case NType::NODE_48:
		return Node48::GetChild(NodePtr::Ref<Node48>(art, node_ptr, type), byte, unsafe);
	case NType::NODE_256: {
		return Node256::GetChild(NodePtr::Ref<Node256>(art, node_ptr, type), byte, unsafe);
	}
	default:
		throw InternalException("Invalid node type for GetChildInternal: %d.", type);
	}
}

const unsafe_optional_ptr<NodePtr> NodePtr::GetChild(ART &art, const uint8_t byte) const {
	return GetChildInternal(art, *this, byte, false);
}

unsafe_optional_ptr<NodePtr> NodePtr::GetChildMutableUnsafe(ART &art, const uint8_t byte, const bool unsafe) const {
	return GetChildInternal(art, *this, byte, unsafe);
}

OptionalNodePtr NodePtr::GetChildNode(const ART &art, const uint8_t byte) const {
	D_ASSERT(HasMetadata());
	auto type = GetType();
	ConstNodeHandle handle(art, *this);
	switch (type) {
	case NType::NODE_4:
		return Node4::GetChildNode(handle.Get<Node4>(), byte);
	case NType::NODE_16:
		return Node16::GetChildNode(handle.Get<Node16>(), byte);
	case NType::NODE_48:
		return Node48::GetChildNode(handle.Get<Node48>(), byte);
	case NType::NODE_256:
		return Node256::GetChildNode(handle.Get<Node256>(), byte);
	default:
		throw InternalException("Invalid node type for GetChildNode: %d.", type);
	}
}

unsafe_optional_ptr<NodePtr> NodePtr::GetChildMutable(NodeHandle &handle, const uint8_t byte) {
	unsafe_optional_ptr<NodePtr> child;
	switch (handle.GetType()) {
	case NType::NODE_4:
		child = Node4::GetChild(handle.Get<Node4>(), byte);
		break;
	case NType::NODE_16:
		child = Node16::GetChild(handle.Get<Node16>(), byte);
		break;
	case NType::NODE_48:
		child = Node48::GetChild(handle.Get<Node48>(), byte);
		break;
	case NType::NODE_256:
		child = Node256::GetChild(handle.Get<Node256>(), byte);
		break;
	default:
		throw InternalException("Invalid node type for GetChildMutable: %d.", handle.GetType());
	}
	return child;
}

OptionalNodePtr NodePtr::GetNextChildNode(const ART &art, uint8_t &byte) const {
	D_ASSERT(HasMetadata());
	auto type = GetType();
	ConstNodeHandle handle(art, *this);
	switch (type) {
	case NType::NODE_4:
		return Node4::GetNextChildNode(handle.Get<Node4>(), byte);
	case NType::NODE_16:
		return Node16::GetNextChildNode(handle.Get<Node16>(), byte);
	case NType::NODE_48:
		return Node48::GetNextChildNode(handle.Get<Node48>(), byte);
	case NType::NODE_256:
		return Node256::GetNextChildNode(handle.Get<Node256>(), byte);
	default:
		throw InternalException("Invalid node type for GetNextChildNode: %d.", type);
	}
}

template <class NODE>
unsafe_optional_ptr<NodePtr> GetNextChildInternal(ART &art, NODE &node_ptr, uint8_t &byte) {
	D_ASSERT(node_ptr.HasMetadata());

	auto type = node_ptr.GetType();
	switch (type) {
	case NType::NODE_4:
		return Node4::GetNextChild(NodePtr::Ref<Node4>(art, node_ptr, type), byte);
	case NType::NODE_16:
		return Node16::GetNextChild(NodePtr::Ref<Node16>(art, node_ptr, type), byte);
	case NType::NODE_48:
		return Node48::GetNextChild(NodePtr::Ref<Node48>(art, node_ptr, type), byte);
	case NType::NODE_256:
		return Node256::GetNextChild(NodePtr::Ref<Node256>(art, node_ptr, type), byte);
	default:
		throw InternalException("Invalid node type for GetNextChildInternal: %d.", type);
	}
}

const unsafe_optional_ptr<NodePtr> NodePtr::GetNextChild(ART &art, uint8_t &byte) const {
	return GetNextChildInternal(art, *this, byte);
}

bool NodePtr::HasByte(const ART &art, const uint8_t byte) const {
	D_ASSERT(HasMetadata());

	auto type = GetType();
	switch (type) {
	case NType::NODE_7_LEAF:
		return Ref<const Node7Leaf>(art, *this, NType::NODE_7_LEAF).HasByte(byte);
	case NType::NODE_15_LEAF:
		return Ref<const Node15Leaf>(art, *this, NType::NODE_15_LEAF).HasByte(byte);
	case NType::NODE_256_LEAF:
		return Ref<const Node256Leaf>(art, *this, NType::NODE_256_LEAF).HasByte(byte);
	default:
		throw InternalException("Invalid node type for GetNextByte: %d.", type);
	}
}

bool NodePtr::GetNextByte(const ART &art, uint8_t &byte) const {
	D_ASSERT(HasMetadata());

	auto type = GetType();
	switch (type) {
	case NType::NODE_7_LEAF:
		return Ref<const Node7Leaf>(art, *this, NType::NODE_7_LEAF).GetNextByte(byte);
	case NType::NODE_15_LEAF:
		return Ref<const Node15Leaf>(art, *this, NType::NODE_15_LEAF).GetNextByte(byte);
	case NType::NODE_256_LEAF:
		return Ref<const Node256Leaf>(art, *this, NType::NODE_256_LEAF).GetNextByte(byte);
	default:
		throw InternalException("Invalid node type for GetNextByte: %d.", type);
	}
}

//===--------------------------------------------------------------------===//
// Utility
//===--------------------------------------------------------------------===//

idx_t GetCapacity(NType type) {
	switch (type) {
	case NType::NODE_4:
		return Node4::CAPACITY;
	case NType::NODE_7_LEAF:
		return Node7Leaf::CAPACITY;
	case NType::NODE_15_LEAF:
		return Node15Leaf::CAPACITY;
	case NType::NODE_16:
		return Node16::CAPACITY;
	case NType::NODE_48:
		return Node48::CAPACITY;
	case NType::NODE_256_LEAF:
		return Node256::CAPACITY;
	case NType::NODE_256:
		return Node256::CAPACITY;
	default:
		throw InternalException("Invalid node type for GetCapacity: %d.", type);
	}
}

NType NodePtr::GetInternalNodeType(idx_t count) {
	if (count <= Node4::CAPACITY) {
		return NType::NODE_4;
	} else if (count <= Node16::CAPACITY) {
		return NType::NODE_16;
	} else if (count <= Node48::CAPACITY) {
		return NType::NODE_48;
	}
	return NType::NODE_256;
}

bool NodePtr::IsInternalNode() const {
	switch (GetType()) {
	case NType::NODE_4:
	case NType::NODE_16:
	case NType::NODE_48:
	case NType::NODE_256:
		return true;
	default:
		return false;
	}
}

bool NodePtr::IsNestedLeaf() const {
	switch (GetType()) {
	case NType::NODE_7_LEAF:
	case NType::NODE_15_LEAF:
	case NType::NODE_256_LEAF:
		return true;
	default:
		return false;
	}
}

bool NodePtr::IsAnyLeaf() const {
	if (IsNestedLeaf()) {
		return true;
	}

	switch (GetType()) {
	case NType::LEAF_INLINED:
	case NType::LEAF:
		return true;
	default:
		return false;
	}
}

//===--------------------------------------------------------------------===//
// TransformToDeprecated
//===--------------------------------------------------------------------===//

void NodePtr::TransformToDeprecated(ART &art, NodePtr &node_ptr, TransformToDeprecatedState &state) {
	auto child_handler = [&](NodePtr &child_ptr) -> OptionalNodePtr {
		D_ASSERT(child_ptr.HasMetadata());
		if (child_ptr.GetGateStatus() == GateStatus::GATE_SET) {
			Leaf::TransformToDeprecated(art, child_ptr);
			return OptionalNodePtr();
		}
		auto type = child_ptr.GetType();
		switch (type) {
		case NType::PREFIX:
			// An empty node stops the traversal.
			return PrefixHandle::TransformToDeprecated(art, child_ptr, state);
		case NType::LEAF_INLINED:
		case NType::LEAF:
			return OptionalNodePtr();
		case NType::NODE_4:
		case NType::NODE_16:
		case NType::NODE_48:
		case NType::NODE_256:
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF:
			return child_ptr;
		default:
			throw InternalException("invalid node type for TransformToDeprecated: %d", type);
		}
	};

	auto on_pop = [&](NodePtr current_ptr) -> ARTScanNodeResult {
		auto type = current_ptr.GetType();
		if (type == NType::NODE_4 || type == NType::NODE_16 || type == NType::NODE_48 || type == NType::NODE_256) {
			auto &alloc = NodePtr::GetAllocator(art, type);
			if (!alloc.LoadedFromStorage(current_ptr)) {
				return ARTScanNodeResult::SKIP;
			}
		}
		return ARTScanNodeResult::SCAN_CHILDREN;
	};

	ARTScanPreorder(art, node_ptr, child_handler, on_pop);
}

//===--------------------------------------------------------------------===//
// Verification
//===--------------------------------------------------------------------===//

void NodePtr::Verify(ART &art) const {
	D_ASSERT(HasMetadata());

	auto type = GetType();
	switch (type) {
	case NType::LEAF_INLINED:
		return;
	case NType::LEAF:
		Leaf::DeprecatedVerify(art, *this);
		return;
	case NType::PREFIX: {
		ConstPrefixHandle::Verify(art, *this);
		return;
	}
	default:
		break;
	}

	if (!IsNestedLeaf()) {
		uint8_t byte = 0;
		auto child_ptr = GetNextChild(art, byte);
		while (child_ptr) {
			child_ptr->Verify(art);
			if (byte == NumericLimits<uint8_t>::Maximum()) {
				break;
			}
			byte++;
			child_ptr = GetNextChild(art, byte);
		}
	}
}

void NodePtr::VerifyAllocations(ART &art, unordered_map<uint8_t, idx_t> &node_counts) const {
	D_ASSERT(HasMetadata());

	auto child_handler = [&](const NodePtr &child_ptr) -> OptionalNodePtr {
		D_ASSERT(child_ptr.HasMetadata());
		auto type = child_ptr.GetType();
		switch (type) {
		case NType::LEAF_INLINED:
			return OptionalNodePtr();
		case NType::LEAF: {
			Leaf::DeprecatedVerifyAllocations(art, child_ptr, node_counts);
			return OptionalNodePtr();
		}
		case NType::NODE_7_LEAF:
		case NType::NODE_15_LEAF:
		case NType::NODE_256_LEAF:
			node_counts[GetAllocatorIdx(type)]++;
			return OptionalNodePtr();
		case NType::PREFIX:
		case NType::NODE_4:
		case NType::NODE_16:
		case NType::NODE_48:
		case NType::NODE_256:
			node_counts[GetAllocatorIdx(type)]++;
			return child_ptr;
		default:
			throw InternalException("invalid node type for VerifyAllocations: %d", type);
		}
	};

	auto on_pop = [](NodePtr) -> ARTScanNodeResult {
		return ARTScanNodeResult::SCAN_CHILDREN;
	};

	ARTConstScanPreorder(art, *this, child_handler, on_pop);
}

//===--------------------------------------------------------------------===//
// Printing
//===--------------------------------------------------------------------===//

namespace {
// Tree-style branch characters
constexpr const char *const NODE_BRANCH_MID = "├── ";
constexpr const char *const NODE_BRANCH_END = "└── ";
constexpr const char *const NODE_VERTICAL = "│   ";
constexpr const char *const NODE_SPACE = "    ";

// ASCII printable character range
constexpr uint8_t NODE_ASCII_PRINTABLE_MIN = 32;
constexpr uint8_t NODE_ASCII_PRINTABLE_MAX = 126;
} // namespace

string NodePtr::ToStringChildren(ART &art, const ToStringOptions &options) const {
	auto format_byte = [&](const uint8_t byte) {
		if (!options.inside_gate && options.display_ascii && byte >= NODE_ASCII_PRINTABLE_MIN &&
		    byte <= NODE_ASCII_PRINTABLE_MAX) {
			return string(1, static_cast<char>(byte));
		}
		return to_string(byte);
	};

	auto is_gate = GetGateStatus() == GateStatus::GATE_SET;
	auto propagate_gate = options.inside_gate || is_gate;
	auto print_full_tree = propagate_gate || !options.key_path || options.expand_after_n_levels == 0;

	string str;

	if (IsNestedLeaf()) {
		str += StringUtil::Format("%s%sLeaf |", options.tree_prefix, NODE_BRANCH_END);
		uint8_t byte = 0;
		auto has_byte = GetNextByte(art, byte);
		while (has_byte) {
			str += StringUtil::Format("%s|", format_byte(byte));
			if (byte == NumericLimits<uint8_t>::Maximum()) {
				break;
			}
			byte++;
			has_byte = GetNextByte(art, byte);
		}
		str += "\n";
	} else if (IsInternalNode()) {
		// Collect all children first to know which is last
		vector<pair<uint8_t, const NodePtr *>> children;
		uint8_t byte = 0;
		auto child_ptr = GetNextChild(art, byte);
		while (child_ptr) {
			children.emplace_back(byte, child_ptr.get());
			if (byte == NumericLimits<uint8_t>::Maximum()) {
				break;
			}
			byte++;
			child_ptr = GetNextChild(art, byte);
		}

		uint8_t expected_byte = 0;
		auto has_expected_byte = false;
		if (options.key_path && !print_full_tree && options.key_depth < options.key_path->len) {
			expected_byte = (*options.key_path)[options.key_depth];
			has_expected_byte = true;
		}

		for (idx_t child_idx = 0; child_idx < children.size(); child_idx++) {
			auto &child_entry = children[child_idx];
			auto child_byte = child_entry.first;
			auto &child_slot = child_entry.second;
			auto is_last = (child_idx == children.size() - 1);
			auto on_path = !has_expected_byte || (has_expected_byte && child_byte == expected_byte);

			// When structure_only is true and we're following a path, treat on-path child as last
			// (since we're hiding off-path siblings, the branch should end here)
			auto effective_last = is_last || (options.structure_only && has_expected_byte && on_path);
			auto branch = effective_last ? NODE_BRANCH_END : NODE_BRANCH_MID;
			auto child_prefix =
			    StringUtil::Format("%s%s", options.tree_prefix, effective_last ? NODE_SPACE : NODE_VERTICAL);

			if (on_path) {
				str += StringUtil::Format("%s%s%s\n", options.tree_prefix, branch, format_byte(child_byte));

				auto child_options = options;
				child_options.inside_gate = propagate_gate;
				child_options.key_depth = has_expected_byte ? options.key_depth + 1 : options.key_depth;
				child_options.expand_after_n_levels =
				    (options.expand_after_n_levels > 0) ? options.expand_after_n_levels - 1 : 0;

				auto child_type = child_slot->GetType();
				auto is_internal = (child_type == NType::NODE_4 || child_type == NType::NODE_16 ||
				                    child_type == NType::NODE_48 || child_type == NType::NODE_256);
				if (is_internal) {
					str += StringUtil::Format("%s%sNode%s\n", child_prefix, NODE_BRANCH_END,
					                          to_string(GetCapacity(child_type)));
					child_options.tree_prefix = StringUtil::Format("%s%s", child_prefix, NODE_SPACE);
					str += child_slot->ToStringChildren(art, child_options);
				} else {
					child_options.tree_prefix = child_prefix;
					str += child_slot->ToString(art, child_options);
				}
			} else {
				if (!options.structure_only) {
					str += StringUtil::Format("%s%s%s ...\n", options.tree_prefix, branch, format_byte(child_byte));
				}
			}
		}
	}
	return str;
}

string NodePtr::ToString(ART &art, const ToStringOptions &options) const {
	auto type = GetType();
	auto is_gate = GetGateStatus() == GateStatus::GATE_SET;
	auto propagate_gate = options.inside_gate || is_gate;

	switch (type) {
	case NType::LEAF_INLINED: {
		return options.tree_prefix + "Inlined Leaf [row ID: " + to_string(GetRowId()) + "]\n";
	}
	case NType::LEAF: {
		return Leaf::DeprecatedToString(art, *this, options);
	}
	case NType::PREFIX: {
		auto prefix_options = options;
		prefix_options.inside_gate = propagate_gate;
		auto str = ConstPrefixHandle::ToString(art, *this, prefix_options);
		if (is_gate) {
			str = options.tree_prefix + "Gate\n" + str;
		}
		return str;
	}
	default:
		break;
	}

	// For internal nodes: print header then children
	auto str = options.tree_prefix + "Node" + to_string(GetCapacity(type)) + "\n";
	str += ToStringChildren(art, options);

	if (is_gate && type != NType::PREFIX) {
		str = options.tree_prefix + "Gate\n" + str;
	}
	return str;
}

} // namespace duckdb
