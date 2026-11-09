//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/storage/table/segment_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/constants.hpp"
#include "duckdb/common/optional.hpp"
#include "duckdb/storage/storage_lock.hpp"
#include "duckdb/storage/table/segment_lock.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/common/atomic.hpp"

namespace duckdb {

template <class T>
struct SegmentNode {
	SegmentNode(idx_t row_start_p, shared_ptr<T> node_p, idx_t index_p)
	    : row_start(row_start_p), node(std::move(node_p)), next(nullptr), index(index_p) {
	}

public:
	optional_ptr<SegmentNode<T>> Next() const {
#ifndef DUCKDB_R_BUILD
		return next.load();
#else
		return next;
#endif
	}

	idx_t GetRowStart() const {
		return row_start;
	}
	idx_t GetRowEnd() const {
		return GetRowStart() + GetCount();
	}
	idx_t GetCount() const {
		return GetNode().count;
	}

	idx_t GetIndex() const {
		return index;
	}

	T &GetNode() const {
		return *node;
	}

	shared_ptr<T> MoveNode() {
		return std::move(node);
	}
	shared_ptr<T> &ReferenceNode() {
		return node;
	}

	bool HasNode() const {
		return node.get();
	}

	void SetNext(optional_ptr<SegmentNode<T>> next) {
		this->next = next.get();
	}

	void SetNode(shared_ptr<T> new_node) {
		node = std::move(new_node);
	}

private:
	idx_t row_start;
	shared_ptr<T> node;
	//! The next segment after this one
#ifndef DUCKDB_R_BUILD
	atomic<SegmentNode<T> *> next;
#else
	SegmentNode<T> *next;
#endif
	//! The index within the segment tree
	idx_t index;
};

template <class T>
struct LoadedSegment {
	LoadedSegment(shared_ptr<T> segment_p, idx_t row_start_p) : segment(std::move(segment_p)), row_start(row_start_p) {
	}

	shared_ptr<T> segment;
	idx_t row_start;
};

enum class SegmentTreeVerifyMode : uint8_t { CONTIGUOUS, NON_OVERLAPPING };

//! The SegmentTree maintains a list of all segments of a specific column in a table, and allows searching for a segment
//! by row number
// The const-ness of the SegmentTree is implemented in an odd manner due to the lazy loading
// in particular, most internal members are `mutable` - i.e. they can be internally modified even through `const`
// methods The reasoning this is implemented this way is that the lazy loading would otherwise
template <class T, bool SUPPORTS_LAZY_LOADING = false>
class SegmentTree {
private:
	class SegmentIterationHelper;
	class SegmentNodeIterationHelper;

public:
	explicit SegmentTree(idx_t base_row_id = 0) : finished_loading(true), base_row_id(base_row_id) {
	}
	virtual ~SegmentTree() {
	}

	//! Locks the segment tree. All methods to the segment tree either lock the segment tree, or take an already
	//! obtained lock.
	SegmentLock Lock() const {
		return SegmentLock(node_lock);
	}

	bool IsEmpty(SegmentLock &l) const {
		return GetRootSegment(l) == nullptr;
	}

	//! Gets a pointer to the first segment. Useful for scans.
	optional_ptr<SegmentNode<T>> GetRootSegment() const {
		auto l = Lock();
		return GetRootSegment(l);
	}

	optional_ptr<SegmentNode<T>> GetRootSegment(SegmentLock &l) const {
		if (nodes.empty()) {
			LoadNextSegment(l);
		}
		return GetRootSegmentInternal();
	}
	//! Obtains ownership of the data of the segment tree
	vector<unique_ptr<SegmentNode<T>>> MoveSegments(SegmentLock &l) {
		LoadAllSegments(l);
		return std::move(nodes);
	}
	vector<unique_ptr<SegmentNode<T>>> MoveSegments() {
		auto l = Lock();
		return MoveSegments(l);
	}

	vector<unique_ptr<SegmentNode<T>>> &ReferenceLoadedSegmentsMutable(SegmentLock &l) {
		return nodes;
	}

