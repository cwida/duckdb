//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/execution/merge_sort_tree.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/array.hpp"
#include "duckdb/common/atomic.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/mutex.hpp"
#include "duckdb/common/pair.hpp"
#include "duckdb/common/printer.hpp"
#include "duckdb/common/stl_allocator.hpp"
#include "duckdb/common/typedefs.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/common/vector_operations/aggregate_executor.hpp"
#include "duckdb/main/client_context.hpp"
#include <iomanip>
#include <thread>

namespace duckdb {

// MIT License Text:
//
// Copyright 2022 salesforce.com, inc.
//
// Permission is hereby granted, free of charge, to any person obtaining
// a copy of this software and associated documentation files (the
// "Software"), to deal in the Software without restriction, including
// without limitation the rights to use, copy, modify, merge, publish,
// distribute, sublicense, and/or sell copies of the Software, and to
// permit persons to whom the Software is furnished to do so, subject to
// the following conditions:
//
// The above copyright notice and this permission notice shall be
// included in all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
// EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
// MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
// BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
// ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

// Implementation of a generic merge-sort-tree
// Rewrite of the original, which was in C++17 and targeted for research,
// instead of deployment.
template <typename T>
struct MergeSortTraits {
	using return_type = T;
	static const return_type SENTINEL() {
		return NumericLimits<T>::Maximum();
	};
};

template <typename... T>
struct MergeSortTraits<std::tuple<T...>> {
	using return_type = std::tuple<T...>;
	static return_type SENTINEL() {
		return std::tuple<T...> {MergeSortTraits<T>::SENTINEL()...};
	};
};

template <typename... T>
struct MergeSortTraits<std::pair<T...>> {
	using return_type = std::pair<T...>;
	static return_type SENTINEL() {
		return std::pair<T...> {MergeSortTraits<T>::SENTINEL()...};
	};
};

template <typename E = idx_t, typename O = idx_t, typename CMP = std::less<E>, uint64_t F = 32, uint64_t C = 32>
struct MergeSortTree {
	using ElementType = E;
	using OffsetType = O;
	//! The levels are allocated through the tree's allocator so that their memory can be tracked
	using Elements = vector<ElementType, true, stl_allocator<ElementType>>;
	using Offsets = vector<OffsetType, true, stl_allocator<OffsetType>>;
	using Level = pair<Elements, Offsets>;
	using Tree = vector<Level>;

	using RunElement = pair<ElementType, idx_t>;
	using RunElements = array<RunElement, F>;
	using Games = array<RunElement, F - 1>;

	static constexpr ElementType INVALID = std::numeric_limits<ElementType>::max();

	struct CompareElements {
		explicit CompareElements(const CMP &cmp) : cmp(cmp) {
		}

		bool operator()(const RunElement &lhs, const RunElement &rhs) {
			if (cmp(lhs.first, rhs.first)) {
				return true;
			}
			if (cmp(rhs.first, lhs.first)) {
				return false;
			}
			return lhs.second < rhs.second;
		}

		CMP cmp;
	};

	explicit MergeSortTree(Allocator &allocator = Allocator::DefaultAllocator(), const CMP &cmp = CMP())
	    : allocator(allocator), cmp(cmp) {
	}
	explicit MergeSortTree(const CMP &cmp) : MergeSortTree(Allocator::DefaultAllocator(), cmp) {
	}

	inline Elements &LowestLevel() {
		return tree[0].first;
	}

	inline const Elements &LowestLevel() const {
		return tree[0].first;
	}

	Elements &Allocate(idx_t count);

	//! Build the tree in parallel: every thread calls this and waits for the others at each level.
	//! The wait is interruptible so that a failed run releases the waiting threads through the executor.
	void Build(ClientContext &context);

	//	{nth index, remainder}
	pair<idx_t, idx_t> SelectNth(const SubFrames &frames, idx_t n) const;

	inline ElementType NthElement(idx_t i) const {
		if (tree.empty() || tree.front().first.empty()) {
			return INVALID;
		}
		return tree.front().first[i];
	}

	template <typename L>
	void AggregateLowerBound(const idx_t lower, const idx_t upper, const E needle, L aggregate) const;

	//! The allocator used for the levels of the tree
	Allocator &allocator;
	Tree tree;
	CompareElements cmp;

