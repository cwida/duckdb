/*
 * Licensed to Derrick R. Burns under one or more
 * contributor license agreements.  See the NOTICES file distributed with
 * this work for additional information regarding copyright ownership.
 * The ASF licenses this file to You under the Apache License, Version 2.0
 * (the "License"); you may not use this file except in compliance with
 * the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <queue>
#include <utility>
#include <vector>

#include "duckdb/common/arena_containers/arena_vector.hpp"

#ifdef min
#undef min
#endif

#ifdef max
#undef max
#endif


namespace duckdb_tdigest {

using Value = double;
using Weight = double;
using Index = size_t;

const size_t kHighWater = 40000;
const double pi = 3.14159265358979323846;
class Centroid {
public:
	Centroid() : Centroid(0.0, 0.0) {
	}

	Centroid(Value mean, Weight weight) : mean_(mean), weight_(weight) {
	}

	inline Value mean() const noexcept {
		return mean_;
	}

	inline Weight weight() const noexcept {
		return weight_;
	}

	inline void add(const Centroid &c) {
		//    CHECK_GT(c.weight_, 0);
		if (weight_ != 0.0) {
			weight_ += c.weight_;
			mean_ += c.weight_ * (c.mean_ - mean_) / weight_;
		} else {
			weight_ = c.weight_;
			mean_ = c.mean_;
		}
	}

private:
	Value mean_ = 0;
	Weight weight_ = 0;
};

struct CentroidList {
	explicit CentroidList(const duckdb::arena_vector<Centroid> &s) : iter(s.cbegin()), end(s.cend()) {
	}
	duckdb::arena_vector<Centroid>::const_iterator iter;
	duckdb::arena_vector<Centroid>::const_iterator end;

	bool advance() {
		return ++iter != end;
	}
};

class CentroidListComparator {
public:
	CentroidListComparator() {
	}

	bool operator()(const CentroidList &left, const CentroidList &right) const {
		return left.iter->mean() > right.iter->mean();
	}
};

using CentroidListQueue = std::priority_queue<CentroidList, std::vector<CentroidList>, CentroidListComparator>;

struct CentroidComparator {
	bool operator()(const Centroid &a, const Centroid &b) const {
		return a.mean() < b.mean();
	}
};

class TDigest {
	static constexpr Index INITIAL_CAPACITY = 8;
	static Index MinimumInt(Index a, Index b) {
		return a < b ? a : b;
	}
	// Make room for `needed` elements, doubling the capacity rather than fitting exactly, up to the `full`
	// size the buffer would have been given up front: a long run of small merges then reallocates a
	// logarithmic number of times instead of once per merge, and never grows past the old fixed reservation.
	template <class VECTOR>
	static void Grow(VECTOR &vec, size_t needed, size_t full) {
		if (needed <= vec.capacity()) {
			return;
		}
		size_t target = vec.capacity() * 2;
		if (target > full) {
			target = full;
		}
		if (target < needed) {
			target = needed;
		}
		vec.reserve(target);
	}

	// Keep every buffer large enough for the next process() -- which appends processed_ to unprocessed_,
	// rebuilds processed_ from the merged run and rebuilds cumulative_ -- so that process() itself never
	// allocates.  process() also runs at finalize time (compress(), quantile()), on a thread that may be
	// finalizing several partitions holding states from the same arena concurrently: the arena a digest's
	// buffers live in is the local hash table's, which the sink hands to the global state and which is not
	// thread-safe.  Growing here, in add()/merge(), keeps all allocation on the thread that owns the arena.
	// A merged digest holds at most as many centroids as it was given, and at most maxProcessed_.
	void reserveForProcess() {
		const size_t total = processed_.size() + unprocessed_.size();
		Grow(unprocessed_, total, size_t(maxUnprocessed_ + maxProcessed_ + 1));
		const size_t merged = total < size_t(maxProcessed_) ? total : size_t(maxProcessed_);
		Grow(processed_, merged, size_t(maxProcessed_));
		Grow(cumulative_, merged + 1, size_t(maxProcessed_ + 1));
	}
	class TDigestComparator {
	public:
		TDigestComparator() {
		}

		bool operator()(const TDigest *left, const TDigest *right) const {
			return left->totalSize() > right->totalSize();
		}
	};

	using TDigestQueue = std::priority_queue<const TDigest *, std::vector<const TDigest *>, TDigestComparator>;

public:
	explicit TDigest(duckdb::ArenaAllocator &allocator, Value compression = 1000) : TDigest(allocator, compression, 0) {
	}

	TDigest(duckdb::ArenaAllocator &allocator, Value compression, Index bufferSize)
	    : TDigest(allocator, compression, bufferSize, 0) {
	}

	TDigest(duckdb::ArenaAllocator &allocator, Value compression, Index unmergedSize, Index mergedSize)
	    : compression_(compression), maxProcessed_(processedSize(mergedSize, compression)),
	      maxUnprocessed_(unprocessedSize(unmergedSize, compression)), processed_(allocator), unprocessed_(allocator),
	      cumulative_(allocator) {
		// The buffers grow on demand: an aggregate state per group that only ever sees a handful of values must not
		// pay for the full merge buffer (~1000 centroids with the default compression) up front.
		processed_.reserve(MinimumInt(maxProcessed_, INITIAL_CAPACITY));
		unprocessed_.reserve(MinimumInt(maxUnprocessed_ + maxProcessed_ + 1, INITIAL_CAPACITY));
		cumulative_.reserve(MinimumInt(maxProcessed_ + 1, INITIAL_CAPACITY));
	}

	TDigest(duckdb::ArenaAllocator &allocator, const TDigest &other)
	    : compression_(other.compression_), min_(other.min_), max_(other.max_), maxProcessed_(other.maxProcessed_),
	      maxUnprocessed_(other.maxUnprocessed_), processedWeight_(other.processedWeight_),
	      unprocessedWeight_(other.unprocessedWeight_), processed_(other.processed_, allocator),
	      unprocessed_(other.unprocessed_, allocator), cumulative_(other.cumulative_, allocator) {
		reserveForProcess();
	}

	duckdb::ArenaAllocator &getAllocator() const {
		return processed_.get_allocator().GetAllocator();
	}

	TDigest(duckdb::arena_vector<Centroid> &&processed, duckdb::arena_vector<Centroid> &&unprocessed, Value compression,
	        Index unmergedSize, Index mergedSize)
	    : TDigest(processed.get_allocator().GetAllocator(), compression, unmergedSize, mergedSize) {
		processed_ = std::move(processed);
		unprocessed_ = std::move(unprocessed);

		processedWeight_ = weight(processed_);
		unprocessedWeight_ = weight(unprocessed_);
		if (!processed_.empty()) {
			min_ = std::min(min_, processed_[0].mean());
			max_ = std::max(max_, (processed_.cend() - 1)->mean());
		}
		updateCumulative();
		reserveForProcess();
	}

	static Weight weight(duckdb::arena_vector<Centroid> &centroids) noexcept {
		Weight w = 0.0;
		for (auto centroid : centroids) {
			w += centroid.weight();
		}
		return w;
	}

	TDigest &operator=(TDigest &&o) {
		compression_ = o.compression_;
		maxProcessed_ = o.maxProcessed_;
		maxUnprocessed_ = o.maxUnprocessed_;
		processedWeight_ = o.processedWeight_;
		unprocessedWeight_ = o.unprocessedWeight_;
		processed_ = std::move(o.processed_);
		unprocessed_ = std::move(o.unprocessed_);
		cumulative_ = std::move(o.cumulative_);
		min_ = o.min_;
		max_ = o.max_;
		return *this;
	}

	TDigest(TDigest &&o)
	    : TDigest(std::move(o.processed_), std::move(o.unprocessed_), o.compression_, o.maxUnprocessed_,
	              o.maxProcessed_) {
	}

	static inline Index processedSize(Index size, Value compression) noexcept {
		return (size == 0) ? static_cast<Index>(2 * std::ceil(compression)) : size;
	}

	static inline Index unprocessedSize(Index size, Value compression) noexcept {
		return (size == 0) ? static_cast<Index>(8 * std::ceil(compression)) : size;
	}

	// merge in another t-digest
	inline void merge(const TDigest *other) {
		std::vector<const TDigest *> others {other};
		add(others.cbegin(), others.cend());
	}

	const duckdb::arena_vector<Centroid> &processed() const {
		return processed_;
	}

	const duckdb::arena_vector<Centroid> &unprocessed() const {
		return unprocessed_;
	}

	Index maxUnprocessed() const {
		return maxUnprocessed_;
	}

	Index maxProcessed() const {
		return maxProcessed_;
	}

	Value min() const {
		return min_;
	}

	Value max() const {
		return max_;
	}

	// override the tracked minimum/maximum - used when reconstructing a serialized digest
	void setMinMax(Value min, Value max) {
		min_ = min;
		max_ = max;
	}

	inline void add(std::vector<const TDigest *> digests) {
		add(digests.cbegin(), digests.cend());
	}

	// merge in a vector of tdigests in the most efficient manner possible
	// in constant space
	// works for any value of kHighWater
	void add(std::vector<const TDigest *>::const_iterator iter, std::vector<const TDigest *>::const_iterator end) {
		if (iter != end) {
			auto size = std::distance(iter, end);
			TDigestQueue pq(TDigestComparator {});
			for (; iter != end; iter++) {
				pq.push((*iter));
			}
			std::vector<const TDigest *> batch;
			batch.reserve(size_t(size));

			size_t totalSize = 0;
			while (!pq.empty()) {
				auto td = pq.top();
				batch.push_back(td);
				pq.pop();
				totalSize += td->totalSize();
				if (totalSize >= kHighWater || pq.empty()) {
					mergeProcessed(batch);
					mergeUnprocessed(batch);
					processIfNecessary();
					reserveForProcess();
					batch.clear();
					totalSize = 0;
				}
			}
			updateCumulative();
		}
	}

	Weight processedWeight() const {
		return processedWeight_;
	}

	Weight unprocessedWeight() const {
		return unprocessedWeight_;
	}

	bool haveUnprocessed() const {
		return unprocessed_.size() > 0;
	}

	size_t totalSize() const {
		return processed_.size() + unprocessed_.size();
	}

	long totalWeight() const {
		return static_cast<long>(processedWeight_ + unprocessedWeight_);
	}

	// return the cdf on the t-digest
	Value cdf(Value x) {
		if (haveUnprocessed() || isDirty()) {
			process();
		}
		return cdfProcessed(x);
	}

	bool isDirty() {
		return processed_.size() > maxProcessed_ || unprocessed_.size() > maxUnprocessed_;
	}

	// return the cdf on the processed values
	Value cdfProcessed(Value x) const {
		if (processed_.empty()) {
			// no data to examin_e

			return 0.0;
		} else if (processed_.size() == 1) {
			// exactly one centroid, should have max_==min_
			auto width = max_ - min_;
			if (x < min_) {
				return 0.0;
			} else if (x > max_) {
				return 1.0;
			} else if (x - min_ <= width) {
				// min_ and max_ are too close together to do any viable interpolation
				return 0.5;
			} else {
				// interpolate if somehow we have weight > 0 and max_ != min_
				return (x - min_) / (max_ - min_);
			}
		} else {
			auto n = processed_.size();
			if (x <= min_) {
				return 0;
			}

			if (x >= max_) {
				return 1;
			}

			// check for the left tail
			if (x <= mean(0)) {

				// note that this is different than mean(0) > min_ ... this guarantees interpolation works
				if (mean(0) - min_ > 0) {
					return (x - min_) / (mean(0) - min_) * weight(0) / processedWeight_ / 2.0;
				} else {
					return 0;
				}
			}

			// and the right tail
			if (x >= mean(n - 1)) {
				if (max_ - mean(n - 1) > 0) {
					return 1.0 - (max_ - x) / (max_ - mean(n - 1)) * weight(n - 1) / processedWeight_ / 2.0;
				} else {
					return 1;
				}
			}

			CentroidComparator cc;
			auto iter = std::upper_bound(processed_.cbegin(), processed_.cend(), Centroid(x, 0), cc);

			auto i = size_t(std::distance(processed_.cbegin(), iter));
			auto z1 = x - (iter - 1)->mean();
			auto z2 = (iter)->mean() - x;
			return weightedAverage(cumulative_[i - 1], z2, cumulative_[i], z1) / processedWeight_;
		}
	}

	// this returns a quantile on the t-digest
	Value quantile(Value q) {
		if (haveUnprocessed() || isDirty()) {
			process();
		}
		return quantileProcessed(q);
	}

	// this returns a quantile on the currently processed values without changing the t-digest
	// the value will not represent the unprocessed values
	Value quantileProcessed(Value q) const {
		if (q < 0 || q > 1) {
			return NAN;
		}

		if (processed_.size() == 0) {
			// no sorted means no data, no way to get a quantile
			return NAN;
		} else if (processed_.size() == 1) {
			// with one data point, all quantiles lead to Rome

			return mean(0);
		}

		// we know that there are at least two sorted now
		auto n = processed_.size();

		// if values were stored in a sorted array, index would be the offset we are Weighterested in
		const auto index = q * processedWeight_;

		// at the boundaries, we return min_ or max_
		if (index <= weight(0) / 2.0) {
			return min_ + 2.0 * index / weight(0) * (mean(0) - min_);
		}

		auto iter = std::lower_bound(cumulative_.cbegin(), cumulative_.cend(), index);

		if (iter + 1 != cumulative_.cend()) {
			auto i = size_t(std::distance(cumulative_.cbegin(), iter));
			auto z1 = index - *(iter - 1);
			auto z2 = *(iter)-index;
			// LOG(INFO) << "z2 " << z2 << " index " << index << " z1 " << z1;
			return weightedAverage(mean(i - 1), z2, mean(i), z1);
		}
		auto z1 = index - processedWeight_ - weight(n - 1) / 2.0;
		auto z2 = weight(n - 1) / 2 - z1;
		return weightedAverage(mean(n - 1), z1, max_, z2);
	}

	Value compression() const {
		return compression_;
	}

	void add(Value x) {
		add(x, 1);
	}

	inline void compress() {
		process();
	}

	// add a single centroid to the unprocessed vector, processing previously unprocessed sorted if our limit has
	// been reached.
	inline bool add(Value x, Weight w) {
		if (std::isnan(x)) {
			return false;
		}
		unprocessed_.push_back(Centroid(x, w));
		unprocessedWeight_ += w;
		processIfNecessary();
		reserveForProcess();
		return true;
	}

	inline void add(duckdb::arena_vector<Centroid>::const_iterator iter, duckdb::arena_vector<Centroid>::const_iterator end) {
		while (iter != end) {
			const size_t diff = size_t(std::distance(iter, end));
			const size_t room = maxUnprocessed_ - unprocessed_.size();
			auto mid = iter + int64_t(std::min(diff, room));
			while (iter != mid) {
				unprocessed_.push_back(*(iter++));
			}
			if (unprocessed_.size() >= maxUnprocessed_) {
				process();
			}
		}
		reserveForProcess();
	}

private:
	Value compression_;

	Value min_ = std::numeric_limits<Value>::max();

	Value max_ = std::numeric_limits<Value>::min();

	Index maxProcessed_;

	Index maxUnprocessed_;

	Value processedWeight_ = 0.0;

	Value unprocessedWeight_ = 0.0;

	duckdb::arena_vector<Centroid> processed_;

	duckdb::arena_vector<Centroid> unprocessed_;

	duckdb::arena_vector<Weight> cumulative_;

	// return mean of i-th centroid
	inline Value mean(size_t i) const noexcept {
		return processed_[i].mean();
	}

	// return weight of i-th centroid
	inline Weight weight(size_t i) const noexcept {
		return processed_[i].weight();
	}

	// append all unprocessed centroids into current unprocessed vector
	void mergeUnprocessed(const std::vector<const TDigest *> &tdigests) {
		if (tdigests.size() == 0) {
			return;
		}

		size_t total = unprocessed_.size();
		for (auto &td : tdigests) {
			total += td->unprocessed_.size();
		}

		Grow(unprocessed_, total, size_t(maxUnprocessed_ + maxProcessed_ + 1));
		for (auto &td : tdigests) {
			unprocessed_.insert(unprocessed_.end(), td->unprocessed_.cbegin(), td->unprocessed_.cend());
			unprocessedWeight_ += td->unprocessedWeight_;
		}
	}

	// merge all processed centroids together into a single sorted vector
	void mergeProcessed(const std::vector<const TDigest *> &tdigests) {
		if (tdigests.size() == 0) {
			return;
		}

		size_t total = 0;
		CentroidListQueue pq(CentroidListComparator {});
		for (auto &td : tdigests) {
			auto &sorted = td->processed_;
			auto size = sorted.size();
			if (size > 0) {
				pq.push(CentroidList(sorted));
				total += size;
				processedWeight_ += td->processedWeight_;
			}
		}
		if (total == 0) {
			return;
		}

		if (processed_.size() > 0) {
			pq.push(CentroidList(processed_));
			total += processed_.size();
		}

		duckdb::arena_vector<Centroid> sorted(processed_.get_allocator());
		sorted.reserve(total);

		while (!pq.empty()) {
			auto best = pq.top();
			pq.pop();
			sorted.push_back(*(best.iter));
			if (best.advance()) {
				pq.push(best);
			}
		}
		processed_ = std::move(sorted);
		if (processed_.size() > 0) {
			min_ = std::min(min_, processed_[0].mean());
			max_ = std::max(max_, (processed_.cend() - 1)->mean());
		}
	}

	inline void processIfNecessary() {
		if (isDirty()) {
			process();
		}
	}

	void updateCumulative() {
		const auto n = processed_.size();
		cumulative_.clear();
		Grow(cumulative_, n + 1, size_t(maxProcessed_ + 1));
		auto previous = 0.0;
		for (Index i = 0; i < n; i++) {
			auto current = weight(i);
			auto halfCurrent = current / 2.0;
			cumulative_.push_back(previous + halfCurrent);
			previous = previous + current;
		}
		cumulative_.push_back(previous);
	}

	// merges unprocessed_ centroids and processed_ centroids together and processes them
	// when complete, unprocessed_ will be empty and processed_ will have at most maxProcessed_ centroids
	inline void process() {
		CentroidComparator cc;
		std::sort(unprocessed_.begin(), unprocessed_.end(), cc);
		auto count = unprocessed_.size();
		unprocessed_.insert(unprocessed_.end(), processed_.cbegin(), processed_.cend());
		std::inplace_merge(unprocessed_.begin(), unprocessed_.begin() + int64_t(count), unprocessed_.end(), cc);

		processedWeight_ += unprocessedWeight_;
		unprocessedWeight_ = 0;
		processed_.clear();

		processed_.push_back(unprocessed_[0]);
		Weight wSoFar = unprocessed_[0].weight();
		Weight wLimit = processedWeight_ * integratedQ(1.0);

		auto end = unprocessed_.end();
		for (auto iter = unprocessed_.cbegin() + 1; iter < end; iter++) {
			auto &centroid = *iter;
			Weight projectedW = wSoFar + centroid.weight();
			if (projectedW <= wLimit) {
				wSoFar = projectedW;
				(processed_.end() - 1)->add(centroid);
			} else {
				auto k1 = integratedLocation(wSoFar / processedWeight_);
				wLimit = processedWeight_ * integratedQ(k1 + 1.0);
				wSoFar += centroid.weight();
				processed_.emplace_back(centroid);
			}
		}
		unprocessed_.clear();
		min_ = std::min(min_, processed_[0].mean());
		max_ = std::max(max_, (processed_.cend() - 1)->mean());
		updateCumulative();
	}

	inline size_t checkWeights() {
		return checkWeights(processed_, processedWeight_);
	}

	size_t checkWeights(const duckdb::arena_vector<Centroid> &sorted, Value total) {
		size_t badWeight = 0;
		auto k1 = 0.0;
		auto q = 0.0;
		for (auto iter = sorted.cbegin(); iter != sorted.cend(); iter++) {
			auto w = iter->weight();
			auto dq = w / total;
			auto k2 = integratedLocation(q + dq);
			if (k2 - k1 > 1 && w != 1) {
				badWeight++;
			}
			if (k2 - k1 > 1.5 && w != 1) {
				badWeight++;
			}
			q += dq;
			k1 = k2;
		}

		return badWeight;
	}

	/**
	 * Converts a quantile into a centroid scale value.  The centroid scale is nomin_ally
	 * the number k of the centroid that a quantile point q should belong to.  Due to
	 * round-offs, however, we can't align things perfectly without splitting points
	 * and sorted.  We don't want to do that, so we have to allow for offsets.
	 * In the end, the criterion is that any quantile range that spans a centroid
	 * scale range more than one should be split across more than one centroid if
	 * possible.  This won't be possible if the quantile range refers to a single point
	 * or an already existing centroid.
	 * <p/>
	 * This mapping is steep near q=0 or q=1 so each centroid there will correspond to
	 * less q range.  Near q=0.5, the mapping is flatter so that sorted there will
	 * represent a larger chunk of quantiles.
	 *
	 * @param q The quantile scale value to be mapped.
	 * @return The centroid scale value corresponding to q.
	 */
	inline Value integratedLocation(Value q) const {
		return compression_ * (std::asin(2.0 * q - 1.0) + pi / 2) / pi;
	}

	inline Value integratedQ(Value k) const {
		return (std::sin(std::min(k, compression_) * pi / compression_ - pi / 2) + 1) / 2;
	}

	/**
	 * Same as {@link #weightedAverageSorted(Value, Value, Value, Value)} but flips
	 * the order of the variables if <code>x2</code> is greater than
	 * <code>x1</code>.
	 */
	static Value weightedAverage(Value x1, Value w1, Value x2, Value w2) {
		return (x1 <= x2) ? weightedAverageSorted(x1, w1, x2, w2) : weightedAverageSorted(x2, w2, x1, w1);
	}

	/**
	 * Compute the weighted average between <code>x1</code> with a weight of
	 * <code>w1</code> and <code>x2</code> with a weight of <code>w2</code>.
	 * This expects <code>x1</code> to be less than or equal to <code>x2</code>
	 * and is guaranteed to return a number between <code>x1</code> and
	 * <code>x2</code>.
	 */
	static Value weightedAverageSorted(Value x1, Value w1, Value x2, Value w2) {
		const Value x = (x1 * w1 + x2 * w2) / (w1 + w2);
		return std::max(x1, std::min(x, x2));
	}

	static Value interpolate(Value x, Value x0, Value x1) {
		return (x - x0) / (x1 - x0);
	}

	/**
	 * Computes an interpolated value of a quantile that is between two sorted.
	 *
	 * Index is the quantile desired multiplied by the total number of samples - 1.
	 *
	 * @param index              Denormalized quantile desired
	 * @param previousIndex      The denormalized quantile corresponding to the center of the previous centroid.
	 * @param nextIndex          The denormalized quantile corresponding to the center of the following centroid.
	 * @param previousMean       The mean of the previous centroid.
	 * @param nextMean           The mean of the following centroid.
	 * @return  The interpolated mean.
	 */
	static Value quantile(Value index, Value previousIndex, Value nextIndex, Value previousMean, Value nextMean) {
		const auto delta = nextIndex - previousIndex;
		const auto previousWeight = (nextIndex - index) / delta;
		const auto nextWeight = (index - previousIndex) / delta;
		return previousMean * previousWeight + nextMean * nextWeight;
	}
};

} // namespace tdigest