	idx_t GetSegmentCount() const {
		auto l = Lock();
		return GetSegmentCount(l);
	}
	idx_t GetSegmentCount(SegmentLock &l) const {
		LoadAllSegments(l);
		return nodes.size();
	}
	//! Gets a pointer to the nth segment. Negative numbers start from the back.
	optional_ptr<SegmentNode<T>> GetSegmentByIndex(int64_t index) const {
		auto l = Lock();
		return GetSegmentByIndex(l, index);
	}
	optional_ptr<SegmentNode<T>> GetSegmentByIndex(SegmentLock &l, int64_t index) const {
		if (index < 0) {
			// load all segments
			LoadAllSegments(l);
			index += nodes.size();
			if (index < 0) {
				return nullptr;
			}
			return nodes[UnsafeNumericCast<idx_t>(index)].get();
		} else {
			// lazily load segments until we reach the specific segment
			while (idx_t(index) >= nodes.size() && LoadNextSegment(l)) {
			}
			if (idx_t(index) >= nodes.size()) {
				return nullptr;
			}
			return nodes[UnsafeNumericCast<idx_t>(index)].get();
		}
	}
	//! Gets the next segment
	optional_ptr<SegmentNode<T>> GetNextSegment(SegmentNode<T> &node) const {
		if (!SUPPORTS_LAZY_LOADING) {
			return node.Next();
		}
		if (finished_loading) {
			return node.Next();
		}
		auto l = Lock();
		return GetNextSegment(l, node);
	}
	optional_ptr<SegmentNode<T>> GetNextSegment(SegmentLock &l, SegmentNode<T> &node) const {
#ifdef DEBUG
		D_ASSERT(RefersToSameObject(*nodes[node.GetIndex()], node));
#endif
		return GetSegmentByIndex(l, UnsafeNumericCast<int64_t>(node.GetIndex() + 1));
	}

	//! Gets a pointer to the last segment. Useful for appends.
	optional_ptr<SegmentNode<T>> GetLastSegment(SegmentLock &l) const {
		LoadAllSegments(l);
		if (nodes.empty()) {
			return nullptr;
		}
		return nodes.back().get();
	}
	//! Gets a pointer to a specific column segment for the given row
	optional_ptr<SegmentNode<T>> GetSegment(idx_t row_number) const {
		auto l = Lock();
		return GetSegment(l, row_number);
	}
	optional_ptr<SegmentNode<T>> GetSegment(SegmentLock &l, idx_t row_number) const {
		return nodes[GetSegmentIndex(l, row_number)].get();
	}

	void AppendSegment(shared_ptr<T> segment) {
		auto l = Lock();
		AppendSegment(l, std::move(segment));
	}
	void AppendSegment(shared_ptr<T> segment, idx_t row_start) {
		auto l = Lock();
		AppendSegment(l, std::move(segment), row_start);
	}
	void AppendSegment(SegmentLock &l, shared_ptr<T> segment) {
		LoadAllSegments(l);
		AppendSegmentInternal(l, std::move(segment));
	}
	void AppendSegment(SegmentLock &l, shared_ptr<T> segment, idx_t row_start) {
		LoadAllSegments(l);
		AppendSegmentInternal(l, std::move(segment), row_start);
	}
	//! Debug method, check whether the segment is in the segment tree
	bool HasSegment(SegmentNode<T> &segment) const {
		auto l = Lock();
		return HasSegment(l, segment);
	}
	bool HasSegment(SegmentLock &, SegmentNode<T> &segment) const {
		auto segment_idx = segment.GetIndex();
		return segment_idx < nodes.size() && RefersToSameObject(*nodes[segment_idx], segment);
	}

	//! Erase all segments after a specific segment
	void EraseSegments(SegmentLock &l, idx_t segment_start) {
		LoadAllSegments(l);
		if (segment_start >= nodes.size()) {
			return;
		}
		nodes.erase(nodes.begin() + UnsafeNumericCast<int64_t>(segment_start), nodes.end());
	}

