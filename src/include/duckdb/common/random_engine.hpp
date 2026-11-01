//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/common/random_engine.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/common.hpp"
#include "duckdb/common/limits.hpp"
#include "duckdb/common/mutex.hpp"

#include <random>

namespace duckdb {
class ClientContext;
struct RandomState;

class RandomEngine {
public:
	explicit RandomEngine(int64_t seed = -1);
	~RandomEngine();

	//! Generate a random number between min and max
	double NextRandom(double min, double max);

	//! Generate a random number between 0 and 1
	double NextRandom();
	//! Generate a random number between 0 and 1, using 32-bits as a base
	double NextRandom32();
	double NextRandom32(double min, double max);
	uint32_t NextRandomInteger32(uint32_t min, uint32_t max);
	uint32_t NextRandomInteger();
	uint32_t NextRandomInteger(uint32_t min, uint32_t max);
	uint64_t NextRandomInteger64();

	void SetSeed(uint64_t seed);
	//! A seed for another generator, mixed with OS entropy unless this engine was seeded explicitly
	uint64_t NextSeed();
	uint32_t NextSeed32();

	void RandomData(duckdb::data_ptr_t data, duckdb::idx_t len);

	static RandomEngine &Get(ClientContext &context);

	mutex lock;

private:
	unique_ptr<RandomState> random_state;
	//! Whether the engine was given a seed (at construction or with SetSeed) rather than seeded from entropy
	bool explicitly_seeded;
};

} // namespace duckdb
