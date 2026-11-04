//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/types/hyperloglog.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/bit_utils.hpp"
#include "duckdb/common/types/vector.hpp"
#include "duckdb/common/vector/flat_vector.hpp"

namespace duckdb {

enum class HLLStorageType : uint8_t {
	HLL_V1 = 1, //! Redis HLL
	HLL_V2 = 2, //! Our own implementation
};

class Serializer;
class Deserializer;

//! Algorithms from
//! "New cardinality estimation algorithms for HyperLogLog sketches"
//! Otmar Ertl, arXiv:1702.01284
class HyperLogLogBase {
public:
	static double HLLSigma(double x);
	static double HLLTau(double x);
};

template <idx_t P>
class HyperLogLogP : public HyperLogLogBase {
public:
	static constexpr idx_t PRECISION = P;
	static constexpr idx_t Q = 64 - P;
	static constexpr idx_t M = 1 << P;
	static constexpr double ALPHA = 0.721347520444481703680; // 1 / (2 log(2))

	static double GetErrorRate() {
		return std::sqrt(PI / 2.0) / sqrt(M);
	}

public:
	HyperLogLogP() {
		memset(k, 0, sizeof(k));
	}

	//! Algorithm 1
	inline void InsertElement(hash_t h) {
		const auto i = h & ((1 << P) - 1);
		h >>= P;
		h |= hash_t(1) << Q;
		const uint8_t z = UnsafeNumericCast<uint8_t>(CountZeros<hash_t>::Trailing(h) + 1);
		Update(i, z);
	}

	inline void Update(const idx_t &i, const uint8_t &z) {
		k[i] = MaxValue<uint8_t>(k[i], z);
	}

	inline uint8_t GetRegister(const idx_t &i) const {
		return k[i];
	}

	//! Overwrites a register, for importing an exported sketch. A register holds the position of the first set bit of
	//! a hash, at most Q + 1 - ExtractCounts indexes an array of Q + 2 counts with it, so callers must check that.
	void SetRegister(const idx_t &i, const uint8_t &value) {
		D_ASSERT(value <= Q + 1);
		k[i] = value;
	}

	idx_t Count() const {
		uint32_t c[Q + 2] = {0};
		ExtractCounts(c);
		return static_cast<idx_t>(EstimateCardinality(c));
	}

	//! Algorithm 2
	void Merge(const HyperLogLogP &other) {
		for (idx_t i = 0; i < M; ++i) {
			Update(i, other.k[i]);
		}
	}

public:
	//! Add data to this HLL
	void Update(const Vector &input, const Vector &hash_vec) {
		auto idata = input.Validity();
		const auto hashes = hash_vec.Values<hash_t>();

		if (hash_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			if (idata.IsValid(0)) {
				InsertElement(hashes[0].GetValue());
			}
		} else {
			D_ASSERT(hash_vec.GetVectorType() == VectorType::FLAT_VECTOR);
			const idx_t count = hash_vec.size();
			if (idata.CannotHaveNull()) {
				for (idx_t i = 0; i < count; ++i) {
					const auto hash = hashes[i].GetValue();
					InsertElement(hash);
				}
			} else {
				for (idx_t i = 0; i < count; ++i) {
					if (idata.IsValid(i)) {
						const auto hash = hashes[i].GetValue();
						InsertElement(hash);
					}
				}
			}
		}
	}

	void Update(const Vector &hash_vec) {
		const auto hashes = FlatVector::GetData<const hash_t>(hash_vec);
		if (hash_vec.GetVectorType() == VectorType::CONSTANT_VECTOR) {
			InsertElement(hashes[0]);
		} else {
			D_ASSERT(hash_vec.GetVectorType() == VectorType::FLAT_VECTOR);
			const auto count = hash_vec.size();
			for (idx_t i = 0; i < count; ++i) {
				InsertElement(hashes[i]);
			}
		}
	}

	//! Algorithm 4
	void ExtractCounts(uint32_t *c) const {
		for (idx_t i = 0; i < M; ++i) {
			c[k[i]]++;
		}
	};

	//! Algorithm 6
	static int64_t EstimateCardinality(uint32_t *c) {
		auto z = M * HLLTau((double(M) - c[Q]) / double(M));

		for (idx_t k = Q; k >= 1; --k) {
			z += c[k];
			z *= 0.5;
		}

		z += M * HLLSigma(c[0] / double(M));

		return llroundl(ALPHA * M * M / z);
	};

protected:
	uint8_t k[M];
};

class HyperLogLog : public HyperLogLogP<6> {
public:
	//! Get copy of the HLL
	unique_ptr<HyperLogLog> Copy() const;

	void Serialize(Serializer &serializer) const;
	static unique_ptr<HyperLogLog> Deserialize(Deserializer &deserializer);
};

//! Utility for computing HLL in parallel
class ParallelHyperLogLogLocalState {
public:
	ParallelHyperLogLogLocalState() : count(0) {
	}

public:
	void Update(const Vector &hashes) {
		annotated_lock_guard<annotated_mutex> guard(lock);
		hll.Update(hashes);
		count += hashes.size();
	}

	void Merge(const ParallelHyperLogLogLocalState &other) DUCKDB_REQUIRES(lock) DUCKDB_REQUIRES(other.lock) {
		hll.Merge(other.hll);
		count += other.count;
	}

	pair<idx_t, idx_t> GetCounts() const {
		annotated_lock_guard<annotated_mutex> guard(lock);
		return {hll.Count(), count};
	}

public:
	mutable annotated_mutex lock;

private:
	HyperLogLog hll DUCKDB_GUARDED_BY(lock);
	idx_t count DUCKDB_GUARDED_BY(lock);
};

class ParallelHyperLogLogGlobalState {
public:
	ParallelHyperLogLogGlobalState() {
	}

public:
	ParallelHyperLogLogLocalState &GetLocalState() {
		auto state = make_uniq<ParallelHyperLogLogLocalState>();
		auto &result = *state;
		annotated_lock_guard<annotated_mutex> guard(lock);
		states.emplace_back(std::move(state));
		return result;
	}

	unique_ptr<ParallelHyperLogLogLocalState> GetMergedState() const {
		auto merged_state = make_uniq<ParallelHyperLogLogLocalState>();
		annotated_lock_guard<annotated_mutex> merged_guard(merged_state->lock);

		annotated_lock_guard<annotated_mutex> global_guard(lock);
		for (const auto &state : states) {
			annotated_lock_guard<annotated_mutex> state_guard(state->lock);
			merged_state->Merge(*state);
		}
		return merged_state;
	}

private:
	mutable annotated_mutex lock;
	vector<unique_ptr<ParallelHyperLogLogLocalState>> states DUCKDB_GUARDED_BY(lock);
};

} // namespace duckdb