	//! Get the segment index of the column segment for the given row
	idx_t GetSegmentIndex(SegmentLock &l, idx_t row_number) const {
		idx_t segment_index;
		if (TryGetSegmentIndex(l, row_number, segment_index)) {
			return segment_index;
		}
		// callers only look up rows that the tree holds, so a row in none of the segments means it is corrupt
		string error = StringUtil::Format("Corrupted segment tree: row %llu is not in any of its %llu segments\n",
		                                  row_number, nodes.size());
		for (idx_t i = 0; i < nodes.size(); i++) {
			error += StringUtil::Format("* Node %lld: Start %lld, Count %lld\n", i, nodes[i]->GetRowStart(),
			                            nodes[i]->GetCount());
		}
		throw DataCorruptionException(error);
	}

	bool TryGetSegmentIndex(SegmentLock &l, idx_t row_number, idx_t &result) const {
		// load segments until the row number is within bounds
		while (nodes.empty() || (row_number >= nodes.back()->GetRowEnd())) {
			if (!LoadNextSegment(l)) {
				break;
			}
		}
		if (nodes.empty()) {
			return false;
		}
		idx_t lower = 0;
		idx_t upper = nodes.size();
		// binary search to find the node. Searches using half-open interval [lower, upper).
		while (lower < upper) {
			idx_t index = lower + (upper - lower) / 2;
			auto &entry = *nodes[index];
			if (row_number < entry.GetRowStart()) {
				// This node and all nodes to the right are excluded.
				upper = index;
			} else if (row_number >= entry.GetRowEnd()) {
				// This node and all nodes to the left are excluded.
				lower = index + 1;
			} else {
				// entry.GetRowStart() <= row_number < entry.GetRowEnd()
				result = index;
				return true;
			}
		}
		D_ASSERT(lower == upper);
		D_ASSERT(lower == 0 || row_number >= nodes[lower - 1]->GetRowEnd());
		D_ASSERT(upper == nodes.size() || row_number < nodes[upper]->GetRowStart());
		return false;
	}

	void Verify(SegmentLock &, SegmentTreeVerifyMode mode = SegmentTreeVerifyMode::CONTIGUOUS) const {
#ifdef DEBUG
		idx_t current_rowid_end = nodes.empty() ? 0 : nodes[0]->GetRowStart();
		for (idx_t i = 0; i < nodes.size(); i++) {
			if (mode == SegmentTreeVerifyMode::NON_OVERLAPPING) {
				D_ASSERT(nodes[i]->GetRowStart() >= current_rowid_end);
			} else {
				D_ASSERT(nodes[i]->GetRowStart() == current_rowid_end);
			}
			current_rowid_end = nodes[i]->GetRowStart() + nodes[i]->GetCount();
		}
#endif
	}
	void Verify(SegmentTreeVerifyMode mode = SegmentTreeVerifyMode::CONTIGUOUS) {
#ifdef DEBUG
		auto l = Lock();
		Verify(l, mode);
#endif
	}

	idx_t GetBaseRowId() const {
		return base_row_id;
	}

	SegmentIterationHelper Segments() const {
		return SegmentIterationHelper(*this);
	}

	SegmentIterationHelper Segments(SegmentLock &l) const {
		return SegmentIterationHelper(*this, l);
	}

	SegmentNodeIterationHelper SegmentNodes() const {
		return SegmentNodeIterationHelper(*this);
	}

	SegmentNodeIterationHelper SegmentNodes(SegmentLock &l) const {
		return SegmentNodeIterationHelper(*this, l);
	}

protected:
	mutable atomic<bool> finished_loading;

	//! Load the next segment - only used when lazily loading
	virtual optional<LoadedSegment<T>> LoadSegment() const {
		return nullopt;
	}

	optional_ptr<SegmentNode<T>> GetRootSegmentInternal() const {
		return nodes.empty() ? nullptr : nodes[0].get();
	}

private:
	//! The nodes in the tree, can be binary searched
	mutable vector<unique_ptr<SegmentNode<T>>> nodes;
	//! Lock to access or modify the nodes
	mutable mutex node_lock;
	//! Base row id (row id of the first segment)
	idx_t base_row_id;

private:
	class BaseSegmentIterator {
	public:
		BaseSegmentIterator(const SegmentTree &tree_p, optional_ptr<SegmentNode<T>> current_p,
		                    optional_ptr<SegmentLock> lock)
		    : tree(tree_p), current(current_p), lock(lock) {
		}