	static constexpr auto FANOUT = F;
	static constexpr auto CASCADING = C;

protected:
	//! Parallel build machinery
	mutex build_lock;
	atomic<idx_t> build_level;
	atomic<idx_t> build_complete;
	idx_t build_run;
	idx_t build_run_length;
	idx_t build_num_runs;

	bool TryNextRun(idx_t &level_idx, idx_t &run_idx) {
		const auto fanout = F;

		lock_guard<mutex> stage_guard(build_lock);

		// Finished with this level?
		if (build_complete >= build_num_runs) {
			++build_level;
			if (build_level >= tree.size()) {
				return false;
			}

			const auto count = LowestLevel().size();
			build_run_length *= fanout;
			build_num_runs = (count + build_run_length - 1) / build_run_length;
			build_run = 0;
			build_complete = 0;
		}

		// If all runs are in flight,
		// yield until the next level is ready
		if (build_run >= build_num_runs) {
			return false;
		}

		level_idx = build_level;
		run_idx = build_run++;

		return true;
	}

	void BuildRun(idx_t level_idx, idx_t run_idx);

	RunElement StartGames(Games &losers, const RunElements &elements, const RunElement &sentinel) {
		const auto elem_nodes = elements.size();
		const auto game_nodes = losers.size();
		Games winners;

		//	Play the first round of games,
		//	placing the losers at the bottom of the game
		const auto base_offset = game_nodes / 2;
		auto losers_base = losers.data() + base_offset;
		auto winners_base = winners.data() + base_offset;

		const auto base_count = elem_nodes / 2;
		for (idx_t i = 0; i < base_count; ++i) {
			const auto &e0 = elements[i * 2 + 0];
			const auto &e1 = elements[i * 2 + 1];
			if (cmp(e0, e1)) {
				losers_base[i] = e1;
				winners_base[i] = e0;
			} else {
				losers_base[i] = e0;
				winners_base[i] = e1;
			}
		}

		//	Fill in any byes
		if (elem_nodes % 2) {
			winners_base[base_count] = elements.back();
			losers_base[base_count] = sentinel;
		}

		//	Pad to a power of 2
		const auto base_size = (game_nodes + 1) / 2;
		for (idx_t i = (elem_nodes + 1) / 2; i < base_size; ++i) {
			winners_base[i] = sentinel;
			losers_base[i] = sentinel;
		}

		//	Play the winners against each other
		//	and stick the losers in the upper levels of the tournament tree
		for (idx_t i = base_offset; i-- > 0;) {
			//	Indexing backwards
			const auto &e0 = winners[i * 2 + 1];
			const auto &e1 = winners[i * 2 + 2];
			if (cmp(e0, e1)) {
				losers[i] = e1;
				winners[i] = e0;
			} else {
				losers[i] = e0;
				winners[i] = e1;
			}
		}

		//	Return the final winner
		return winners[0];
	}

	RunElement ReplayGames(Games &losers, idx_t slot_idx, const RunElement &insert_val) {
		RunElement smallest = insert_val;
		//	Start at a fake level below
		auto idx = slot_idx + losers.size();
		do {
			// Parent index
			idx = (idx - 1) / 2;
			// swap if out of order
			if (cmp(losers[idx], smallest)) {
				std::swap(losers[idx], smallest);
			}
		} while (idx);

		return smallest;
	}

