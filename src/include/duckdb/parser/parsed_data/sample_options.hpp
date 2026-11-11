//===----------------------------------------------------------------------===//
//                         DuckDB
//
// duckdb/parser/parsed_data/sample_options.hpp
//
//
//===----------------------------------------------------------------------===//

#pragma once

#include "duckdb/common/types/value.hpp"
#include "duckdb/common/optional_idx.hpp"

namespace duckdb {

// Invalid is 255 because previously stored databases have SampleMethods according to the current ENUMS and we need to
// maintain that
enum class SampleMethod : uint8_t { SYSTEM_SAMPLE = 0, BERNOULLI_SAMPLE = 1, RESERVOIR_SAMPLE = 2, INVALID = 255 };

// **DEPRECATED**: Use EnumUtil directly instead.
string SampleMethodToString(SampleMethod method);

class SampleOptions {
public:
	// 1 billion rows should be enough.
	static constexpr idx_t MAX_SAMPLE_ROWS = 1000000000;

public:
	explicit SampleOptions(int64_t seed_ = -1);

	Value sample_size;
	bool is_percentage;
	SampleMethod method;
	optional_idx seed = optional_idx::Invalid();
	bool repeatable;
	double sample_rate = -1.0;

	unique_ptr<SampleOptions> Copy();
	void SetSeed(idx_t new_seed);
	static bool Equals(SampleOptions *a, SampleOptions *b);
	void Serialize(Serializer &serializer) const;
	static unique_ptr<SampleOptions> Deserialize(Deserializer &deserializer);
	int64_t GetSeed() const;
	//! Throws if the sample size is not a valid percentage or row count
	void Verify() const;
};

} // namespace duckdb