		const SegmentTree &tree;
		optional_ptr<SegmentNode<T>> current;
		optional_ptr<SegmentLock> lock;

	public:
		void Next() {
			current = lock ? tree.GetNextSegment(*lock, *current) : tree.GetNextSegment(*current);
		}

		BaseSegmentIterator &operator++() {
			Next();
			return *this;
		}
		bool operator!=(const BaseSegmentIterator &other) const {
			return current != other.current;
		}
	};
	class SegmentIterationHelper {
	public:
		explicit SegmentIterationHelper(const SegmentTree &tree) : tree(tree) {
		}
		SegmentIterationHelper(const SegmentTree &tree, SegmentLock &l) : tree(tree), lock(l) {
		}

	private:
		const SegmentTree &tree;
		optional_ptr<SegmentLock> lock;

	private:
		class SegmentIterator : public BaseSegmentIterator {
		public:
			SegmentIterator(const SegmentTree &tree_p, optional_ptr<SegmentNode<T>> current_p,
			                optional_ptr<SegmentLock> lock)
			    : BaseSegmentIterator(tree_p, current_p, lock) {
			}

			T &operator*() const {
				return BaseSegmentIterator::current->GetNode();
			}
		};

	public:
		SegmentIterator begin() { // NOLINT: match stl API
			auto root = lock ? tree.GetRootSegment(*lock) : tree.GetRootSegment();
			return SegmentIterator(tree, root, lock);
		}
		SegmentIterator end() { // NOLINT: match stl API
			return SegmentIterator(tree, nullptr, lock);
		}
	};
	class SegmentNodeIterationHelper {
	public:
		explicit SegmentNodeIterationHelper(const SegmentTree &tree) : tree(tree) {
		}
		SegmentNodeIterationHelper(const SegmentTree &tree, SegmentLock &l) : tree(tree), lock(l) {
		}

	private:
		const SegmentTree &tree;
		optional_ptr<SegmentLock> lock;

	private:
		class SegmentIterator : public BaseSegmentIterator {
		public:
			SegmentIterator(const SegmentTree &tree_p, optional_ptr<SegmentNode<T>> current_p,
			                optional_ptr<SegmentLock> lock)
			    : BaseSegmentIterator(tree_p, current_p, lock) {
			}

			SegmentNode<T> &operator*() {
				return *BaseSegmentIterator::current;
			}
		};

	public:
		SegmentIterator begin() { // NOLINT: match stl API
			auto root = lock ? tree.GetRootSegment(*lock) : tree.GetRootSegment();
			return SegmentIterator(tree, root, lock);
		}
		SegmentIterator end() { // NOLINT: match stl API
			return SegmentIterator(tree, nullptr, lock);
		}
	};

	//! Load the next segment, if there are any left to load
	bool LoadNextSegment(SegmentLock &l) const {
		if (!SUPPORTS_LAZY_LOADING) {
			return false;
		}
		if (finished_loading) {
			return false;
		}
		auto result = LoadSegment();
		if (result) {
			AppendSegmentInternal(l, std::move(result->segment), result->row_start);
			return true;
		}
		return false;
	}

	//! Load all segments, if there are any left to load
	void LoadAllSegments(SegmentLock &l) const {
		if (!SUPPORTS_LAZY_LOADING) {
			return;
		}
		while (LoadNextSegment(l)) {
		}
	}

	//! Append a column segment to the tree
	void AppendSegmentInternal(SegmentLock &l, shared_ptr<T> segment, idx_t row_start) const {
		D_ASSERT(segment);
		// add the node to the list of nodes
		auto node = make_uniq<SegmentNode<T>>(row_start, std::move(segment), nodes.size());
		if (!nodes.empty()) {
			nodes.back()->SetNext(*node);
		}
		nodes.push_back(std::move(node));
	}
	void AppendSegmentInternal(SegmentLock &l, shared_ptr<T> segment) const {
		idx_t row_start;
		if (nodes.empty()) {
			row_start = base_row_id;
		} else {
			auto &last_node = nodes.back();
			row_start = last_node->GetRowEnd();
		}
		AppendSegmentInternal(l, std::move(segment), row_start);
	}
};

} // namespace duckdb