	static idx_t LowestCascadingLevel() {
		idx_t level = 0;
		idx_t level_width = 1;
		while (level_width <= CASCADING) {
			++level;
			level_width *= FANOUT;
		}
		return level;
	}

public:
	void Print() const {
		std::ostringstream out;
		const char *separator = "    ";
		const char *group_separator = " || ";
		idx_t level_width = 1;
		idx_t number_width = 0;
		for (auto &level : tree) {
			for (auto &e : level.first) {
				if (e) {
					idx_t digits = ceil(log10(fabs(e))) + (e < 0);
					if (digits > number_width) {
						number_width = digits;
					}
				}
			}
		}
		for (auto &level : tree) {
			// Print the elements themself
			{
				out << 'd';
				for (size_t i = 0; i < level.first.size(); ++i) {
					out << ((i && i % level_width == 0) ? group_separator : separator);
					out << std::setw(NumericCast<int32_t>(number_width)) << level.first[i];
				}
				out << '\n';
			}
			// Print the pointers
			if (!level.second.empty()) {
				idx_t run_cnt = (level.first.size() + level_width - 1) / level_width;
				idx_t cascading_idcs_cnt = run_cnt * (2 + level_width / CASCADING) * FANOUT;
				for (idx_t child_nr = 0; child_nr < FANOUT; ++child_nr) {
					out << " ";
					for (idx_t idx = 0; idx < cascading_idcs_cnt; idx += FANOUT) {
						out << ((idx && ((idx / FANOUT) % (level_width / CASCADING + 2) == 0)) ? group_separator
						                                                                       : separator);
						out << std::setw(NumericCast<int32_t>(number_width)) << level.second[idx + child_nr];
					}
					out << '\n';
				}
			}
			level_width *= FANOUT;
		}

		Printer::Print(out.str());
	}
};

template <typename E, typename O, typename CMP, uint64_t F, uint64_t C>
typename MergeSortTree<E, O, CMP, F, C>::Elements &MergeSortTree<E, O, CMP, F, C>::Allocate(idx_t count) {
	const auto fanout = F;
	const auto cascading = C;
	Elements lowest_level(allocator);
	lowest_level.resize(count);
	tree.emplace_back(Level(std::move(lowest_level), Offsets(allocator)));

	for (idx_t child_run_length = 1; child_run_length < count;) {
		const auto run_length = child_run_length * fanout;
		const auto num_runs = (count + run_length - 1) / run_length;

		Elements elements(allocator);
		elements.resize(count);

		//	Allocate cascading pointers only if there is room
		Offsets cascades(allocator);
		if (cascading > 0 && run_length > cascading) {
			const auto num_cascades = fanout * num_runs * (run_length / cascading + 2);
			cascades.resize(num_cascades);
		}

		//	Insert completed level and move up to the next one
		tree.emplace_back(std::move(elements), std::move(cascades));
		child_run_length = run_length;
	}

	//	Set up for parallel build
	build_level = 1;
	build_complete = 0;
	build_run = 0;
	build_run_length = fanout;
	build_num_runs = (count + build_run_length - 1) / build_run_length;

	return LowestLevel();
}

template <typename E, typename O, typename CMP, uint64_t F, uint64_t C>
void MergeSortTree<E, O, CMP, F, C>::Build(ClientContext &context) {
	//	Fan in parent levels until we are at the top
	//	Note that we don't build the top layer as that would just be all the data.
	while (build_level.load() < tree.size()) {
		//	A failed run never completes its level, so the waiting threads
		//	leave through the interrupt that the executor raises for the failure
		context.InterruptCheck();
		idx_t level_idx;
		idx_t run_idx;
		if (TryNextRun(level_idx, run_idx)) {
			BuildRun(level_idx, run_idx);
		} else {
			std::this_thread::yield();
		}
	}
}

template <typename E, typename O, typename CMP, uint64_t F, uint64_t C>
void MergeSortTree<E, O, CMP, F, C>::BuildRun(idx_t level_idx, idx_t run_idx) {
	//	Create each parent run by merging the child runs using a tournament tree
	// 	https://en.wikipedia.org/wiki/K-way_merge_algorithm
	const auto fanout = F;
	const auto cascading = C;

	auto &elements = tree[level_idx].first;
	auto &cascades = tree[level_idx].second;
	const auto &child_level = tree[level_idx - 1];
	const auto count = elements.size();

	idx_t child_run_length = 1;
	auto run_length = child_run_length * fanout;
	for (idx_t l = 1; l < level_idx; ++l) {
		child_run_length = run_length;
		run_length *= fanout;
	}

	const RunElement SENTINEL(MergeSortTraits<ElementType>::SENTINEL(), MergeSortTraits<idx_t>::SENTINEL());

	//	Position markers for scanning the children.
	using Bounds = pair<OffsetType, OffsetType>;
	array<Bounds, fanout> bounds;
	//	Start with first element of each (sorted) child run
	RunElements players;
	const auto child_base = run_idx * run_length;
	for (idx_t child_run = 0; child_run < fanout; ++child_run) {
		const auto child_idx = child_base + child_run * child_run_length;
		bounds[child_run] = {OffsetType(MinValue<idx_t>(child_idx, count)),
		                     OffsetType(MinValue<idx_t>(child_idx + child_run_length, count))};
		if (bounds[child_run].first != bounds[child_run].second) {
			players[child_run] = {child_level.first[child_idx], child_run};
		} else {
			//	Empty child
			players[child_run] = SENTINEL;
		}
	}

	//	Play the first round and extract the winner
	Games games;
	auto element_idx = child_base;
	auto cascade_idx = fanout * run_idx * (run_length / cascading + 2);
	auto winner = StartGames(games, players, SENTINEL);
	while (winner != SENTINEL) {
		// Add fractional cascading pointers
		// if we are on a fraction boundary
		if (!cascades.empty() && element_idx % cascading == 0) {
			for (idx_t i = 0; i < fanout; ++i) {
				cascades[cascade_idx++] = bounds[i].first;
			}
		}

		//	Insert new winner element into the current run
		elements[element_idx++] = winner.first;
		const auto child_run = winner.second;
		auto &child_idx = bounds[child_run].first;
		++child_idx;

		//	Move to the next entry in the child run (if any)
		if (child_idx < bounds[child_run].second) {
			winner = ReplayGames(games, child_run, {child_level.first[child_idx], child_run});
		} else {
			winner = ReplayGames(games, child_run, SENTINEL);
		}
	}

	// Add terminal cascade pointers to the end
	if (!cascades.empty()) {
		for (idx_t j = 0; j < 2; ++j) {
			for (idx_t i = 0; i < fanout; ++i) {
				cascades[cascade_idx++] = bounds[i].first;
			}
		}
	}

	++build_complete;
}

template <typename E, typename O, typename CMP, uint64_t F, uint64_t C>
pair<idx_t, idx_t> MergeSortTree<E, O, CMP, F, C>::SelectNth(const SubFrames &frames, idx_t n) const {
	// Find Nth element in a top-down traversal
	idx_t result = 0;

	// Handle special case of a 0/1-element tree
	if (tree.size() < 2) {
		++n;

		//	Zero element tree always fails
		if (tree[0].first.empty()) {
			return {result, n};
		}

		//	Either the element is in the frame or it isn't
		const auto *level_data = tree[0].first.data();
		const auto v = level_data[result];
		for (const auto &frame : frames) {
			n -= (v >= frame.start) && (v < frame.end);
		}

		return {result, n};
	}

	// 	The first level contains a single run,
	//	so the only thing we need is any cascading pointers
	auto level_no = tree.size() - 2;
	idx_t level_width = 1;
	for (idx_t i = 0; i < level_no; ++i) {
		level_width *= FANOUT;
	}

	// First, handle levels with cascading pointers
	const auto min_cascaded = LowestCascadingLevel();
	if (level_no > min_cascaded) {
		//	Initialise the cascade indices from the previous level
		using CascadeRange = pair<idx_t, idx_t>;
		std::array<CascadeRange, 3> cascades;
		const auto &level = tree[level_no + 1].first;
		for (idx_t f = 0; f < frames.size(); ++f) {
			const auto &frame = frames[f];
			auto &cascade_idx = cascades[f];
			const auto lower_idx =
			    UnsafeNumericCast<idx_t>(std::lower_bound(level.begin(), level.end(), frame.start) - level.begin());
			cascade_idx.first = lower_idx / CASCADING * FANOUT;
			const auto upper_idx =
			    UnsafeNumericCast<idx_t>(std::lower_bound(level.begin(), level.end(), frame.end) - level.begin());
			cascade_idx.second = upper_idx / CASCADING * FANOUT;
		}

		// 	Walk the cascaded levels
		for (; level_no >= min_cascaded; --level_no) {
			//	The cascade indices into this level are in the previous level
			const auto &level_cascades = tree[level_no + 1].second;

			// Go over all children until we found enough in range
			const auto *level_data = tree[level_no].first.data();
			const auto level_size = tree[level_no].first.size();
			while (true) {
				idx_t matched = 0;
				std::array<CascadeRange, 3> matches;
				for (idx_t f = 0; f < frames.size(); ++f) {
					const auto &frame = frames[f];
					auto &cascade_idx = cascades[f];
					auto &match = matches[f];

					const auto lower_begin = level_data + level_cascades[cascade_idx.first];
					const auto lower_end = level_data + level_cascades[cascade_idx.first + FANOUT];
					match.first =
					    UnsafeNumericCast<idx_t>(std::lower_bound(lower_begin, lower_end, frame.start) - level_data);

					const auto upper_begin = level_data + level_cascades[cascade_idx.second];
					const auto upper_end = level_data + level_cascades[cascade_idx.second + FANOUT];
					match.second =
					    UnsafeNumericCast<idx_t>(std::lower_bound(upper_begin, upper_end, frame.end) - level_data);

					matched += idx_t(match.second - match.first);
				}
				// 	Move down if there are enough in this child, or if this is the last child of the level
				if (matched > n || (result + 1) * level_width >= level_size) {
					// 	Too much in this level, so move down to leftmost child candidate within the cascade range
					for (idx_t f = 0; f < frames.size(); ++f) {
						auto &cascade_idx = cascades[f];
						auto &match = matches[f];
						cascade_idx.first = (match.first / CASCADING + 2 * result) * FANOUT;
						cascade_idx.second = (match.second / CASCADING + 2 * result) * FANOUT;
					}
					break;
				}

				//	Not enough in this child, so move right
				for (idx_t f = 0; f < frames.size(); ++f) {
					auto &cascade_idx = cascades[f];
					++cascade_idx.first;
					++cascade_idx.second;
				}
				++result;
				n -= matched;
			}
			result *= FANOUT;
			level_width /= FANOUT;
		}
	}

	//	Continue with the uncascaded levels (except the first)
	for (; level_no > 0; --level_no) {
		const auto &level = tree[level_no].first;
		auto range_begin = level.begin() + UnsafeNumericCast<int64_t>(result * level_width);
		auto range_end = range_begin + UnsafeNumericCast<int64_t>(level_width);
		while (range_end < level.end()) {
			idx_t matched = 0;
			for (idx_t f = 0; f < frames.size(); ++f) {
				const auto &frame = frames[f];
				const auto lower_match = std::lower_bound(range_begin, range_end, frame.start);
				const auto upper_match = std::lower_bound(lower_match, range_end, frame.end);
				matched += idx_t(upper_match - lower_match);
			}
			if (matched > n) {
				// 	Too much in this level, so move down to leftmost child candidate
				//	Since we have no cascade pointers left, this is just the start of the next level.
				break;
			}
			//	Not enough in this child, so move right
			range_begin = range_end;
			range_end += UnsafeNumericCast<int64_t>(level_width);
			++result;
			n -= matched;
		}
		result *= FANOUT;
		level_width /= FANOUT;
	}

	// The last level
	const auto *level_data = tree[level_no].first.data();
	++n;

	const auto count = tree[level_no].first.size();
	for (const auto limit = MinValue<idx_t>(result + FANOUT, count); result < limit; ++result) {
		const auto v = level_data[result];
		for (const auto &frame : frames) {
			n -= (v >= frame.start) && (v < frame.end);
		}
		if (!n) {
			break;
		}
	}

	return {result, n};
}

template <typename E, typename O, typename CMP, uint64_t F, uint64_t C>
template <typename L>
void MergeSortTree<E, O, CMP, F, C>::AggregateLowerBound(const idx_t lower, const idx_t upper, const E needle,
                                                         L aggregate) const {
	if (lower >= upper) {
		return;
	}

	D_ASSERT(upper <= tree[0].first.size());

	using IdxRange = std::pair<idx_t, idx_t>;

	// Find the entry point into the tree
	IdxRange run_idx(lower, upper - 1);
	idx_t level_width = 1;
	idx_t level = 0;
	IdxRange prev_run_idx;
	IdxRange curr;
	if (run_idx.first == run_idx.second) {
		curr.first = curr.second = run_idx.first;
	} else {
		do {
			prev_run_idx.second = run_idx.second;
			run_idx.first /= FANOUT;
			run_idx.second /= FANOUT;
			level_width *= FANOUT;
			++level;
		} while (run_idx.first != run_idx.second);
		curr.second = prev_run_idx.second * level_width / FANOUT;
		curr.first = curr.second;
	}

	// Aggregate layers using the cascading indices
	if (level > LowestCascadingLevel()) {
		IdxRange cascading_idx;
		// Find the initial cascading idcs
		{
			IdxRange entry;
			entry.first = run_idx.first * level_width;
			entry.second = std::min(entry.first + level_width, static_cast<idx_t>(tree[0].first.size()));
			auto *level_data = tree[level].first.data();
			auto entry_idx = NumericCast<idx_t>(
			    std::lower_bound(level_data + entry.first, level_data + entry.second, needle) - level_data);
			cascading_idx.first = cascading_idx.second =
			    (entry_idx / CASCADING + 2 * (entry.first / level_width)) * FANOUT;

			// We have to slightly shift the initial CASCADING idcs because at the top level
			// we won't be exactly on a boundary
			auto correction = (prev_run_idx.second - run_idx.second * FANOUT);
			cascading_idx.first -= (FANOUT - correction);
			cascading_idx.second += correction;
		}

		// Aggregate all layers until we reach a layer without cascading indices
		// For the first layer, we already checked we have cascading indices available, otherwise
		// we wouldn't have even searched the entry points. Hence, we use a `do-while` instead of `while`
		do {
			--level;
			level_width /= FANOUT;
			auto *level_data = tree[level].first.data();
			auto &cascading_idcs = tree[level + 1].second;
			// Left side of tree
			// Handle all completely contained runs
			cascading_idx.first += FANOUT - 1;
			while (curr.first - lower >= level_width) {
				// Search based on cascading info from previous level
				const auto *search_begin = level_data + cascading_idcs[cascading_idx.first];
				const auto *search_end = level_data + cascading_idcs[cascading_idx.first + FANOUT];
				const auto run_pos = std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data;
				// Compute runBegin and pass it to our callback
				const auto run_begin = curr.first - level_width;
				aggregate(level, run_begin, NumericCast<idx_t>(run_pos));
				// Update state for next round
				curr.first -= level_width;
				--cascading_idx.first;
			}
			// Handle the partial last run to find the cascading entry point for the next level
			if (curr.first != lower) {
				const auto *search_begin = level_data + cascading_idcs[cascading_idx.first];
				const auto *search_end = level_data + cascading_idcs[cascading_idx.first + FANOUT];
				auto idx = NumericCast<idx_t>(std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data);
				cascading_idx.first = (idx / CASCADING + 2 * (lower / level_width)) * FANOUT;
			}

			// Right side of tree
			// Handle all completely contained runs
			while (upper - curr.second >= level_width) {
				// Search based on cascading info from previous level
				const auto *search_begin = level_data + cascading_idcs[cascading_idx.second];
				const auto *search_end = level_data + cascading_idcs[cascading_idx.second + FANOUT];
				const auto run_pos = std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data;
				// Compute runBegin and pass it to our callback
				const auto run_begin = curr.second;
				aggregate(level, run_begin, NumericCast<idx_t>(run_pos));
				// Update state for next round
				curr.second += level_width;
				++cascading_idx.second;
			}
			// Handle the partial last run to find the cascading entry point for the next level
			if (curr.second != upper) {
				const auto *search_begin = level_data + cascading_idcs[cascading_idx.second];
				const auto *search_end = level_data + cascading_idcs[cascading_idx.second + FANOUT];
				auto idx = NumericCast<idx_t>(std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data);
				cascading_idx.second = (idx / CASCADING + 2 * (upper / level_width)) * FANOUT;
			}
		} while (level >= LowestCascadingLevel());
	}

	// Handle lower levels which won't have cascading info
	if (level) {
		while (--level) {
			level_width /= FANOUT;
			auto *level_data = tree[level].first.data();
			// Left side
			while (curr.first - lower >= level_width) {
				const auto *search_end = level_data + curr.first;
				const auto *search_begin = search_end - level_width;
				const auto run_pos =
				    NumericCast<idx_t>(std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data);
				const auto run_begin = NumericCast<idx_t>(search_begin - level_data);
				aggregate(level, run_begin, run_pos);
				curr.first -= level_width;
			}
			// Right side
			while (upper - curr.second >= level_width) {
				const auto *search_begin = level_data + curr.second;
				const auto *search_end = search_begin + level_width;
				const auto run_pos =
				    NumericCast<idx_t>(std::lower_bound(search_begin, search_end, needle, cmp.cmp) - level_data);
				const auto run_begin = NumericCast<idx_t>(search_begin - level_data);
				aggregate(level, run_begin, run_pos);
				curr.second += level_width;
			}
		}
	}

	// The last layer
	{
		auto *level_data = tree[0].first.data();
		// Left side
		auto lower_it = lower;
		while (lower_it != curr.first) {
			const auto *search_begin = level_data + lower_it;
			const auto run_begin = lower_it;
			const auto run_pos = run_begin + cmp.cmp(*search_begin, needle);
			aggregate(level, run_begin, run_pos);
			++lower_it;
		}
		// Right side
		while (curr.second != upper) {
			const auto *search_begin = level_data + curr.second;
			const auto run_begin = curr.second;
			const auto run_pos = run_begin + cmp.cmp(*search_begin, needle);
			aggregate(level, run_begin, run_pos);
			++curr.second;
		}
	}
}

} // namespace duckdb
