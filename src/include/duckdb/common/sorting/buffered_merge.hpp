//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/sorting/buffered_merge.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/algorithm.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/allocator.hpp"
#include "duckdb/common/exception.hpp"

namespace duckdb {

//! Scratch buffer for merging sorted runs: the merge is correct without it, so the allocation must never fail a sort.
//! Starting at half the keys, the size is halved until the buffer manager can provide it (or it becomes too small to
//! be worth having).
class SortMergeBuffer {
public:
	//! Number of keys the buffer holds at most: the smaller side of a merge is at most half the keys
	static idx_t MaximumKeyCount(idx_t key_count) {
		return key_count / 2 + 1;
	}
	//! Memory the buffer takes at most, so that it can be budgeted along with the keys it merges
	static idx_t MaximumSize(idx_t key_count, idx_t key_size) {
		return MaximumKeyCount(key_count) * key_size;
	}

	SortMergeBuffer(Allocator &allocator, idx_t key_count, idx_t key_size) : count(0) {
		static constexpr idx_t MINIMUM_KEYS = 1024;
		for (idx_t keys = MaximumKeyCount(key_count); keys >= MINIMUM_KEYS; keys /= 2) {
			try {
				data = allocator.Allocate(keys * key_size);
				count = keys;
				return;
			} catch (OutOfMemoryException &) {
				// try a smaller buffer
			}
		}
	}

	template <class SORT_KEY>
	SORT_KEY *Keys() {
		return reinterpret_cast<SORT_KEY *>(data.get());
	}
	idx_t Count() const {
		return count;
	}

private:
	AllocatedData data;
	idx_t count;
};

//! Rotates [first, last) so that [middle, last) comes before [first, middle) and returns the new middle. The smaller
//! of the two pieces goes through the scratch buffer when it fits, otherwise this is a plain std::rotate; neither
//! allocates.
template <class SORT_KEY, class ITERATOR>
ITERATOR BufferedRotate(const ITERATOR &first, const ITERATOR &middle, const ITERATOR &last, SORT_KEY *buffer,
                        const idx_t buffer_count) {
	const auto left_count = static_cast<idx_t>(middle - first);
	const auto right_count = static_cast<idx_t>(last - middle);
	if (left_count > right_count && right_count <= buffer_count) {
		std::move(middle, last, buffer);
		std::move_backward(first, middle, last);
		std::move(buffer, buffer + right_count, first);
		return first + right_count;
	}
	if (left_count <= buffer_count) {
		std::move(first, middle, buffer);
		std::move(middle, last, first);
		std::move_backward(buffer, buffer + left_count, last);
		return last - left_count;
	}
	return std::rotate(first, middle, last);
}

//! Merges the sorted ranges [first, middle) and [middle, last) in place using the given scratch buffer, which may be
//! any size (including empty): nothing is allocated. When the smaller side fits in the buffer it is moved out and
//! merged back, forward when it is the left side and backward when it is the right side, which keeps equal keys in
//! order. Otherwise the larger side is split in half, the other side is split at the matching key by binary search,
//! the two middle pieces are rotated and both halves are merged recursively, until the pieces fit the buffer. Without a
//! buffer this is the O(n log n) in-place merge that std::inplace_merge uses when it cannot get memory, with a buffer
//! of half the keys it is a single linear pass, and anything in between makes progressively fewer passes.
template <class SORT_KEY, class ITERATOR, class COMPARE>
void BufferedMerge(ITERATOR first, ITERATOR middle, ITERATOR last, const COMPARE &compare, SORT_KEY *buffer,
                   const idx_t buffer_count) {
	while (true) {
		const auto left_count = static_cast<idx_t>(middle - first);
		const auto right_count = static_cast<idx_t>(last - middle);
		if (left_count == 0 || right_count == 0) {
			return;
		}
		// the ranges are already in order, nothing to merge
		if (!compare(*middle, *(middle - 1))) {
			return;
		}
		// one key on each side: swap them (the split below needs at least two keys on the larger side to progress)
		if (left_count + right_count == 2) {
			std::iter_swap(first, middle);
			return;
		}
		if (left_count <= right_count && left_count <= buffer_count) {
			// move the left side out and merge forward into [first, last)
			std::move(first, middle, buffer);
			auto buffer_end = buffer + left_count;
			auto buffer_it = buffer;
			auto right_it = middle;
			auto out = first;
			while (buffer_it != buffer_end && right_it != last) {
				if (compare(*right_it, *buffer_it)) {
					*out = std::move(*right_it);
					++right_it;
				} else {
					*out = std::move(*buffer_it);
					++buffer_it;
				}
				++out;
			}
			std::move(buffer_it, buffer_end, out);
			return;
		}
		if (right_count <= buffer_count) {
			// move the right side out and merge backward into [first, last)
			std::move(middle, last, buffer);
			auto buffer_begin = buffer;
			auto buffer_it = buffer + right_count;
			auto left_it = middle;
			auto out = last;
			while (buffer_it != buffer_begin && left_it != first) {
				--out;
				if (compare(*(buffer_it - 1), *(left_it - 1))) {
					--left_it;
					*out = std::move(*left_it);
				} else {
					--buffer_it;
					*out = std::move(*buffer_it);
				}
			}
			// anything left in the buffer goes to the front of the remaining gap
			std::move_backward(buffer_begin, buffer_it, out);
			return;
		}
		// neither side fits: split the larger side in half and the other side at the matching key, so that
		// everything in [first_cut, middle) is greater than or equal to everything in [middle, second_cut) ...
		// (copies of existing iterators: a default-constructed block iterator has no state to iterate over)
		ITERATOR first_cut = first;
		ITERATOR second_cut = middle;
		if (left_count > right_count) {
			first_cut = first + left_count / 2;
			second_cut = std::lower_bound(middle, last, *first_cut, compare);
		} else {
			second_cut = middle + right_count / 2;
			first_cut = std::upper_bound(first, middle, *second_cut, compare);
		}
		// ... swap those two pieces, which leaves two independent merges on either side of the new middle
		auto new_middle = BufferedRotate<SORT_KEY>(first_cut, middle, second_cut, buffer, buffer_count);
		// recurse into the smaller one and loop on the larger one, which bounds the recursion depth by log2(n)
		if (new_middle - first < last - new_middle) {
			BufferedMerge<SORT_KEY>(first, first_cut, new_middle, compare, buffer, buffer_count);
			first = new_middle;
			middle = second_cut;
		} else {
			BufferedMerge<SORT_KEY>(new_middle, second_cut, last, compare, buffer, buffer_count);
			last = new_middle;
			middle = first_cut;
		}
	}
}

//! Merge policy for vergesort (and the sorts it calls into) that merges through a SortMergeBuffer, so that every run
//! merge of a sort, including the nested ones, uses the one buffer that was budgeted for it and nothing else
template <class SORT_KEY>
class BufferedMergePolicy {
public:
	explicit BufferedMergePolicy(SortMergeBuffer &buffer) : keys(buffer.Keys<SORT_KEY>()), count(buffer.Count()) {
	}

	template <class ITERATOR, class COMPARE>
	void operator()(const ITERATOR &first, const ITERATOR &middle, const ITERATOR &last, const COMPARE &compare) const {
		BufferedMerge<SORT_KEY>(first, middle, last, compare, keys, count);
	}

private:
	SORT_KEY *keys;
	idx_t count;
};

} // namespace duckdb
