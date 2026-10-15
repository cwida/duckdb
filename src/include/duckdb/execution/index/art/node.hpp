//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/node.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/assert.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/index_pointer.hpp"
#include "duckdb/execution/index/art/node_handle.hpp"

namespace duckdb {

enum class NType : uint8_t {
	PREFIX = 1,
	LEAF = 2,
	NODE_4 = 3,
	NODE_16 = 4,
	NODE_48 = 5,
	NODE_256 = 6,
	LEAF_INLINED = 7,
	NODE_7_LEAF = 8,
	NODE_15_LEAF = 9,
	NODE_256_LEAF = 10,
};

enum class GateStatus : uint8_t {
	GATE_NOT_SET = 0,
	GATE_SET = 1,
};

class ART;
class Prefix;
class ARTKey;
class FixedSizeAllocator;
class OptionalNodePtr;

//! State for TransformToDeprecated operations
class TransformToDeprecatedState {
public:
	explicit TransformToDeprecatedState(unsafe_unique_ptr<FixedSizeAllocator> allocator_p)
	    : allocator(std::move(allocator_p)) {
	}

	TransformToDeprecatedState() = delete;
	TransformToDeprecatedState(const TransformToDeprecatedState &) = delete;
	TransformToDeprecatedState &operator=(const TransformToDeprecatedState &) = delete;
	TransformToDeprecatedState(TransformToDeprecatedState &&) = delete;
	TransformToDeprecatedState &operator=(TransformToDeprecatedState &&) = delete;

public:
	bool HasAllocator() const {
		return allocator != nullptr;
	}

	FixedSizeAllocator &GetAllocator() const {
		D_ASSERT(HasAllocator());
		return *allocator;
	}

	unsafe_unique_ptr<FixedSizeAllocator> TakeAllocator() {
		return std::move(allocator);
	}

private:
	//! Allocator for creating deprecated nodes.
	unsafe_unique_ptr<FixedSizeAllocator> allocator;
};

//! Options for ToString printing functions
struct ToStringOptions {
	bool inside_gate = false;
	bool display_ascii = false;
	// Optional key argument to only print the path along to a specific key.
	// This prints nodes along the path, as well as the child bytes, but doesn't traverse into children not on the path
	// to the optional key_path.
	// This works in conjunction with the expand_after_n_levels and structure_only arguments.
	// Note that nested ARTs are printed in their entirety regardless.
	optional_ptr<const ARTKey> key_path = nullptr;
	idx_t key_depth = 0;
	// When using key_path or structure_only, this controls when to override those options and print the full tree.
	// Set to 0 to print full tree immediately. Set to N to traverse N levels (following key_path/structure_only
	// behavior) before expanding to print the full tree at that depth. This is useful to see a region of the ART
	// around a specific key - e.g., set to (key_depth - 1) to see siblings of the target key.
	idx_t expand_after_n_levels = 0;
	bool print_deprecated_leaves = true;
	// Similar to key path, but don't print the other child bytes at each node along the path to the key, i.e. skip
	// printing node contents. This gives a very barebones skeleton of the node structure leading to a key, and this
	// can also be short circuited by expand_after_n_levels.
	bool structure_only = false;
	// Accumulated prefix for tree-style rendering (contains "│   " and "    " segments)
	string tree_prefix = "";

	ToStringOptions() = default;

	ToStringOptions(bool inside_gate, bool display_ascii, optional_ptr<const ARTKey> key_path, idx_t key_depth,
	                idx_t expand_after_n_levels, bool print_deprecated_leaves, bool structure_only)
	    : inside_gate(inside_gate), display_ascii(display_ascii), key_path(key_path), key_depth(key_depth),
	      expand_after_n_levels(expand_after_n_levels), print_deprecated_leaves(print_deprecated_leaves),
	      structure_only(structure_only) {
	}
};

//! The NodePtr is the pointer class of the ART index.
//! It inherits from the IndexPointer, and adds ART-specific functionality.
class NodePtr : public IndexPointer {
	friend class Prefix;

public:
	//! A gate sets the leftmost bit of the metadata, binary: 1000-0000.
	static constexpr uint8_t AND_GATE = 0x80;
	static constexpr idx_t AND_ROW_ID = 0x00FFFFFFFFFFFFFF;

public:
	//! Get a new pointer to a node and initialize it.
	static void New(ART &art, NodePtr &node_ptr, const NType type);
	//! Free the node.
	static void FreeNode(ART &art, NodePtr &node_ptr);
	//! Free the node and its children.
	static void FreeTree(ART &art, NodePtr &subtree_root_ptr);

	//! Get a reference to the allocator.
	static FixedSizeAllocator &GetAllocator(const ART &art, const NType type);
	//! Get the index of a node type's allocator.
	static uint8_t GetAllocatorIdx(const NType type);

	//! Get a reference to a node.
	template <class NODE>
	static inline NODE &Ref(const ART &art, const NodePtr node_ptr, const NType type) {
		D_ASSERT(node_ptr.GetType() != NType::PREFIX);
		return *(GetAllocator(art, type).Get<NODE>(node_ptr, !std::is_const<NODE>::value));
	}

