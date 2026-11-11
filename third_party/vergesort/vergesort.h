/*
 * vergesort.h - General-purpose hybrid sort
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015-2020 Morwenn <morwenn29@hotmail.fr>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#ifndef VERGESORT_VERGESORT_H_
#define VERGESORT_VERGESORT_H_

// Used by several other internal headers, hence defined first
#if __cplusplus >= 201103L
    #include <utility>
    #define VERGESORT_PREFER_MOVE(x) std::move(x)
#else
    #define VERGESORT_PREFER_MOVE(x) (x)
#endif

#include <algorithm>
#include <cstddef>
#include <functional>
#include <iterator>
#include <list>
#include "detail/is_sorted_until.h"
#include "detail/log2.h"
// #include "detail/pdqsort.h"
#include "detail/prevnext.h"
#include "detail/quicksort.h"

namespace duckdb_vergesort
{
    // Merge policy used for the final pairwise run merges: a callable merge(first, middle, last, compare) that
    // merges the sorted ranges [first, middle) and [middle, last). The default is std::inplace_merge, which takes
    // its temporary buffer from operator new; callers that want to control that memory pass their own policy.
    struct inplace_merge_policy
    {
        template<typename RandomAccessIterator, typename Compare>
        void operator()(RandomAccessIterator first, RandomAccessIterator middle,
                        RandomAccessIterator last, Compare compare) const
        {
            std::inplace_merge(first, middle, last, compare);
        }
    };

    namespace detail
    {
        // In-place merge where [first, middle1), [middle1, middle2)
        // and [middle2, last) are sorted. The two in-place merges are
        // done in the order that should result in the smallest number
        // of comparisons
        template<typename BidirectionalIterator, typename Compare>
        void inplace_merge3(BidirectionalIterator first, BidirectionalIterator middle1,
                            BidirectionalIterator middle2, BidirectionalIterator last,
                            Compare compare)
        {
            if (std::distance(first, middle1) < std::distance(middle2, last)) {
                std::inplace_merge(first, middle1, middle2, compare);
                std::inplace_merge(first, middle2, last, compare);
            } else {
                std::inplace_merge(middle1, middle2, last, compare);
                std::inplace_merge(first, middle1, last, compare);
            }
        }

        // vergesort for bidirectional iterators
        template<typename BidirectionalIterator, typename Compare>
        void vergesort(BidirectionalIterator first, BidirectionalIterator last,
                       Compare compare, std::bidirectional_iterator_tag)
        {
            typedef typename std::iterator_traits<BidirectionalIterator>::difference_type difference_type;

            difference_type dist = std::distance(first, last);
            if (dist < 80) {
                // vergesort is inefficient for small collections
                quicksort(first, last, dist, compare);
                return;
            }

            // Limit under which quicksort is used
            int unstable_limit = dist / log2(dist);

            // Beginning of an unstable partition, last if the
            // previous partition is stable
            BidirectionalIterator begin_unstable = last;

            // Size of the unstable partition
            difference_type size_unstable = 0;

            // Pair of iterators to iterate through the collection
            BidirectionalIterator next = detail::is_sorted_until(first, last, compare);
            if (next == last) return;
            BidirectionalIterator current = detail::prev(next);

            while (true) {
                // Decreasing range
                {
                    BidirectionalIterator begin_rng = current;

                    difference_type run_size = 1;
                    while (next != last) {
                        if (compare(*current, *next)) break;
                        ++current;
                        ++next;
                        ++run_size;
                    }

                    // Reverse and merge
                    if (run_size > unstable_limit) {
                        if (begin_unstable != last) {
                            quicksort(begin_unstable, begin_rng, size_unstable, compare);
                            std::reverse(begin_rng, next);
                            std::inplace_merge(begin_unstable, begin_rng, next, compare);
                            std::inplace_merge(first, begin_unstable, next, compare);
                            begin_unstable = last;
                            size_unstable = 0;
                        } else {
                            std::reverse(begin_rng, next);
                            std::inplace_merge(first, begin_rng, next, compare);
                        }
                    } else {
                        size_unstable += run_size;
                        if (begin_unstable == last) {
                            begin_unstable = begin_rng;
                        }
                    }

                    if (next == last) break;

                    ++current;
                    ++next;
                }

                // Increasing range
                {
                    BidirectionalIterator begin_rng = current;

                    difference_type run_size = 1;
                    while (next != last) {
                        if (compare(*next, *current)) break;
                        ++current;
                        ++next;
                        ++run_size;
                    }

                    // Merge
                    if (run_size > unstable_limit) {
                        if (begin_unstable != last) {
                            quicksort(begin_unstable, begin_rng, size_unstable, compare);
                            std::inplace_merge(begin_unstable, begin_rng, next, compare);
                            std::inplace_merge(first, begin_unstable, next, compare);
                            begin_unstable = last;
                            size_unstable = 0;
                        } else {
                            std::inplace_merge(first, begin_rng, next, compare);
                        }
                    } else {
                        size_unstable += run_size;
                        if (begin_unstable == last) {
                            begin_unstable = begin_rng;
                        }
                    }

                    if (next == last) break;

                    ++current;
                    ++next;
                }
            }

            if (begin_unstable != last) {
                quicksort(begin_unstable, last, size_unstable, compare);
                std::inplace_merge(first, begin_unstable, last, compare);
            }
        }

        template<typename RandomAccessIterator, typename Compare, typename Fallback, typename Merge>
        void vergesort(RandomAccessIterator first, RandomAccessIterator last,
                       Compare compare, std::random_access_iterator_tag, Fallback fallback, Merge merge)
        {
            typedef typename std::iterator_traits<RandomAccessIterator>::difference_type difference_type;
            difference_type dist = last - first;

            if (dist < 128) {
                // Vergesort is inefficient for small collections
                fallback(first, last);
                return;
            }

            // Limit under which pdqsort is used to sort a sub-sequence
            const difference_type unstable_limit = dist / detail::log2(dist);

            // Vergesort detects big runs in ascending or descending order,
            // and remember where each run ends by storing the end iterator
            // of each run in this list, then it merges everything in the end
            std::list<RandomAccessIterator> runs;

            // Beginning of an unstable partition, or last if the previous
            // partition is stable
            RandomAccessIterator begin_unstable = last;

            // Pair of iterators to iterate through the collection
            RandomAccessIterator current = first;
            RandomAccessIterator next = detail::next(first);

            while (true) {
                // Beginning of the current sequence
                RandomAccessIterator begin_range = current;

                // If the last part of the collection to sort isn't
                // big enough, consider that it is an unstable sequence
                if ((last - next) <= unstable_limit) {
                    if (begin_unstable == last) {
                        begin_unstable = begin_range;
                    }
                    break;
                }

                // Set backward iterators
                current += unstable_limit;
                next += unstable_limit;

                // Set forward iterators
                RandomAccessIterator current2 = current;
                RandomAccessIterator next2 = next;

                if (compare(*next, *current)) {
                    // Found a decreasing sequence, move iterators
                    // to the limits of the sequence
                    do {
                        --current;
                        --next;
                        if (compare(*current, *next)) break;
                    } while (current != begin_range);
                    if (compare(*current, *next)) ++current;

                    ++current2;
                    ++next2;
                    while (next2 != last) {
                        if (compare(*current2, *next2)) break;
                        ++current2;
                        ++next2;
                    }

                    // Check whether we found a big enough sorted sequence
                    if ((next2 - current) >= unstable_limit) {
                        std::reverse(current, next2);
                        if ((current - begin_range) && begin_unstable == last) {
                            begin_unstable = begin_range;
                        }
                        if (begin_unstable != last) {
                            fallback(begin_unstable, current);
                            runs.push_back(current);
                            begin_unstable = last;
                        }
                        runs.push_back(next2);
                    } else {
                        // Remember the beginning of the unsorted sequence
                        if (begin_unstable == last) {
                            begin_unstable = begin_range;
                        }
                    }
                } else {
                    // Found an increasing sequence, move iterators
                    // to the limits of the sequence
                    do {
                        --current;
                        --next;
                        if (compare(*next, *current)) break;
                    } while (current != begin_range);
                    if (compare(*next, *current)) ++current;

                    ++current2;
                    ++next2;
                    while (next2 != last) {
                        if (compare(*next2, *current2)) break;
                        ++current2;
                        ++next2;
                    }

                    // Check whether we found a big enough sorted sequence
                    if ((next2 - current) >= unstable_limit) {
                        if ((current - begin_range) && begin_unstable == last) {
                            begin_unstable = begin_range;
                        }
                        if (begin_unstable != last) {
                            fallback(begin_unstable, current);
                            runs.push_back(current);
                            begin_unstable = last;
                        }
                        runs.push_back(next2);
                    } else {
                        // Remember the beginning of the unsorted sequence
                        if (begin_unstable == last) {
                            begin_unstable = begin_range;
                        }
                    }
                }

                if (next2 == last) break;

                current = detail::next(current2);
                next = detail::next(next2);
            }

            if (begin_unstable != last) {
                // If there are unsorted elements left, sort them
                runs.push_back(last);
                fallback(begin_unstable, last);
            }

            if (runs.size() < 2) return;

            // Merge runs pairwise until there aren't runs left
            do {
                RandomAccessIterator begin = first;
                for (typename std::list<RandomAccessIterator>::iterator it = runs.begin() ;
                     it != runs.end() && it != detail::prev(runs.end()) ;
                     ++it) {
                    merge(begin, *it, *detail::next(it), compare);

                    // Remove the middle iterator and advance
                    it = runs.erase(it);
                    begin = *it;
                }
            } while (runs.size() > 1);
        }
    }

    template<typename BidirectionalIterator, typename Compare, typename Fallback, typename Merge>
    void vergesort(BidirectionalIterator first, BidirectionalIterator last,
                   Compare compare, Fallback fallback, Merge merge)
    {
        typedef typename std::iterator_traits<BidirectionalIterator>::iterator_category category;
        detail::vergesort(first, last, compare, category(), fallback, merge);
    }

    template<typename BidirectionalIterator, typename Compare, typename Fallback>
    void vergesort(BidirectionalIterator first, BidirectionalIterator last,
                   Compare compare, Fallback fallback)
    {
        vergesort(first, last, compare, fallback, inplace_merge_policy());
    }

    template<typename BidirectionalIterator, typename Fallback>
    void vergesort(BidirectionalIterator first, BidirectionalIterator last, Fallback fallback)
    {
        typedef typename std::iterator_traits<BidirectionalIterator>::value_type value_type;
        vergesort(first, last, std::less<value_type>(), fallback);
    }
}

#undef VERGESORT_PREFER_MOVE

#endif // VERGESORT_VERGESORT_H_
