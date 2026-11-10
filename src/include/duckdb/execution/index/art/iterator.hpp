//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/index/art/iterator.hpp
//
//
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/stack.hpp"
#include "duckdb/execution/index/art/art_key.hpp"
#include "duckdb/execution/index/art/leaf.hpp"
#include "duckdb/execution/index/art/node.hpp"
#include "duckdb/storage/arena_allocator.hpp"

namespace duckdb {

//! Keeps track of the byte leading to the currently active child of the node.
struct IteratorEntry {
	IteratorEntry(NodePtr node_ptr, uint8_t byte) : node_ptr(node_ptr), byte(byte) {
	}

	NodePtr node_ptr;
	uint8_t byte = 0;
};

//! Keeps track of the current key in the iterator leading down to the top node in the stack.
class IteratorKey {
public:
	//! Pushes a byte into the current key.
	inline void Push(const uint8_t key_byte) {
		key_bytes.push_back(key_byte);
	}
	//! Pops n bytes from the current key.
	inline void Pop(const idx_t n) {
		key_bytes.resize(key_bytes.size() - n);
	}
	//! Returns the byte at idx.
	inline uint8_t &operator[](idx_t idx) {
		D_ASSERT(idx < key_bytes.size());
		return key_bytes[idx];
	}
	// Returns the number of key bytes.
	inline idx_t Size() const {
		return key_bytes.size();
	}
	//! Returns a pointer to the key bytes.
	inline const_data_ptr_t Data() const {
		return const_data_ptr_cast(key_bytes.data());
	}

	//! Returns true, if key_bytes contains all bytes of key.
	bool Contains(const ARTKey &key) const;
	//! Returns true, if key_bytes is greater than [or equal to] the key.
	bool GreaterThan(const ARTKey &key, const bool equal, const uint8_t nested_depth) const;

private:
	unsafe_vector<uint8_t> key_bytes;
};

//===--------------------------------------------------------------------===//
// Scan Output Policies
//===--------------------------------------------------------------------===//

//! Output policy for scanning row IDs only into a set.
struct RowIdSetOutput {
	set<row_t> &row_ids;
	const idx_t capacity;

	RowIdSetOutput(set<row_t> &row_ids, const idx_t capacity) : row_ids(row_ids), capacity(capacity) {
	}

	bool TryAdd(const row_t rid) {
		D_ASSERT(row_ids.size() <= capacity);
		if (row_ids.size() >= capacity) {
			return false;
		}
		row_ids.insert(rid);
		return true;
	}
	void SetKey(const IteratorKey &, const idx_t) {
		// No-op: we don't need keys for row ID output.
	}
};

//! Stores Row IDs in an unsafe_vector with sorting and deduplication.
class RowIdVectorOutput {
public:
	explicit RowIdVectorOutput(idx_t capacity);

	bool TryAdd(row_t row_id);
	void SetKey(const IteratorKey &, const idx_t) {
		// No-op: we don't need keys for row ID output.
	}

	//! Clear all collection state while retaining reusable vector storage.
	void Reset();
	//! Normalize and transfer ownership of the contiguous Row IDs.
	unsafe_vector<row_t> TakeRows();

private:
	void Normalize();

private:
	unsafe_vector<row_t> row_ids;
	const idx_t capacity;
};

//! Output policy for scanning keys and row IDs.
struct KeyRowIdOutput {
	ArenaAllocator &arena;
	unsafe_vector<ARTKey> &keys;
	unsafe_vector<ARTKey> &row_id_keys;
	const idx_t capacity;
	idx_t count = 0;
	const_data_ptr_t key_data = nullptr;
	idx_t key_len = 0;

	KeyRowIdOutput(ArenaAllocator &arena, unsafe_vector<ARTKey> &keys, unsafe_vector<ARTKey> &row_id_keys,
	               const idx_t capacity)
	    : arena(arena), keys(keys), row_id_keys(row_id_keys), capacity(capacity) {
	}

	idx_t Count() const {
		return count;
	}
	void Reset() {
		count = 0;
		arena.Reset();
	}
	bool TryAdd(const row_t rid) {
		D_ASSERT(count <= capacity);
		if (count >= capacity) {
			return false;
		}
		keys[count] = ARTKey::CreateARTKeyFromBytes(arena, key_data, key_len);
		row_id_keys[count] = ARTKey::CreateARTKey<row_t>(arena, rid);
		count++;
		return true;
	}
	void SetKey(const IteratorKey &current_key, const idx_t column_key_len) {
		key_data = current_key.Data();
		key_len = column_key_len;
	}
};

//! Scanning state. The scanning output policies allow us to pass in a capacity while scanning, so that when the
//! scan fills up the capacity, we can pause the scan state at that location, and resume scanning later.
enum class ARTScanProgress : uint8_t { COMPLETED = 0, PAUSED = 1 };

class Iterator {
public:
	static constexpr uint8_t ROW_ID_SIZE = sizeof(row_t);

public:
	explicit Iterator(const ART &art) : art(art), status(GateStatus::GATE_NOT_SET) {};
	//! Holds the current key leading down to the top node on the stack.
	IteratorKey current_key;

public:
	//! Templated scan implementation. Output policy defines how results are emitted.
	//! Returns COMPLETED if scan finished, PAUSED if stopped due to output capacity.
	template <typename Output>
	ARTScanProgress Scan(const ARTKey &upper_bound, Output &output, bool equal);

	//! Finds the minimum (leaf) of the current subtree.
	void FindMinimum(NodePtr current_ptr);
	//! Finds the lower bound of the ART and adds the nodes to the stack. Returns false, if the lower
	//! bound exceeds the maximum value of the ART.
	bool LowerBound(NodePtr current_ptr, const ARTKey &key, const bool equal);

	//! Returns the nested depth.
	uint8_t GetNestedDepth() const {
		return nested_depth;
	}

private:
	//! The ART.
	const ART &art;
	//! Stack of nodes from the root to the currently active node.
	stack<IteratorEntry> nodes;
	//! Last visited leaf node.
	NodePtr last_leaf_ptr = NodePtr();
	//! Holds the row ID of nested leaves.
	uint8_t row_id[ROW_ID_SIZE];
	//! True, if we passed a gate.
	GateStatus status;
	//! Depth in a nested leaf.
	uint8_t nested_depth = 0;
	//! True, if we entered a nested leaf to retrieve the next node.
	bool entered_nested_leaf = false;

	//! State for resuming a scan after early return due to the Output policy capacity (see note for the ARTScanProgress
	//! enum).
	struct ResumeScanState {
		//! For LEAF: cached row IDs and current position.
		set<row_t> cached_row_ids;
		set<row_t>::iterator cached_row_ids_it;
		bool has_cached_row_ids = false;
		//! For nested leaves: current byte position and whether we've started.
		uint8_t nested_byte = 0;
		bool nested_started = false;
	};
	ResumeScanState resume_state;

private:
	//! Goes to the next leaf in the ART and sets it as last_leaf_ptr,
	//! returns false if there is no next leaf.
	bool Next();
	//! Pop the top node from the stack of iterator entries and adjust the current key.
	void PopNode();
};
} // namespace duckdb