	//! Replace the child at byte.
	void ReplaceChild(const ART &art, const uint8_t byte, const NodePtr child_ptr = NodePtr()) const;
	//! Insert the child at byte.
	static void InsertChild(ART &art, NodePtr &node_ptr, const uint8_t byte, const NodePtr child_ptr = NodePtr());
	//! Delete the child at byte.
	static void DeleteChild(ART &art, NodePtr &node_ptr, NodePtr &parent_ptr, const uint8_t byte,
	                        const GateStatus status, const ARTKey &row_id);

	//! Get the child node at byte, if it exists.
	OptionalNodePtr GetChildNode(const ART &art, const uint8_t byte) const;
	//! Get the child at byte, if it exists. The caller must retain handle while using the reference.
	static unsafe_optional_ptr<NodePtr> GetChildMutable(NodeHandle &handle, const uint8_t byte);
	//! Get the first child node >= byte, if it exists, and update byte.
	OptionalNodePtr GetNextChildNode(const ART &art, uint8_t &byte) const;
	//! Get the immutable child at byte.
	const unsafe_optional_ptr<NodePtr> GetChild(ART &art, const uint8_t byte) const;
	//! Get the child at byte.
	unsafe_optional_ptr<NodePtr> GetChildMutableUnsafe(ART &art, const uint8_t byte, const bool unsafe = false) const;
	//! Get the first immutable child greater than or equal to the byte.
	const unsafe_optional_ptr<NodePtr> GetNextChild(ART &art, uint8_t &byte) const;
	//! Returns true, if the byte exists, else false.
	bool HasByte(const ART &art, const uint8_t byte) const;
	//! Get the first byte greater than or equal to the byte.
	bool GetNextByte(const ART &art, uint8_t &byte) const;

	//! Traverses and verifies the node.
	void Verify(ART &art) const;
	//! Counts each node type.
	void VerifyAllocations(ART &art, unordered_map<uint8_t, idx_t> &node_counts) const;

	//! Returns the internal node type for a count.
	static NType GetInternalNodeType(const idx_t count);

	//! Transform the node storage to deprecated storage.
	static void TransformToDeprecated(ART &art, NodePtr &node_ptr, TransformToDeprecatedState &state);

	//! Returns the string representation of the node at indentation level.
	//!
	//! Parameters:
	//! - art: root node of tree being printed.
	//! - options: Printing options (see ToStringOptions struct for details).
	string ToString(ART &art, const ToStringOptions &options) const;

	//! Returns the node type.
	inline NType GetType() const {
		return NType(GetMetadata() & ~AND_GATE);
	}

	//! True, if the node is a Node4, Node16, Node48, or Node256.
	bool IsInternalNode() const;
	//! True, if the node is a Node7Leaf, Node15Leaf, or Node256Leaf.
	bool IsNestedLeaf() const;
	//! True, if the node is any leaf.
	bool IsAnyLeaf() const;

	//! Get the row ID (8th to 63rd bit).
	inline row_t GetRowId() const {
		return UnsafeNumericCast<row_t>(Get() & AND_ROW_ID);
	}
	//! Set the row ID (8th to 63rd bit).
	inline void SetRowId(const row_t row_id) {
		Set((Get() & AND_METADATA) | UnsafeNumericCast<idx_t>(row_id));
	}

	//! Returns the gate status of a node.
	inline GateStatus GetGateStatus() const {
		return (GetMetadata() & AND_GATE) == 0 ? GateStatus::GATE_NOT_SET : GateStatus::GATE_SET;
	}
	//! Sets the gate status of a node.
	inline void SetGateStatus(const GateStatus status) {
		switch (status) {
		case GateStatus::GATE_SET:
			D_ASSERT(GetType() != NType::LEAF_INLINED);
			SetMetadata(GetMetadata() | AND_GATE);
			break;
		case GateStatus::GATE_NOT_SET:
			SetMetadata(GetMetadata() & ~AND_GATE);
			break;
		}
	}

	//! Assign operator.
	inline void operator=(const IndexPointer &ptr) {
		Set(ptr.Get());
	}

private:
	//! Prints only the children of an internal node (used for tree-style printing).
	string ToStringChildren(ART &art, const ToStringOptions &options) const;
};

//! NodeChildren holds the extracted bytes of a node, and their respective children.
//! The bytes and children are valid as long as the arena is valid,
//! even if the original node has been freed.
struct NodeChildren {
	NodeChildren() = delete;
	NodeChildren(array_ptr<uint8_t> bytes, array_ptr<NodePtr> children) : bytes(bytes), children(children) {};

	array_ptr<uint8_t> bytes;
	array_ptr<NodePtr> children;
};

//! OptionalNodePtr holds a copied NodePtr value or is empty.
//! A NodePtr without metadata is the empty state.
class OptionalNodePtr {
public:
	OptionalNodePtr() = default;
	OptionalNodePtr(const NodePtr node_ptr) : node_ptr(node_ptr) { // NOLINT: allow implicit conversion from NodePtr
	}

	//! Returns true if the OptionalNodePtr holds a valid NodePtr.
	explicit operator bool() const {
		return node_ptr.HasMetadata();
	}

	//! Returns the copied NodePtr. Must only be called if it is valid.
	NodePtr Get() const {
		D_ASSERT(node_ptr.HasMetadata());
		return node_ptr;
	}

private:
	NodePtr node_ptr;
};

} // namespace duckdb
