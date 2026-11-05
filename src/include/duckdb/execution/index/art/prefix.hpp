//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/prefix.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"

namespace duckdb {

class ARTKey;

//! Prefix is a wrapper class to access a prefix.
//! The prefix contains up to the ART's prefix size bytes and an additional byte for the count.
//! It also contains a child NodePtr.
class Prefix {
public:
	static constexpr NType PREFIX = NType::PREFIX;

	static constexpr uint8_t ROW_ID_SIZE = sizeof(row_t);
	static constexpr uint8_t ROW_ID_COUNT = ROW_ID_SIZE - 1;
	static constexpr uint8_t DEPRECATED_COUNT = 15;
	// The child pointer and the in_memory boolean.
	static constexpr uint8_t METADATA_SIZE = sizeof(NodePtr) + 1;

public:
	Prefix() = delete;
	Prefix(const ART &art, const NodePtr node_ptr, const bool is_mutable = false, const bool set_in_memory = false);
	Prefix(FixedSizeAllocator &allocator, const NodePtr node_ptr, const idx_t count);

	data_ptr_t data;
	NodePtr *child_slot;
	bool in_memory;

public:
	static uint8_t GetByte(const ART &art, const NodePtr &node_ptr, const uint8_t pos);

public:
	//! Get a new list of prefix nodes. The node reference holds the child of the last prefix node.
	static void New(ART &art, reference<NodePtr> &node_ptr_ref, const ARTKey &key, const idx_t depth, idx_t count);

	//! Concatenates parent -> prev_node4 -> child.
	static void Concat(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte,
	                   const GateStatus node4_status, const GateStatus status);

	//! Removes up to pos bytes from the prefix.
	//! Shifts all subsequent bytes by pos. Frees empty nodes.
	static void Reduce(ART &art, NodePtr &node_ptr, const idx_t pos);

private:
	static Prefix NewInternal(ART &art, NodePtr &node_ptr, const data_ptr_t data, const uint8_t count,
	                          const idx_t offset);

	static Prefix GetTail(ART &art, const NodePtr &node_ptr);

	static void ConcatInternal(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte,
	                           const GateStatus status);
	static void ConcatNode4WasGate(ART &art, NodePtr &node4_ptr, const NodePtr child_ptr, uint8_t byte);
	static void ConcatChildIsGate(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr,
	                              uint8_t byte);
	static void ConcatOutsideGate(ART &art, NodePtr &parent_ptr, NodePtr &node4_ptr, const NodePtr child_ptr,
	                              uint8_t byte);

	Prefix Append(ART &art, const uint8_t byte);
	void Append(ART &art, NodePtr other_ptr);
	Prefix TransformToDeprecatedAppend(ART &art, FixedSizeAllocator &allocator, uint8_t byte);

private:
	template <class F, class NODE>
	static void Iterator(ART &art, reference<NODE> &node_ptr_ref, const bool exit_gate, const bool is_mutable,
	                     F &&lambda) {
		while (node_ptr_ref.get().HasMetadata() && node_ptr_ref.get().GetType() == PREFIX) {
			Prefix prefix(art, node_ptr_ref, is_mutable);
			lambda(prefix);

			node_ptr_ref = *prefix.child_slot;
			if (exit_gate && node_ptr_ref.get().GetGateStatus() == GateStatus::GATE_SET) {
				break;
			}
		}
	}
};
} // namespace duckdb
