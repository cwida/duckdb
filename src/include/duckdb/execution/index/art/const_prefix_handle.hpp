//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/const_prefix_handle.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/node_handle.hpp"

namespace duckdb {

//! ConstPrefixHandle owns the pin for a read-only prefix node.
class ConstPrefixHandle {
public:
	static constexpr NType PREFIX = NType::PREFIX;

public:
	explicit ConstPrefixHandle(const ART &art, const NodePtr node_ptr) : handle(art, node_ptr) {
		D_ASSERT(node_ptr.GetType() == PREFIX);
	}

	ConstPrefixHandle(const ConstPrefixHandle &) = delete;
	ConstPrefixHandle &operator=(const ConstPrefixHandle &) = delete;
	ConstPrefixHandle(ConstPrefixHandle &&) = delete;
	ConstPrefixHandle &operator=(ConstPrefixHandle &&) = delete;

public:
	const_data_ptr_t Data() const {
		return handle.GetPtr();
	}

	uint8_t GetCount(const ART &art) const {
		return Data()[art.PrefixCount()];
	}

	uint8_t GetByte(const idx_t pos) const {
		return Data()[pos];
	}

	//! Returns the child slot. The reference is valid while this ConstPrefixHandle is alive.
	const NodePtr &Child(const ART &art) const {
		return ChildRef(art, handle);
	}

public:
	//! Get a const reference to the child slot of the prefix.
	static const NodePtr &ChildRef(const ART &art, const ConstNodeHandle &handle) {
		return *reinterpret_cast<const NodePtr *>(handle.GetPtr() + art.PrefixCount() + 1);
	}

	//! Traverses and verifies the node and its subtree.
	static void Verify(ART &art, const NodePtr &node_ptr);

	//! Returns the string representation of the node using ToStringOptions.
	static string ToString(ART &art, const NodePtr &node_ptr, const ToStringOptions &options);

private:
	template <class F>
	static NodePtr Iterator(ART &art, NodePtr node_ptr, const bool exit_gate, F &&lambda) {
		while (node_ptr.HasMetadata() && node_ptr.GetType() == PREFIX) {
			ConstNodeHandle handle(art, node_ptr);
			auto data = handle.GetPtr();
			NodePtr child_ptr = ChildRef(art, handle);

			lambda(handle, data, child_ptr);

			node_ptr = child_ptr;
			if (exit_gate && node_ptr.GetGateStatus() == GateStatus::GATE_SET) {
				break;
			}
		}
		return node_ptr;
	}

private:
	ConstNodeHandle handle;
};

} // namespace duckdb
