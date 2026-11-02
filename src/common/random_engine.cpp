#include "duckdb/common/random_engine.hpp"
#include "duckdb/common/numeric_utils.hpp"
#include "pcg_random.hpp"

#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#else
#include <random>
#endif
namespace duckdb {

struct RandomState {
	RandomState() {
	}

	pcg32 pcg;
};

//! A seed drawn from the operating system's entropy source
static uint64_t EntropySeed() {
#ifdef __linux__
	uint64_t random_seed = 0;
	int result = -1;
#if defined(SYS_getrandom)
	result = static_cast<int>(syscall(SYS_getrandom, &random_seed, sizeof(random_seed), 0));
#endif
	if (result == -1) {
		// Something went wrong with the syscall, we use chrono
		const auto now = std::chrono::high_resolution_clock::now();
		random_seed = static_cast<uint64_t>(now.time_since_epoch().count());
	}
	return random_seed;
#else
	std::random_device device;
	return (static_cast<uint64_t>(device()) << UINT64_C(32)) | static_cast<uint64_t>(device());
#endif
}

RandomEngine::RandomEngine(int64_t seed) : random_state(make_uniq<RandomState>()), explicitly_seeded(seed >= 0) {
	if (seed < 0) {
#ifdef __linux__
		random_state->pcg.seed(EntropySeed());
#else
		random_state->pcg.seed(pcg_extras::seed_seq_from<std::random_device>());
#endif
	} else {
		random_state->pcg.seed(NumericCast<uint64_t>(seed));
	}
}

RandomEngine::~RandomEngine() {
}

double RandomEngine::NextRandom(double min, double max) {
	D_ASSERT(max >= min);
	return min + (NextRandom() * (max - min));
}

double RandomEngine::NextRandom() {
	auto uint64 = NextRandomInteger64();
	return std::ldexp(uint64, -64);
}

double RandomEngine::NextRandom32(double min, double max) {
	D_ASSERT(max >= min);
	return min + (NextRandom32() * (max - min));
}

double RandomEngine::NextRandom32() {
	auto uint32 = NextRandomInteger();
	return std::ldexp(uint32, -32);
}

uint32_t RandomEngine::NextRandomInteger() {
	return random_state->pcg();
}

uint64_t RandomEngine::NextRandomInteger64() {
	return (static_cast<uint64_t>(NextRandomInteger()) << UINT64_C(32)) | static_cast<uint64_t>(NextRandomInteger());
}

uint32_t RandomEngine::NextRandomInteger(uint32_t min, uint32_t max) {
	return min + static_cast<uint32_t>(NextRandom() * double(max - min));
}

uint32_t RandomEngine::NextRandomInteger32(uint32_t min, uint32_t max) {
	return min + static_cast<uint32_t>(NextRandom32() * double(max - min));
}

void RandomEngine::SetSeed(uint64_t seed) {
	random_state->pcg.seed(seed);
	explicitly_seeded = true;
}

uint64_t RandomEngine::NextSeed() {
	auto seed = NextRandomInteger64();
	return explicitly_seeded ? seed : seed ^ EntropySeed();
}

uint32_t RandomEngine::NextSeed32() {
	auto seed = NextRandomInteger();
	return explicitly_seeded ? seed : seed ^ static_cast<uint32_t>(EntropySeed());
}

void RandomEngine::RandomData(duckdb::data_ptr_t data, duckdb::idx_t len) {
	while (len) {
		const auto random_integer = NextRandomInteger();
		const auto next = duckdb::MinValue<duckdb::idx_t>(len, sizeof(random_integer));
		memcpy(data, duckdb::const_data_ptr_cast(&random_integer), next);
		data += next;
		len -= next;
	}
}

} // namespace duckdb
