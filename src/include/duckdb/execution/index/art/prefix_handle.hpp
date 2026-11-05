//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/prefix_handle.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/execution/index/fixed_size_allocator.hpp"
#include "duckdb/execution/index/art/art.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/execution/index/art/node_handle.hpp"

namespace duckdb {

class ARTKey;

struct PrefixChain;

//! PrefixHandle owns the pin for a mutable prefix node.
class PrefixHandle {
public:
	static constexpr NType PREFIX = NType::PREFIX;
	static constexpr uint8_t DEPRECATED_COUNT = 15;

public:
	explicit PrefixHandle(NodeHandle &&handle_p) : handle(std::move(handle_p)) {
		D_ASSERT(handle.GetType() == PREFIX);
	}

	PrefixHandle(const PrefixHandle &) = delete;
	PrefixHandle &operator=(const PrefixHandle &) = delete;
	PrefixHandle(PrefixHandle &&) = default;
	PrefixHandle &operator=(PrefixHandle &&) = default;

public:
	//! Create a non-empty prefix chain and return its root pointer and pinned final child location.
	static PrefixChain New(ART &art, const ARTKey &key, const idx_t depth, const idx_t count);

	//! Splits the prefix at pos. branching_node4_ptr must identify an already allocated, ungated node.
	//! If pos > 0, prefix_ptr keeps the bytes before pos and points to branching_node4_ptr; its gate is unchanged.
	//! If pos == 0, the prefix is freed and prefix_ptr is set to branching_node4_ptr, which inherits the prefix's gate
	//! status. Returns the child containing the remaining bytes and subtree after the split byte. The caller must save
	//! the split byte before calling and attach the returned child to branching_node4_ptr under that byte. Gate status
	//! is handled internally; the caller does not need to set it after splitting.
	static NodePtr Split(ART &art, NodePtr &prefix_ptr, NodePtr &branching_node4_ptr, const uint8_t pos);

	//! Create a new deprecated prefix node and return a handle to it.
	static NodeHandle NewDeprecated(FixedSizeAllocator &allocator, NodePtr &node_ptr);

public:
	data_ptr_t Data() {
		return handle.GetPtr();
	}

	uint8_t GetCount(const ART &art) {
		return Data()[art.PrefixCount()];
	}

	void SetCount(const ART &art, const uint8_t count) {
		Data()[art.PrefixCount()] = count;
	}

	uint8_t GetByte(const idx_t pos) {
		return Data()[pos];
	}

	void SetByte(const idx_t pos, const uint8_t byte) {
		Data()[pos] = byte;
	}

	//! Returns the child NodePtr. The reference is valid while this handle owns the prefix pin.
	NodePtr &Child(const ART &art) {
		return ChildRef(art, handle);
	}

	//! Get a mutable reference to the child NodePtr of the prefix.
	static NodePtr &ChildRef(const ART &art, NodeHandle &handle) {
		return *reinterpret_cast<NodePtr *>(handle.GetPtr() + art.PrefixCount() + 1);
	}

	//! Get a mutable reference to the child NodePtr using an explicit prefix byte count.
	static NodePtr &ChildRefWithCount(NodeHandle &handle, const idx_t count) {
		return ChildRefWithCount(handle.GetPtr(), count);
	}

	//! Get a mutable reference to the child NodePtr using an explicit prefix byte count.
	static NodePtr &ChildRefWithCount(const data_ptr_t data, const idx_t count) {
		return *reinterpret_cast<NodePtr *>(data + count + 1);
	}

	//! Transform prefix chain to deprecated format.
	//! Returns an empty OptionalNodePtr if the prefix was not loaded from storage (early out) or if the endpoint
	//! was a gated node (handled internally). Otherwise, returns a copy of the child pointer at the tail of
	//! the prefix chain for further traversal.
	static OptionalNodePtr TransformToDeprecated(ART &art, NodePtr &node_ptr, TransformToDeprecatedState &state);

private:
	static PrefixHandle NewInternal(ART &art, NodePtr &node_ptr, const_data_ptr_t data, const uint8_t count,
	                                const idx_t offset);
	static PrefixHandle AppendByte(ART &art, PrefixHandle prefix, const uint8_t byte);
	static void Append(ART &art, PrefixHandle prefix, NodePtr other_ptr);

	static NodeHandle TransformToDeprecatedAppend(NodeHandle tail_handle, ART &art, FixedSizeAllocator &allocator,
	                                              const uint8_t byte);

private:
	NodeHandle handle;
};

//! A newly allocated prefix chain with a pinned final child location.
struct PrefixChain {
	//! Pointer value identifying the first prefix.
	NodePtr root_ptr;
	//! Pins the final prefix containing the child pointer to fill.
	PrefixHandle tail;
};

} // namespace duckdb
