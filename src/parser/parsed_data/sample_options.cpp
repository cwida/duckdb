#include "duckdb/parser/parsed_data/sample_options.hpp"
#include "duckdb/common/enum_util.hpp"
#include "duckdb/common/to_string.hpp"

namespace duckdb {

// **DEPRECATED**: Use EnumUtil directly instead.
string SampleMethodToString(SampleMethod method) {
	return EnumUtil::ToString(method);
}

SampleOptions::SampleOptions(int64_t seed_) {
	repeatable = false;
	if (seed_ >= 0) {
		seed = static_cast<idx_t>(seed_);
	}
	sample_size = 0;
	is_percentage = false;
	method = SampleMethod::INVALID;
}

unique_ptr<SampleOptions> SampleOptions::Copy() {
	auto result = make_uniq<SampleOptions>();
	result->sample_size = sample_size;
	result->is_percentage = is_percentage;
	result->method = method;
	result->seed = seed;
	result->repeatable = repeatable;
	result->sample_rate = sample_rate;
	return result;
}

void SampleOptions::SetSeed(idx_t new_seed) {
	seed = new_seed;
}

bool SampleOptions::Equals(SampleOptions *a, SampleOptions *b) {
	if (a == b) {
		return true;
	}
	if (!a || !b) {
		return false;
	}
	// if only one is valid, they are not equal
	if (a->seed.IsValid() != b->seed.IsValid()) {
		return false;
	}
	// if both are invalid, then they are technically the same
	if (!a->seed.IsValid() && !b->seed.IsValid()) {
		return true;
	}
	if (a->sample_size != b->sample_size || a->is_percentage != b->is_percentage || a->method != b->method ||
	    a->sample_rate != b->sample_rate || a->seed.GetIndex() != b->seed.GetIndex()) {
		return false;
	}
	return true;
}

void SampleOptions::Verify() const {
	Value size = sample_size;
	if (size.IsNull()) {
		throw SerializationException("Sample size cannot be NULL");
	}
	if (is_percentage) {
		if (!size.DefaultTryCastAs(LogicalType::DOUBLE)) {
			throw SerializationException("Sample size must be a number");
		}
		auto percentage = size.GetValue<double>();
		if (!(percentage >= 0 && percentage <= 100)) {
			throw SerializationException("Sample percentage %f out of range, must be between 0 and 100", percentage);
		}
	} else {
		if (!size.DefaultTryCastAs(LogicalType::BIGINT)) {
			throw SerializationException("Sample size must be a number");
		}
		auto rows = size.GetValue<int64_t>();
		if (rows < 0 || rows > int64_t(MAX_SAMPLE_ROWS)) {
			throw SerializationException("Sample rows %lld out of range, must be between 0 and %lld", rows,
			                             int64_t(MAX_SAMPLE_ROWS));
		}
	}
}

int64_t SampleOptions::GetSeed() const {
	if (seed.IsValid()) {
		return static_cast<int64_t>(seed.GetIndex());
	}
	return -1;
}

} // namespace duckdb
